/*
 * Tab5 port-specific implementation.
 * Intent: CPU0 compositor for Tab5: consume compact CPU1 snapshots and use validated PIE/scalar paths without making CPU1 repeat final host-side blending work.
 * Layer8 Aug/17/2026
 */
#include "tab5_compose.h"
#include "tab5_guest_bus.h"
/* PX68K_R57A_GUEST_RENDER_JOURNAL */

#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_async_memcpy.h"
#include "esp_timer.h"

#include "tab5_video.h"
#include "tab5_screen_manager.h"
#include "tab5_video_flow.h"
#include "libretro/tab5_video_cpu1.h"
#include "esp_rom_sys.h"

/* R38: compare CPU0-composed candidate against the currently published ScrBuf line before committing it. */
extern int tab5_pie_graphics_diff(const void *a, const void *b, uint32_t bytes);

/* Shared PX68K graphics hot-set. These live in application internal SRAM. */
extern uint8_t BG[0x8000];
extern uint8_t BGCHR8[8 * 8 * 256];
extern uint8_t BGCHR16[16 * 16 * 256];
extern uint8_t Sprite_Regs[0x800];
extern uint16_t BG_LineBuf[1600];
extern void BG_DrawLine(int opaq, int gd);
extern volatile uint32_t GVRAM_RowGeneration[512];
extern uint16_t Pal16[65536];

#define TAB5_COMPOSE_MAX_SLOTS 16u
#define TAB5_COMPOSE_MIN_SLOTS 8u
#define TAB5_COMPOSE_MAX_WIDTH 800u
#define TAB5_CPU_MHZ 360u
#define TAB5_BG_SCRATCH_PIXELS (TAB5_COMPOSE_MAX_WIDTH + 32u)
/* Build 6.15f: the proven 8-slot internal queue is intentionally kept for
 * legacy/common paths.  65K raster traffic gets a separate PSRAM-backed burst
 * queue so CPU1 never has to wait for CPU0 merely to expose a frame. */
#define TAB5_GBT65K_BURST_SLOTS 16u /* R57E50: tight latest-wins queue; stale work is discarded, never backpressures CPU1 */
#define TAB5_GBT65K_BURST_MASK  (TAB5_GBT65K_BURST_SLOTS - 1u)
#define TAB5_GBT65K_CACHE_ROWS 64u /* R57E50: 64KB direct-map decoded-row cache instead of 512KB */
#define TAB5_GBT65K_CACHE_MASK (TAB5_GBT65K_CACHE_ROWS - 1u)
/* Build 5.98g6: runtime traces kept compose pending at zero/qfull=0 while
 * the old 16-slot arena consumed 144 KiB of scarce internal DMA SRAM.  Eight
 * slots were already the supported minimum, so make that the production pool
 * and right-size the early arena to 80 KiB. */
#define TAB5_ARENA_TARGET_BYTES (80u * 1024u)
#define TAB5_ARENA_MIN_BYTES    (80u * 1024u)

#if defined(MALLOC_CAP_SPM)
#define TAB5_FAST_MAILBOX_CAP MALLOC_CAP_SPM
#define TAB5_FAST_MAILBOX_NAME "SPM"
#elif defined(MALLOC_CAP_TCM)
/* ESP-IDF 5.4.x name for the P4 8 KiB zero-wait SPM/TCM region. */
#define TAB5_FAST_MAILBOX_CAP MALLOC_CAP_TCM
#define TAB5_FAST_MAILBOX_NAME "TCM/SPM"
#else
#define TAB5_FAST_MAILBOX_CAP 0u
#define TAB5_FAST_MAILBOX_NAME "none"
#endif

/* Intent: CPU1 publishes compact authoritative snapshots; CPU0 performs final composition so the guest core does not repeat host-visible blending.  Layer8 Aug/17/2026 */
static const char *TAG = "TAB5_COMPOSE";

/* BAT177NW3: CPU1 exact-final reference is the production path. The source-
 * skipping CPU0 caches below are unreachable while
 * TAB5_CPU1_AUTHORITATIVE_REFERENCE=1 and final-stage packet offload is off.
 * Do not reserve >1.3 MiB PSRAM for dormant accelerators. */
#define TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES 0
/* R57E49: reactivate only the normal-65K CPU0 final worker. Keep the old
 * generic 8-slot Internal-SRAM packet pool/GDMA/GRP8 caches disabled. */
#define TAB5_R57E49_ASYNC65K_ONLY 1
#define TAB5_R57E52_SUBMITFIX 1
#define TAB5_R57E53_PREFLIGHT 1
#define TAB5_R57E54_FLOWPACE 1

typedef enum {
    COMPOSE_JOB_BLEND = 0,
    COMPOSE_JOB_GRP8PAIR = 1,
    COMPOSE_JOB_GRP8PAIR_BGSP = 2,
    COMPOSE_JOB_GRP8SPLIT = 3,
    COMPOSE_JOB_GRP8SPLIT_BGSP = 4,
    COMPOSE_JOB_GBT = 5,
    COMPOSE_JOB_GBT_RAWPAIR = 6,
    COMPOSE_JOB_GBT_SCROLLCACHE = 7,
    COMPOSE_JOB_GBT65K = 8,
    COMPOSE_JOB_LEGACY_FINAL = 9,
} compose_job_type_t;

typedef struct {
    uint8_t type;
    uint8_t bottom_page;
    uint8_t top_page;
    uint8_t bg_on_top;
    uint8_t selfcheck;
    uint8_t grp_pri;
    uint8_t bg_pri;
    uint8_t text_pri;
    uint32_t y;
    uint32_t width;
    uint16_t *dst;
    union {
        struct {
            uint16_t bottom[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t top[TAB5_COMPOSE_MAX_WIDTH];
        } blend;
        struct {
            union {
                struct {
                    uint16_t low_pair[TAB5_COMPOSE_MAX_WIDTH];
                    uint16_t high_pair[TAB5_COMPOSE_MAX_WIDTH];
                } pair;
                /* Build 5.53a: keep unequal-scroll source interleaved as raw
                 * 16-bit page-pairs. CPU1 can snapshot each row with at most
                 * two block memcpy operations; CPU0 extracts the selected page
                 * byte while rendering. This deliberately trades Internal SRAM
                 * for much less CPU1 PSRAM/de-interleave work. */
                struct {
                    uint16_t bottom_lo[TAB5_COMPOSE_MAX_WIDTH];
                    uint16_t bottom_hi[TAB5_COMPOSE_MAX_WIDTH];
                    uint16_t top_lo[TAB5_COMPOSE_MAX_WIDTH];
                    uint16_t top_hi[TAB5_COMPOSE_MAX_WIDTH];
                } split;
            } src;
            uint16_t pal[256];
            union {
                uint16_t bg[TAB5_COMPOSE_MAX_WIDTH];
                struct {
                    uint16_t text_pal[256];
                    BG_HOST_LINE_STATE state;
                } host_bgsp;
            } aux;
        } grp8;
        struct {
            uint16_t grp[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t bg_text[TAB5_COMPOSE_MAX_WIDTH];
            uint8_t flags[TAB5_COMPOSE_MAX_WIDTH];
        } gbt;
        /* Build 6.14b: 6112-byte payload at max width, still smaller than
         * the existing GRP8 split packet, so compose_slot_t does not grow. */
        struct {
            uint16_t low_pair[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t high_pair[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t pal[256];
            uint16_t bg_text[TAB5_COMPOSE_MAX_WIDTH];
            uint8_t flags[TAB5_COMPOSE_MAX_WIDTH];
        } gbt_raw;
        struct {
            const uint8_t *gvram;
            uint32_t y_lo_base, y_hi_base;
            uint32_t x_lo, x_hi;
            uint16_t pal[256];
            uint16_t bg_text[TAB5_COMPOSE_MAX_WIDTH];
            uint8_t flags[TAB5_COMPOSE_MAX_WIDTH];
        } gbt_scroll;
        /* BAT167M1: CPU1 has already produced the guest-semantic layer
         * rasters; CPU0 owns the legacy/special priority, transparency and
         * half-transparency final RGB565 composition. All inputs are immutable
         * line snapshots, so CPU0 never reads live WinDraw scratch. */
        struct {
            uint16_t grp[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t grp_sp[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t grp_sp2[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t bg_text[TAB5_COMPOSE_MAX_WIDTH];
            uint8_t flags[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t half_mask;
            uint16_t ix2;
            uint16_t ibit;
            uint8_t vc1_0;
            uint8_t vc2_0;
            uint8_t gon, bgon, ton, tron, pron;
            uint8_t reserved[3];
        } legacy;
        /* R56 timeline contract: the 65K source row is captured when the
         * render ticket is issued.  CPU0 never re-reads mutable guest GVRAM
         * for an older scanline after guest time has advanced. */
        struct {
            uint16_t raw_row[512];
            uint32_t gvram_row, gvram_x;
            uint32_t row_generation;
            uint32_t pal_generation;
            uint32_t frame_epoch;
            uint8_t contrast;
            uint16_t text_x, text_y;
            uint8_t pal_regs[512];
            uint8_t text_idx[TAB5_COMPOSE_MAX_WIDTH];
            uint16_t text_pal[256];
            BG_HOST_LINE_STATE bg_state;
            /* PX68K_R56S4_GBT65K_EXACT_BT
             * Stock WinDraw has already resolved BG/TEXT interaction into
             * BG_LineBuf + Text_TrFlag.  These snapshots are used only when
             * selfcheck bit3 is set; simplified MDX/transparent-TEXT packets
             * continue using text_idx/text_pal/bg_state above. */
            uint16_t exact_bg_text[TAB5_COMPOSE_MAX_WIDTH];
            uint8_t exact_flags[TAB5_COMPOSE_MAX_WIDTH];
        } gbt65k;
    } u __attribute__((aligned(16)));
} compose_slot_t;

_Static_assert((offsetof(compose_slot_t, u) & 15u) == 0u,
               "Build 5.89 raster snapshot payload must be 16-byte aligned");
_Static_assert(sizeof(compose_slot_t) <= 8544u,
               "Build 6.15e must not enlarge the proven 8-slot internal-SRAM pool");

/* Build 5.49: only tiny descriptors/counters live in the 8 KiB zero-wait
 * mailbox.  The raster payloads live in ordinary internal L2 SRAM so we can
 * make the road wide without consuming the scarce TCM/SPM.  Two SPSC rings
 * are used: CPU1->CPU0 ready jobs and CPU0->CPU1 returned free slots. */
typedef struct __attribute__((aligned(64))) {
    volatile uint32_t ready_head;
    volatile uint32_t ready_tail;
    volatile uint32_t free_head;
    volatile uint32_t free_tail;
    volatile uint32_t submit_seq;
    volatile uint32_t done_seq;
    volatile uint32_t notify_count;
    volatile uint32_t ready_empty;
    volatile uint32_t free_empty;
    uint8_t ready_ring[TAB5_COMPOSE_MAX_SLOTS];
    uint8_t free_ring[TAB5_COMPOSE_MAX_SLOTS];
} compose_mailbox_t;

static compose_slot_t *s_slots;
static uint64_t s_slot_render_ticket[TAB5_COMPOSE_MAX_SLOTS];
/* BAT177NW0: no-Screen final-stage publication metadata. */
static uint64_t s_slot_flow_seq[TAB5_COMPOSE_MAX_SLOTS];
static uint32_t s_slot_flow_epoch[TAB5_COMPOSE_MAX_SLOTS];
static uint32_t s_slot_flow_visual[TAB5_COMPOSE_MAX_SLOTS];
static compose_mailbox_t *s_mb;
static TaskHandle_t s_task;
/* PX68K_R57A_GUEST_RENDER_JOURNAL: cross-core producer wake only; never waits. */
void tab5_compose_guest_event_kick(void)
{
    TaskHandle_t t=s_task;
    if (t) xTaskNotifyGive(t);
}
static uint32_t s_slot_count;
static uint32_t s_slot_mask; /* Build 5.60: nonzero for 16/8-slot power-of-two pools. */
static uint32_t s_pool_bytes;
static uint32_t s_mailbox_bytes;
static int s_mailbox_tcm;
/* Build 5.53a: reserve one contiguous L2/DMA arena before USB/text workers can
 * fragment the heap. Raster slots and host-BG scratch are carved from it. */
static uint8_t *s_arena;
static uint32_t s_arena_bytes;
static uint32_t s_arena_used;
static uint16_t *s_bg_line_scratch;
static uint16_t *s_bg_pri_scratch;
static uint8_t *s_bg_flag_scratch;
static uint16_t *s_bgsp_ref_scratch;
static uint16_t *s_grp8split_ref_scratch;
static volatile uint32_t s_pending;
static volatile uint32_t s_submitted;
static volatile uint32_t s_completed;
static volatile uint32_t s_fallback;
static volatile uint32_t s_queue_full;
static volatile uint32_t s_frame_waits;
static volatile uint32_t s_max_pending;
static volatile uint32_t s_last_copy_us;
static volatile uint32_t s_last_blend_us;
static volatile uint32_t s_grp8_submitted;
static volatile uint32_t s_grp8_completed;
static volatile uint32_t s_last_grp8_copy_us;
static volatile uint32_t s_last_grp8_render_us;
static volatile uint32_t s_bgsp_submitted;
static volatile uint32_t s_bgsp_completed;
static volatile uint32_t s_bgsource_pending;
static volatile uint32_t s_last_bgsp_render_us;
static volatile uint32_t s_gbt_submitted;
static volatile uint32_t s_gbt_completed;
static volatile uint32_t s_last_gbt_copy_us;
static volatile uint32_t s_last_gbt_render_us;
static volatile uint32_t s_gbt_raw_submitted;
static volatile uint32_t s_gbt_raw_completed;
static volatile uint32_t s_last_gbt_raw_copy_us;
static volatile uint32_t s_last_gbt_raw_render_us;
/* Build 6.14b2a: guarded ESP32-P4 AXI-GDMA raw-GVRAM snapshot.
 * The externally visible pending flag lets the guest GVRAM writer avoid a
 * function call in the overwhelmingly common zero-pending case. */
volatile uint32_t tab5_compose_gvram_dma_pending;
static async_memcpy_handle_t s_gdma614b2;
static int s_gdma614b2_ready;
static volatile uint32_t s_gdma614b2_lines;
static volatile uint32_t s_gdma614b2_reqs;
static volatile uint32_t s_gdma614b2_submit_fail;
static volatile uint32_t s_last_gdma614b2_us;
static volatile uint32_t s_gvram_barrier_calls;
static volatile uint32_t s_gvram_barrier_waits;
static volatile uint32_t s_gvram_barrier_us;
volatile uint32_t tab5_compose_gvram_cache_pending;
typedef struct {
    uint32_t gen_lo, gen_hi;
    uint16_t y_hi, dx_hi;
    uint8_t bottom_page, top_page, valid, reserved;
} scroll614c_meta_t;
static uint8_t *s_scroll614c_idx;
static scroll614c_meta_t *s_scroll614c_meta;
static int s_scroll614c_ready;
static volatile uint32_t s_scroll614c_submitted, s_scroll614c_completed;
static volatile uint32_t s_scroll614c_hits, s_scroll614c_misses, s_scroll614c_rebuilds;
static volatile uint32_t s_last_scroll614c_build_us, s_last_scroll614c_render_us;
/* Build 6.15e: 512x512 decoded RGB565 cache for normal 65K graphics. */
typedef struct {
    uint32_t row_gen;
    uint32_t pal_gen;
    uint16_t source_row;
    uint8_t contrast;
    uint8_t valid;
} gbt65k_meta_t;
static uint16_t *s_gbt65k_rgb;
static gbt65k_meta_t *s_gbt65k_meta;
static int s_gbt65k_ready;
static int s_gbt65k_pal_selfcheck_state; /* 0=untested, 1=pass, 2=failed */
static volatile uint32_t s_gbt65k_submitted, s_gbt65k_completed;
static volatile uint32_t s_gbt65k_hits, s_gbt65k_misses, s_gbt65k_rebuilds;
static volatile uint32_t s_last_gbt65k_build_us, s_last_gbt65k_render_us;
/* Build 6.15f: dedicated asynchronous 65K burst queue.  Its pending counter is
 * deliberately NOT part of s_pending, so WinDraw_Draw() does not re-couple
 * CPU1 to CPU0 at every rendered guest frame.  GVRAM/BG writer barriers still
 * protect the shared sources and therefore also drain this queue on guest
 * mutation/reset. */
static compose_slot_t *s_gbt65k_burst_slots;
static uint64_t s_gbt65k_burst_render_ticket[TAB5_GBT65K_BURST_SLOTS];
static uint64_t s_gbt65k_burst_flow_seq[TAB5_GBT65K_BURST_SLOTS];
static uint32_t s_gbt65k_burst_flow_epoch[TAB5_GBT65K_BURST_SLOTS];
static uint32_t s_gbt65k_burst_flow_visual[TAB5_GBT65K_BURST_SLOTS];
static volatile uint32_t s_r57e49_latest_seq32[600];
static uint8_t s_gbt65k_ready_ring[TAB5_GBT65K_BURST_SLOTS];
static uint8_t s_gbt65k_free_ring[TAB5_GBT65K_BURST_SLOTS];
static volatile uint32_t s_gbt65k_ready_head, s_gbt65k_ready_tail;
static volatile uint32_t s_gbt65k_free_head, s_gbt65k_free_tail;
static volatile uint32_t s_gbt65k_burst_pending;
static volatile uint32_t s_gbt65k_burst_max_pending;
static volatile uint32_t s_gbt65k_burst_full;
static volatile uint32_t s_gbt65k_burst_notify;
static int s_gbt65k_burst_ready;
/* 0=untested, 1=PIE exact, 2=PIE disabled -> scalar reference. */
static int s_gbt65k_pie_selfcheck_state;
static uint16_t *s_gbt65k_ref_scratch;
static uint16_t *s_screen_result_scratch;
/* R38 producer-side exact dirty accounting. Stale jobs and bit-identical
 * composed lines retire their writer without bumping the LCD generation. */
static uint64_t s_r38_prod_same = 0u;
static uint64_t s_r38_prod_changed = 0u;
static uint64_t s_r38_prod_stale_nogen = 0u;
static uint64_t s_r40_prod_changed_pixels = 0u;
static uint64_t s_r42_prod_changed_tiles = 0u;
static uint64_t s_r42_prod_published_pixels = 0u;
static volatile uint32_t s_gbt65k_pie_lines;
static volatile uint32_t s_gbt65k_scalar_lines;
/* PX68K_R56S5_HOSTBT_EXACT: 0=untested 1=live A/B pending 2=exact 3=failed. */
static volatile uint32_t s_r56s5_hostbt_state;
/* R57E5: state 0=pending, 2=certified. Shadow mismatches self-heal on CPU0; no permanent CPU1 fallback state. */
#define R57D_CLASS_COUNT 1024u
#define R57D_CLASS_PASS_NEED 8u
static uint8_t *s_r57d_class_state;
static uint8_t *s_r57d_class_pass;
static uint16_t *s_r57d_class_epoch;
static uint32_t s_r57d_slot_hold[TAB5_COMPOSE_MAX_SLOTS];
static uint32_t s_r57d_burst_hold[TAB5_GBT65K_BURST_SLOTS];
static volatile uint32_t s_r57d_shadow_render_active;
static const uint8_t *s_r57d_bg_render_src, *s_r57d_c8_render_src;
static const uint8_t *s_r57d_c16_render_src, *s_r57d_sprite_render_src;
static const uint8_t *s_r57e_tvram_render_src;
static volatile uint32_t s_r57d_hold_wait_fail;
static volatile uint32_t s_r57e_text_shadow_lines, s_r57e_text_shadow_fail, s_r57e_text_shadow_match, s_r57e_text_shadow_repair;
static volatile uint32_t s_gbt65k_frame_epoch = 1u;
static volatile uint32_t s_gbt65k_render_seq;
static volatile uint32_t s_gbt65k_budget_mode;
static volatile uint32_t s_gbt65k_admit_bands = 1u;
static volatile uint32_t s_gbt65k_stale_dropped;
static volatile uint32_t s_gbt65k_window_skipped;
static volatile uint32_t s_gbt65k_cpu0_work_us;
static volatile uint32_t s_bg_barrier_calls;
static volatile uint32_t s_bg_barrier_waits;
static volatile uint32_t s_bg_barrier_us;
/* BAT167M1 CPU0 legacy-final compositor diagnostics. */
static volatile uint32_t s_legacy_final_submitted, s_legacy_final_completed;
static volatile uint32_t s_legacy_final_fallback;
static volatile uint32_t s_last_legacy_final_copy_us, s_last_legacy_final_render_us;
/* 0=not tried, 1=reference pending, 2=verified, 3=failed/disable host BGSP. */
static volatile uint32_t s_bgsp_selfcheck_state;
/* 0=not tried, 1=reference pending, 2=verified, 3=failed/disable split-scroll host GRP8. */
static volatile uint32_t s_grp8split_selfcheck_state;
static int s_ready;
static int s_gbt614a_selfcheck_ok;

typedef struct {
    volatile uint32_t pending_reqs;
    volatile uint32_t metadata_ready;
    volatile uint32_t queued;
    volatile uint32_t gvram_released;
    int64_t start_us;
    uint8_t slot_idx;
    uint8_t reserved[3];
} gbt_raw_dma_state_t;

static gbt_raw_dma_state_t s_gbt_raw_dma[TAB5_COMPOSE_MAX_SLOTS];

/* Build 5.99rc1: CPU0 common two-layer compositor uses fixed PIE after
 * deterministic startup self-check; live A/B timing and runtime shadow were
 * development-only and are retired. */
static uint8_t s_cblend598g3_selfcheck_ok;

/* Build 5.91: production P4 PIE/XespV raster snapshot backend.
 * 5.89a live A/B on real 512-byte GVRAM runs measured roughly 0.49 host
 * cycles/byte for PIE-128 versus 5.30 cycles/byte for libc memcpy (~10.9x).
 * Production therefore selects PIE for every eligible aligned run.
 * Unaligned, sub-64-byte, or self-check-failed runs retain the proven memcpy
 * fallback.  Raster snapshot semantics and packet contents are unchanged.
 *
 * Keep the PIE helper in this translation unit so incremental PlatformIO
 * builds cannot reuse a stale px68k component archive without the symbol. */
#if defined(__riscv)
__asm__(
    ".section .iram1,\"ax\"\n"
    ".align 2\n"
    ".global tab5_xespv_copy16_blocks\n"
    ".type tab5_xespv_copy16_blocks, @function\n"
    ".balign 4\n"
    "tab5_xespv_copy16_blocks:\n"
    "beqz a2, 3f\n"
    "andi t0, a2, 1\n"
    "srli t1, a2, 1\n"
    "beqz t1, 2f\n"
    "1:\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vld.128.ip q1, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "esp.vst.128.ip q1, a0, 16\n"
    "addi t1, t1, -1\n"
    "bnez t1, 1b\n"
    "2:\n"
    "beqz t0, 3f\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "3:\n"
    "ret\n"
    ".size tab5_xespv_copy16_blocks, .-tab5_xespv_copy16_blocks\n"
    ".previous\n"
);
extern void tab5_xespv_copy16_blocks(void *dst, const void *src, uint32_t blocks);
#else
/* Host-only fallback for syntax/unit checks; never selected on ESP32-P4. */
static void tab5_xespv_copy16_blocks(void *dst, const void *src, uint32_t blocks)
{
    memcpy(dst, src, (size_t)blocks * 16u);
}
#endif
static int s_raster590_pie_enabled;
static uint8_t s_raster590_probe_src[256] __attribute__((aligned(16)));
static uint8_t s_raster590_probe_dst[256] __attribute__((aligned(16)));

static inline uint32_t cycles_to_us(uint32_t cycles)
{
    return (cycles + TAB5_CPU_MHZ - 1u) / TAB5_CPU_MHZ;
}

static inline uint32_t load_acquire(volatile uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline __attribute__((always_inline)) uint32_t slot_ring_index(uint32_t seq)
{
    if (__builtin_expect(s_slot_mask != 0u, 1))
        return seq & s_slot_mask;
    return seq % s_slot_count;
}

static inline uint32_t load_relaxed(volatile uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void store_release(volatile uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline void blend_key0(uint16_t *dst, const uint16_t *bottom,
                              const uint16_t *top, uint32_t width)
{
    uint32_t i = 0;
    for (; i + 3u < width; i += 4u) {
        const uint16_t t0 = top[i + 0u];
        const uint16_t t1 = top[i + 1u];
        const uint16_t t2 = top[i + 2u];
        const uint16_t t3 = top[i + 3u];
        dst[i + 0u] = t0 ? t0 : bottom[i + 0u];
        dst[i + 1u] = t1 ? t1 : bottom[i + 1u];
        dst[i + 2u] = t2 ? t2 : bottom[i + 2u];
        dst[i + 3u] = t3 ? t3 : bottom[i + 3u];
    }
    for (; i < width; ++i) {
        const uint16_t t = top[i];
        dst[i] = t ? t : bottom[i];
    }
}

#if defined(__riscv)
/* a0=dst, a1=bottom, a2=top, a3=8-pixel blocks.  Each stream owns one
 * post-increment cursor, avoiding the g/g1 dst double-increment bug. */
__asm__(
    ".section .iram1,\"ax\"\n"
    ".align 2\n"
    ".global tab5_compose598g3_blend3_8\n"
    ".type tab5_compose598g3_blend3_8, @function\n"
    ".balign 4\n"
    "tab5_compose598g3_blend3_8:\n"
    "beqz a3, 2f\n"
    "esp.xorq q7, q7, q7\n"
    "1:\n"
    "esp.vld.128.ip q0, a2, 16\n"      /* top */
    "esp.vld.128.ip q1, a1, 16\n"      /* bottom */
    "esp.vcmp.eq.u16 q2, q0, q7\n"
    "esp.andq q1, q1, q2\n"
    "esp.orq q0, q0, q1\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a3, a3, -1\n"
    "bnez a3, 1b\n"
    "2:\n"
    "ret\n"
    ".size tab5_compose598g3_blend3_8, .-tab5_compose598g3_blend3_8\n"
    ".previous\n"
);
extern void tab5_compose598g3_blend3_8(uint16_t *dst, const uint16_t *bottom,
                                       const uint16_t *top, uint32_t blocks8);
#else
static void tab5_compose598g3_blend3_8(uint16_t *dst, const uint16_t *bottom,
                                       const uint16_t *top, uint32_t blocks8)
{
    blend_key0(dst, bottom, top, blocks8 * 8u);
}
#endif

/* Execute a direct three-source PIE blend when all three streams can be
 * brought to the same 16-byte boundary.  A short scalar prefix/tail makes
 * same-offset misaligned rows eligible without a scratch copy.  Returns 1
 * only when at least one 128-bit PIE block actually ran. */
static inline int cblend598g3_pie(uint16_t *dst, const uint16_t *bottom,
                                 const uint16_t *top, uint32_t width)
{
    const uintptr_t da = (uintptr_t)dst & 15u;
    const uintptr_t ba = (uintptr_t)bottom & 15u;
    const uintptr_t ta = (uintptr_t)top & 15u;
    if (!s_cblend598g3_selfcheck_ok || da != ba || da != ta) {
        blend_key0(dst, bottom, top, width);
        return 0;
    }

    uint32_t prefix = da ? (uint32_t)((16u - da) >> 1) : 0u;
    if (prefix > width) prefix = width;
    if (prefix) {
        blend_key0(dst, bottom, top, prefix);
        dst += prefix; bottom += prefix; top += prefix; width -= prefix;
    }

    const uint32_t blocks = width >> 3;
    const uint32_t vec = blocks << 3;
    if (!blocks) {
        if (width) blend_key0(dst, bottom, top, width);
        return 0;
    }
    tab5_compose598g3_blend3_8(dst, bottom, top, blocks);
    if (vec != width)
        blend_key0(dst + vec, bottom + vec, top + vec, width - vec);
    return 1;
}

static void cblend598g3_selfcheck(void)
{
    /* Reuse CPU0-only host scratch: no permanent SRAM cost. */
    if (!s_bg_line_scratch || !s_bg_pri_scratch || !s_bgsp_ref_scratch) {
        s_cblend598g3_selfcheck_ok = 0;
        ESP_LOGW(TAG, "Build 5.98g3 PIE-3SRC self-check skipped: scratch unavailable");
        return;
    }
    static const uint16_t widths[] = {8, 16, 31, 256, 512, 768, 800};
    s_cblend598g3_selfcheck_ok = 1; /* allow helper during the check */
    for (uint32_t pat = 0; pat < 4u; ++pat) {
        for (uint32_t wi = 0; wi < sizeof(widths)/sizeof(widths[0]); ++wi) {
            const uint32_t width = widths[wi];
            for (uint32_t i = 0; i < width; ++i) {
                uint16_t b = (uint16_t)((i * 1103u + pat * 7919u + 0x1234u) & 0xffffu);
                if (!b) b = 0x39e7u;
                uint16_t t;
                if (pat == 0u) t = 0u;
                else if (pat == 1u) { t = (uint16_t)((i * 977u + 1u) & 0xffffu); if (!t) t = 1u; }
                else if (pat == 2u) t = (i & 1u) ? (uint16_t)(0x8001u + i) : 0u;
                else { t = ((i % 3u) == 0u || (i % 11u) == 0u) ? 0u : (uint16_t)((i * 3571u + 0x55aau) & 0xffffu); if (!t && (i % 3u) && (i % 11u)) t = 0x1234u; }
                s_bg_pri_scratch[i] = b;
                s_bgsp_ref_scratch[i] = t;
                s_bg_line_scratch[i] = 0xdead;
            }
            if (!cblend598g3_pie(s_bg_line_scratch, s_bg_pri_scratch, s_bgsp_ref_scratch, width)) {
                s_cblend598g3_selfcheck_ok = 0;
                ESP_LOGE(TAG, "PX68K_COMPOSE598G3: PIE-3SRC synthetic SKIP width=%lu align=%lu/%lu/%lu; disabled",
                         (unsigned long)width,
                         (unsigned long)((uintptr_t)s_bg_line_scratch & 15u),
                         (unsigned long)((uintptr_t)s_bg_pri_scratch & 15u),
                         (unsigned long)((uintptr_t)s_bgsp_ref_scratch & 15u));
                return;
            }
            for (uint32_t i = 0; i < width; ++i) {
                const uint16_t exp = s_bgsp_ref_scratch[i] ? s_bgsp_ref_scratch[i] : s_bg_pri_scratch[i];
                if (s_bg_line_scratch[i] != exp) {
                    s_cblend598g3_selfcheck_ok = 0;
                    ESP_LOGE(TAG, "PX68K_COMPOSE598G3: PIE-3SRC synthetic FAIL pat=%lu width=%lu x=%lu bottom=%04X top=%04X got=%04X exp=%04X; disabled",
                             (unsigned long)pat, (unsigned long)width, (unsigned long)i,
                             s_bg_pri_scratch[i], s_bgsp_ref_scratch[i], s_bg_line_scratch[i], exp);
                    return;
                }
            }
        }
    }
    ESP_LOGI(TAG, "PX68K_COMPOSE599RC1: PIE-3SRC synthetic PASS; fixed production backend=PIE-128 with scalar alignment fallback");
}

static inline void cblend598g3_run_job(uint16_t *dst, const uint16_t *bottom,
                                       const uint16_t *top, uint32_t width)
{
    /* cblend598g3_pie() performs the exact scalar fallback itself when the
     * self-check failed or the three streams cannot be aligned together. */
    (void)cblend598g3_pie(dst, bottom, top, width);
}

static void tab5_raster590_selfcheck(void)
{
    for (uint32_t i = 0; i < sizeof(s_raster590_probe_src); ++i)
        s_raster590_probe_src[i] = (uint8_t)(0x39u + i * 17u);
    memset(s_raster590_probe_dst, 0, sizeof(s_raster590_probe_dst));
    tab5_xespv_copy16_blocks(s_raster590_probe_dst, s_raster590_probe_src,
                             (uint32_t)(sizeof(s_raster590_probe_src) / 16u));
    s_raster590_pie_enabled =
        (memcmp(s_raster590_probe_dst, s_raster590_probe_src,
                sizeof(s_raster590_probe_src)) == 0);
    ESP_LOGI(TAG,
             "Build 5.91 P4 PIE/XespV raster snapshot self-check %s; production backend=%s (aligned >=64B, memcpy fallback)",
             s_raster590_pie_enabled ? "PASS" : "FAIL",
             s_raster590_pie_enabled ? "PIE-128" : "MEMCPY");
}

static inline __attribute__((always_inline)) void
tab5_raster590_copy_run(uint8_t *dst, const uint8_t *src, uint32_t bytes)
{
    const int eligible = s_raster590_pie_enabled && bytes >= 64u &&
                         ((((uintptr_t)dst | (uintptr_t)src) & 15u) == 0u);
    if (__builtin_expect(eligible, 1)) {
        const uint32_t blocks = bytes >> 4;
        const uint32_t vec_bytes = blocks << 4;
        tab5_xespv_copy16_blocks(dst, src, blocks);
        if (__builtin_expect(vec_bytes != bytes, 0))
            memcpy(dst + vec_bytes, src + vec_bytes, bytes - vec_bytes);
        return;
    }
    memcpy(dst, src, bytes);
}

static inline void snapshot_pair_lane(uint16_t *dst, const uint8_t *gvram,
                                      uint32_t y_base, uint32_t x,
                                      uint32_t width)
{
    uint32_t remaining = width;
    x &= 0x1ffu;
    while (remaining) {
        uint32_t run = 0x200u - x;
        if (run > remaining)
            run = remaining;
        tab5_raster590_copy_run((uint8_t *)dst,
                                 gvram + y_base + (x << 1),
                                 run * (uint32_t)sizeof(uint16_t));
        dst += run;
        remaining -= run;
        x = 0u;
    }
}

typedef struct {
    uint8_t *dst;
    const uint8_t *src;
    uint32_t bytes;
    uint8_t dma_ok;
} gdma614b2_run_t;

static inline uint32_t gdma614b2_build_pair_runs(gdma614b2_run_t *runs,
                                                  uint32_t n,
                                                  uint16_t *dst,
                                                  const uint8_t *gvram,
                                                  uint32_t y_base, uint32_t x,
                                                  uint32_t width)
{
    uint32_t remaining = width;
    x &= 0x1ffu;
    while (remaining && n < 4u) {
        uint32_t run = 0x200u - x;
        if (run > remaining) run = remaining;
        const uint32_t bytes = run * (uint32_t)sizeof(uint16_t);
        uint8_t *d = (uint8_t *)dst;
        const uint8_t *sp = gvram + y_base + (x << 1);
        runs[n].dst = d;
        runs[n].src = sp;
        runs[n].bytes = bytes;
        /* IDF async memcpy on P4 can move PSRAM with proper alignment.
         * Keep the production gate stricter than the driver: 16-byte aligned
         * endpoints/length and at least one cache line worth of payload. */
        runs[n].dma_ok = (uint8_t)(s_gdma614b2_ready && bytes >= 64u &&
            ((((uintptr_t)d | (uintptr_t)sp | (uintptr_t)bytes) & 15u) == 0u));
        ++n;
        dst += run;
        remaining -= run;
        x = 0u;
    }
    return n;
}

static inline void snapshot_page_stream(uint8_t *dst, const uint8_t *gvram,
                                        uint32_t y_base, uint32_t x, int page,
                                        uint32_t width)
{
    uint32_t remaining = width;
    x &= 0x1ffu;
    page &= 1;
    while (remaining) {
        uint32_t run = 0x200u - x;
        if (run > remaining)
            run = remaining;
        const uint8_t *src8 = gvram + y_base + (x << 1);
#if !defined(MSB_FIRST)
        if (((uintptr_t)src8 & 1u) == 0u) {
            const uint16_t *src16 = (const uint16_t *)src8;
            const unsigned shift = (unsigned)page * 8u;
            uint32_t i = 0;
            for (; i + 4u <= run; i += 4u) {
                dst[i + 0u] = (uint8_t)(src16[i + 0u] >> shift);
                dst[i + 1u] = (uint8_t)(src16[i + 1u] >> shift);
                dst[i + 2u] = (uint8_t)(src16[i + 2u] >> shift);
                dst[i + 3u] = (uint8_t)(src16[i + 3u] >> shift);
            }
            for (; i < run; ++i)
                dst[i] = (uint8_t)(src16[i] >> shift);
        } else
#endif
        {
            const uint8_t *sp = src8 + page;
            for (uint32_t i = 0; i < run; ++i) {
                dst[i] = *sp;
                sp += 2;
            }
        }
        dst += run;
        remaining -= run;
        x = 0u;
    }
}

static inline void render_grp8split_bg(uint16_t *dst,
                                       const uint16_t *bottom_lo,
                                       const uint16_t *bottom_hi,
                                       const uint16_t *top_lo,
                                       const uint16_t *top_hi,
                                       const uint16_t *pal,
                                       const uint16_t *bg,
                                       uint32_t width, int bottom_page,
                                       int top_page, int bg_on_top)
{
    const unsigned bshift = (unsigned)(bottom_page & 1) * 8u;
    const unsigned tshift = (unsigned)(top_page & 1) * 8u;
    for (uint32_t i = 0; i < width; ++i) {
        const uint8_t blo = (uint8_t)(bottom_lo[i] >> bshift);
        const uint8_t bhi = (uint8_t)(bottom_hi[i] >> bshift);
        const uint8_t tlo = (uint8_t)(top_lo[i] >> tshift);
        const uint8_t thi = (uint8_t)(top_hi[i] >> tshift);
        const uint16_t bidx = (uint16_t)((bhi & 0xf0u) | (blo & 0x0fu));
        const uint16_t tidx = (uint16_t)((thi & 0xf0u) | (tlo & 0x0fu));
        const uint16_t grp = pal[tidx ? tidx : bidx];
        const uint16_t bgc = bg[i];
        dst[i] = bg_on_top ? (bgc ? bgc : grp) : (grp ? grp : bgc);
    }
}

static inline void render_grp8pair_bg(uint16_t *dst,
                                      const uint16_t *low_pair,
                                      const uint16_t *high_pair,
                                      const uint16_t *pal,
                                      const uint16_t *bg,
                                      uint32_t width,
                                      int bottom_page, int top_page,
                                      int bg_on_top)
{
    uint32_t i = 0;
    bottom_page &= 1;
    top_page &= 1;
    for (; i + 3u < width; i += 4u) {
        for (uint32_t k = 0; k < 4u; ++k) {
            const uint16_t lw = low_pair[i + k];
            const uint16_t hw = high_pair[i + k];
            const uint16_t p0 = (uint16_t)((hw & 0x00f0u) | (lw & 0x000fu));
            const uint16_t p1 = (uint16_t)(((hw >> 8) & 0x00f0u) | ((lw >> 8) & 0x000fu));
            const uint16_t bidx = bottom_page ? p1 : p0;
            const uint16_t tidx = top_page ? p1 : p0;
            const uint16_t grp = pal[tidx ? tidx : bidx];
            const uint16_t bgc = bg[i + k];
            dst[i + k] = bg_on_top ? (bgc ? bgc : grp) : (grp ? grp : bgc);
        }
    }
    for (; i < width; ++i) {
        const uint16_t lw = low_pair[i];
        const uint16_t hw = high_pair[i];
        const uint16_t p0 = (uint16_t)((hw & 0x00f0u) | (lw & 0x000fu));
        const uint16_t p1 = (uint16_t)(((hw >> 8) & 0x00f0u) | ((lw >> 8) & 0x000fu));
        const uint16_t bidx = bottom_page ? p1 : p0;
        const uint16_t tidx = top_page ? p1 : p0;
        const uint16_t grp = pal[tidx ? tidx : bidx];
        const uint16_t bgc = bg[i];
        dst[i] = bg_on_top ? (bgc ? bgc : grp) : (grp ? grp : bgc);
    }
}

/* Build 6.14a: common three-layer final compositor.
 *
 * CPU1 has already produced the authoritative GRP line and PX68K's shared
 * BG/TEXT raster.  Text_TrFlag bit0/bit1 says whether TEXT/BG-SP contributed
 * at this pixel.  The shared raster contains the higher-priority visible
 * BG/TEXT candidate because PX68K generates those two layers in priority
 * order.  Therefore the remaining job is just a two-candidate priority/key0
 * selection against GRP.
 *
 * Smaller numeric priority is visually higher.  On an equal priority the
 * stock compositor orders TEXT > BG/SP > GRP, so a present BG/TEXT candidate
 * wins ties against GRP.  RGB565 value zero remains transparent for overlay
 * passes, matching WD_SUB() in windraw.c. */
static inline void render_gbt_line(uint16_t *dst,
                                   const uint16_t *grp,
                                   const uint16_t *bg_text,
                                   const uint8_t *flags,
                                   uint32_t width,
                                   uint8_t grp_pri,
                                   uint8_t bg_pri,
                                   uint8_t text_pri)
{
    grp_pri &= 3u;
    bg_pri &= 3u;
    text_pri &= 3u;

    for (uint32_t i = 0; i < width; ++i) {
        const uint16_t g = grp[i];
        const uint16_t bt = bg_text[i];
        const uint8_t f = (uint8_t)(flags[i] & 3u);

        if (__builtin_expect(f == 0u, 0)) {
            dst[i] = g;
            continue;
        }

        uint8_t bt_pri = 4u;
        if (f & 2u) bt_pri = bg_pri;
        if ((f & 1u) && text_pri < bt_pri) bt_pri = text_pri;

        if (bt_pri <= grp_pri)
            dst[i] = bt ? bt : g;
        else
            dst[i] = g ? g : bt;
    }
}

/* Build 6.14b: reconstruct the common shared-scroll 256-colour GRP pair
 * directly from the packed GVRAM lanes and immediately apply the validated
 * 6.14a G/B/T selector.  No intermediate GRP line is materialized. */
static inline void render_grp8pair_gbt(uint16_t *dst,
                                       const uint16_t *low_pair,
                                       const uint16_t *high_pair,
                                       const uint16_t *pal,
                                       const uint16_t *bg_text,
                                       const uint8_t *flags,
                                       uint32_t width,
                                       int bottom_page, int top_page,
                                       uint8_t grp_pri,
                                       uint8_t bg_pri,
                                       uint8_t text_pri)
{
    bottom_page &= 1;
    top_page &= 1;
    grp_pri &= 3u;
    bg_pri &= 3u;
    text_pri &= 3u;

    for (uint32_t i = 0; i < width; ++i) {
        const uint16_t lw = low_pair[i];
        const uint16_t hw = high_pair[i];
        const uint16_t p0 = (uint16_t)((hw & 0x00f0u) | (lw & 0x000fu));
        const uint16_t p1 = (uint16_t)(((hw >> 8) & 0x00f0u) | ((lw >> 8) & 0x000fu));
        const uint16_t bidx = bottom_page ? p1 : p0;
        const uint16_t tidx = top_page ? p1 : p0;
        const uint16_t g = pal[tidx ? tidx : bidx];
        const uint16_t bt = bg_text[i];
        const uint8_t f = (uint8_t)(flags[i] & 3u);

        if (__builtin_expect(f == 0u, 0)) {
            dst[i] = g;
            continue;
        }

        uint8_t bt_pri = 4u;
        if (f & 2u) bt_pri = bg_pri;
        if ((f & 1u) && text_pri < bt_pri) bt_pri = text_pri;
        dst[i] = (bt_pri <= grp_pri) ? (bt ? bt : g) : (g ? g : bt);
    }
}


/* Build 6.14c: decoded GRP8 scroll cache. */
static int scroll614c_get_row(const uint8_t *gvram,
                              uint32_t y_lo, uint32_t y_hi, uint32_t dx_hi,
                              int bottom_page, int top_page,
                              const uint8_t **row_out)
{
    if (!s_scroll614c_ready || !gvram || !row_out) return 0;
    y_lo &= 511u; y_hi &= 511u; dx_hi &= 511u;
    bottom_page &= 1; top_page &= 1;
    scroll614c_meta_t *m = &s_scroll614c_meta[y_lo];
    const uint32_t gl = __atomic_load_n(&GVRAM_RowGeneration[y_lo], __ATOMIC_ACQUIRE);
    const uint32_t gh = __atomic_load_n(&GVRAM_RowGeneration[y_hi], __ATOMIC_ACQUIRE);
    uint8_t *row = s_scroll614c_idx + (y_lo << 9);
    if (m->valid && m->gen_lo == gl && m->gen_hi == gh &&
        m->y_hi == y_hi && m->dx_hi == dx_hi &&
        m->bottom_page == (uint8_t)bottom_page && m->top_page == (uint8_t)top_page) {
        ++s_scroll614c_hits;
        *row_out = row;
        return 1;
    }

    ++s_scroll614c_misses;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    uint32_t g0l = __atomic_load_n(&GVRAM_RowGeneration[y_lo], __ATOMIC_ACQUIRE);
    uint32_t g0h = __atomic_load_n(&GVRAM_RowGeneration[y_hi], __ATOMIC_ACQUIRE);

    for (uint32_t x = 0; x < 512u; ++x) {
        const uint32_t xh = (x + dx_hi) & 511u;
        const uint16_t lw = *(const uint16_t *)(gvram + (y_lo << 10) + (x << 1));
        const uint16_t hw = *(const uint16_t *)(gvram + (y_hi << 10) + (xh << 1));
        const uint8_t p0 = (uint8_t)((hw & 0x00f0u) | (lw & 0x000fu));
        const uint8_t p1 = (uint8_t)(((hw >> 8) & 0x00f0u) | ((lw >> 8) & 0x000fu));
        const uint8_t b = bottom_page ? p1 : p0;
        const uint8_t t = top_page ? p1 : p0;
        row[x] = t ? t : b;
    }

    uint32_t g1l = __atomic_load_n(&GVRAM_RowGeneration[y_lo], __ATOMIC_ACQUIRE);
    uint32_t g1h = __atomic_load_n(&GVRAM_RowGeneration[y_hi], __ATOMIC_ACQUIRE);
    if (g0l != g1l || g0h != g1h) {
        ++s_scroll614c_rebuilds;
        for (uint32_t x = 0; x < 512u; ++x) {
            const uint32_t xh = (x + dx_hi) & 511u;
            const uint16_t lw = *(const uint16_t *)(gvram + (y_lo << 10) + (x << 1));
            const uint16_t hw = *(const uint16_t *)(gvram + (y_hi << 10) + (xh << 1));
            const uint8_t p0 = (uint8_t)((hw & 0x00f0u) | (lw & 0x000fu));
            const uint8_t p1 = (uint8_t)(((hw >> 8) & 0x00f0u) | ((lw >> 8) & 0x000fu));
            const uint8_t b = bottom_page ? p1 : p0;
            const uint8_t t = top_page ? p1 : p0;
            row[x] = t ? t : b;
        }
        g1l = __atomic_load_n(&GVRAM_RowGeneration[y_lo], __ATOMIC_ACQUIRE);
        g1h = __atomic_load_n(&GVRAM_RowGeneration[y_hi], __ATOMIC_ACQUIRE);
    }
    m->gen_lo = g1l; m->gen_hi = g1h; m->y_hi = (uint16_t)y_hi; m->dx_hi = (uint16_t)dx_hi;
    m->bottom_page = (uint8_t)bottom_page; m->top_page = (uint8_t)top_page; m->valid = 1u;
    s_last_scroll614c_build_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
    *row_out = row;
    return 1;
}

static inline void render_scroll614c_gbt(uint16_t *dst, const uint8_t *idx_row,
                                         uint32_t x_lo, const uint16_t *pal,
                                         const uint16_t *bg_text, const uint8_t *flags,
                                         uint32_t width, uint8_t grp_pri,
                                         uint8_t bg_pri, uint8_t text_pri)
{
    grp_pri &= 3u; bg_pri &= 3u; text_pri &= 3u; x_lo &= 511u;
    for (uint32_t i = 0; i < width; ++i) {
        const uint16_t g = pal[idx_row[(x_lo + i) & 511u]];
        const uint16_t bt = bg_text[i];
        const uint8_t f = (uint8_t)(flags[i] & 3u);
        if (__builtin_expect(f == 0u, 0)) { dst[i] = g; continue; }
        uint8_t bt_pri = 4u;
        if (f & 2u) bt_pri = bg_pri;
        if ((f & 1u) && text_pri < bt_pri) bt_pri = text_pri;
        dst[i] = (bt_pri <= grp_pri) ? (bt ? bt : g) : (g ? g : bt);
    }
}


static const uint16_t *render_host_bgsp(const BG_HOST_LINE_STATE *st,
                                        const uint16_t *text_pal,
                                        uint32_t width);
static const uint16_t *r56s5_render_host_bt_exact(const BG_HOST_LINE_STATE *st,
                                                   const uint8_t *text_idx,
                                                   const uint16_t *text_pal,
                                                   uint32_t width, int text_on,
                                                   const uint8_t **flags_out);

/* Build 6.15e: exact Tab5 RGB565 conversion used by Pal16[].  X68K has
 * five G bits; RGB565's otherwise-extra 0x0020 bit is PX68K's intensity bit.
 * Therefore Pal_ChangeContrast() masks green with 0x07c0, not 0x07e0.
 * Doing this directly avoids a random 128-KiB lookup while preserving the
 * exact integer contrast semantics. */
static inline uint16_t gbt65k_x68_to_rgb565(uint16_t c, uint8_t contrast)
{
    uint16_t g = (uint16_t)((c & 0xf800u) >> 5);
    uint16_t r = (uint16_t)((c & 0x07c0u) << 5);
    uint16_t b = (uint16_t)((c & 0x003eu) >> 1);
    const uint16_t non_i = (uint16_t)(r | g | b);
    const uint32_t n = (uint32_t)(contrast & 15u);
    r = (uint16_t)((((uint32_t)r * n) / 15u) & 0xf800u);
    g = (uint16_t)((((uint32_t)g * n) / 15u) & 0x07c0u);
    b = (uint16_t)((((uint32_t)b * n) / 15u) & 0x001fu);
    uint16_t out = (uint16_t)(r | g | b);
    if (non_i && !out) out = 0x0001u;
    if (c & 1u) out |= 0x0020u;
    return out;
}

static inline uint32_t gbt65k_pal_addr(uint8_t v)
{
    return ((uint32_t)v << 1) - (uint32_t)(v & 1u);
}

static int gbt65k_runtime_pal_selfcheck(uint8_t contrast)
{
    if (s_gbt65k_pal_selfcheck_state == 1) return 1;
    if (s_gbt65k_pal_selfcheck_state == 2) return 0;

    /* Compare a deterministic spread across the full X68000 16-bit colour
     * word against PX68K's authoritative Pal16[] after Pal_Init/contrast. */
    for (uint32_t i = 0; i < 256u; ++i) {
        const uint16_t c = (uint16_t)((i * 257u) ^ (i * 73u) ^ 0x5a21u);
        const uint16_t got = gbt65k_x68_to_rgb565(c, contrast);
        const uint16_t ref = Pal16[c];
        if (got != ref) {
            s_gbt65k_pal_selfcheck_state = 2;
            s_gbt65k_ready = 0;
            ESP_LOGE(TAG,
                     "PX68K_GBT65K615E: Pal16 runtime self-check FAIL c=%04X got=%04X ref=%04X contrast=%u; disabling 65K fast path",
                     c, got, ref, (unsigned)(contrast & 15u));
            return 0;
        }
    }
    s_gbt65k_pal_selfcheck_state = 1;
    ESP_LOGI(TAG, "PX68K_GBT65K615E: Pal16 runtime self-check PASS (256 samples) contrast=%u",
             (unsigned)(contrast & 15u));
    return 1;
}

static int gbt65k_get_snapshot_row(const uint16_t *raw_row, uint32_t row,
                                    uint32_t row_generation,
                                    const uint8_t *pal_regs,
                                    uint32_t pal_generation,
                                    uint8_t contrast,
                                    const uint16_t **row_out)
{
    if (!s_gbt65k_ready || !raw_row || !pal_regs || !row_out) return 0;
    if (!gbt65k_runtime_pal_selfcheck(contrast)) return 0;
    row &= 511u;
    const uint32_t cache_row = row & TAB5_GBT65K_CACHE_MASK;
    gbt65k_meta_t *m = &s_gbt65k_meta[cache_row];
    uint16_t *dst = s_gbt65k_rgb + (cache_row << 9);

    /* R57E50: small direct-map PSRAM cache. Include physical source row in
     * the tag so collisions are exact misses, never stale hits. */
    if (m->valid && m->source_row == (uint16_t)row &&
        m->row_gen == row_generation &&
        m->pal_gen == pal_generation &&
        m->contrast == (uint8_t)(contrast & 15u)) {
        ++s_gbt65k_hits;
        *row_out = dst;
        return 1;
    }

    ++s_gbt65k_misses;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
#endif
    for (uint32_t x = 0; x < 512u; ++x) {
        const uint16_t raw = raw_row[x];
        if (__builtin_expect(raw == 0u, 0)) {
            dst[x] = 0u;
            continue;
        }
        const uint8_t lo = (uint8_t)raw;
        const uint8_t hi = (uint8_t)(raw >> 8);
        const uint16_t mapped = (uint16_t)pal_regs[gbt65k_pal_addr(lo)] |
            (uint16_t)((uint16_t)pal_regs[gbt65k_pal_addr(hi) + 2u] << 8);
        dst[x] = gbt65k_x68_to_rgb565(mapped, contrast);
    }

    m->row_gen = row_generation;
    m->pal_gen = pal_generation;
    m->source_row = (uint16_t)row;
    m->contrast = (uint8_t)(contrast & 15u);
    m->valid = 1u;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_last_gbt65k_build_us =
        cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
#endif
    *row_out = dst;
    return 1;
}

static void gbt65k_scalar_compose(uint16_t *dst,
                                  const uint16_t *grp_row, uint32_t gx,
                                  const uint16_t *bg,
                                  const uint8_t *text_idx,
                                  const uint16_t *text_pal,
                                  uint32_t width,
                                  int bg_on, int text_on,
                                  uint8_t gp, uint8_t bp, uint8_t tp)
{
    for (uint32_t i = 0; i < width; ++i) {
        uint16_t out = grp_row[(gx + i) & 511u];
        uint8_t best = out ? gp : 0xffu;

        if (bg_on && bg) {
            const uint16_t b = bg[i];
            if (b && (best == 0xffu || bp <= best)) {
                out = b; best = bp; /* BG wins a priority tie over GRP. */
            }
        }
        if (text_on) {
            const uint8_t ti = (uint8_t)(text_idx[i] & 0x0fu);
            const uint16_t t = ti ? text_pal[ti] : 0u;
            if (t && (best == 0xffu || tp <= best)) {
                out = t; best = tp; /* TEXT wins ties over BG and GRP. */
            }
        }
        dst[i] = out;
    }
}

/* Build 6.15f: turn the fixed-priority three-layer selection into one PIE
 * copy plus at most two validated key-zero overlays.  The scratch rows are
 * offset to the framebuffer's 16-byte phase so cblend598g3_pie() can stay on
 * its 128-bit path even though ScrBuf itself is not necessarily 16-byte zero
 * phase. */
static int gbt65k_pie_compose(compose_slot_t *slot,
                              const uint16_t *grp_row, uint32_t gx,
                              const uint16_t *bg, int bg_on, int text_on)
{
    if (!s_cblend598g3_selfcheck_ok || !s_bg_pri_scratch ||
        !s_bgsp_ref_scratch || !s_grp8split_ref_scratch)
        return 0;

    const uint32_t phase_px = (uint32_t)(((uintptr_t)slot->dst & 15u) >> 1);
    uint16_t *grp = s_bgsp_ref_scratch + phase_px;
    uint16_t *bg_aligned = s_bg_pri_scratch + phase_px;
    uint16_t *text = s_grp8split_ref_scratch + phase_px;
    const uint32_t width = slot->width;
    const uint32_t line_bytes = width * (uint32_t)sizeof(uint16_t);

    /* Make the wrapped 512-pixel 65K row contiguous and phase-matched. */
    uint32_t first = 512u - (gx & 511u);
    if (first > width) first = width;
    tab5_raster590_copy_run((uint8_t *)grp,
                            (const uint8_t *)&grp_row[gx & 511u],
                            first * (uint32_t)sizeof(uint16_t));
    if (first < width)
        tab5_raster590_copy_run((uint8_t *)&grp[first],
                                (const uint8_t *)grp_row,
                                (width - first) * (uint32_t)sizeof(uint16_t));

    if (bg_on && bg)
        tab5_raster590_copy_run((uint8_t *)bg_aligned,
                                (const uint8_t *)bg, line_bytes);

    if (text_on) {
        const uint8_t *idx = slot->u.gbt65k.text_idx;
        const uint16_t *pal = slot->u.gbt65k.text_pal;
        uint32_t i = 0;
        for (; i + 3u < width; i += 4u) {
            const uint8_t i0 = (uint8_t)(idx[i + 0u] & 0x0fu);
            const uint8_t i1 = (uint8_t)(idx[i + 1u] & 0x0fu);
            const uint8_t i2 = (uint8_t)(idx[i + 2u] & 0x0fu);
            const uint8_t i3 = (uint8_t)(idx[i + 3u] & 0x0fu);
            text[i + 0u] = i0 ? pal[i0] : 0u;
            text[i + 1u] = i1 ? pal[i1] : 0u;
            text[i + 2u] = i2 ? pal[i2] : 0u;
            text[i + 3u] = i3 ? pal[i3] : 0u;
        }
        for (; i < width; ++i) {
            const uint8_t ti = (uint8_t)(idx[i] & 0x0fu);
            text[i] = ti ? pal[ti] : 0u;
        }
    }

    const uint16_t *layers[3];
    uint32_t nl = 0;
    const uint8_t gp = (uint8_t)(slot->grp_pri & 3u);
    const uint8_t bp = (uint8_t)(slot->bg_pri & 3u);
    const uint8_t tp = (uint8_t)(slot->text_pri & 3u);
    for (int pri = 3; pri >= 0; --pri) {
        if ((int)gp == pri) layers[nl++] = grp;
        if (bg_on && bg && (int)bp == pri) layers[nl++] = bg_aligned;
        if (text_on && (int)tp == pri) layers[nl++] = text;
    }
    if (!nl)
        return 0;

    tab5_raster590_copy_run((uint8_t *)slot->dst,
                            (const uint8_t *)layers[0], line_bytes);
    for (uint32_t n = 1; n < nl; ++n)
        (void)cblend598g3_pie(slot->dst, slot->dst, layers[n], width);
    return 1;
}

static inline void r56s4_render_gbt65k_exact_bt(uint16_t *dst,
                                                    const uint16_t *grp_row,
                                                    uint32_t gx,
                                                    const uint16_t *bg_text,
                                                    const uint8_t *flags,
                                                    uint32_t width,
                                                    uint8_t grp_pri,
                                                    uint8_t bg_pri,
                                                    uint8_t text_pri)
{
    grp_pri &= 3u;
    bg_pri &= 3u;
    text_pri &= 3u;
    for (uint32_t i = 0; i < width; ++i) {
        const uint16_t g = grp_row[(gx + i) & 511u];
        const uint16_t bt = bg_text[i];
        const uint8_t f = (uint8_t)(flags[i] & 3u);
        if (__builtin_expect(f == 0u, 0)) {
            dst[i] = g;
            continue;
        }
        uint8_t bt_pri = 4u;
        if (f & 2u) bt_pri = bg_pri;
        if ((f & 1u) && text_pri < bt_pri) bt_pri = text_pri;
        dst[i] = (bt_pri <= grp_pri) ? (bt ? bt : g) : (g ? g : bt);
    }
}

static inline uint8_t r57e_packed_text_pixel(const uint8_t *tv,uint32_t y,uint32_t x)
{
    const uint32_t px=((y&1023u)<<10)+(x&1023u);
    const uint8_t v=tv[px>>1];
    return (uint8_t)((px&1u)?(v>>4):(v&15u));
}

static int r57e_expand_text_shadow(compose_slot_t *slot)
{
    if (!(slot->selfcheck & 64u)) return 1;
    const uint8_t *tv=s_r57e_tvram_render_src;
    if (!tv) return 0;
    const uint32_t x=slot->u.gbt65k.text_x&1023u;
    const uint32_t y=slot->u.gbt65k.text_y&1023u;
    const uint32_t width=slot->width;
    if (!width || x+width>1024u) return 0;
    for (uint32_t i=0;i<width;++i)
        slot->u.gbt65k.text_idx[i]=r57e_packed_text_pixel(tv,y,x+i);
    __atomic_add_fetch(&s_r57e_text_shadow_lines,1u,__ATOMIC_RELAXED);
    return 1;
}

static uint32_t r57e_validate_text_shadow(const compose_slot_t *slot)
{
    const uint8_t *tv=s_r57e_tvram_render_src;
    if (!tv) return 0u;
    const uint32_t x=slot->u.gbt65k.text_x&1023u;
    const uint32_t y=slot->u.gbt65k.text_y&1023u;
    if (!slot->width || x+slot->width>1024u) return 0u;
    for (uint32_t i=0;i<slot->width;++i)
        if (r57e_packed_text_pixel(tv,y,x+i)!=(slot->u.gbt65k.text_idx[i]&15u)) return i;
    return slot->width;
}

static int render_gbt65k_line(compose_slot_t *slot)
{
    const uint16_t *grp_row = NULL;
    if (!gbt65k_get_snapshot_row(slot->u.gbt65k.raw_row,
                                 slot->u.gbt65k.gvram_row,
                                 slot->u.gbt65k.row_generation,
                                 slot->u.gbt65k.pal_regs,
                                 slot->u.gbt65k.pal_generation,
                                 slot->u.gbt65k.contrast,
                                 &grp_row))
        return 0;

    /* PX68K_R57D_CLASS_VALIDATE
     * Every class is first displayed from the authoritative stock CPU1 BT
     * snapshot. CPU0 builds the candidate from the ordered frozen BG/Sprite
     * shadow. Only after 8 consecutive exact lines does this class bypass the
     * CPU1 stock Text_DrawLine/BG_DrawLine path. */
    if (slot->selfcheck & 32u) {
        const uint16_t key=(uint16_t)slot->bottom_page | ((uint16_t)(slot->top_page & 3u)<<8);
        const int text_on5=(slot->selfcheck & 1u)!=0u;
        const int bg_on5=(slot->selfcheck & 2u)!=0u;
        const uint8_t *hf=NULL;
        const uint16_t *hb=r56s5_render_host_bt_exact(
            bg_on5 ? &slot->u.gbt65k.bg_state : NULL,
            slot->u.gbt65k.text_idx, slot->u.gbt65k.text_pal,
            slot->width, text_on5, &hf);
        uint32_t bad=slot->width;
        if (!hb || !hf) bad=0u;
        else for (uint32_t i=0;i<slot->width;++i) {
            if (hb[i]!=slot->u.gbt65k.exact_bg_text[i] ||
                (hf[i]&3u)!=(slot->u.gbt65k.exact_flags[i]&3u)) { bad=i; break; }
        }
        const uint32_t shadow_bad=text_on5?r57e_validate_text_shadow(slot):slot->width;
        if (key<R57D_CLASS_COUNT && s_r57d_class_state && s_r57d_class_epoch) {
            const uint16_t epoch=(uint16_t)tab5_guest_bus_tvram_epoch();
            const uint8_t cur = (__atomic_load_n(&s_r57d_class_epoch[key],__ATOMIC_ACQUIRE)==epoch)
                              ? __atomic_load_n(&s_r57d_class_state[key], __ATOMIC_ACQUIRE) : 0u;
            if (bad==slot->width && shadow_bad==slot->width) {
                __atomic_add_fetch(&s_r57e_text_shadow_match,1u,__ATOMIC_RELAXED);
                uint8_t n=s_r57d_class_pass[key];
                if (n<255u) ++n;
                s_r57d_class_pass[key]=n;
                if (n>=R57D_CLASS_PASS_NEED && cur!=2u) {
                    __atomic_store_n(&s_r57d_class_epoch[key],epoch,__ATOMIC_RELEASE);
                    __atomic_store_n(&s_r57d_class_state[key],2u,__ATOMIC_RELEASE);
                    printf("PX68K_R57E5_CLASS: CERTIFIED key=%u epoch=%u after=%u host+PACKED-SHADOW exact lines; TEXT remains CPU0-owned\n",
                           (unsigned)key,(unsigned)epoch,(unsigned)n);
                }
            } else {
                s_r57d_class_pass[key]=0u;
                __atomic_store_n(&s_r57d_class_state[key],0u,__ATOMIC_RELEASE);
                if (shadow_bad!=slot->width && text_on5) {
                    const uint32_t tx=slot->u.gbt65k.text_x&1023u;
                    const uint32_t ty=slot->u.gbt65k.text_y&1023u;
                    const uint8_t packed_before=r57e_packed_text_pixel(s_r57e_tvram_render_src,ty,tx+shadow_bad);
                    const uint8_t guest_value=(uint8_t)(slot->u.gbt65k.text_idx[shadow_bad]&15u);
                    (void)tab5_guest_bus_text_shadow_repair(ty,tx,slot->u.gbt65k.text_idx,slot->width);
                    const uint32_t n=__atomic_add_fetch(&s_r57e_text_shadow_repair,1u,__ATOMIC_RELAXED);
                    if (n<=16u)
                        printf("PX68K_R57E5_TEXTSHADOW: REPAIR #%lu key=%u y=%lu x=%lu bad=%lu packed=%u guest=%u; CPU0 ownership retained, certification restarted\n",
                               (unsigned long)n,(unsigned)key,(unsigned long)ty,(unsigned long)tx,(unsigned long)shadow_bad,
                               (unsigned)packed_before,(unsigned)guest_value);
                }
                if (bad!=slot->width) {
                    const uint32_t n=__atomic_add_fetch(&s_r57e_text_shadow_fail,1u,__ATOMIC_RELAXED);
                    if (n<=8u)
                        printf("PX68K_R57E5_HOSTBT: RETRY key=%u x=%lu host=%04X/%u stock=%04X/%u; no permanent CPU1 fallback\n",
                               (unsigned)key,(unsigned long)bad,
                               hb?hb[bad]:0u,hf?(unsigned)(hf[bad]&3u):0u,
                               slot->u.gbt65k.exact_bg_text[bad],
                               (unsigned)(slot->u.gbt65k.exact_flags[bad]&3u));
                }
            }
        }
        r56s4_render_gbt65k_exact_bt(slot->dst,grp_row,
                                     slot->u.gbt65k.gvram_x&511u,
                                     slot->u.gbt65k.exact_bg_text,
                                     slot->u.gbt65k.exact_flags,slot->width,
                                     slot->grp_pri,slot->bg_pri,slot->text_pri);
        ++s_gbt65k_scalar_lines;
        return 1;
    }

    /* R56s5 bit4 means the packet carries expanded TEXT + BG host state and
     * asks CPU0 to construct the exact stock combined BG/TEXT raster.
     * bit3+bit4 is the one-shot live validator: current output remains the
     * authoritative R56s4 CPU1 snapshot while the host result is compared. */
    if (slot->selfcheck & 16u) {
        const int text_on5 = (slot->selfcheck & 1u) != 0u;
        const int bg_on5 = (slot->selfcheck & 2u) != 0u;
        const uint8_t *host_flags = NULL;
        const uint16_t *host_bt = r56s5_render_host_bt_exact(
            bg_on5 ? &slot->u.gbt65k.bg_state : NULL,
            slot->u.gbt65k.text_idx, slot->u.gbt65k.text_pal,
            slot->width, text_on5, &host_flags);
        if (!host_bt || !host_flags) {
            if (slot->selfcheck & 8u) {
                store_release(&s_r56s5_hostbt_state, 3u);
                r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,
                                             slot->u.gbt65k.gvram_x & 511u,
                                             slot->u.gbt65k.exact_bg_text,
                                             slot->u.gbt65k.exact_flags,
                                             slot->width, slot->grp_pri,
                                             slot->bg_pri, slot->text_pri);
                ++s_gbt65k_scalar_lines;
                return 1;
            }
            return 0;
        }

        if (slot->selfcheck & 8u) {
            uint32_t bad = slot->width;
            for (uint32_t i = 0; i < slot->width; ++i) {
                if (host_bt[i] != slot->u.gbt65k.exact_bg_text[i] ||
                    (host_flags[i] & 3u) != (slot->u.gbt65k.exact_flags[i] & 3u)) {
                    bad = i; break;
                }
            }
            if (bad == slot->width) {
                store_release(&s_r56s5_hostbt_state, 2u);
                printf("PX68K_R56S5: CPU0 stock-order BG/TEXT live self-check PASS; visible TEXT 65K rows fully host-offloaded\n");
            } else {
                store_release(&s_r56s5_hostbt_state, 3u);
                printf("PX68K_R56S5: CPU0 stock-order BG/TEXT live self-check FAIL x=%lu host=%04X/%u stock=%04X/%u; R56s4 exact fallback retained\n",
                       (unsigned long)bad, host_bt[bad],
                       (unsigned)(host_flags[bad] & 3u),
                       slot->u.gbt65k.exact_bg_text[bad],
                       (unsigned)(slot->u.gbt65k.exact_flags[bad] & 3u));
            }
            r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,
                                         slot->u.gbt65k.gvram_x & 511u,
                                         slot->u.gbt65k.exact_bg_text,
                                         slot->u.gbt65k.exact_flags,
                                         slot->width, slot->grp_pri,
                                         slot->bg_pri, slot->text_pri);
            ++s_gbt65k_scalar_lines;
            return 1;
        }

        r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,
                                     slot->u.gbt65k.gvram_x & 511u,
                                     host_bt, host_flags, slot->width,
                                     slot->grp_pri, slot->bg_pri, slot->text_pri);
        ++s_gbt65k_scalar_lines;
        return 1;
    }

    /* R56s4 bit3 without bit4: CPU1 exact snapshot fallback after validation
     * failure or while another validation packet is pending. */
    if (slot->selfcheck & 8u) {
        r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,
                                     slot->u.gbt65k.gvram_x & 511u,
                                     slot->u.gbt65k.exact_bg_text,
                                     slot->u.gbt65k.exact_flags,
                                     slot->width, slot->grp_pri,
                                     slot->bg_pri, slot->text_pri);
        ++s_gbt65k_scalar_lines;
        return 1;
    }

    const int text_on = (slot->selfcheck & 1u) != 0u;
    const int bg_on = (slot->selfcheck & 2u) != 0u;
    const uint16_t *bg = NULL;
    if (bg_on) {
        bg = render_host_bgsp(&slot->u.gbt65k.bg_state,
                              slot->u.gbt65k.text_pal, slot->width);
        if (bg && (slot->selfcheck & 4u) &&
            load_acquire(&s_bgsp_selfcheck_state) == 1u) {
            uint32_t bad = slot->width;
            for (uint32_t i = 0; i < slot->width; ++i) {
                if (bg[i] != s_bgsp_ref_scratch[i]) { bad = i; break; }
            }
            if (bad == slot->width) {
                store_release(&s_bgsp_selfcheck_state, 2u);
                printf("PX68K_GBT65K615E: BG/Sprite host self-check PASS\n");
            } else {
                const uint16_t got = bg[bad];
                const uint16_t ref = s_bgsp_ref_scratch[bad];
                store_release(&s_bgsp_selfcheck_state, 3u);
                printf("PX68K_GBT65K615E: BG/Sprite host self-check FAIL x=%lu host=%04X stock=%04X; future 65K BG lines use legacy path\n",
                       (unsigned long)bad, (unsigned)got, (unsigned)ref);
                bg = s_bgsp_ref_scratch;
            }
        }
    }

    const uint32_t gx = slot->u.gbt65k.gvram_x & 511u;
    const uint8_t gp = (uint8_t)(slot->grp_pri & 3u);
    const uint8_t bp = (uint8_t)(slot->bg_pri & 3u);
    const uint8_t tp = (uint8_t)(slot->text_pri & 3u);

    /* If BG live validation failed on this very line, keep the stock scalar
     * result. Future lines will be rejected before they enter the 65K path. */
    if (bg_on && load_acquire(&s_bgsp_selfcheck_state) == 3u) {
        gbt65k_scalar_compose(slot->dst, grp_row, gx, bg,
                              slot->u.gbt65k.text_idx,
                              slot->u.gbt65k.text_pal, slot->width,
                              bg_on, text_on, gp, bp, tp);
        ++s_gbt65k_scalar_lines;
        return 1;
    }

    if (s_gbt65k_pie_selfcheck_state != 2 &&
        gbt65k_pie_compose(slot, grp_row, gx, bg, bg_on, text_on)) {
        /* First real line: shadow the accelerated result once against the
         * exact 6.15e scalar selector. No recurring runtime shadow cost. */
        if (s_gbt65k_pie_selfcheck_state == 0 && s_gbt65k_ref_scratch) {
            gbt65k_scalar_compose(s_gbt65k_ref_scratch, grp_row, gx, bg,
                                  slot->u.gbt65k.text_idx,
                                  slot->u.gbt65k.text_pal, slot->width,
                                  bg_on, text_on, gp, bp, tp);
            uint32_t bad = slot->width;
            for (uint32_t i = 0; i < slot->width; ++i) {
                if (slot->dst[i] != s_gbt65k_ref_scratch[i]) {
                    bad = i; break;
                }
            }
            if (bad == slot->width) {
                s_gbt65k_pie_selfcheck_state = 1;
                printf("PX68K_GBT65K615F: PIE final G/B/T self-check PASS\n");
            } else {
                s_gbt65k_pie_selfcheck_state = 2;
                printf("PX68K_GBT65K615F: PIE final G/B/T self-check FAIL x=%lu got=%04X ref=%04X; scalar fallback retained\n",
                       (unsigned long)bad, slot->dst[bad],
                       s_gbt65k_ref_scratch[bad]);
                tab5_raster590_copy_run((uint8_t *)slot->dst,
                                        (const uint8_t *)s_gbt65k_ref_scratch,
                                        slot->width * (uint32_t)sizeof(uint16_t));
                ++s_gbt65k_scalar_lines;
                return 1;
            }
        }
        ++s_gbt65k_pie_lines;
        return 1;
    }

    gbt65k_scalar_compose(slot->dst, grp_row, gx, bg,
                          slot->u.gbt65k.text_idx,
                          slot->u.gbt65k.text_pal, slot->width,
                          bg_on, text_on, gp, bp, tp);
    ++s_gbt65k_scalar_lines;
    return 1;
}

static inline uint16_t gbt614a_reference_pixel(uint16_t grp, uint16_t bt,
                                                uint8_t flags,
                                                uint8_t grp_pri,
                                                uint8_t bg_pri,
                                                uint8_t text_pri)
{
    uint16_t out = 0u;
    int opaq = 1;
    flags &= 3u;
    grp_pri &= 3u;
    bg_pri &= 3u;
    text_pri &= 3u;

    /* Exact structure of the normal legacy final compositor: draw low visual
     * priority first (3 -> 0); equal-priority order is GRP, BG/SP, TEXT.  The
     * first participating layer is opaque, later layers use key-zero. */
    for (int pri = 3; pri >= 0; --pri) {
        if ((int)grp_pri == pri) {
            if (opaq) { out = grp; opaq = 0; }
            else if (grp) out = grp;
        }
        if ((flags & 2u) && (int)bg_pri == pri) {
            if (opaq) { out = bt; opaq = 0; }
            else if (bt) out = bt;
        }
        if ((flags & 1u) && (int)text_pri == pri) {
            if (opaq) { out = bt; opaq = 0; }
            else if (bt) out = bt;
        }
    }
    return out;
}

/* BAT167M1: exact transcription of WinDraw's legacy final compositor.
 * Layer generation remains guest-semantic and unchanged on CPU1 in M1;
 * only final host-visible RGB565 selection moves to CPU0. */
static inline void legacy_overlay_key0(uint16_t *dst, const uint16_t *src, uint32_t width)
{
    for (uint32_t x = 0; x < width; ++x) {
        const uint16_t v = src[x];
        if (v) dst[x] = v;
    }
}

static inline void legacy_draw_grp(uint16_t *dst, const uint16_t *src,
                                   uint32_t width, int opaq)
{
    if (opaq) memcpy(dst, src, width * sizeof(uint16_t));
    else legacy_overlay_key0(dst, src, width);
}

static inline void legacy_draw_masked(uint16_t *dst, const uint16_t *src,
                                      const uint8_t *flags, uint8_t mask,
                                      uint32_t width, int opaq, int td)
{
    if (opaq) {
        memcpy(dst, src, width * sizeof(uint16_t));
        return;
    }
    if (td) {
        for (uint32_t x = 0; x < width; ++x) {
            if (flags[x] & mask) {
                const uint16_t v = src[x];
                if (v) dst[x] = v;
            }
        }
    } else {
        legacy_overlay_key0(dst, src, width);
    }
}

static inline uint16_t legacy_half_mix(uint16_t w, uint16_t v,
                                       uint16_t half_mask, uint16_t ibit, uint16_t ix2)
{
    if (w != 0u) {
        w &= half_mask;
        if (v & ibit) w = (uint16_t)(w + ix2);
        v &= half_mask;
        v = (uint16_t)((v + w) >> 1);
    }
    return v;
}

static void render_legacy_final(compose_slot_t *slot)
{
    uint16_t *dst = slot->dst;
    const uint32_t width = slot->width;
    const uint16_t *grp = slot->u.legacy.grp;
    const uint16_t *grp_sp = slot->u.legacy.grp_sp;
    const uint16_t *grp_sp2 = slot->u.legacy.grp_sp2;
    const uint16_t *bt = slot->u.legacy.bg_text;
    const uint8_t *fl = slot->u.legacy.flags;
    const uint8_t vc1 = slot->u.legacy.vc1_0;
    const uint8_t vc2 = slot->u.legacy.vc2_0;
    const int gon = slot->u.legacy.gon;
    const int bgon = slot->u.legacy.bgon;
    const int ton = slot->u.legacy.ton;
    const int tron = slot->u.legacy.tron;
    const int pron = slot->u.legacy.pron;
    const uint16_t half_mask = slot->u.legacy.half_mask;
    const uint16_t ibit = slot->u.legacy.ibit;
    const uint16_t ix2 = slot->u.legacy.ix2;
    int opaq = 1;
    int tdrawed = 0;

#define LEG_GRP(OPAQUE) legacy_draw_grp(dst, grp, width, (OPAQUE))
#define LEG_NSP(OPAQUE) legacy_draw_grp(dst, grp_sp2, width, (OPAQUE))
#define LEG_TEXT(OPAQUE,TD) legacy_draw_masked(dst, bt, fl, 1u, width, (OPAQUE), (TD))
#define LEG_BG(OPAQUE,TD) legacy_draw_masked(dst, bt, fl, 2u, width, (OPAQUE), (TD))

    if (vc1 & 0x02u) {
        if (gon) { LEG_GRP(opaq); opaq = 0; }
        if (tron) { LEG_NSP(opaq); opaq = 0; }
    }
    if ((vc1 & 0x20u) && bgon) {
        if (((vc2 & 0x5du) == 0x1du) && ((vc1 & 0x03u) != 0x02u) && tron) {
            if ((vc1 & 3u) < ((vc1 >> 2) & 3u)) {
                for (uint32_t x=0; x<width; ++x) {
                    if (opaq || (fl[x] & 2u)) {
                        uint16_t v = bt[x];
                        if (opaq || v) dst[x] = legacy_half_mix(grp_sp[x], v, half_mask, ibit, ix2);
                    }
                }
                tdrawed = 1; opaq = 0;
            }
        } else {
            LEG_BG(opaq, tdrawed); tdrawed = 1; opaq = 0;
        }
    }
    if ((vc1 & 0x08u) && ton) {
        if (((vc2 & 0x5du) == 0x1du) && ((vc1 & 0x03u) != 0x02u) && tron) {
            for (uint32_t x=0; x<width; ++x) {
                if (opaq) {
                    uint16_t w = grp_sp[x];
                    uint16_t v;
                    if (w) {
                        w &= half_mask;
                        v = bt[x];
                        if (v & ibit) w = (uint16_t)(w + ix2);
                        v &= half_mask;
                        v = (uint16_t)((v + w) >> 1);
                    } else {
                        v = (fl[x] & 1u) ? bt[x] : 0u;
                    }
                    dst[x] = v;
                } else if (fl[x] & 1u) {
                    uint16_t w = grp_sp[x], v = bt[x];
                    if (v) {
                        if (w) v = legacy_half_mix(w, v, half_mask, ibit, ix2);
                        dst[x] = v;
                    }
                }
            }
        } else {
            LEG_TEXT(opaq, tdrawed);
        }
        opaq = 0; tdrawed = 1;
    }

    if (((vc1 & 0x03u) == 0x01u) && gon) { LEG_GRP(opaq); opaq = 0; }
    if (((vc1 & 0x30u) == 0x10u) && bgon) {
        if (((vc2 & 0x5du) == 0x1du) && !(vc1 & 0x03u) && tron) {
            if ((vc1 & 3u) < ((vc1 >> 2) & 3u)) {
                for (uint32_t x=0; x<width; ++x) {
                    if (opaq || (fl[x] & 2u)) {
                        uint16_t v = bt[x];
                        if (opaq || v) dst[x] = legacy_half_mix(grp_sp[x], v, half_mask, ibit, ix2);
                    }
                }
                tdrawed = 1; opaq = 0;
            }
        } else {
            LEG_BG(opaq, ((vc1 & 0x0cu) == 0x08u));
            tdrawed = 1; opaq = 0;
        }
    }
    if (((vc1 & 0x0cu) == 0x04u) && ((vc2 & 0x5du) == 0x1du) &&
        (vc1 & 0x03u) && (((vc1 >> 4) & 3u) > (vc1 & 3u)) && bgon && tron) {
        for (uint32_t x=0; x<width; ++x) {
            if (opaq || (fl[x] & 2u)) {
                uint16_t v = bt[x];
                if (opaq || v) dst[x] = legacy_half_mix(grp_sp[x], v, half_mask, ibit, ix2);
            }
        }
        tdrawed = 1; opaq = 0;
        if (tron) LEG_NSP(opaq);
    } else if (((vc1 & 0x03u) == 0x01u) && tron && gon && (vc2 & 0x10u)) {
        LEG_NSP(opaq); opaq = 0;
    }
    if (((vc1 & 0x0cu) == 0x04u) && ton) {
        if (((vc2 & 0x5du) == 0x1du) && !(vc1 & 0x03u) && tron) {
            for (uint32_t x=0; x<width; ++x) {
                if (opaq) {
                    uint16_t w = grp_sp[x];
                    uint16_t v;
                    if (w) {
                        w &= half_mask; v = bt[x];
                        if (v & ibit) w = (uint16_t)(w + ix2);
                        v &= half_mask; v = (uint16_t)((v + w) >> 1);
                    } else v = (fl[x] & 1u) ? bt[x] : 0u;
                    dst[x] = v;
                } else if (fl[x] & 1u) {
                    uint16_t w = grp_sp[x], v = bt[x];
                    if (v) { if (w) v = legacy_half_mix(w, v, half_mask, ibit, ix2); dst[x] = v; }
                }
            }
        } else {
            LEG_TEXT(opaq, ((vc1 & 0x30u) >= 0x10u));
        }
        opaq = 0; tdrawed = 1;
    }

    if (!(vc1 & 0x03u) && gon) { LEG_GRP(opaq); opaq = 0; }
    if (!(vc1 & 0x30u) && bgon) {
        LEG_BG(opaq, ((vc1 & 0x0cu) >= 0x04u));
        tdrawed = 1; opaq = 0;
    }
    if (!(vc1 & 0x0cu) && ((vc2 & 0x5du) == 0x1du) &&
        (((vc1 >> 4) & 3u) > (vc1 & 3u)) && bgon && tron) {
        for (uint32_t x=0; x<width; ++x) {
            if (opaq || (fl[x] & 2u)) {
                uint16_t v = bt[x];
                if (opaq || v) dst[x] = legacy_half_mix(grp_sp[x], v, half_mask, ibit, ix2);
            }
        }
        tdrawed = 1; opaq = 0;
        if (tron) LEG_NSP(opaq);
    } else if (!(vc1 & 0x03u) && tron && (vc2 & 0x10u)) {
        LEG_NSP(opaq); opaq = 0;
    }
    if (!(vc1 & 0x0cu) && ton) {
        LEG_TEXT(opaq, 1);
        tdrawed = 1; opaq = 0;
    }

    if (((vc2 & 0x5cu) == 0x14u) && pron) {
        legacy_overlay_key0(dst, grp_sp, width);
    } else if (((vc2 & 0x5du) == 0x1cu) && tron) {
        for (uint32_t x=0; x<width; ++x) {
            const uint16_t w = grp_sp[x];
            if (w != 0u && dst[x] == 0u)
                dst[x] = (uint16_t)((w & half_mask) >> 1);
        }
    }

    if (opaq)
        memset(dst, 0, width * sizeof(uint16_t));

#undef LEG_GRP
#undef LEG_NSP
#undef LEG_TEXT
#undef LEG_BG
}

static void gbt614a_selfcheck(void)
{
    if (!s_bg_line_scratch || !s_bg_pri_scratch ||
        !s_bgsp_ref_scratch || !s_bg_flag_scratch) {
        s_gbt614a_selfcheck_ok = 0;
        ESP_LOGW(TAG, "PX68K_GBT614A: self-check skipped: scratch unavailable; path disabled");
        return;
    }

    const uint32_t width = 256u;
    s_gbt614a_selfcheck_ok = 1;
    for (uint8_t gp = 0; gp < 4u; ++gp) {
        for (uint8_t bp = 0; bp < 4u; ++bp) {
            for (uint8_t tp = 0; tp < 4u; ++tp) {
                for (uint32_t i = 0; i < width; ++i) {
                    /* Deliberately include zero-colour overlay cases. */
                    s_bg_pri_scratch[i] = (i & 4u) ? 0u : (uint16_t)(0x0101u + i * 37u);
                    s_bgsp_ref_scratch[i] = (i & 8u) ? 0u : (uint16_t)(0x0201u + i * 53u);
                    s_bg_flag_scratch[i] = (uint8_t)(i & 3u);
                    s_bg_line_scratch[i] = 0xdeadu;
                }

                render_gbt_line(s_bg_line_scratch,
                                s_bg_pri_scratch,
                                s_bgsp_ref_scratch,
                                s_bg_flag_scratch,
                                width, gp, bp, tp);
                for (uint32_t i = 0; i < width; ++i) {
                    const uint16_t exp = gbt614a_reference_pixel(
                        s_bg_pri_scratch[i], s_bgsp_ref_scratch[i],
                        s_bg_flag_scratch[i], gp, bp, tp);
                    if (s_bg_line_scratch[i] != exp) {
                        s_gbt614a_selfcheck_ok = 0;
                        ESP_LOGE(TAG,
                                 "PX68K_GBT614A: SELF-CHECK FAIL G/B/T=%u/%u/%u x=%lu flags=%u g=%04X bt=%04X got=%04X exp=%04X; path disabled",
                                 (unsigned)gp, (unsigned)bp, (unsigned)tp,
                                 (unsigned long)i, (unsigned)s_bg_flag_scratch[i],
                                 s_bg_pri_scratch[i], s_bgsp_ref_scratch[i],
                                 s_bg_line_scratch[i], exp);
                        return;
                    }
                }
            }
        }
    }
    ESP_LOGI(TAG, "PX68K_GBT614A: synthetic legacy-order/key0 self-check PASS; CPU0 G+BG+TEXT final compositor armed");
}


/* ---------------- Build 5.53a host BG/Sprite renderer ---------------- */
typedef struct __attribute__((packed)) {
    uint16_t sprite_posx;
    uint16_t sprite_posy;
    uint16_t sprite_ctrl;
    uint8_t sprite_ply;
    uint8_t dummy;
} host_sprite_ctrl_t;

static void *arena_alloc(size_t bytes, size_t align)
{
    if (!s_arena || !bytes)
        return NULL;
    if (align < 4u)
        align = 4u;
    uintptr_t base = (uintptr_t)s_arena;
    uintptr_t pos = base + s_arena_used;
    uintptr_t aligned = (pos + align - 1u) & ~(uintptr_t)(align - 1u);
    size_t off = (size_t)(aligned - base);
    if (off + bytes > s_arena_bytes)
        return NULL;
    s_arena_used = (uint32_t)(off + bytes);
    memset((void *)aligned, 0, bytes);
    return (void *)aligned;
}

int tab5_compose_reserve_arena(void)
{
#if TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES || TAB5_R57E49_ASYNC65K_ONLY
    /* R57E49 async-only does not allocate the historical 80 KiB generic arena.
     * R57E33: no CPU0 compose packets are reachable in production. Keep the
     * scarce Internal/DMA arena for audio/USB/guest runtime instead. */
    return 1;
#endif
    if (s_arena)
        return 1;

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    static const uint32_t candidates[] = { TAB5_ARENA_TARGET_BYTES };
    const size_t free_before = heap_caps_get_free_size(caps);
    const size_t largest_before = heap_caps_get_largest_free_block(caps);

    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        uint32_t bytes = candidates[i];
        s_arena = (uint8_t *)heap_caps_aligned_calloc(64, 1, bytes, caps);
        if (s_arena) {
            s_arena_bytes = bytes;
            s_arena_used = 0;
            ESP_LOGI(TAG,
                     "Build 5.98g6 compact L2 raster arena reserved bytes=%u free_before=%u largest_before=%u",
                     (unsigned)bytes, (unsigned)free_before, (unsigned)largest_before);
            return 1;
        }
    }

    ESP_LOGW(TAG,
             "Build 5.98g6 compact L2 raster arena unavailable free=%u largest=%u; dynamic pool fallback",
             (unsigned)free_before, (unsigned)largest_before);
    return 0;
}

static inline void host_bg_store(uint32_t idx, uint8_t dat, uint8_t pal_hi,
                                 int gd, int ng,
                                 uint16_t *line, uint8_t *flags,
                                 const uint16_t *text_pal)
{
    if (idx >= TAB5_BG_SCRATCH_PIXELS)
        return;
    if (ng) {
        uint8_t pix = dat & 0x0fu;
        if (pix) {
            line[idx] = text_pal[(uint8_t)(pix | pal_hi)];
            flags[idx] |= 2u;
        }
    } else {
        uint8_t v = (uint8_t)(dat | pal_hi);
        if (v != 0u && ((v & 0x0fu) || !(flags[idx] & 2u))) {
            line[idx] = text_pal[v];
            flags[idx] |= 2u;
        }
    }
    (void)gd;
}

static inline const uint8_t *r57d_bg_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_bg_render_src) ? s_r57d_bg_render_src : BG;
}
static inline const uint8_t *r57d_c8_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_c8_render_src) ? s_r57d_c8_render_src : BGCHR8;
}
static inline const uint8_t *r57d_c16_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_c16_render_src) ? s_r57d_c16_render_src : BGCHR16;
}
static inline const uint8_t *r57d_sprite_src(void)
{
    return (load_acquire(&s_r57d_shadow_render_active) && s_r57d_sprite_render_src) ? s_r57d_sprite_render_src : Sprite_Regs;
}

static void host_bg_plane8(uint16_t top, uint32_t sx, uint32_t sy,
                           const BG_HOST_LINE_STATE *st, int ng,
                           uint16_t *line, uint8_t *flags,
                           const uint16_t *text_pal, uint32_t width)
{
    uint32_t ebp = ((sy + st->vline_bg - (uint32_t)st->bg_vline) & 7u) << 3;
    uint32_t edx = (uint32_t)top + (((sy + st->vline_bg - (uint32_t)st->bg_vline) & 0x1f8u) << 4);
    uint32_t edi = ((sx - (uint32_t)st->h_adjust) & 7u) ^ 15u;
    uint32_t ecx = ((sx - (uint32_t)st->h_adjust) & 0x1f8u) >> 2;

    for (int tile = (int)(width >> 3); tile >= 0; --tile) {
        const uint8_t *mp = &r57d_bg_src()[ecx + edx];
#ifndef MSB_FIRST
        const uint16_t mw = *(const uint16_t *)mp;
        uint8_t map = (uint8_t)mw;
        uint16_t si = (uint16_t)(mw >> 8) << 6;
#else
        uint8_t map = mp[0];
        uint16_t si = (uint16_t)mp[1] << 6;
#endif
        uint8_t pal_hi = (uint8_t)(map << 4);
        const uint8_t *src;
        int step;

        if (map < 0x40u) {
            src = &r57d_c8_src()[si + ebp]; step = 1;
        } else if ((uint8_t)(map - 0x40u) & 0x80u) {
            src = &r57d_c8_src()[si + 0x3fu - ebp]; step = -1;
        } else if ((int8_t)map >= 0x40) {
            src = &r57d_c8_src()[si + ebp + 7u]; step = -1;
        } else {
            src = &r57d_c8_src()[si + 0x38u - ebp]; step = 1;
        }

        for (int j = 0; j < 8; ++j) {
            host_bg_store(1u + edi + (uint32_t)j, *src, pal_hi, !ng, ng,
                          line, flags, text_pal);
            src += step;
        }
        edi += 8u;
        ecx = (ecx + 2u) & 0x7fu;
    }
}

static void host_bg_plane16(uint16_t top, uint32_t sx, uint32_t sy,
                            const BG_HOST_LINE_STATE *st, int ng,
                            uint16_t *line, uint8_t *flags,
                            const uint16_t *text_pal, uint32_t width)
{
    const int32_t adjust = ng ? 0 : st->h_adjust; /* matches PX68K 16-dot NG path */
    uint32_t ebp = ((sy + st->vline_bg - (uint32_t)st->bg_vline) & 15u) << 4;
    uint32_t edx = (uint32_t)top + (((sy + st->vline_bg - (uint32_t)st->bg_vline) & 0x3f0u) << 3);
    uint32_t edi = ((sx - (uint32_t)adjust) & 15u) ^ 15u;
    uint32_t ecx = ((sx - (uint32_t)adjust) & 0x3f0u) >> 3;

    for (int tile = (int)(width >> 4); tile >= 0; --tile) {
        const uint8_t *mp = &r57d_bg_src()[ecx + edx];
#ifndef MSB_FIRST
        const uint16_t mw = *(const uint16_t *)mp;
        uint8_t map = (uint8_t)mw;
        uint16_t si = (uint16_t)(mw >> 8) << 8;
#else
        uint8_t map = mp[0];
        uint16_t si = (uint16_t)mp[1] << 8;
#endif
        uint8_t pal_hi = (uint8_t)(map << 4);
        const uint8_t *src;
        int step;

        if (map < 0x40u) {
            src = &r57d_c16_src()[si + ebp]; step = 1;
        } else if ((uint8_t)(map - 0x40u) & 0x80u) {
            src = &r57d_c16_src()[si + 0xffu - ebp]; step = -1;
        } else if ((int8_t)map >= 0x40) {
            src = &r57d_c16_src()[si + ebp + 15u]; step = -1;
        } else {
            src = &r57d_c16_src()[si + 0xf0u - ebp]; step = 1;
        }

        for (int j = 0; j < 16; ++j) {
            host_bg_store(1u + edi + (uint32_t)j, *src, pal_hi, !ng, ng,
                          line, flags, text_pal);
            src += step;
        }
        edi += 16u;
        ecx = (ecx + 2u) & 0x7fu;
    }
}

static void host_sprite_priority(const BG_HOST_LINE_STATE *st, unsigned pri_group,
                                 uint16_t *line, uint16_t *pri, uint8_t *flags,
                                 const uint16_t *text_pal, uint32_t width)
{
    const host_sprite_ctrl_t *sct = (const host_sprite_ctrl_t *)r57d_sprite_src();
    const uint32_t count = st->sprite_count[pri_group];

    for (uint32_t k = 0; k < count; ++k) {
        const unsigned n = st->sprite_idx[pri_group][k];
        const host_sprite_ctrl_t *sp = &sct[n];
        uint32_t t = ((uint32_t)sp->sprite_posx + (uint32_t)st->h_adjust) & 0x3ffu;
        if (t >= width + 16u)
            continue;

        uint32_t y = (uint32_t)sp->sprite_posy & 0x3ffu;
        y -= st->vline_bg;
        y += (uint32_t)st->bg_vline;
        y = 0u - y;
        y += 16u;
        if (y > 15u)
            continue;

        uint16_t ctrl = sp->sprite_ctrl;
        const uint8_t *src;
        int step;
        if (ctrl < 0x4000u) {
            src = &r57d_c16_src()[(((uint32_t)ctrl * 256u) & 0xffffu) + y * 16u];
            step = 1;
        } else if ((uint16_t)(ctrl - 0x4000u) & 0x8000u) {
            src = &r57d_c16_src()[(((uint32_t)ctrl * 256u) & 0xffffu)
                         + (((y * 16u) & 0xffu) ^ 0xf0u) + 15u];
            step = -1;
        } else if ((int16_t)ctrl >= 0x4000) {
            src = &r57d_c16_src()[(((uint32_t)ctrl * 256u) & 0xffffu) + y * 16u + 15u];
            step = -1;
        } else {
            src = &r57d_c16_src()[(((uint32_t)ctrl << 8) & 0xffffu)
                         + (((y * 16u) & 0xffu) ^ 0xf0u)];
            step = 1;
        }

        uint32_t pal_base = (ctrl >> 4) & 0xf0u;
        const uint16_t pri_key = (uint16_t)(n * 8u);
        for (int i = 0; i < 16; ++i, ++t, src += step) {
            if (t >= TAB5_BG_SCRATCH_PIXELS)
                continue;
            uint32_t pal = *src & 0x0fu;
            if (pal) {
                pal |= pal_base;
                if (pri[t] >= pri_key) {
                    line[t] = text_pal[pal];
                    flags[t] |= 2u;
                    pri[t] = pri_key;
                }
            }
        }
    }
}

static const uint16_t *render_host_bgsp(const BG_HOST_LINE_STATE *st,
                                        const uint16_t *text_pal,
                                        uint32_t width)
{
    uint16_t *line = s_bg_line_scratch;
    uint16_t *pri = s_bg_pri_scratch;
    uint8_t *flags = s_bg_flag_scratch;
    if (!line || !pri || !flags)
        return NULL;

    memset(flags, 0, TAB5_BG_SCRATCH_PIXELS * sizeof(flags[0]));
    memset(pri, 0xff, TAB5_BG_SCRATCH_PIXELS * sizeof(pri[0]));
    if (text_pal[0] == 0u)
        memset(&line[16], 0, (size_t)width * sizeof(line[0]));
    else
        for (uint32_t i = 0; i < width; ++i) line[16u + i] = text_pal[0];

    host_sprite_priority(st, 0u, line, pri, flags, text_pal, width);
    if ((st->reg9 & 8u) && st->chr_size == 8u)
        host_bg_plane8(st->bg1_top, st->bg1_scroll_x, st->bg1_scroll_y,
                       st, st->gd ? 0 : 1, line, flags, text_pal, width);
    host_sprite_priority(st, 1u, line, pri, flags, text_pal, width);
    if (st->reg9 & 1u) {
        if (st->chr_size == 8u)
            host_bg_plane8(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,
                           st, st->gd ? 0 : 1, line, flags, text_pal, width);
        else
            host_bg_plane16(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,
                            st, st->gd ? 0 : 1, line, flags, text_pal, width);
    }
    host_sprite_priority(st, 2u, line, pri, flags, text_pal, width);
    return &line[16];
}

/* R56s5: reproduce the stock common BG/TEXT construction order on CPU0.
 * The fast-path admission already guarantees TextPal[0]==0 and no text wrap.
 * gd=0 is the stock "TEXT opaque, then higher-priority BG/Sprite overlay" case;
 * gd=1 is "BG/Sprite opaque, then TEXT key-zero overlay".  This preserves
 * BG_LineBuf + Text_TrFlag semantics instead of treating TEXT as an independent
 * global-priority layer (the R56s bug exposed by filenames in X68000~1.HDS). */
static const uint16_t *r56s5_render_host_bt_exact(const BG_HOST_LINE_STATE *st,
                                                   const uint8_t *text_idx,
                                                   const uint16_t *text_pal,
                                                   uint32_t width, int text_on,
                                                   const uint8_t **flags_out)
{
    uint16_t *line = s_bg_line_scratch;
    uint16_t *pri = s_bg_pri_scratch;
    uint8_t *flags = s_bg_flag_scratch;
    if (!line || !pri || !flags || !text_pal || width > TAB5_COMPOSE_MAX_WIDTH)
        return NULL;

    memset(flags, 0, TAB5_BG_SCRATCH_PIXELS * sizeof(flags[0]));
    memset(pri, 0xff, TAB5_BG_SCRATCH_PIXELS * sizeof(pri[0]));
    memset(&line[16], 0, (size_t)width * sizeof(line[0]));

    const int text_first = text_on && st && !st->gd;
    if (text_first) {
        for (uint32_t i = 0; i < width; ++i) {
            const uint8_t ti = (uint8_t)(text_idx[i] & 0x0fu);
            line[16u + i] = text_pal[ti];
            flags[16u + i] = ti ? 1u : 0u;
        }
    }

    if (st) {
        host_sprite_priority(st, 0u, line, pri, flags, text_pal, width);
        if ((st->reg9 & 8u) && st->chr_size == 8u)
            host_bg_plane8(st->bg1_top, st->bg1_scroll_x, st->bg1_scroll_y,
                           st, st->gd ? 0 : 1, line, flags, text_pal, width);
        host_sprite_priority(st, 1u, line, pri, flags, text_pal, width);
        if (st->reg9 & 1u) {
            if (st->chr_size == 8u)
                host_bg_plane8(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,
                               st, st->gd ? 0 : 1, line, flags, text_pal, width);
            else
                host_bg_plane16(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,
                                st, st->gd ? 0 : 1, line, flags, text_pal, width);
        }
        host_sprite_priority(st, 2u, line, pri, flags, text_pal, width);
    }

    if (text_on && !text_first) {
        for (uint32_t i = 0; i < width; ++i) {
            const uint8_t ti = (uint8_t)(text_idx[i] & 0x0fu);
            if (ti) {
                line[16u + i] = text_pal[ti];
                flags[16u + i] |= 1u;
            }
        }
    }

    if (flags_out) *flags_out = &flags[16];
    return &line[16];
}

/* Build 6.15f: dedicated 65K SPSC rings.  CPU1 owns ready_head/free_tail;
 * CPU0 owns ready_tail/free_head.  128 slots is a power of two, so these stay
 * branch-free in the hot path. */
static inline int gbt65k_ready_push(uint8_t idx)
{
    const uint32_t head = load_relaxed(&s_gbt65k_ready_head);
    const uint32_t tail = load_acquire(&s_gbt65k_ready_tail);
    if ((uint32_t)(head - tail) >= TAB5_GBT65K_BURST_SLOTS)
        return 0;
    s_gbt65k_ready_ring[head & TAB5_GBT65K_BURST_MASK] = idx;
    store_release(&s_gbt65k_ready_head, head + 1u);
    return 1;
}

static inline int gbt65k_ready_pop(uint8_t *idx)
{
    const uint32_t tail = load_relaxed(&s_gbt65k_ready_tail);
    const uint32_t head = load_acquire(&s_gbt65k_ready_head);
    if (tail == head)
        return 0;
    *idx = s_gbt65k_ready_ring[tail & TAB5_GBT65K_BURST_MASK];
    store_release(&s_gbt65k_ready_tail, tail + 1u);
    return 1;
}

static inline int gbt65k_free_push(uint8_t idx)
{
    const uint32_t head = load_relaxed(&s_gbt65k_free_head);
    const uint32_t tail = load_acquire(&s_gbt65k_free_tail);
    if ((uint32_t)(head - tail) >= TAB5_GBT65K_BURST_SLOTS)
        return 0;
    s_gbt65k_free_ring[head & TAB5_GBT65K_BURST_MASK] = idx;
    store_release(&s_gbt65k_free_head, head + 1u);
    return 1;
}

static inline int gbt65k_free_pop(uint8_t *idx)
{
    const uint32_t tail = load_relaxed(&s_gbt65k_free_tail);
    const uint32_t head = load_acquire(&s_gbt65k_free_head);
    if (tail == head)
        return 0;
    *idx = s_gbt65k_free_ring[tail & TAB5_GBT65K_BURST_MASK];
    store_release(&s_gbt65k_free_tail, tail + 1u);
    return 1;
}

/* CPU1 producer -> CPU0 consumer. */
static int ready_push(uint8_t idx)
{
#if TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES
    /* R57E33HF1: compositor is compile-time dormant.  Do not leave atomic
     * references to the intentionally-unallocated mailbox for GCC to fold
     * into NULL accesses under -Werror=stringop-overflow. */
    (void)idx;
    return 0;
#else
    const uint32_t head = load_relaxed(&s_mb->ready_head);
    const uint32_t tail = load_acquire(&s_mb->ready_tail);
    if ((uint32_t)(head - tail) >= s_slot_count)
        return 0;
    s_mb->ready_ring[slot_ring_index(head)] = idx;
    store_release(&s_mb->ready_head, head + 1u);
    __atomic_add_fetch(&s_mb->submit_seq, 1u, __ATOMIC_RELAXED);
    return 1;
#endif
}

static int ready_pop(uint8_t *idx)
{
#if TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES
    (void)idx;
    return 0;
#else
    const uint32_t tail = load_relaxed(&s_mb->ready_tail);
    const uint32_t head = load_acquire(&s_mb->ready_head);
    if (tail == head)
        return 0;
    *idx = s_mb->ready_ring[slot_ring_index(tail)];
    store_release(&s_mb->ready_tail, tail + 1u);
    return 1;
#endif
}

/* CPU0 producer -> CPU1 consumer. */
static int free_push(uint8_t idx)
{
#if TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES
    (void)idx;
    return 0;
#else
    const uint32_t head = load_relaxed(&s_mb->free_head);
    const uint32_t tail = load_acquire(&s_mb->free_tail);
    if ((uint32_t)(head - tail) >= s_slot_count)
        return 0;
    s_mb->free_ring[slot_ring_index(head)] = idx;
    store_release(&s_mb->free_head, head + 1u);
    return 1;
#endif
}

static int free_pop(uint8_t *idx)
{
#if TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES
    (void)idx;
    return 0;
#else
    const uint32_t tail = load_relaxed(&s_mb->free_tail);
    const uint32_t head = load_acquire(&s_mb->free_head);
    if (tail == head)
        return 0;
    *idx = s_mb->free_ring[slot_ring_index(tail)];
    store_release(&s_mb->free_tail, tail + 1u);
    return 1;
#endif
}

static inline void gdma614b2_release_gvram_once(gbt_raw_dma_state_t *st)
{
    uint32_t expect = 0u;
    if (__atomic_compare_exchange_n(&st->gvram_released, &expect, 1u, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_sub_fetch(&tab5_compose_gvram_dma_pending, 1u, __ATOMIC_RELEASE);
        const int64_t now_us = esp_timer_get_time();
        s_last_gdma614b2_us = (uint32_t)((now_us > st->start_us) ? (now_us - st->start_us) : 0);
    }
}

static inline int gdma614b2_claim_queue(gbt_raw_dma_state_t *st)
{
    if (load_acquire(&st->pending_reqs) != 0u ||
        load_acquire(&st->metadata_ready) == 0u)
        return 0;
    uint32_t expect = 0u;
    return __atomic_compare_exchange_n(&st->queued, &expect, 1u, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

static void gdma614b2_queue_from_task(gbt_raw_dma_state_t *st)
{
    if (!gdma614b2_claim_queue(st)) return;
    if (!ready_push(st->slot_idx)) {
        /* Ring capacity is tied 1:1 to the slot pool, so this is an invariant
         * fault rather than a normal queue-full fallback. Keep it visible. */
        ++s_queue_full;
        ++s_fallback;
        return;
    }
    __atomic_add_fetch(&s_mb->notify_count, 1u, __ATOMIC_RELAXED);
    xTaskNotifyGive(s_task);
}

static bool gdma614b2_done_cb(async_memcpy_handle_t mcp_hdl,
                                        async_memcpy_event_t *event,
                                        void *cb_args)
{
    (void)mcp_hdl; (void)event;
    gbt_raw_dma_state_t *st = (gbt_raw_dma_state_t *)cb_args;
    const uint32_t left = __atomic_sub_fetch(&st->pending_reqs, 1u, __ATOMIC_ACQ_REL);
    if (left != 0u) return false;

    gdma614b2_release_gvram_once(st);
    if (!gdma614b2_claim_queue(st)) return false;

    if (!ready_push(st->slot_idx)) {
        ++s_queue_full;
        ++s_fallback;
        return false;
    }
    __atomic_add_fetch(&s_mb->notify_count, 1u, __ATOMIC_RELAXED);
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &hp);
    return hp == pdTRUE;
}

static inline void gdma614b2_finish_sync_run(gbt_raw_dma_state_t *st)
{
    const uint32_t left = __atomic_sub_fetch(&st->pending_reqs, 1u, __ATOMIC_ACQ_REL);
    if (left == 0u) gdma614b2_release_gvram_once(st);
}

static void compose_task(void *arg)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    {
        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);
        esp_rom_printf("R56K5_TASKSELF name=px68k_comp core=%d base=0x%08x top=0x%08x bytes=4096 hwm=%u\n",
                       (int)xPortGetCoreID(), (unsigned)r56k5_base,
                       (unsigned)(r56k5_base + 4096u),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif
    (void)arg;
    for (;;) {
        uint8_t idx;
        int burst65k = 0;
        compose_slot_t *slot = NULL;

        /* R56k: common and 65K work share the CPU0 priority-3 host class.
         * Exactly one completed compose job is the scheduling quantum. */
        if (ready_pop(&idx)) {
            slot = &s_slots[idx];
        } else if (s_gbt65k_burst_ready && gbt65k_ready_pop(&idx)) {
            slot = &s_gbt65k_burst_slots[idx];
            burst65k = 1;
        } else {
            __atomic_add_fetch(&s_mb->ready_empty, 1u, __ATOMIC_RELAXED);
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const uint64_t render_ticket = burst65k
            ? s_gbt65k_burst_render_ticket[idx]
            : s_slot_render_ticket[idx];
        const uint64_t flow_seq = burst65k ? s_gbt65k_burst_flow_seq[idx] : s_slot_flow_seq[idx];
        const uint32_t flow_epoch = burst65k ? s_gbt65k_burst_flow_epoch[idx] : s_slot_flow_epoch[idx];
        const uint32_t flow_visual = burst65k ? s_gbt65k_burst_flow_visual[idx] : s_slot_flow_visual[idx];
        const int r57e49_stale =
            slot->type == COMPOSE_JOB_GBT65K && flow_seq && slot->y < 600u &&
            (uint32_t)flow_seq != __atomic_load_n(&s_r57e49_latest_seq32[slot->y], __ATOMIC_ACQUIRE);

        uint32_t r57d_hold_seq = 0u;
        int r57d_hold_ok = 1;
        if (slot->type == COMPOSE_JOB_GBT65K) {
            r57d_hold_seq = burst65k ? s_r57d_burst_hold[idx] : s_r57d_slot_hold[idx];
            if (r57e49_stale) {
                /* Latest-wins must be cheap: stale CPU0 work never waits for a
                 * shadow freeze it will not render. Cancelling is ordered and
                 * lets the R57 consumer skip this bounded hold. */
                if (r57d_hold_seq) {
                    tab5_guest_bus_shadow_hold_cancel(r57d_hold_seq);
                    if (burst65k) s_r57d_burst_hold[idx] = 0u;
                    else s_r57d_slot_hold[idx] = 0u;
                    r57d_hold_seq = 0u;
                }
            } else if (r57d_hold_seq) {
                if (tab5_guest_bus_shadow_hold_wait(r57d_hold_seq)) {
                    s_r57d_bg_render_src = tab5_guest_bus_bg_shadow();
                    s_r57d_c8_render_src = tab5_guest_bus_bgchr8_shadow();
                    s_r57d_c16_render_src = tab5_guest_bus_bgchr16_shadow();
                    s_r57d_sprite_render_src = tab5_guest_bus_sprite_shadow();
                    s_r57e_tvram_render_src = tab5_guest_bus_tvram_shadow();
                    store_release(&s_r57d_shadow_render_active, 1u);
                    if ((slot->selfcheck & 64u) && !r57e_expand_text_shadow(slot)) {
                        r57d_hold_ok=0;
                        __atomic_add_fetch(&s_r57e_text_shadow_fail,1u,__ATOMIC_RELAXED);
                    }
                } else {
                    r57d_hold_ok = 0;
                    __atomic_add_fetch(&s_r57d_hold_wait_fail, 1u, __ATOMIC_RELAXED);
                    printf("PX68K_R57D: HOLD WAIT FAIL seq=%lu; exact packet fallback/cancel for this line\n",
                           (unsigned long)r57d_hold_seq);
                }
            }
        }

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
#else
        const uint32_t cc0 = 0u;
#endif
        int source_completed = 0;
        /* R56 ownership boundary: CPU0 renders only into a renderer-private
         * scratch line.  Screen Manager receives the result message and is
         * the only code allowed to mutate the displayable working surface. */
        slot->dst = s_screen_result_scratch;
        if (slot->type == COMPOSE_JOB_GRP8PAIR_BGSP ||
            slot->type == COMPOSE_JOB_GRP8SPLIT_BGSP) {
            const uint16_t *bg = render_host_bgsp(&slot->u.grp8.aux.host_bgsp.state,
                                                   slot->u.grp8.aux.host_bgsp.text_pal,
                                                   slot->width);
            if (bg) {
                if ((slot->selfcheck & 1u) && load_acquire(&s_bgsp_selfcheck_state) == 1u) {
                    uint32_t bad = slot->width;
                    for (uint32_t i = 0; i < slot->width; ++i) {
                        if (bg[i] != s_bgsp_ref_scratch[i]) { bad = i; break; }
                    }
                    if (bad == slot->width) {
                        store_release(&s_bgsp_selfcheck_state, 2u);
                        printf("PX68K_BGSPHOST: *** BUILD 5.53a SELF-CHECK OK; CPU0 BG/Sprite matches stock PX68K ***\n");
                    } else {
                        const uint16_t got = bg[bad];
                        const uint16_t ref = s_bgsp_ref_scratch[bad];
                        store_release(&s_bgsp_selfcheck_state, 3u);
                        printf("PX68K_BGSPHOST: *** SELF-CHECK FAILED x=%lu host=%04X stock=%04X; disabling CPU0 BG/Sprite path ***\n",
                               (unsigned long)bad, (unsigned)got, (unsigned)ref);
                    }
                }
                if (slot->type == COMPOSE_JOB_GRP8SPLIT_BGSP) {
                    int split_ok = 1;
                    if ((slot->selfcheck & 2u) && load_acquire(&s_grp8split_selfcheck_state) == 1u) {
                        uint32_t bad = slot->width;
                        const uint16_t *blo = slot->u.grp8.src.split.bottom_lo;
                        const uint16_t *bhi = slot->u.grp8.src.split.bottom_hi;
                        const uint16_t *tlo = slot->u.grp8.src.split.top_lo;
                        const uint16_t *thi = slot->u.grp8.src.split.top_hi;
                        const unsigned bshift = (unsigned)(slot->bottom_page & 1u) * 8u;
                        const unsigned tshift = (unsigned)(slot->top_page & 1u) * 8u;
                        for (uint32_t i = 0; i < slot->width; ++i) {
                            const uint8_t bl = (uint8_t)(blo[i] >> bshift);
                            const uint8_t bh = (uint8_t)(bhi[i] >> bshift);
                            const uint8_t tl = (uint8_t)(tlo[i] >> tshift);
                            const uint8_t th = (uint8_t)(thi[i] >> tshift);
                            uint16_t bi = (uint16_t)((bh & 0xf0u) | (bl & 0x0fu));
                            uint16_t ti = (uint16_t)((th & 0xf0u) | (tl & 0x0fu));
                            uint16_t got = slot->u.grp8.pal[ti ? ti : bi];
                            if (got != s_grp8split_ref_scratch[i]) { bad = i; break; }
                        }
                        if (bad == slot->width) {
                            store_release(&s_grp8split_selfcheck_state, 2u);
                            printf("PX68K_GRP8SPLIT: *** BUILD 5.53a SELF-CHECK OK; unequal-scroll raw16 CPU0 GRP8 matches stock PX68K ***\n");
                        } else {
                            split_ok = 0;
                            store_release(&s_grp8split_selfcheck_state, 3u);
                            printf("PX68K_GRP8SPLIT: *** SELF-CHECK FAILED x=%lu; disabling unequal-scroll raw16 host path ***\n",
                                   (unsigned long)bad);
                        }
                    }
                    if (split_ok) {
                        render_grp8split_bg(slot->dst,
                                             slot->u.grp8.src.split.bottom_lo,
                                             slot->u.grp8.src.split.bottom_hi,
                                             slot->u.grp8.src.split.top_lo,
                                             slot->u.grp8.src.split.top_hi,
                                             slot->u.grp8.pal, bg, slot->width,
                                             slot->bottom_page, slot->top_page,
                                             slot->bg_on_top);
                    } else {
                        if (slot->bg_on_top)
                            blend_key0(slot->dst, s_grp8split_ref_scratch, bg, slot->width);
                        else
                            blend_key0(slot->dst, bg, s_grp8split_ref_scratch, slot->width);
                    }
                } else {
                    render_grp8pair_bg(slot->dst,
                                       slot->u.grp8.src.pair.low_pair,
                                       slot->u.grp8.src.pair.high_pair,
                                       slot->u.grp8.pal, bg, slot->width,
                                       slot->bottom_page, slot->top_page,
                                       slot->bg_on_top);
                }
            }
            s_last_bgsp_render_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
            ++s_bgsp_completed;
            ++s_grp8_completed;
            __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
        } else if (slot->type == COMPOSE_JOB_GBT) {
            render_gbt_line(slot->dst,
                            slot->u.gbt.grp,
                            slot->u.gbt.bg_text,
                            slot->u.gbt.flags,
                            slot->width,
                            slot->grp_pri,
                            slot->bg_pri,
                            slot->text_pri);
            s_last_gbt_render_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
            ++s_gbt_completed;
        } else if (slot->type == COMPOSE_JOB_LEGACY_FINAL) {
            const uint32_t cc_legacy = (uint32_t)esp_cpu_get_cycle_count();
            render_legacy_final(slot);
            s_last_legacy_final_render_us =
                cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc_legacy);
            ++s_legacy_final_completed;
        } else if (slot->type == COMPOSE_JOB_GBT65K) {
            /* R57E49: stale work is rejected before any shadow wait above.
             * A current packet waits only on CPU0, renders from the ordered
             * R57 shadow, then publishes through Screen Manager latest-wins. */
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            const int64_t work_t0 = esp_timer_get_time();
#endif
            int rendered = 0;
            if (r57e49_stale) {
                __atomic_add_fetch(&s_gbt65k_stale_dropped, 1u, __ATOMIC_RELAXED);
            } else if (!r57d_hold_seq || r57d_hold_ok) {
                rendered = render_gbt65k_line(slot);
            } else if (slot->selfcheck & 8u) {
                const uint8_t saved_selfcheck = slot->selfcheck;
                slot->selfcheck = (uint8_t)(saved_selfcheck & ~32u);
                rendered = render_gbt65k_line(slot);
                slot->selfcheck = saved_selfcheck;
            }
            source_completed = rendered ? 1 : 0;
            if (rendered)
                ++s_r38_prod_changed;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            const uint32_t work_us = (uint32_t)(esp_timer_get_time() - work_t0);
            s_last_gbt65k_render_us = work_us;
            s_gbt65k_cpu0_work_us += work_us;
#endif
            ++s_gbt65k_completed;
            if ((slot->selfcheck & 2u) && !r57d_hold_seq)
                __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
        } else if (slot->type == COMPOSE_JOB_GBT_SCROLLCACHE) {
            const uint8_t *idx_row = NULL;
            const uint32_t ylo = (slot->u.gbt_scroll.y_lo_base >> 10) & 511u;
            const uint32_t yhi = (slot->u.gbt_scroll.y_hi_base >> 10) & 511u;
            const uint32_t dx = (slot->u.gbt_scroll.x_hi - slot->u.gbt_scroll.x_lo) & 511u;
            if (scroll614c_get_row(slot->u.gbt_scroll.gvram, ylo, yhi, dx,
                                   slot->bottom_page, slot->top_page, &idx_row)) {
                render_scroll614c_gbt(slot->dst, idx_row, slot->u.gbt_scroll.x_lo,
                                      slot->u.gbt_scroll.pal, slot->u.gbt_scroll.bg_text,
                                      slot->u.gbt_scroll.flags, slot->width, slot->grp_pri,
                                      slot->bg_pri, slot->text_pri);
            }
            s_last_scroll614c_render_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
            ++s_scroll614c_completed;
            ++s_gbt_completed;
            __atomic_sub_fetch(&tab5_compose_gvram_cache_pending, 1u, __ATOMIC_RELEASE);
        } else if (slot->type == COMPOSE_JOB_GBT_RAWPAIR) {
            render_grp8pair_gbt(slot->dst,
                                slot->u.gbt_raw.low_pair,
                                slot->u.gbt_raw.high_pair,
                                slot->u.gbt_raw.pal,
                                slot->u.gbt_raw.bg_text,
                                slot->u.gbt_raw.flags,
                                slot->width,
                                slot->bottom_page,
                                slot->top_page,
                                slot->grp_pri,
                                slot->bg_pri,
                                slot->text_pri);
            s_last_gbt_raw_render_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
            ++s_gbt_raw_completed;
            ++s_gbt_completed;
        } else if (slot->type == COMPOSE_JOB_GRP8PAIR ||
                   slot->type == COMPOSE_JOB_GRP8SPLIT) {
            if (slot->type == COMPOSE_JOB_GRP8SPLIT) {
                render_grp8split_bg(slot->dst,
                                     slot->u.grp8.src.split.bottom_lo,
                                     slot->u.grp8.src.split.bottom_hi,
                                     slot->u.grp8.src.split.top_lo,
                                     slot->u.grp8.src.split.top_hi,
                                     slot->u.grp8.pal, slot->u.grp8.aux.bg,
                                     slot->width, slot->bottom_page,
                                     slot->top_page, slot->bg_on_top);
            } else {
                render_grp8pair_bg(slot->dst,
                                   slot->u.grp8.src.pair.low_pair,
                                   slot->u.grp8.src.pair.high_pair,
                                   slot->u.grp8.pal, slot->u.grp8.aux.bg,
                                   slot->width, slot->bottom_page,
                                   slot->top_page, slot->bg_on_top);
            }
            s_last_grp8_render_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
            ++s_grp8_completed;
        } else {
            cblend598g3_run_job(slot->dst, slot->u.blend.bottom, slot->u.blend.top, slot->width);
            s_last_blend_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
        }

        if (slot->type != COMPOSE_JOB_GBT65K)
            source_completed = 1;

        /* R57E54: the ordered shadow is needed only until the exact RGB565
         * scratch line has been produced.  Screen Manager/LCD are downstream
         * host services and must never extend a guest-shadow freeze. */
        if (r57d_hold_seq) {
            store_release(&s_r57d_shadow_render_active,0u);
            tab5_guest_bus_shadow_hold_release(r57d_hold_seq);
            if (burst65k) s_r57d_burst_hold[idx]=0u; else s_r57d_slot_hold[idx]=0u;
            r57d_hold_seq = 0u;
        }

        if (source_completed) {
            if (flow_seq) {
                /* Exact CPU0 composition is complete regardless of whether a
                 * Screen result slot is available.  Publish progress first,
                 * then perform a strictly nonblocking host handoff. */
                tab5_video_flow_cpu0_complete(flow_seq);
                uint32_t flow_slot = UINT32_MAX;
                uint16_t *flow_pixels = tab5_screen_cpu0_flow_acquire(&flow_slot);
                if (flow_pixels) {
                    memcpy(flow_pixels, slot->dst, (size_t)slot->width * sizeof(uint16_t));
                    if (!tab5_screen_cpu0_flow_submit(flow_slot, slot->y, slot->width,
                                                      flow_seq, flow_epoch, flow_visual)) {
                        tab5_screen_cpu0_flow_release(flow_slot);
                        tab5_cpu1_video_retry_from_cpu0(slot->y);
                    }
                } else {
                    /* Latest-wins visual drop. CPU1 owns TextDirtyLine, so ask
                     * it to retry this row at the next guest frame boundary. */
                    tab5_cpu1_video_retry_from_cpu0(slot->y);
                }
            } else if (render_ticket) {
                (void)tab5_screen_render_result(render_ticket, slot->width, slot->dst);
            }
        } else {
            if (!flow_seq && render_ticket)
                tab5_screen_render_cancel(render_ticket);
        }

        if (burst65k) {
            s_gbt65k_burst_render_ticket[idx] = 0u;
            s_gbt65k_burst_flow_seq[idx] = 0u;
            s_gbt65k_burst_flow_epoch[idx] = 0u;
            s_gbt65k_burst_flow_visual[idx] = 0u;
            if (!gbt65k_free_push(idx))
                ++s_gbt65k_burst_full; /* invariant fault */
            __atomic_sub_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_RELEASE);
        } else {
            s_slot_render_ticket[idx] = 0u;
            s_slot_flow_seq[idx] = 0u;
            s_slot_flow_epoch[idx] = 0u;
            s_slot_flow_visual[idx] = 0u;
            ++s_completed;
            __atomic_add_fetch(&s_mb->done_seq, 1u, __ATOMIC_RELEASE);

            if (!free_push(idx)) {
                /* This should be impossible: each consumed ready slot creates
                 * at most one returned free slot. */
                ++s_queue_full;
            }
            __atomic_sub_fetch(&s_pending, 1u, __ATOMIC_RELEASE);
        }

        /* Complete render-result/slot-return boundary; no mid-raster yield. */
        taskYIELD();
    }
}

static compose_slot_t *alloc_slot_pool(uint32_t *slot_count_out)
{
    /* Build 5.98g6: 8-slot pool is sufficient for the observed workload and
     * releases roughly 64 KiB of contiguous internal/DMA SRAM for USB/HID. */
    static const uint8_t candidates[] = {8u};
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    for (unsigned i = 0; i < sizeof(candidates); ++i) {
        const uint32_t n = candidates[i];
        const size_t bytes = (size_t)n * sizeof(compose_slot_t);
        compose_slot_t *p = NULL;
        if (s_arena)
            p = (compose_slot_t *)arena_alloc(bytes, 64u);
        else
            p = (compose_slot_t *)heap_caps_aligned_calloc(64, 1, bytes, caps);
        if (p) {
            *slot_count_out = n;
            return p;
        }
    }
    *slot_count_out = 0;
    return NULL;
}

int tab5_compose_init(void)
{
    if (s_ready)
        return 1;

#if TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES
    /* Keep the ordered guest bus / LP shadow alive, but do not create the CPU0
     * compositor task, GDMA client, packet pool, or Internal raster arena.
     * Any accidental submit sees s_ready==0 and safely falls back to CPU1. */
    (void)tab5_guest_bus_init();
    ESP_LOGI(TAG,
             "PX68K_COMPOSE_R57E33HF1: compositor DORMANT; task=0 arena=0 packetPool=0 GDMA=0 sourceCaches=0; R57 guest-bus retained");
    return 1;
#endif

    (void)tab5_guest_bus_init(); /* observation-only; allocation failure keeps R56s5k exact path */
    if (!s_r57d_class_state) {
        uint8_t *p=(uint8_t*)heap_caps_aligned_calloc(64, R57D_CLASS_COUNT, 4u,
                                                      MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (p) {
            s_r57d_class_state=p;
            s_r57d_class_pass=p+R57D_CLASS_COUNT;
            s_r57d_class_epoch=(uint16_t*)(void*)(p+R57D_CLASS_COUNT*2u);
        }
        else ESP_LOGW(TAG,"PX68K_R57E5: class table allocation failed; CPU0 TEXT certification unavailable");
    }


    (void)tab5_compose_reserve_arena();

    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
#if TAB5_FAST_MAILBOX_CAP
    const size_t fast_total = heap_caps_get_total_size(TAB5_FAST_MAILBOX_CAP);
    const size_t fast_before = heap_caps_get_free_size(TAB5_FAST_MAILBOX_CAP);
    const size_t fast_largest = heap_caps_get_largest_free_block(TAB5_FAST_MAILBOX_CAP);
    s_mb = (compose_mailbox_t *)heap_caps_aligned_calloc(64, 1, sizeof(*s_mb), TAB5_FAST_MAILBOX_CAP);
    s_mailbox_tcm = s_mb != NULL;
#else
    const size_t fast_total = 0, fast_before = 0, fast_largest = 0;
    s_mb = NULL;
    s_mailbox_tcm = 0;
#endif
    if (!s_mb) {
        s_mb = (compose_mailbox_t *)heap_caps_aligned_calloc(
            64, 1, sizeof(*s_mb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    }
    if (!s_mb)
        return 0;
    s_mailbox_bytes = (uint32_t)sizeof(*s_mb);

#if TAB5_R57E49_ASYNC65K_ONLY
    s_slots = NULL;
    s_slot_count = 0u;
    s_pool_bytes = 0u;
    s_slot_mask = 0u;
#else
    s_slots = alloc_slot_pool(&s_slot_count);
    if (!s_slots || s_slot_count < TAB5_COMPOSE_MIN_SLOTS)
        return 0;
    s_pool_bytes = (uint32_t)((size_t)s_slot_count * sizeof(compose_slot_t));
    s_slot_mask = ((s_slot_count & (s_slot_count - 1u)) == 0u) ? (s_slot_count - 1u) : 0u;
#endif

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    if (s_arena) {
        s_bg_line_scratch = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
        s_bg_pri_scratch  = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
        s_bg_flag_scratch = (uint8_t  *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint8_t), 64u);
        s_bgsp_ref_scratch = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
        s_grp8split_ref_scratch = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
        s_gbt65k_ref_scratch = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
        s_screen_result_scratch = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
    } else {
        s_bg_line_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
        s_bg_pri_scratch  = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
        s_bg_flag_scratch = (uint8_t  *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint8_t), caps);
        s_bgsp_ref_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
        s_grp8split_ref_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
        s_gbt65k_ref_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
        s_screen_result_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
    }
    if (!s_bg_line_scratch || !s_bg_pri_scratch || !s_bg_flag_scratch ||
        !s_bgsp_ref_scratch || !s_grp8split_ref_scratch || !s_screen_result_scratch)
        return 0;

    /* Initial free ring is populated before CPU1 guest task exists. */
    for (uint32_t i = 0; i < s_slot_count; ++i)
        s_mb->free_ring[i] = (uint8_t)i;
    store_release(&s_mb->free_head, s_slot_count);
    store_release(&s_mb->free_tail, 0u);
    store_release(&s_mb->ready_head, 0u);
    store_release(&s_mb->ready_tail, 0u);

    cblend598g3_selfcheck();
    gbt614a_selfcheck();

    /* Build 6.14b2a: explicitly select P4 AXI-GDMA.  The generic installer
     * defaults to AHB-GDMA on this IDF, and the v5.4.2 AHB backend rejects
     * external PSRAM (the X68000 GVRAM allocation lives there).  AXI-GDMA
     * is the backend intended for external-memory traffic on ESP32-P4.
     * Backlog 16 covers two raw pair requests for every compose slot. */
#if TAB5_R57E49_ASYNC65K_ONLY
    s_gdma614b2 = NULL;
    s_gdma614b2_ready = 0;
#else
    async_memcpy_config_t gdmacfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    gdmacfg.backlog = 16;
    esp_err_t gdma_err = esp_async_memcpy_install_gdma_axi(&gdmacfg, &s_gdma614b2);
    s_gdma614b2_ready = (gdma_err == ESP_OK && s_gdma614b2 != NULL);
    if (s_gdma614b2_ready)
        ESP_LOGI(TAG, "PX68K_GDMA614B2A: P4 AXI-GDMA async-memcpy ready backlog=16; PSRAM raw GVRAM guarded by guest-write barrier");
    else
        ESP_LOGW(TAG, "PX68K_GDMA614B2A: AXI-GDMA install failed err=%d; exact 6.14b PIE snapshot fallback", (int)gdma_err);
#endif

#if !TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES
#if TAB5_R57E49_ASYNC65K_ONLY
    s_scroll614c_idx = NULL;
    s_scroll614c_meta = NULL;
    s_scroll614c_ready = 0;
#else
    s_scroll614c_idx = (uint8_t *)heap_caps_aligned_calloc(64, 512u * 512u, 1u,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scroll614c_meta = (scroll614c_meta_t *)heap_caps_aligned_calloc(64, 512u,
                                                                      sizeof(scroll614c_meta_t),
                                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scroll614c_ready = (s_scroll614c_idx != NULL && s_scroll614c_meta != NULL);
    if (s_scroll614c_ready)
        ESP_LOGI(TAG, "PX68K_SCROLL614C: 512x512 GRP8 palette-index scroll cache ready in PSRAM; scroll-only motion reuses decoded rows");
    else
        ESP_LOGW(TAG, "PX68K_SCROLL614C: cache allocation failed; 6.14b2a raw snapshot fallback retained");
#endif

    /* R57E50: the old 512-row decoded cache occupied 512 KiB and competed
     * with the queue/R57 shadows for the 128 KiB L2. A 64-row direct-map cache
     * keeps exact tags while shrinking the persistent PSRAM working set 8x. */
    s_gbt65k_rgb = (uint16_t *)heap_caps_aligned_calloc(
        64, TAB5_GBT65K_CACHE_ROWS * 512u, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_gbt65k_meta = (gbt65k_meta_t *)heap_caps_aligned_calloc(
        64, TAB5_GBT65K_CACHE_ROWS, sizeof(gbt65k_meta_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_gbt65k_ready = (s_gbt65k_rgb != NULL && s_gbt65k_meta != NULL);
    if (s_gbt65k_ready)
        ESP_LOGI(TAG, "PX68K_R57E50_CACHE: 65K decoded direct-map cache rows=%u bytes=%u in PSRAM; exact row/gen/palette tags",
                 (unsigned)TAB5_GBT65K_CACHE_ROWS,
                 (unsigned)(TAB5_GBT65K_CACHE_ROWS * 512u * sizeof(uint16_t)));
    else
        ESP_LOGW(TAG, "PX68K_GBT65K615E: cache allocation failed; 65K CPU0 path disabled for this boot");

    /* Build 6.15f: absorb the ~85 dirty-line bursts/frame measured on MDX
     * without consuming another ~1 MiB of scarce internal SRAM.  Average CPU0
     * service rate was already comfortably above producer rate; the 8-slot
     * queue was the synchronisation bottleneck, not steady-state throughput. */
    s_gbt65k_burst_slots = (compose_slot_t *)heap_caps_aligned_calloc(
        64, TAB5_GBT65K_BURST_SLOTS, sizeof(compose_slot_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_gbt65k_burst_ready = (s_gbt65k_burst_slots != NULL);
    if (s_gbt65k_burst_ready) {
        for (uint32_t i = 0; i < TAB5_GBT65K_BURST_SLOTS; ++i)
            s_gbt65k_free_ring[i] = (uint8_t)i;
        store_release(&s_gbt65k_free_head, TAB5_GBT65K_BURST_SLOTS);
        store_release(&s_gbt65k_free_tail, 0u);
        store_release(&s_gbt65k_ready_head, 0u);
        store_release(&s_gbt65k_ready_tail, 0u);
        ESP_LOGI(TAG,
                 "PX68K_GBT65K615G: PSRAM burst queue ready slots=%u bytes=%u; stale-frame discard + rotating raster admission armed",
                 (unsigned)TAB5_GBT65K_BURST_SLOTS,
                 (unsigned)(TAB5_GBT65K_BURST_SLOTS * sizeof(compose_slot_t)));
    } else {
        ESP_LOGW(TAG,
                 "PX68K_GBT65K615G: burst queue allocation failed; exact 6.15e 8-slot fallback retained");
    }
#else
    s_scroll614c_idx = NULL;
    s_scroll614c_meta = NULL;
    s_scroll614c_ready = 0;
    s_gbt65k_rgb = NULL;
    s_gbt65k_meta = NULL;
    s_gbt65k_ready = 0;
    s_gbt65k_burst_slots = NULL;
    s_gbt65k_burst_ready = 0;
    ESP_LOGI(TAG,
             "PX68K_COMPOSE_R57E33HF1: dormant CPU0 source caches QUARANTINED; reclaimed PSRAM ~= 1.3 MiB; exact-final CPU1 + NO-WAIT flow authoritative");
#endif
    if (!s_gbt65k_ref_scratch) {
        s_gbt65k_pie_selfcheck_state = 2;
        ESP_LOGW(TAG, "PX68K_GBT65K615F: PIE live-reference scratch unavailable; scalar final selector retained");
    }

#if portNUM_PROCESSORS > 1
    BaseType_t ok = xTaskCreatePinnedToCore(compose_task, "px68k_comp", 4096,
                                            NULL, 3, &s_task, 0);
#else
    BaseType_t ok = xTaskCreate(compose_task, "px68k_comp", 4096,
                                NULL, 3, &s_task);
#endif
    if (ok != pdPASS)
        return 0;

    tab5_raster590_selfcheck();
    s_ready = 1;
    ESP_LOGI(TAG,
             "Build 5.53a mailbox=%s bytes=%u fast_total=%u free_before=%u largest=%u; early-arena=%u used=%u spare=%u; packet slots=%u slot=%u pool=%u internal_now=%u largest_dma=%u",
             s_mailbox_tcm ? TAB5_FAST_MAILBOX_NAME : "L2-fallback",
             (unsigned)s_mailbox_bytes,
             (unsigned)fast_total, (unsigned)fast_before, (unsigned)fast_largest,
             (unsigned)s_arena_bytes, (unsigned)s_arena_used,
             (unsigned)(s_arena_bytes > s_arena_used ? s_arena_bytes - s_arena_used : 0u),
             (unsigned)s_slot_count, (unsigned)sizeof(compose_slot_t),
             (unsigned)s_pool_bytes, (unsigned)internal_before, (unsigned)largest_before);
    ESP_LOGI(TAG,
             "PX68K_R57E54: compositor prio=3 host-RT peer; R57 hold ends at RGB565 completion; Screen handoff is downstream/nonblocking");
#if TAB5_R57E49_ASYNC65K_ONLY
    ESP_LOGI(TAG,
             "PX68K_R57E54: FLOWPACE worker ACTIVE; R57E53 preflight retained; Screen slot pressure re-dirties on CPU1 without extending hold");
#endif
    ESP_LOGI(TAG,
             "PX68K_CPU1VID_R57E54: pressure skip + CPU0 Screen-drop retry both retain guest-owned dirty debt; no host wait on CPU1");
    return 1;
}

static int acquire_slot(uint8_t *idx_out)
{
    uint8_t idx;
    if (!s_ready || !free_pop(&idx)) {
        if (s_mb)
            __atomic_add_fetch(&s_mb->free_empty, 1u, __ATOMIC_RELAXED);
        ++s_queue_full;
        ++s_fallback;
        return 0;
    }
    *idx_out = idx;
    return 1;
}

static int queue_slot(uint8_t idx)
{
#if TAB5_NW3_QUARANTINE_UNUSED_SOURCE_CACHES
    /* R57E33HF1: no CPU0 compose queue exists in this build. */
    (void)idx;
    ++s_fallback;
    return 0;
#else
    const uint32_t pending_now = __atomic_add_fetch(&s_pending, 1u, __ATOMIC_ACQ_REL);
    uint32_t old_max = load_relaxed(&s_max_pending);
    while (pending_now > old_max &&
           !__atomic_compare_exchange_n(&s_max_pending, &old_max, pending_now, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }

    if (!ready_push(idx)) {
        __atomic_sub_fetch(&s_pending, 1u, __ATOMIC_RELEASE);
        (void)free_push(idx);
        ++s_queue_full;
        ++s_fallback;
        return 0;
    }

    ++s_submitted;
    __atomic_add_fetch(&s_mb->notify_count, 1u, __ATOMIC_RELAXED);
    xTaskNotifyGive(s_task);
    return 1;
#endif
}


int tab5_compose_submit_legacy_final(uint32_t y, uint32_t width,
                                     const uint16_t *grp,
                                     const uint16_t *grp_sp,
                                     const uint16_t *grp_sp2,
                                     const uint16_t *bg_text,
                                     const uint8_t *flags,
                                     uint8_t vc1_0, uint8_t vc2_0,
                                     int gon, int bgon, int ton, int tron, int pron,
                                     uint16_t half_mask, uint16_t ix2, uint16_t ibit,
                                     uint16_t *dst, uint64_t render_seq, uint32_t video_epoch, uint32_t visual_seq)
{
    if (!s_ready || !grp || !grp_sp || !grp_sp2 || !bg_text || !flags || !dst ||
        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH) {
        ++s_legacy_final_fallback;
        ++s_fallback;
        return 0;
    }
    uint8_t idx;
    if (!acquire_slot(&idx)) {
        ++s_legacy_final_fallback;
        return 0;
    }
    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = 0u;
    s_slot_flow_seq[idx] = render_seq;
    s_slot_flow_epoch[idx] = video_epoch;
    s_slot_flow_visual[idx] = visual_seq;
    slot->type = COMPOSE_JOB_LEGACY_FINAL;
    slot->y = y;
    slot->width = width;
    slot->dst = dst;
    slot->u.legacy.vc1_0 = vc1_0;
    slot->u.legacy.vc2_0 = vc2_0;
    slot->u.legacy.gon = gon ? 1u : 0u;
    slot->u.legacy.bgon = bgon ? 1u : 0u;
    slot->u.legacy.ton = ton ? 1u : 0u;
    slot->u.legacy.tron = tron ? 1u : 0u;
    slot->u.legacy.pron = pron ? 1u : 0u;
    slot->u.legacy.half_mask = half_mask;
    slot->u.legacy.ix2 = ix2;
    slot->u.legacy.ibit = ibit;

    const uint32_t px_bytes = width * (uint32_t)sizeof(uint16_t);
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    /* Copy only layers the exact legacy selector can observe on this line.
     * This keeps M1's CPU1 handoff cheaper than blindly copying the whole
     * 7.2 KiB worst-case packet. */
    if (gon)
        tab5_raster590_copy_run((uint8_t *)slot->u.legacy.grp,
                                (const uint8_t *)grp, px_bytes);
    if (tron || pron)
        tab5_raster590_copy_run((uint8_t *)slot->u.legacy.grp_sp,
                                (const uint8_t *)grp_sp, px_bytes);
    if (tron)
        tab5_raster590_copy_run((uint8_t *)slot->u.legacy.grp_sp2,
                                (const uint8_t *)grp_sp2, px_bytes);
    if (bgon || ton) {
        tab5_raster590_copy_run((uint8_t *)slot->u.legacy.bg_text,
                                (const uint8_t *)bg_text, px_bytes);
        memcpy(slot->u.legacy.flags, flags, width);
    }
    s_last_legacy_final_copy_us =
        cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    if (!queue_slot(idx)) {
        s_slot_render_ticket[idx] = 0u;
        s_slot_flow_seq[idx] = 0u;
        ++s_legacy_final_fallback;
        return 0;
    }
    ++s_legacy_final_submitted;
    return 1;
}

int tab5_compose_submit_line(uint32_t y, uint32_t width,
                             const uint16_t *bottom, const uint16_t *top,
                             uint16_t *dst, uint64_t render_seq,
                             uint32_t video_epoch, uint32_t visual_seq)
{
    if (!s_ready || !bottom || !top || !dst || width == 0u ||
        width > TAB5_COMPOSE_MAX_WIDTH) {
        ++s_fallback;
        return 0;
    }

    uint8_t idx;
    if (!acquire_slot(&idx))
        return 0;

    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = 0u;
    s_slot_flow_seq[idx] = render_seq;
    s_slot_flow_epoch[idx] = video_epoch;
    s_slot_flow_visual[idx] = visual_seq;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_BLEND;
    slot->y = y;
    slot->width = width;
    slot->dst = dst;
    /* Build 5.98g3: the common-line handoff used to spend CPU1 time in two
     * libc memcpy calls before CPU0 could blend. Reuse the already-proven
     * 5.91 PIE snapshot primitive here as well; alignment/size checks inside
     * tab5_raster590_copy_run retain memcpy as the exact fallback. */
    const uint32_t line_bytes = width * (uint32_t)sizeof(uint16_t);
    tab5_raster590_copy_run((uint8_t *)slot->u.blend.bottom,
                            (const uint8_t *)bottom, line_bytes);
    tab5_raster590_copy_run((uint8_t *)slot->u.blend.top,
                            (const uint8_t *)top, line_bytes);
    s_last_copy_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    return queue_slot(idx);
}

int tab5_compose_submit_grp8pair_line(uint32_t y, uint32_t width,
                                      const uint8_t *gvram,
                                      uint32_t y_lo_base, uint32_t y_hi_base,
                                      uint32_t x_lo, uint32_t x_hi,
                                      int bottom_page, int top_page,
                                      const uint16_t *palette,
                                      const uint16_t *bg, int bg_on_top,
                                      uint16_t *dst, uint64_t render_ticket)
{
    if (!s_ready || !gvram || !palette || !bg || !dst || width == 0u ||
        width > TAB5_COMPOSE_MAX_WIDTH || bottom_page == top_page) {
        ++s_fallback;
        return 0;
    }

    uint8_t idx;
    if (!acquire_slot(&idx))
        return 0;

    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = render_ticket;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GRP8PAIR;
    slot->bottom_page = (uint8_t)(bottom_page & 1);
    slot->top_page = (uint8_t)(top_page & 1);
    slot->bg_on_top = (uint8_t)(bg_on_top ? 1 : 0);
    slot->selfcheck = 0u;
    slot->y = y;
    slot->width = width;
    slot->dst = dst;

    snapshot_pair_lane(slot->u.grp8.src.pair.low_pair, gvram, y_lo_base, x_lo, width);
    snapshot_pair_lane(slot->u.grp8.src.pair.high_pair, gvram, y_hi_base, x_hi, width);
    memcpy(slot->u.grp8.pal, palette, sizeof(slot->u.grp8.pal));
    memcpy(slot->u.grp8.aux.bg, bg, (size_t)width * sizeof(uint16_t));
    s_last_grp8_copy_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    if (!queue_slot(idx))
        return 0;

    ++s_grp8_submitted;
    return 1;
}


int tab5_compose_submit_grp8pair_bgsp_line(uint32_t y, uint32_t width,
                                           const uint8_t *gvram,
                                           uint32_t y_lo_base, uint32_t y_hi_base,
                                           uint32_t x_lo, uint32_t x_hi,
                                           int bottom_page, int top_page,
                                           const uint16_t *grph_palette,
                                           const uint16_t *text_palette,
                                           const BG_HOST_LINE_STATE *bg_state,
                                           int bg_on_top, uint16_t *dst,
                                           uint64_t render_ticket)
{
    if (!s_ready || !gvram || !grph_palette || !text_palette || !bg_state || !dst ||
        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH || bottom_page == top_page ||
        load_acquire(&s_bgsp_selfcheck_state) == 3u) {
        ++s_fallback;
        return 0;
    }

    uint8_t idx;
    if (!acquire_slot(&idx))
        return 0;

    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = render_ticket;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GRP8PAIR_BGSP;
    slot->bottom_page = (uint8_t)(bottom_page & 1);
    slot->top_page = (uint8_t)(top_page & 1);
    slot->bg_on_top = (uint8_t)(bg_on_top ? 1 : 0);
    slot->selfcheck = 0u;
    slot->y = y;
    slot->width = width;
    slot->dst = dst;

    snapshot_pair_lane(slot->u.grp8.src.pair.low_pair, gvram, y_lo_base, x_lo, width);
    snapshot_pair_lane(slot->u.grp8.src.pair.high_pair, gvram, y_hi_base, x_hi, width);
    memcpy(slot->u.grp8.pal, grph_palette, sizeof(slot->u.grp8.pal));
    memcpy(slot->u.grp8.aux.host_bgsp.text_pal, text_palette,
           sizeof(slot->u.grp8.aux.host_bgsp.text_pal));
    memcpy(&slot->u.grp8.aux.host_bgsp.state, bg_state, sizeof(*bg_state));

    /* One-shot exactness guard. Generate one reference line with the original
     * PX68K BG_DrawLine() on CPU1, then compare it against the new CPU0 host
     * renderer. If any pixel differs, all later lines automatically fall back. */
    if (load_acquire(&s_bgsp_selfcheck_state) == 0u && s_bgsp_ref_scratch) {
        BG_DrawLine(1, bg_state->gd);
        memcpy(s_bgsp_ref_scratch, &BG_LineBuf[16], (size_t)width * sizeof(uint16_t));
        slot->selfcheck = 1u;
        store_release(&s_bgsp_selfcheck_state, 1u);
    }
    s_last_grp8_copy_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    /* Publish the live-source dependency before the ready descriptor. The guest
     * write-side barrier will not mutate BG/sprite source while this is nonzero. */
    __atomic_add_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
    if (!queue_slot(idx)) {
        __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
        if (slot->selfcheck)
            store_release(&s_bgsp_selfcheck_state, 0u);
        return 0;
    }

    ++s_bgsp_submitted;
    ++s_grp8_submitted;
    return 1;
}

int tab5_compose_submit_grp8split_line(uint32_t y, uint32_t width,
                                       const uint8_t *gvram,
                                       uint32_t by_lo_base, uint32_t by_hi_base,
                                       uint32_t ty_lo_base, uint32_t ty_hi_base,
                                       uint32_t bx_lo, uint32_t bx_hi,
                                       uint32_t tx_lo, uint32_t tx_hi,
                                       int bottom_page, int top_page,
                                       const uint16_t *palette,
                                       const uint16_t *bg, int bg_on_top,
                                       uint16_t *dst,
                                       const uint16_t *selfcheck_ref,
                                       uint64_t render_ticket)
{
    if (!s_ready || !gvram || !palette || !bg || !dst || width == 0u ||
        width > TAB5_COMPOSE_MAX_WIDTH || bottom_page == top_page ||
        load_acquire(&s_grp8split_selfcheck_state) == 3u) {
        ++s_fallback;
        return 0;
    }
    uint8_t idx;
    if (!acquire_slot(&idx))
        return 0;
    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = render_ticket;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GRP8SPLIT;
    slot->bottom_page = (uint8_t)(bottom_page & 1);
    slot->top_page = (uint8_t)(top_page & 1);
    slot->bg_on_top = (uint8_t)(bg_on_top ? 1 : 0);
    slot->selfcheck = 0u;
    slot->y = y; slot->width = width; slot->dst = dst;
    snapshot_pair_lane(slot->u.grp8.src.split.bottom_lo, gvram, by_lo_base, bx_lo, width);
    snapshot_pair_lane(slot->u.grp8.src.split.bottom_hi, gvram, by_hi_base, bx_hi, width);
    snapshot_pair_lane(slot->u.grp8.src.split.top_lo, gvram, ty_lo_base, tx_lo, width);
    snapshot_pair_lane(slot->u.grp8.src.split.top_hi, gvram, ty_hi_base, tx_hi, width);
    memcpy(slot->u.grp8.pal, palette, sizeof(slot->u.grp8.pal));
    memcpy(slot->u.grp8.aux.bg, bg, (size_t)width * sizeof(uint16_t));
    if (selfcheck_ref && s_grp8split_ref_scratch &&
        load_acquire(&s_grp8split_selfcheck_state) == 0u) {
        memcpy(s_grp8split_ref_scratch, selfcheck_ref, (size_t)width * sizeof(uint16_t));
        slot->selfcheck |= 2u;
        store_release(&s_grp8split_selfcheck_state, 1u);
    }
    s_last_grp8_copy_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
    if (!queue_slot(idx)) {
        if (slot->selfcheck & 2u) store_release(&s_grp8split_selfcheck_state, 0u);
        return 0;
    }
    ++s_grp8_submitted;
    return 1;
}

int tab5_compose_submit_grp8split_bgsp_line(uint32_t y, uint32_t width,
                                            const uint8_t *gvram,
                                            uint32_t by_lo_base, uint32_t by_hi_base,
                                            uint32_t ty_lo_base, uint32_t ty_hi_base,
                                            uint32_t bx_lo, uint32_t bx_hi,
                                            uint32_t tx_lo, uint32_t tx_hi,
                                            int bottom_page, int top_page,
                                            const uint16_t *grph_palette,
                                            const uint16_t *text_palette,
                                            const BG_HOST_LINE_STATE *bg_state,
                                            int bg_on_top, uint16_t *dst,
                                            const uint16_t *selfcheck_ref,
                                            uint64_t render_ticket)
{
    if (!s_ready || !gvram || !grph_palette || !text_palette || !bg_state || !dst ||
        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH || bottom_page == top_page ||
        load_acquire(&s_bgsp_selfcheck_state) == 3u ||
        load_acquire(&s_grp8split_selfcheck_state) == 3u) {
        ++s_fallback;
        return 0;
    }
    uint8_t idx;
    if (!acquire_slot(&idx))
        return 0;
    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = render_ticket;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GRP8SPLIT_BGSP;
    slot->bottom_page = (uint8_t)(bottom_page & 1);
    slot->top_page = (uint8_t)(top_page & 1);
    slot->bg_on_top = (uint8_t)(bg_on_top ? 1 : 0);
    slot->selfcheck = 0u;
    slot->y = y; slot->width = width; slot->dst = dst;
    snapshot_pair_lane(slot->u.grp8.src.split.bottom_lo, gvram, by_lo_base, bx_lo, width);
    snapshot_pair_lane(slot->u.grp8.src.split.bottom_hi, gvram, by_hi_base, bx_hi, width);
    snapshot_pair_lane(slot->u.grp8.src.split.top_lo, gvram, ty_lo_base, tx_lo, width);
    snapshot_pair_lane(slot->u.grp8.src.split.top_hi, gvram, ty_hi_base, tx_hi, width);
    memcpy(slot->u.grp8.pal, grph_palette, sizeof(slot->u.grp8.pal));
    memcpy(slot->u.grp8.aux.host_bgsp.text_pal, text_palette,
           sizeof(slot->u.grp8.aux.host_bgsp.text_pal));
    memcpy(&slot->u.grp8.aux.host_bgsp.state, bg_state, sizeof(*bg_state));
    if (load_acquire(&s_bgsp_selfcheck_state) == 0u && s_bgsp_ref_scratch) {
        BG_DrawLine(1, bg_state->gd);
        memcpy(s_bgsp_ref_scratch, &BG_LineBuf[16], (size_t)width * sizeof(uint16_t));
        slot->selfcheck |= 1u;
        store_release(&s_bgsp_selfcheck_state, 1u);
    }
    if (selfcheck_ref && s_grp8split_ref_scratch &&
        load_acquire(&s_grp8split_selfcheck_state) == 0u) {
        memcpy(s_grp8split_ref_scratch, selfcheck_ref, (size_t)width * sizeof(uint16_t));
        slot->selfcheck |= 2u;
        store_release(&s_grp8split_selfcheck_state, 1u);
    }
    s_last_grp8_copy_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
    __atomic_add_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
    if (!queue_slot(idx)) {
        __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
        if (slot->selfcheck & 1u) store_release(&s_bgsp_selfcheck_state, 0u);
        if (slot->selfcheck & 2u) store_release(&s_grp8split_selfcheck_state, 0u);
        return 0;
    }
    ++s_bgsp_submitted;
    ++s_grp8_submitted;
    return 1;
}

int tab5_compose_submit_gbt_line(uint32_t y, uint32_t width,
                                 const uint16_t *grp,
                                 const uint16_t *bg_text,
                                 const uint8_t *text_tr_flags,
                                 uint8_t grp_pri, uint8_t bg_pri,
                                 uint8_t text_pri, uint16_t *dst,
                                 uint64_t render_seq, uint32_t video_epoch,
                                 uint32_t visual_seq)
{
    if (!s_ready || !s_gbt614a_selfcheck_ok ||
        !grp || !bg_text || !text_tr_flags || !dst ||
        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH) {
        ++s_fallback;
        return 0;
    }

    uint8_t idx;
    if (!acquire_slot(&idx))
        return 0;

    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = 0u;
    s_slot_flow_seq[idx] = render_seq;
    s_slot_flow_epoch[idx] = video_epoch;
    s_slot_flow_visual[idx] = visual_seq;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GBT;
    slot->bottom_page = 0u;
    slot->top_page = 0u;
    slot->bg_on_top = 0u;
    slot->selfcheck = 0u;
    slot->grp_pri = (uint8_t)(grp_pri & 3u);
    slot->bg_pri = (uint8_t)(bg_pri & 3u);
    slot->text_pri = (uint8_t)(text_pri & 3u);
    slot->y = y;
    slot->width = width;
    slot->dst = dst;

    const uint32_t line_bytes = width * (uint32_t)sizeof(uint16_t);
    tab5_raster590_copy_run((uint8_t *)slot->u.gbt.grp,
                            (const uint8_t *)grp, line_bytes);
    tab5_raster590_copy_run((uint8_t *)slot->u.gbt.bg_text,
                            (const uint8_t *)bg_text, line_bytes);
    tab5_raster590_copy_run(slot->u.gbt.flags,
                            text_tr_flags, width);
    s_last_gbt_copy_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    if (!queue_slot(idx))
        return 0;

    ++s_gbt_submitted;
    return 1;
}


uint32_t tab5_compose_gbt65k_pending(void)
{
    return load_acquire(&s_gbt65k_burst_pending);
}

void tab5_compose_gbt65k_begin_frame(int render_enabled, int budget_mode)
{
    uint32_t e = __atomic_add_fetch(&s_gbt65k_frame_epoch, 1u, __ATOMIC_ACQ_REL);
    if (!e) store_release(&s_gbt65k_frame_epoch, 1u);
    uint32_t bands = (budget_mode >= 2) ? 8u : ((budget_mode == 1) ? 6u : 4u);
    store_release(&s_gbt65k_budget_mode, (uint32_t)(budget_mode < 0 ? 0 : budget_mode));
    store_release(&s_gbt65k_admit_bands, bands);
    if (render_enabled) __atomic_add_fetch(&s_gbt65k_render_seq, 1u, __ATOMIC_ACQ_REL);
}

int tab5_compose_gbt65k_line_admit(uint32_t y, uint32_t height, uint64_t render_seq)
{
    (void)height;
    /* R57E53 producer-side pressure preflight.  This runs immediately after
     * CPU1 line_begin, before BG capture/classification/packet construction.
     * Publish the newest sequence here so a pressure-skipped line still makes
     * older queued work for the same y stale.  The dedicated burst FIFO has
     * exactly one producer (CPU1), while CPU0 only returns free slots, so a
     * full pending count is sufficient to reject expensive preparation. */
    if (!s_gbt65k_burst_ready) return 1;
    if (y < 600u && render_seq)
        __atomic_store_n(&s_r57e49_latest_seq32[y], (uint32_t)render_seq, __ATOMIC_RELEASE);
    if (load_acquire(&s_gbt65k_burst_pending) >= TAB5_GBT65K_BURST_SLOTS) {
        ++s_gbt65k_window_skipped;
        return 0;
    }
    return 1;
}

int tab5_compose_gbt65k_hostbt_state(void)
{
    return (int)load_acquire(&s_r56s5_hostbt_state);
}

int tab5_compose_r57d_class_state(uint16_t key)
{
    if (!tab5_guest_bus_bg_shadow_ready() || !s_r57d_class_state || !s_r57d_class_epoch || key>=R57D_CLASS_COUNT) return 3;
    const uint16_t epoch=(uint16_t)tab5_guest_bus_tvram_epoch();
    if (__atomic_load_n(&s_r57d_class_epoch[key],__ATOMIC_ACQUIRE)!=epoch) {
        __atomic_store_n(&s_r57d_class_pass[key],0u,__ATOMIC_RELAXED);
        __atomic_store_n(&s_r57d_class_state[key],0u,__ATOMIC_RELEASE);
        return 0;
    }
    return (int)__atomic_load_n(&s_r57d_class_state[key],__ATOMIC_ACQUIRE);
}


int tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,
                                    const uint8_t *gvram,
                                    uint32_t gvram_row, uint32_t gvram_x,
                                    const uint8_t *pal_regs,
                                    uint32_t pal_generation, uint8_t contrast,
                                    const uint8_t *text_src, uint32_t text_valid,
                                    uint32_t text_x, uint32_t text_y, int shadow_text,
                                    const uint16_t *text_palette,
                                    const BG_HOST_LINE_STATE *bg_state,
                                    int bg_on, int text_on, int exact_host_bt,
                                    uint8_t grp_pri, uint8_t bg_pri,
                                    uint8_t text_pri, uint16_t *dst,
                                    uint32_t r57d_shadow_seq,
                                    uint64_t render_seq, uint32_t video_epoch,
                                    uint32_t visual_seq)
{
    const int burst65k = s_gbt65k_burst_ready;
    /* R57E52: burst production allocates its ordered R57 hold only after a
     * queue slot is reserved.  Therefore a zero incoming hold is valid on
     * shadow-TEXT burst submissions.  The legacy non-burst fallback still
     * requires the caller-supplied hold exactly as before. */
    if (!s_ready || !s_gbt65k_ready || !gvram || !pal_regs || !text_palette ||
        !dst || width == 0u || width > TAB5_COMPOSE_MAX_WIDTH ||
        (text_on && !text_src && !shadow_text) ||
        (shadow_text && (!text_on || (!r57d_shadow_seq && !burst65k) ||
                         (text_x&1023u)+width>1024u)) ||
        (bg_on && !bg_state) || text_valid > width ||
        (bg_on && load_acquire(&s_bgsp_selfcheck_state) == 3u)) {
        ++s_fallback;
        return -1;
    }
    if (!gbt65k_runtime_pal_selfcheck(contrast)) {
        ++s_fallback;
        return -1;
    }

    uint8_t idx;
    compose_slot_t *slot = NULL;

    /* R57E53: latest_seq was already published by the producer preflight
     * immediately after line_begin.  Do not repeat the atomic store here. */

    /* Queue pressure is visual pressure only. Never return expensive compose
     * work to CPU1; retain dirty debt and retry the newest state next frame. */
    if (burst65k) {
        if (!gbt65k_free_pop(&idx)) {
            ++s_gbt65k_burst_full;
            ++s_gbt65k_window_skipped;
            if (r57d_shadow_seq)
                tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq);
            if (s_task) xTaskNotifyGive(s_task);
            return 2;
        }
        slot = &s_gbt65k_burst_slots[idx];
    } else {
        if (!acquire_slot(&idx))
            return 0;
        slot = &s_slots[idx];
    }

    /* R57E51: allocate the ordered shadow freeze only AFTER a burst slot has
     * been reserved. R57E50 posted holds in WinDraw before queue capacity was
     * known, so a starved compositor could fill all 256 hold records even
     * though only 16 visual jobs existed. Queue pressure must never create
     * shadow pressure. */
    if (burst65k && !r57d_shadow_seq && (bg_on || shadow_text)) {
        r57d_shadow_seq = tab5_guest_bus_post_raster_hold(y);
        if (!r57d_shadow_seq) {
            (void)gbt65k_free_push(idx);
            ++s_gbt65k_window_skipped;
            if (s_task) xTaskNotifyGive(s_task);
            return 2;
        }
    }

    if (burst65k) {
        s_gbt65k_burst_render_ticket[idx] = 0u;
        s_gbt65k_burst_flow_seq[idx] = render_seq;
        s_gbt65k_burst_flow_epoch[idx] = video_epoch;
        s_gbt65k_burst_flow_visual[idx] = visual_seq;
    } else {
        s_slot_render_ticket[idx] = 0u;
        s_slot_flow_seq[idx] = render_seq;
        s_slot_flow_epoch[idx] = video_epoch;
        s_slot_flow_visual[idx] = visual_seq;
    }

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
#endif
    slot->type = COMPOSE_JOB_GBT65K;
    slot->bottom_page = 0u;
    slot->top_page = 0u;
    slot->bg_on_top = 0u;
    slot->selfcheck = (uint8_t)((text_on ? 1u : 0u) |
                                (bg_on ? 2u : 0u) |
                                (exact_host_bt ? 16u : 0u) |
                                (shadow_text ? 64u : 0u));
    slot->grp_pri = (uint8_t)(grp_pri & 3u);
    slot->bg_pri = (uint8_t)(bg_pri & 3u);
    slot->text_pri = (uint8_t)(text_pri & 3u);
    slot->y = y;
    slot->width = width;
    slot->dst = dst;
    slot->u.gbt65k.gvram_row = gvram_row & 511u;
    slot->u.gbt65k.gvram_x = gvram_x & 511u;
    /* R56: capture the complete 65K source row at the guest-time latch.
     * CPU1 is the sole guest writer, so no guest write can interleave this
     * call.  The generation is retained as cache identity/diagnostics only. */
    slot->u.gbt65k.row_generation =
        __atomic_load_n(&GVRAM_RowGeneration[slot->u.gbt65k.gvram_row],
                        __ATOMIC_ACQUIRE);
    tab5_raster590_copy_run(
        (uint8_t *)slot->u.gbt65k.raw_row,
        gvram + (slot->u.gbt65k.gvram_row << 10),
        (uint32_t)sizeof(slot->u.gbt65k.raw_row));
    slot->u.gbt65k.pal_generation = pal_generation;
    slot->u.gbt65k.frame_epoch = load_acquire(&s_gbt65k_frame_epoch);
    slot->u.gbt65k.contrast = (uint8_t)(contrast & 15u);
    slot->u.gbt65k.text_x=(uint16_t)(text_x&1023u);
    slot->u.gbt65k.text_y=(uint16_t)(text_y&1023u);

    tab5_raster590_copy_run(slot->u.gbt65k.pal_regs, pal_regs,
                            (uint32_t)sizeof(slot->u.gbt65k.pal_regs));
    tab5_raster590_copy_run((uint8_t *)slot->u.gbt65k.text_pal,
                            (const uint8_t *)text_palette,
                            (uint32_t)sizeof(slot->u.gbt65k.text_pal));
    memset(slot->u.gbt65k.text_idx, 0, width);
    if (text_on && !shadow_text && text_valid)
        tab5_raster590_copy_run(slot->u.gbt65k.text_idx, text_src, text_valid);
    if (bg_on) {
        memcpy(&slot->u.gbt65k.bg_state, bg_state, sizeof(*bg_state));
        if (!exact_host_bt && load_acquire(&s_bgsp_selfcheck_state) == 0u && s_bgsp_ref_scratch) {
            BG_DrawLine(1, bg_state->gd);
            memcpy(s_bgsp_ref_scratch, &BG_LineBuf[16],
                   (size_t)width * sizeof(uint16_t));
            slot->selfcheck |= 4u;
            store_release(&s_bgsp_selfcheck_state, 1u);
        }
    }

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_last_gbt65k_build_us =
        cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);
#endif

    /* GVRAM is already immutable inside the packet; no guest GVRAM writer
     * barrier is needed for this 65K request. BG/Sprite retains its existing
     * source barrier until that source family is packetized separately. */
    if (bg_on && !r57d_shadow_seq)
        __atomic_add_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);

    /* PX68K_R57D3_HOLD_BIND: bind selected slot immediately before publication. */

    if (burst65k) s_r57d_burst_hold[idx]=r57d_shadow_seq;

    else s_r57d_slot_hold[idx]=r57d_shadow_seq;

    if (burst65k) {
        const uint32_t pending_now =
            __atomic_add_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_ACQ_REL);
        uint32_t old_max = load_relaxed(&s_gbt65k_burst_max_pending);
        while (pending_now > old_max &&
               !__atomic_compare_exchange_n(&s_gbt65k_burst_max_pending,
                                            &old_max, pending_now, 0,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
        }
        if (!gbt65k_ready_push(idx)) {
            __atomic_sub_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_RELEASE);
            if (bg_on && !r57d_shadow_seq)
                __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
            if (slot->selfcheck & 4u)
                store_release(&s_bgsp_selfcheck_state, 0u);
            if (r57d_shadow_seq)
                tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq);
            s_r57d_burst_hold[idx] = 0u;
            s_gbt65k_burst_flow_seq[idx] = 0u;
            s_gbt65k_burst_flow_epoch[idx] = 0u;
            s_gbt65k_burst_flow_visual[idx] = 0u;
            (void)gbt65k_free_push(idx);
            ++s_gbt65k_burst_full;
            ++s_gbt65k_window_skipped;
            return 2;
        }
        ++s_gbt65k_burst_notify;
        ++s_gbt65k_submitted;
        __atomic_add_fetch(&s_mb->notify_count, 1u, __ATOMIC_RELAXED);
        xTaskNotifyGive(s_task);
        return 1;
    }

    if (!queue_slot(idx)) {
        if (bg_on && !r57d_shadow_seq)
            __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);
        if (slot->selfcheck & 4u)
            store_release(&s_bgsp_selfcheck_state, 0u);
        s_r57d_slot_hold[idx] = 0u;
        return 0;
    }

    ++s_gbt65k_submitted;
    return 1;
}

int tab5_compose_submit_gbt65k_exact_bt_line(uint32_t y, uint32_t width,
                                             const uint8_t *gvram,
                                             uint32_t gvram_row, uint32_t gvram_x,
                                             const uint8_t *pal_regs,
                                             uint32_t pal_generation, uint8_t contrast,
                                             const uint16_t *bg_text,
                                             const uint8_t *text_tr_flags,
                                             const uint8_t *text_src, uint32_t text_valid,
                                             uint32_t text_x, uint32_t text_y,
                                             const uint16_t *text_palette,
                                             const BG_HOST_LINE_STATE *bg_state,
                                             int bg_on, int text_on,
                                             uint8_t grp_pri, uint8_t bg_pri,
                                             uint8_t text_pri, uint16_t *dst,
                                             uint16_t r57d_class, uint32_t r57d_shadow_seq,
                                             uint64_t render_ticket)
{
    if (!s_ready || !s_gbt65k_ready || !gvram || !pal_regs ||
        !bg_text || !text_tr_flags || !dst ||
        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH ||
        (text_on && (!text_src || !text_palette || text_valid > width)) ||
        (bg_on && !bg_state)) {
        ++s_fallback;
        return -1;
    }
    if (!gbt65k_runtime_pal_selfcheck(contrast)) {
        ++s_fallback;
        return -1;
    }

    /* R57E5 per-class certification. Validation and production use the same
     * burst FIFO so ordered shadow holds cannot overtake one another. A TEXT
     * shadow mismatch is repaired on CPU0 and restarts certification; it never
     * creates a permanent CPU1 fallback state. */
    const int r57d_validate = s_gbt65k_burst_ready && text_on && text_src &&
        r57d_class < R57D_CLASS_COUNT && s_r57d_class_state &&
        tab5_compose_r57d_class_state(r57d_class) == 0 &&
        r57d_shadow_seq != 0u;
    const int burst65k = s_gbt65k_burst_ready;

    uint8_t idx;
    compose_slot_t *slot = NULL;
    if (burst65k) {
        if (!gbt65k_free_pop(&idx)) {
            /* R57E56: exact-BT callers may have already published an ordered
             * raster hold before queue capacity is known.  A rejected visual
             * job must never leave that hold orphaned. */
            if (r57d_shadow_seq)
                tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq);
            ++s_gbt65k_burst_full;
            return 0;
        }
        slot = &s_gbt65k_burst_slots[idx];
        s_gbt65k_burst_render_ticket[idx] = render_ticket;
    } else {
        if (!acquire_slot(&idx)) {
            if (r57d_shadow_seq)
                tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq);
            return 0;
        }
        slot = &s_slots[idx];
        s_slot_render_ticket[idx] = render_ticket;
    }

    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GBT65K;
    slot->bottom_page = (uint8_t)(r57d_class & 0xffu);
    slot->top_page = (uint8_t)((r57d_class >> 8) & 3u);
    slot->bg_on_top = 0u;
    slot->selfcheck = (uint8_t)(8u | (r57d_validate ? 32u : 0u) |
                                (r57d_validate && text_on ? 1u : 0u) |
                                (r57d_validate && bg_on ? 2u : 0u));
    slot->grp_pri = (uint8_t)(grp_pri & 3u);
    slot->bg_pri = (uint8_t)(bg_pri & 3u);
    slot->text_pri = (uint8_t)(text_pri & 3u);
    slot->y = y;
    slot->width = width;
    slot->dst = dst;
    slot->u.gbt65k.gvram_row = gvram_row & 511u;
    slot->u.gbt65k.gvram_x = gvram_x & 511u;
    slot->u.gbt65k.row_generation =
        __atomic_load_n(&GVRAM_RowGeneration[slot->u.gbt65k.gvram_row],
                        __ATOMIC_ACQUIRE);
    tab5_raster590_copy_run((uint8_t *)slot->u.gbt65k.raw_row,
                            gvram + (slot->u.gbt65k.gvram_row << 10),
                            (uint32_t)sizeof(slot->u.gbt65k.raw_row));
    slot->u.gbt65k.pal_generation = pal_generation;
    slot->u.gbt65k.frame_epoch = load_acquire(&s_gbt65k_frame_epoch);
    slot->u.gbt65k.contrast = (uint8_t)(contrast & 15u);
    slot->u.gbt65k.text_x=(uint16_t)(text_x&1023u);
    slot->u.gbt65k.text_y=(uint16_t)(text_y&1023u);
    tab5_raster590_copy_run(slot->u.gbt65k.pal_regs, pal_regs,
                            (uint32_t)sizeof(slot->u.gbt65k.pal_regs));
    tab5_raster590_copy_run((uint8_t *)slot->u.gbt65k.exact_bg_text,
                            (const uint8_t *)bg_text,
                            width * (uint32_t)sizeof(uint16_t));
    tab5_raster590_copy_run(slot->u.gbt65k.exact_flags,
                            text_tr_flags, width);

    if (r57d_validate) {
        tab5_raster590_copy_run((uint8_t *)slot->u.gbt65k.text_pal,
                                (const uint8_t *)text_palette,
                                (uint32_t)sizeof(slot->u.gbt65k.text_pal));
        memset(slot->u.gbt65k.text_idx, 0, width);
        if (text_on && text_valid)
            tab5_raster590_copy_run(slot->u.gbt65k.text_idx, text_src, text_valid);
        if (bg_on)
            memcpy(&slot->u.gbt65k.bg_state, bg_state, sizeof(*bg_state));
    }

    s_last_gbt65k_build_us =
        cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    /* Binding is slot-local and occurs only after acquisition/capture has
     * succeeded, immediately before queue publication. */
    if (burst65k)
        s_r57d_burst_hold[idx] = r57d_shadow_seq;
    else
        s_r57d_slot_hold[idx] = r57d_shadow_seq;

    if (burst65k) {
        const uint32_t pending_now =
            __atomic_add_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_ACQ_REL);
        uint32_t old_max = load_relaxed(&s_gbt65k_burst_max_pending);
        while (pending_now > old_max &&
               !__atomic_compare_exchange_n(&s_gbt65k_burst_max_pending,
                                            &old_max, pending_now, 0,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
        }
        if (!gbt65k_ready_push(idx)) {
            __atomic_sub_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_RELEASE);
            if (r57d_shadow_seq)
                tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq);
            s_r57d_burst_hold[idx] = 0u;
            (void)gbt65k_free_push(idx);
            ++s_gbt65k_burst_full;
            return 0;
        }
        ++s_gbt65k_burst_notify;
        ++s_gbt65k_submitted;
        __atomic_add_fetch(&s_mb->notify_count, 1u, __ATOMIC_RELAXED);
        xTaskNotifyGive(s_task);
        return 1;
    }

    if (!queue_slot(idx)) {
        if (r57d_shadow_seq)
            tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq);
        s_r57d_slot_hold[idx] = 0u;
        return 0;
    }

    ++s_gbt65k_submitted;
    return 1;
}

int tab5_compose_submit_gbt_scrollcache_line(uint32_t y, uint32_t width,
                                              const uint8_t *gvram,
                                              uint32_t y_lo_base, uint32_t y_hi_base,
                                              uint32_t x_lo, uint32_t x_hi,
                                              int bottom_page, int top_page,
                                              const uint16_t *palette,
                                              const uint16_t *bg_text,
                                              const uint8_t *text_tr_flags,
                                              uint8_t grp_pri, uint8_t bg_pri,
                                              uint8_t text_pri, uint16_t *dst,
                                              uint64_t render_ticket)
{
    (void)y; (void)width; (void)gvram;
    (void)y_lo_base; (void)y_hi_base; (void)x_lo; (void)x_hi;
    (void)bottom_page; (void)top_page; (void)palette; (void)bg_text;
    (void)text_tr_flags; (void)grp_pri; (void)bg_pri; (void)text_pri;
    (void)dst; (void)render_ticket;
    static int reported = 0;
    if (!reported) {
        reported = 1;
        printf("PX68K_SCREEN_R56: shared-GVRAM GRP8 scroll-cache job disabled; immutable rawpair snapshot fallback authoritative\n");
    }
    return 0;
}

int tab5_compose_submit_gbt_rawpair_line(uint32_t y, uint32_t width,
                                         const uint8_t *gvram,
                                         uint32_t y_lo_base, uint32_t y_hi_base,
                                         uint32_t x_lo, uint32_t x_hi,
                                         int bottom_page, int top_page,
                                         const uint16_t *palette,
                                         const uint16_t *bg_text,
                                         const uint8_t *text_tr_flags,
                                         uint8_t grp_pri, uint8_t bg_pri,
                                         uint8_t text_pri, uint16_t *dst,
                                         uint64_t render_ticket)
{
    if (!s_ready || !s_gbt614a_selfcheck_ok ||
        !gvram || !palette || !bg_text || !text_tr_flags || !dst ||
        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH ||
        ((bottom_page ^ top_page) & 1) == 0) {
        ++s_fallback;
        return 0;
    }

    uint8_t idx;
    if (!acquire_slot(&idx))
        return 0;

    compose_slot_t *slot = &s_slots[idx];
    s_slot_render_ticket[idx] = render_ticket;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GBT_RAWPAIR;
    slot->bottom_page = (uint8_t)(bottom_page & 1);
    slot->top_page = (uint8_t)(top_page & 1);
    slot->bg_on_top = 0u;
    slot->selfcheck = 0u;
    slot->grp_pri = (uint8_t)(grp_pri & 3u);
    slot->bg_pri = (uint8_t)(bg_pri & 3u);
    slot->text_pri = (uint8_t)(text_pri & 3u);
    slot->y = y;
    slot->width = width;
    slot->dst = dst;

    /* R56 timeline contract: snapshot both packed GVRAM lanes synchronously
     * while the guest line is latched.  Do not DMA from mutable guest GVRAM
     * after returning to the emulator and do not make guest writes wait for
     * host completion.  CPU0 may finish this request later in any order. */
    snapshot_pair_lane(slot->u.gbt_raw.low_pair, gvram, y_lo_base, x_lo, width);
    snapshot_pair_lane(slot->u.gbt_raw.high_pair, gvram, y_hi_base, x_hi, width);
    memcpy(slot->u.gbt_raw.pal, palette, sizeof(slot->u.gbt_raw.pal));
    tab5_raster590_copy_run((uint8_t *)slot->u.gbt_raw.bg_text,
                            (const uint8_t *)bg_text,
                            width * (uint32_t)sizeof(uint16_t));
    tab5_raster590_copy_run(slot->u.gbt_raw.flags, text_tr_flags, width);
    s_last_gbt_raw_copy_us = cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    if (!queue_slot(idx))
        return 0;
    ++s_gbt_raw_submitted;
    ++s_gbt_submitted;
    return 1;
}

int tab5_compose_grp8split_needs_selfcheck(void)
{
    return load_acquire(&s_grp8split_selfcheck_state) == 0u;
}

void tab5_compose_guest_gvram_barrier(void)
{
    /* BAT177NW0 CPU1 NO-WAIT contract.  Production paths that would read
     * mutable guest GVRAM directly are quarantined.  Keep the ABI for old
     * callers, but never turn a guest write into a CPU0-completion wait. */
    ++s_gvram_barrier_calls;
}

void tab5_compose_guest_bg_barrier(void)
{
    /* Same contract for BG/Sprite source memory: CPU0 work must own an
     * immutable snapshot before CPU1 continues. */
    ++s_bg_barrier_calls;
}

void tab5_compose_wait_idle(void)
{
    if (!s_ready || load_acquire(&s_pending) == 0u)
        return;

    ++s_frame_waits;
    /* Build 6.15f: normal frames keep the asynchronous 65K burst queue out of
     * this common frame-tail wait so CPU1 is not recoupled to CPU0.  R51 keeps
     * that production behavior unchanged; source transitions use the explicit
     * tab5_compose_wait_source_frame_idle() barrier below. */
    while (load_acquire(&s_pending) != 0u)
        taskYIELD();
}


/* Legacy R51 ABI helper retained for older callers outside the R56 managed
 * lifecycle.  R56 Screen Manager never invokes it: guest time is not stopped
 * for host rendering, and correctness comes from immutable request snapshots
 * plus opaque-ticket validation instead of a source-frame drain barrier. */
uint32_t tab5_compose_wait_source_frame_idle(void)
{
    if (!s_ready)
        return 0u;

    const uint32_t burst_on_entry = load_acquire(&s_gbt65k_burst_pending);

    while ((load_acquire(&s_pending) |
            load_acquire(&s_gbt65k_burst_pending)) != 0u)
        taskYIELD();

    return burst_on_entry;
}


void tab5_compose_get_stats(tab5_compose_stats_t *out)
{
    if (!out)
        return;
    out->submitted_lines = s_submitted;
    out->completed_lines = s_completed;
    out->fallback_lines = s_fallback;
    out->queue_full = s_queue_full;
    out->frame_waits = s_frame_waits;
    out->max_pending = s_max_pending;
    out->last_copy_us = s_last_copy_us;
    out->last_blend_us = s_last_blend_us;
    out->grp8_submitted_lines = s_grp8_submitted;
    out->grp8_completed_lines = s_grp8_completed;
    out->last_grp8_copy_us = s_last_grp8_copy_us;
    out->last_grp8_render_us = s_last_grp8_render_us;
    out->slot_count = s_slot_count;
    out->slot_bytes = (uint32_t)sizeof(compose_slot_t);
    out->pool_bytes = s_pool_bytes;
    out->mailbox_bytes = s_mailbox_bytes;
    out->mailbox_tcm = (uint32_t)(s_mailbox_tcm ? 1 : 0);
    out->notify_count = s_mb ? load_relaxed(&s_mb->notify_count) : 0u;
    out->ready_empty = s_mb ? load_relaxed(&s_mb->ready_empty) : 0u;
    out->free_empty = s_mb ? load_relaxed(&s_mb->free_empty) : 0u;
    out->arena_bytes = s_arena_bytes;
    out->arena_used = s_arena_used;
    out->arena_spare = s_arena_bytes > s_arena_used ? s_arena_bytes - s_arena_used : 0u;
    out->bgsp_submitted_lines = s_bgsp_submitted;
    out->bgsp_completed_lines = s_bgsp_completed;
    out->last_bgsp_render_us = s_last_bgsp_render_us;
    out->gbt_submitted_lines = s_gbt_submitted;
    out->gbt_completed_lines = s_gbt_completed;
    out->last_gbt_copy_us = s_last_gbt_copy_us;
    out->last_gbt_render_us = s_last_gbt_render_us;
    out->gbt_raw_submitted_lines = s_gbt_raw_submitted;
    out->gbt_raw_completed_lines = s_gbt_raw_completed;
    out->last_gbt_raw_copy_us = s_last_gbt_raw_copy_us;
    out->last_gbt_raw_render_us = s_last_gbt_raw_render_us;
    out->gbt_raw_dma_lines = s_gdma614b2_lines;
    out->gbt_raw_dma_reqs = s_gdma614b2_reqs;
    out->gbt_raw_dma_submit_fail = s_gdma614b2_submit_fail;
    out->last_gbt_raw_dma_us = s_last_gdma614b2_us;
    out->scroll_cache_submitted_lines = s_scroll614c_submitted;
    out->scroll_cache_completed_lines = s_scroll614c_completed;
    out->scroll_cache_hits = s_scroll614c_hits;
    out->scroll_cache_misses = s_scroll614c_misses;
    out->scroll_cache_rebuilds = s_scroll614c_rebuilds;
    out->last_scroll_cache_build_us = s_last_scroll614c_build_us;
    out->last_scroll_cache_render_us = s_last_scroll614c_render_us;
    out->gbt65k_submitted_lines = s_gbt65k_submitted;
    out->gbt65k_completed_lines = s_gbt65k_completed;
    out->gbt65k_cache_hits = s_gbt65k_hits;
    out->gbt65k_cache_misses = s_gbt65k_misses;
    out->gbt65k_cache_rebuilds = s_gbt65k_rebuilds;
    out->last_gbt65k_build_us = s_last_gbt65k_build_us;
    out->last_gbt65k_render_us = s_last_gbt65k_render_us;
    out->gbt65k_burst_pending = load_acquire(&s_gbt65k_burst_pending);
    out->gbt65k_burst_max_pending = load_relaxed(&s_gbt65k_burst_max_pending);
    out->gbt65k_burst_full = load_relaxed(&s_gbt65k_burst_full);
    out->gbt65k_burst_slots = s_gbt65k_burst_ready ? TAB5_GBT65K_BURST_SLOTS : 0u;
    out->gbt65k_pie_lines = load_relaxed(&s_gbt65k_pie_lines);
    out->gbt65k_scalar_lines = load_relaxed(&s_gbt65k_scalar_lines);
    out->gbt65k_stale_dropped = load_relaxed(&s_gbt65k_stale_dropped);
    out->gbt65k_window_skipped = load_relaxed(&s_gbt65k_window_skipped);
    out->gbt65k_frame_epoch = load_relaxed(&s_gbt65k_frame_epoch);
    out->gbt65k_render_seq = load_relaxed(&s_gbt65k_render_seq);
    out->gbt65k_budget_mode = load_relaxed(&s_gbt65k_budget_mode);
    out->gbt65k_admit_bands = load_relaxed(&s_gbt65k_admit_bands);
    out->gbt65k_cpu0_work_us = load_relaxed(&s_gbt65k_cpu0_work_us);
    out->gvram_barrier_calls = s_gvram_barrier_calls;
    out->gvram_barrier_waits = s_gvram_barrier_waits;
    out->gvram_barrier_us = s_gvram_barrier_us;
    out->bg_barrier_calls = s_bg_barrier_calls;
    out->bg_barrier_waits = s_bg_barrier_waits;
    out->bg_barrier_us = s_bg_barrier_us;
    out->p4_blend_scalar_calls = 0;
    out->p4_blend_pie_calls = 0;
    out->p4_blend_scalar_pixels = 0;
    out->p4_blend_pie_pixels = 0;
    out->p4_blend_align_fallbacks = 0;
    out->p4_blend_failures = 0;
    out->p4_blend_backend0 = s_cblend598g3_selfcheck_ok ? 2u : 1u;
    out->p4_blend_backend1 = out->p4_blend_backend0;
    out->p4_blend_backend2 = out->p4_blend_backend0;
    out->legacy_final_submitted_lines = load_relaxed(&s_legacy_final_submitted);
    out->legacy_final_completed_lines = load_relaxed(&s_legacy_final_completed);
    out->legacy_final_fallback_lines = load_relaxed(&s_legacy_final_fallback);
    out->last_legacy_final_copy_us = load_relaxed(&s_last_legacy_final_copy_us);
    out->last_legacy_final_render_us = load_relaxed(&s_last_legacy_final_render_us);
}

uint32_t tab5_compose_stack_highwater(void)
{
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0u;
}
