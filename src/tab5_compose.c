/*
 * Tab5 port-specific implementation.
 * Intent: CPU0 compositor for Tab5: consume compact CPU1 snapshots and use validated PIE/scalar paths without making CPU1 repeat final host-side blending work.
 * Layer8 Aug/17/2026
 */
#include "tab5_compose.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tab5_video.h"

/* Shared PX68K graphics hot-set. These live in application internal SRAM. */
extern uint8_t BG[0x8000];
extern uint8_t BGCHR8[8 * 8 * 256];
extern uint8_t BGCHR16[16 * 16 * 256];
extern uint8_t Sprite_Regs[0x800];
extern uint16_t BG_LineBuf[1600];
extern void BG_DrawLine(int opaq, int gd);

#define TAB5_COMPOSE_MAX_SLOTS 16u
#define TAB5_COMPOSE_MIN_SLOTS 8u
#define TAB5_COMPOSE_MAX_WIDTH 800u
#define TAB5_CPU_MHZ 360u
#define TAB5_BG_SCRATCH_PIXELS (TAB5_COMPOSE_MAX_WIDTH + 32u)
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

typedef enum {
    COMPOSE_JOB_BLEND = 0,
    COMPOSE_JOB_GRP8PAIR = 1,
    COMPOSE_JOB_GRP8PAIR_BGSP = 2,
    COMPOSE_JOB_GRP8SPLIT = 3,
    COMPOSE_JOB_GRP8SPLIT_BGSP = 4,
} compose_job_type_t;

typedef struct {
    uint8_t type;
    uint8_t bottom_page;
    uint8_t top_page;
    uint8_t bg_on_top;
    uint8_t selfcheck;
    uint8_t _pad0[3];
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
    } u __attribute__((aligned(16)));
} compose_slot_t;

_Static_assert((offsetof(compose_slot_t, u) & 15u) == 0u,
               "Build 5.89 raster snapshot payload must be 16-byte aligned");

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
static compose_mailbox_t *s_mb;
static TaskHandle_t s_task;
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
static volatile uint32_t s_bg_barrier_calls;
static volatile uint32_t s_bg_barrier_waits;
static volatile uint32_t s_bg_barrier_us;
/* 0=not tried, 1=reference pending, 2=verified, 3=failed/disable host BGSP. */
static volatile uint32_t s_bgsp_selfcheck_state;
/* 0=not tried, 1=reference pending, 2=verified, 3=failed/disable split-scroll host GRP8. */
static volatile uint32_t s_grp8split_selfcheck_state;
static int s_ready;

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
        const uint8_t *mp = &BG[ecx + edx];
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
            src = &BGCHR8[si + ebp]; step = 1;
        } else if ((uint8_t)(map - 0x40u) & 0x80u) {
            src = &BGCHR8[si + 0x3fu - ebp]; step = -1;
        } else if ((int8_t)map >= 0x40) {
            src = &BGCHR8[si + ebp + 7u]; step = -1;
        } else {
            src = &BGCHR8[si + 0x38u - ebp]; step = 1;
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
        const uint8_t *mp = &BG[ecx + edx];
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
            src = &BGCHR16[si + ebp]; step = 1;
        } else if ((uint8_t)(map - 0x40u) & 0x80u) {
            src = &BGCHR16[si + 0xffu - ebp]; step = -1;
        } else if ((int8_t)map >= 0x40) {
            src = &BGCHR16[si + ebp + 15u]; step = -1;
        } else {
            src = &BGCHR16[si + 0xf0u - ebp]; step = 1;
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
    const host_sprite_ctrl_t *sct = (const host_sprite_ctrl_t *)Sprite_Regs;
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
            src = &BGCHR16[(((uint32_t)ctrl * 256u) & 0xffffu) + y * 16u];
            step = 1;
        } else if ((uint16_t)(ctrl - 0x4000u) & 0x8000u) {
            src = &BGCHR16[(((uint32_t)ctrl * 256u) & 0xffffu)
                         + (((y * 16u) & 0xffu) ^ 0xf0u) + 15u];
            step = -1;
        } else if ((int16_t)ctrl >= 0x4000) {
            src = &BGCHR16[(((uint32_t)ctrl * 256u) & 0xffffu) + y * 16u + 15u];
            step = -1;
        } else {
            src = &BGCHR16[(((uint32_t)ctrl << 8) & 0xffffu)
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

/* CPU1 producer -> CPU0 consumer. */
static int ready_push(uint8_t idx)
{
    const uint32_t head = load_relaxed(&s_mb->ready_head);
    const uint32_t tail = load_acquire(&s_mb->ready_tail);
    if ((uint32_t)(head - tail) >= s_slot_count)
        return 0;
    s_mb->ready_ring[slot_ring_index(head)] = idx;
    store_release(&s_mb->ready_head, head + 1u);
    __atomic_add_fetch(&s_mb->submit_seq, 1u, __ATOMIC_RELAXED);
    return 1;
}

static int ready_pop(uint8_t *idx)
{
    const uint32_t tail = load_relaxed(&s_mb->ready_tail);
    const uint32_t head = load_acquire(&s_mb->ready_head);
    if (tail == head)
        return 0;
    *idx = s_mb->ready_ring[slot_ring_index(tail)];
    store_release(&s_mb->ready_tail, tail + 1u);
    return 1;
}

/* CPU0 producer -> CPU1 consumer. */
static int free_push(uint8_t idx)
{
    const uint32_t head = load_relaxed(&s_mb->free_head);
    const uint32_t tail = load_acquire(&s_mb->free_tail);
    if ((uint32_t)(head - tail) >= s_slot_count)
        return 0;
    s_mb->free_ring[slot_ring_index(head)] = idx;
    store_release(&s_mb->free_head, head + 1u);
    return 1;
}

static int free_pop(uint8_t *idx)
{
    const uint32_t tail = load_relaxed(&s_mb->free_tail);
    const uint32_t head = load_acquire(&s_mb->free_head);
    if (tail == head)
        return 0;
    *idx = s_mb->free_ring[slot_ring_index(tail)];
    store_release(&s_mb->free_tail, tail + 1u);
    return 1;
}

static void compose_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t idx;
        if (!ready_pop(&idx)) {
            __atomic_add_fetch(&s_mb->ready_empty, 1u, __ATOMIC_RELAXED);
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        compose_slot_t *slot = &s_slots[idx];
        const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
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

        tab5_video_fb_line_write_end(slot->y);
        ++s_completed;
        __atomic_add_fetch(&s_mb->done_seq, 1u, __ATOMIC_RELEASE);

        if (!free_push(idx)) {
            /* This should be impossible: each consumed ready slot creates at
             * most one returned free slot.  Count it as a queue fault rather
             * than blocking the host core. */
            ++s_queue_full;
        }
        __atomic_sub_fetch(&s_pending, 1u, __ATOMIC_RELEASE);
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

    s_slots = alloc_slot_pool(&s_slot_count);
    if (!s_slots || s_slot_count < TAB5_COMPOSE_MIN_SLOTS)
        return 0;
    s_pool_bytes = (uint32_t)((size_t)s_slot_count * sizeof(compose_slot_t));
    s_slot_mask = ((s_slot_count & (s_slot_count - 1u)) == 0u) ? (s_slot_count - 1u) : 0u;

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    if (s_arena) {
        s_bg_line_scratch = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
        s_bg_pri_scratch  = (uint16_t *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint16_t), 64u);
        s_bg_flag_scratch = (uint8_t  *)arena_alloc(TAB5_BG_SCRATCH_PIXELS * sizeof(uint8_t), 64u);
        s_bgsp_ref_scratch = (uint16_t *)arena_alloc(TAB5_COMPOSE_MAX_WIDTH * sizeof(uint16_t), 64u);
        s_grp8split_ref_scratch = (uint16_t *)arena_alloc(TAB5_COMPOSE_MAX_WIDTH * sizeof(uint16_t), 64u);
    } else {
        s_bg_line_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
        s_bg_pri_scratch  = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint16_t), caps);
        s_bg_flag_scratch = (uint8_t  *)heap_caps_aligned_calloc(64, TAB5_BG_SCRATCH_PIXELS, sizeof(uint8_t), caps);
        s_bgsp_ref_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_COMPOSE_MAX_WIDTH, sizeof(uint16_t), caps);
        s_grp8split_ref_scratch = (uint16_t *)heap_caps_aligned_calloc(64, TAB5_COMPOSE_MAX_WIDTH, sizeof(uint16_t), caps);
    }
    if (!s_bg_line_scratch || !s_bg_pri_scratch || !s_bg_flag_scratch ||
        !s_bgsp_ref_scratch || !s_grp8split_ref_scratch)
        return 0;

    /* Initial free ring is populated before CPU1 guest task exists. */
    for (uint32_t i = 0; i < s_slot_count; ++i)
        s_mb->free_ring[i] = (uint8_t)i;
    store_release(&s_mb->free_head, s_slot_count);
    store_release(&s_mb->free_tail, 0u);
    store_release(&s_mb->ready_head, 0u);
    store_release(&s_mb->ready_tail, 0u);

    cblend598g3_selfcheck();

#if portNUM_PROCESSORS > 1
    BaseType_t ok = xTaskCreatePinnedToCore(compose_task, "px68k_comp", 4096,
                                            NULL, 2, &s_task, 0);
#else
    BaseType_t ok = xTaskCreate(compose_task, "px68k_comp", 4096,
                                NULL, 2, &s_task);
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
}

int tab5_compose_submit_line(uint32_t y, uint32_t width,
                             const uint16_t *bottom, const uint16_t *top,
                             uint16_t *dst)
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
                                      uint16_t *dst)
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
                                           int bg_on_top, uint16_t *dst)
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
                                       const uint16_t *selfcheck_ref)
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
                                            const uint16_t *selfcheck_ref)
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

int tab5_compose_grp8split_needs_selfcheck(void)
{
    return load_acquire(&s_grp8split_selfcheck_state) == 0u;
}

void tab5_compose_guest_bg_barrier(void)
{
    ++s_bg_barrier_calls;
    if (!s_ready || load_acquire(&s_bgsource_pending) == 0u)
        return;

    ++s_bg_barrier_waits;
    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    while (load_acquire(&s_bgsource_pending) != 0u)
        taskYIELD();
    __atomic_add_fetch(&s_bg_barrier_us,
                       cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0),
                       __ATOMIC_RELAXED);
}

void tab5_compose_wait_idle(void)
{
    if (!s_ready || load_acquire(&s_pending) == 0u)
        return;

    ++s_frame_waits;
    /* CPU0 owns the worker and continues independently.  Yielding CPU1 here
     * avoids a hot spin in the rare case a frame ends with jobs still in flight. */
    while (load_acquire(&s_pending) != 0u)
        taskYIELD();
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
}
