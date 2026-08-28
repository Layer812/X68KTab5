/*
 * Tab5 port-specific implementation.
 * Intent: Tab5 LCD frontend: scale generation-dirty guest rows, suppress pixel-identical LCD bands, and keep LCD work on CPU0.
 * Layer8 Aug/17/2026
 */
#include "tab5_video.h"
#include "tab5_branding.h"

#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif

#include "tab5_guest_input.h"
#include "tab5_lp_broker.h"
#include "tab5_panic.h"
#include "tab5_audio.h"
#include "tab5_launcher.h"
#include "tab5_media_ui.h"
#include "libretro/joystick.h"
#include "libretro/keyboard.h"

#include <M5Unified.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"
#endif
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "driver/gpio.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "sdkconfig.h"
#include "esp_rom_sys.h"

/* Build 5.98g8: reuse the proven PIE graphics primitives from tab5_ppa.c
 * in the CPU0 LCD frontend.  C linkage is required because this file is C++. */
extern "C" int tab5_pie_graphics_diff(const void *a, const void *b, uint32_t bytes);
extern "C" void tab5_pie_graphics_copy(void *dst, const void *src, uint32_t bytes);

/*
 * Intent: PANIC compatibility presentation reads the authoritative X68000
 * video state on CPU0.  These are C globals owned by the frozen PX68K core;
 * normal PX68K rendering remains unchanged.
 * Layer8 Aug/17/2026
 */
extern "C" {
extern uint8_t GVRAM[0x80000];
extern uint16_t GrphPal[256];
extern uint16_t TextPal[256];
extern uint8_t Pal_Regs[1024];
extern uint16_t Pal16[65536];
extern uint16_t Pal16Adr[256];
extern uint32_t GrphScrollX[4];
extern uint32_t GrphScrollY[4];
extern uint8_t VCReg0[2];
extern uint8_t VCReg1[2];
extern uint8_t VCReg2[2];
extern uint8_t CRTC_Regs[48];
extern uint8_t BG[0x8000];
extern uint8_t Sprite_Regs[0x800];
extern uint8_t BG_Regs[0x12];
}

/* BUILD2_TAB5_PSRAM_GUARD */
#ifndef CONFIG_SPIRAM_SPEED_200M
#error "M5Stack Tab5 requires CONFIG_SPIRAM_SPEED_200M=y"
#endif

/* Intent: Submit only changed guest generations and suppress pixel-identical LCD rows; LCD bandwidth is host work and must stay off CPU1.  Layer8 Aug/17/2026 */
static const char *TAG = "TAB5_VIDEO";

/* LPFAB R2: touch joypad is a level/state signal, so LATEST semantics are
 * exact for the intended ownership contract.  CPU0 publishes to LP; CPU1
 * applies the brokered latest state at its guest-frame boundary.  If LP did
 * not start, fall back to the proven direct atomic path. */
static inline void tab5_touch_joy_publish(uint16_t joy)
{
    if (!tab5_lp_broker_touch_latest_publish(joy)) {
        (void)tab5_guest_input_queue_touch_joypad(joy);
    }
}
extern "C" void tab5_screen_manager_present_complete(uint64_t screen_token, int success);
static bool s_present_started = false;
static bool s_game_controls_enabled = false;
static bool s_host_ui_exclusive = false;
static bool s_panic_compat_enabled = false;

/*
 * Build 5.45 video split
 * ----------------------
 * Composite/PX68K framebuffer:
 *   CPU1 guest only posts pointer + geometry.  CPU0 host reads the completed ScrBuf
 *   directly and pushes rows to the LCD.  There is no PSRAM->PSRAM snapshot.
 *   A per-scanline generation handshake lets the reader retry a row if CPU1 guest
 *   happened to update that row while the LCD consumed it.  CPU1 never waits.
 *
 * Host text view:
 *   Keep the proven Build 5.44 packed snapshot slots because that buffer is
 *   produced by the host text renderer rather than WinDraw and therefore has
 *   no scanline-generation markers.
 */
static constexpr uint32_t kPresentSlots = 2;
static constexpr uint32_t kTrackedLines = 1024;
static constexpr uint32_t kLiveRowRetryLimit = 8;

enum present_mode_t : uint8_t
{
    PRESENT_SNAPSHOT = 0,
    PRESENT_LIVE_FB = 1,
    /* R52: immutable copy of a completed LIVE guest frame.  This mode is
     * deliberately not eligible for latest-frame coalescing. */
    PRESENT_LIVE_FROZEN = 2,
    /* R56: immutable Screen Manager snapshot.  Never coalesced and completion
     * is reported by opaque screen token rather than a CPU1 semaphore. */
    PRESENT_MANAGED = 3,
};

typedef struct
{
    uint16_t *pixels;
    uint32_t width;
    uint32_t height;
} present_slot_t;

typedef struct
{
    present_mode_t mode;
    uint8_t slot_index;
    const uint16_t *frame;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
    uint32_t live_epoch;
    uint64_t screen_token;
} present_request_t;

static present_slot_t s_slots[kPresentSlots] = {};
static size_t s_slot_capacity_pixels = 0;
static QueueHandle_t s_free_slots = nullptr;
static QueueHandle_t s_ready_requests = nullptr;
static QueueHandle_t s_ui_actions = nullptr;
static SemaphoreHandle_t s_display_mutex = nullptr;
static TaskHandle_t s_present_task = nullptr;

/* R57E12 touch policy: GPIO23 ISR sets one atomic edge flag only.  The
 * existing presenter wake remains lightweight, but M5Unified/I2C touch reads
 * happen only on IRQ, while a finger is active, or on a sparse safety sample. */
static volatile uint32_t s_touch_irq_pending = 0;
static bool s_touch_irq_enabled = false;
static bool s_touch_contact_active = false;
static int64_t s_touch_last_sample_us = 0;
static volatile uint32_t s_touch_irq_count = 0;
static volatile uint32_t s_touch_update_count = 0;
static volatile uint32_t s_touch_irq_samples = 0;
static volatile uint32_t s_touch_active_samples = 0;
static volatile uint32_t s_touch_safety_samples = 0;
static volatile uint32_t s_touch_update_us = 0;
static volatile uint32_t s_touch_update_max_us = 0;
static volatile uint32_t s_touch_irq_fallback = 0;
static bool s_async_ready = false;
/* R48: LIVE requests carry a presentation epoch.  A geometry/source barrier
 * increments this before rebuilding ScrBuf so any request already queued or
 * sleeping in the CPU0 pace timer can never interpret the mutable ScrBuf with
 * stale width/height/pitch metadata. */
static volatile uint32_t s_live_present_epoch = 1u;
static uint64_t s_live_epoch_drops = 0u;

/* R52 transition publish acknowledgement.
 * Only CPU1 submits frozen LIVE frames and it waits for each one to complete,
 * so a single binary semaphore is sufficient. */
static SemaphoreHandle_t s_frozen_present_done = nullptr;
static uint64_t s_r52_frozen_submits = 0u;
static uint64_t s_r52_frozen_presented = 0u;
static uint64_t s_r52_frozen_timeouts = 0u;
static uint64_t s_r52_frozen_copy_us = 0u;
static uint64_t s_r52_frozen_wait_us = 0u;

/* Writer count permits nested marking (normal line + PPA batch flush).  The
 * generation increments once for every completed writer section. */
static uint32_t s_line_writers[kTrackedLines] = {};
static uint32_t s_line_generation[kTrackedLines] = {};
/* R40: producer-published source-X dirty envelope.  Multiple producer writes
 * before the next LCD consumption union into the same pending span.  LCD
 * atomically consumes it under the same tiny spinlock; an empty x0/x1 pair
 * then lets the next producer start a fresh envelope. */
static DRAM_ATTR uint16_t s_line_dirty_x0[kTrackedLines] = {};
static DRAM_ATTR uint16_t s_line_dirty_x1[kTrackedLines] = {};
/* R42: 32-source-pixel sparse producer mask. 800px max width => 25 useful bits,
 * so one uint32_t per tracked line preserves sparse X without another large SRAM table. */
static DRAM_ATTR uint32_t s_line_dirty_tile32[kTrackedLines] = {};
static portMUX_TYPE s_line_dirty_span_mux = portMUX_INITIALIZER_UNLOCKED;

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_submitted_frames = 0;
static uint32_t s_presented_frames = 0;
static uint32_t s_dropped_frames = 0;
static uint32_t s_last_copy_us = 0;
static uint32_t s_last_push_us = 0;
static uint32_t s_cpu0_push_total_us = 0;
static uint32_t s_live_presented_frames = 0;
static uint32_t s_live_row_retries = 0;
static uint32_t s_live_unstable_rows = 0;

/* Build 6.14d: presentation pacing is host-only.  Quantize normal live-FB
 * presentation onto the X68000 CRTC cadence while keeping the newest queued
 * live frame.  This never stalls CPU1; it only schedules CPU0 LCD work. */
static esp_timer_handle_t s_pace_timer = nullptr;
static int64_t s_pace_next_us = 0;
static int64_t s_pace_last_present_us = 0;
static uint32_t s_pace_waits = 0;
static uint32_t s_pace_wait_us = 0;
static uint32_t s_pace_last_wait_us = 0;
static uint32_t s_pace_coalesced_frames = 0;
static uint32_t s_pace_skipped_slots = 0;
static uint32_t s_pace_last_interval_us = 0;
static bool s_pace_active_logged = false;

/* Build 5.98: libretro advertises PX68K output at a fixed 4:3 display
 * aspect even when the raw CRTC raster is e.g. 512x240.  The standalone
 * Tab5 frontend used to push those raw pixels 1:1, which made 240-line game
 * modes appear as a very short strip.  Scale only on CPU0 at presentation
 * time; the guest/WinDraw framebuffer and all CRTC semantics stay untouched. */
static constexpr uint32_t kAspectMapMaxWidth = 1280;
static constexpr uint32_t kAspectViewportWidth = 960;
static constexpr uint32_t kAspectViewportHeight = 720;
static uint16_t *s_aspect_frame = nullptr;
static bool s_aspect_frame_ppa_capable = false;

/* Build 6.15h17: true ESP-IDF DPI double framebuffer path.
 *
 * M5GFX is build-time patched (pinned 0.2.26) so Panel_DSI asks ESP-IDF for
 * two driver-owned RGB565 framebuffers and exposes both pointers + the DPI
 * panel handle via config_detail().  CPU0 never writes the actively scanned
 * framebuffer.  After each swap, the old Front is repaired with only the row
 * span changed by the previous frame (PIE + explicit C2M writeback), keeping both buffers
 * coherent without a full-screen copy on steady-state frames.  Full sync is
 * reserved for initial/UI/geometry transitions; new generation-dirty rows are
 * modified only in Back.  esp_lcd_panel_draw_bitmap() receives the driver-owned Back
 * pointer, which switches the DPI DMA source without a pixel copy.  The old
 * Front is not reused until on_refresh_done confirms one physical refresh.
 *
 * This deliberately measures the panel's real refresh cadence rather than
 * assuming that the guest's 16.271-ms (~61.46-Hz) host pace equals DSI VSYNC. */
#if defined(CONFIG_IDF_TARGET_ESP32P4)
static lgfx::Panel_DSI *s_dsi_panel = nullptr;
static esp_lcd_panel_handle_t s_dsi_dpi_panel = nullptr;
static uint16_t *s_dsi_fb[2] = { nullptr, nullptr };
static uint8_t s_dsi_front_idx = 0;
static uint8_t s_dsi_back_idx = 1;
/* Tab5's DPI scanout is physically portrait even though M5GFX presents a
 * 1280x720 logical surface after setRotation(1).  Driver-owned framebuffer
 * memory therefore remains 720x1280 with a 720-pixel stride. */
static constexpr uint32_t kDsiPhysWidth = 720;
static constexpr uint32_t kDsiPhysHeight = 1280;
static constexpr uint32_t kDsiPhysStridePixels = kDsiPhysWidth;
static uint32_t s_dsi_stride_pixels = 0;
static bool s_dsi_double_live = false;
/* Build 6.15h17R11: keep LIVE rendering in a contiguous landscape staging
 * surface, then let the P4 PPA SRM engine rotate only the dirty horizontal
 * band into the portrait-native DSI Back framebuffer.  This removes R6's
 * 960 tiny transpose/cache-sync operations per changed band. */
static ppa_client_handle_t s_dsi_ppa_srm = nullptr;
static bool s_dsi_ppa_live = false;
static bool s_dsi_ppa_prime_other = false;
static uint32_t s_dsi_ppa_geom_w = 0;
static uint32_t s_dsi_ppa_geom_h = 0;
static uint32_t s_dsi_ppa_geom_x = 0;
static uint32_t s_dsi_ppa_geom_y = 0;

/* Build 6.15h17R11: keep exact dirty bands instead of collapsing all changed
 * rows into one min..max rectangle.  Back is frame N-2, so each presentation
 * applies the union of frame N-1 and frame N bands before the swap. */
struct dbfb_dirty_band_t { uint16_t y0; uint16_t y1; };
static constexpr uint32_t kDbfbDirtyBandCap = 24;
static constexpr uint32_t kDbfbPpaSubmitCap = 2;
static constexpr uint32_t kDbfbPpaMergeGapRows = 8;
static dbfb_dirty_band_t s_dsi_ppa_prev_bands[kDbfbDirtyBandCap] = {};
static uint32_t s_dsi_ppa_prev_band_count = 0;
static uint64_t s_dsi_ppa_us = 0;
static uint64_t s_dsi_ppa_pixels = 0;
static uint64_t s_dsi_ppa_rows = 0;
static uint64_t s_dsi_ppa_ops = 0;
static uint64_t s_dsi_ppa_frames = 0;
/* Back-buffer coherence state.  Most frames repair only the previous frame's
 * changed LCD row span after refresh_done.  A full Front->Back sync is needed
 * only after M5GFX/UI writes, geometry resets, or initial activation. */
static bool s_dsi_need_full_sync = true;
static bool s_dsi_repair_pending = false;
static uint32_t s_dsi_repair_x0 = 0;
static uint32_t s_dsi_repair_y0 = 0;
static uint32_t s_dsi_repair_x1 = 0;
static uint32_t s_dsi_repair_y1 = 0;
static bool s_dsi_swap_wait_refresh = false;
static uint32_t s_dsi_swap_refresh_seq = 0;
static async_memcpy_handle_t s_dsi_sync_gdma = nullptr;
static SemaphoreHandle_t s_dsi_sync_done = nullptr;
static SemaphoreHandle_t s_dsi_refresh_event = nullptr;
static volatile uint32_t s_dsi_refresh_seq = 0;
static volatile uint32_t s_dsi_refresh_last_us = 0;
static volatile uint32_t s_dsi_refresh_period_us = 0;
static volatile uint32_t s_dsi_sync_pending = 0;
static uint64_t s_dbfb_sync_us = 0;
static uint64_t s_dbfb_sync_bytes = 0;
static uint64_t s_dbfb_sync_count = 0;
static uint64_t s_dbfb_sync_full_count = 0;
static uint64_t s_dbfb_sync_repair_count = 0;
static uint64_t s_dbfb_sync_fallback = 0;
static uint64_t s_dbfb_flush_us = 0;
static uint64_t s_dbfb_flush_bytes = 0;
static uint64_t s_dbfb_flush_count = 0;
static uint64_t s_dbfb_swap_count = 0;
static uint64_t s_dbfb_refresh_wait_us = 0;
static uint64_t s_dbfb_refresh_timeouts = 0;

/* Build 6.15h17R32: R31 proved that the expensive part is not scale/diff or
 * PPA Back writes, but the DPI framebuffer switch/refresh path.  Keep the
 * driver-owned double buffers allocated for M5GFX/UI compatibility, warm up
 * the proven DBFB path for a few frames, then stop per-frame A/B swaps and
 * rotate dirty bands directly into the framebuffer currently scanned by DSI.
 * This can trade some tearing for substantially lower shared-fabric pressure. */
#ifndef PX68K_TAB5_R32_DIRECT_FRONT
#define PX68K_TAB5_R32_DIRECT_FRONT 1
#endif
static constexpr uint32_t kR32WarmupSwapFrames = 8u;
static bool s_r32_direct_front = false;
static uint32_t s_r32_warmup_swaps = 0u;
static uint64_t s_r32_direct_frames = 0u;
static uint64_t s_r32_direct_ppa_us = 0u;
static uint64_t s_r32_direct_rows = 0u;
static uint64_t s_r32_direct_ops = 0u;

/* R49 correctness hardening:
 * - keep normal scanout on driver FB0 only after init;
 * - require one full 960x720 repaint after every host-UI/source/geometry
 *   transition before sparse TILE32 updates resume.
 *
 * R48's source/present epochs prevent stale source jobs and stale LIVE tokens,
 * but R38 direct-native previously ignored s_dsi_need_full_sync, and software
 * front bookkeeping did not prove the physical DPI scanout stayed on FB0. */
#ifndef PX68K_TAB5_R49_FB0_NOFLIP
#define PX68K_TAB5_R49_FB0_NOFLIP 1
#endif
static bool s_r49_force_full_front = true;
static uint64_t s_r49_full_repaint_requests = 0u;
static uint64_t s_r49_full_repaints = 0u;
static uint64_t s_r49_fb0_pin_failures = 0u;

/* Build 6.15h17R34: replace steady-state PPA rotation with an IRAM/XespV-
 * assisted software 8x8 transpose into the currently scanned native DSI FB.
 * R30/R31 showed that the scale+PIE-diff staging path can run near 18-20 ms/f
 * when PPA presentation is absent, while R32/R33 stayed near 22.5 ms/f even
 * with per-frame FB swaps disabled and PPA burst reduced 64->32 bytes.
 *
 * Keep the proven 960x720 staging surface for this one-variable A/B.  The
 * kernel consumes horizontal 8x8 source tiles, transposes them in a 128-byte
 * Internal-DRAM scratch, then uses one 128-bit XespV store per native row.
 * CPU-written PSRAM must be made visible to DSI DMA, so cache writeback is
 * batched over 64 native rows rather than R6's ~960 tiny msync calls. */
#ifndef PX68K_TAB5_R34_SWROT_FRONT
#define PX68K_TAB5_R34_SWROT_FRONT 1
#endif
static DRAM_ATTR uint16_t s_r34_tile[64] __attribute__((aligned(16))) = {};
static bool s_r34_front_cache_primed = false;
static bool s_r34_selfcheck_ok = false;
static uint64_t s_r34_frames = 0u;
static uint64_t s_r34_rotate_us = 0u;
static uint64_t s_r34_sync_us = 0u;
static uint64_t s_r34_rows = 0u;
static uint64_t s_r34_tiles = 0u;
static uint64_t s_r34_scalar_pixels = 0u;

/* Build 6.15h17R35: R34 proved that merely replacing PPA with software
 * rotation is not enough: the 8x8 kernel still performs thousands of small
 * PSRAM reads from the landscape staging surface and settles near 8 ms per
 * changed present.  Pack one complete 8-row stripe (15 KiB at 960px) into
 * Internal SRAM with a sequential XespV copy, then transpose entirely from
 * Internal SRAM and keep only the final native-FB stores in PSRAM.  The
 * existing R34 path remains the allocation-failure fallback. */
#ifndef PX68K_TAB5_R35_INTERNAL_STRIPE
#define PX68K_TAB5_R35_INTERNAL_STRIPE 1
#endif
static uint16_t *s_r35_pack8 = nullptr;
static uint64_t s_r35_pack_us = 0u;
static uint64_t s_r35_pack_stripes = 0u;
static bool s_r35_line_scratch_internal = false;

/* Build 6.15h17R37: R36 proved that eliminating the 960x720 scale staging
 * is valuable (~23.4 -> ~21.1 ms/f on the MDX workload), but its scalar raw
 * row hash consumed the new hot path: almost every generation-dirty row was
 * pixel-identical and avgSCAN grew to ~22 ms per changed present.  Keep the
 * R36 direct-native renderer, but replace the scalar hash with an exact XespV
 * row compare against a raw-source shadow.  No extra PSRAM is allocated: once
 * the eight-frame PPA warmup is over, the old 960x720 staging allocation is
 * repurposed as the raw shadow.  The shadow uses the guest pitch and an offset
 * chosen to match the source modulo-16 alignment so tab5_pie_graphics_diff /
 * copy stay on their 128-bit XespV paths. */
#ifndef PX68K_TAB5_R36_DIRECT_NATIVE
#define PX68K_TAB5_R36_DIRECT_NATIVE 1
#endif
#ifndef PX68K_TAB5_R37_EXACT_RAW_SHADOW
#define PX68K_TAB5_R37_EXACT_RAW_SHADOW 1
#endif
#ifndef PX68K_TAB5_R38_PRODUCER_EXACT_DIRTY
#define PX68K_TAB5_R38_PRODUCER_EXACT_DIRTY 1
#endif
static DRAM_ATTR uint8_t s_r37_shadow_valid[kTrackedLines] = {};
static uint16_t *s_r37_shadow = nullptr;
static const uint16_t *s_r37_shadow_source = nullptr;
static uint32_t s_r37_shadow_pitch = 0u;
static uint32_t s_r37_shadow_width = 0u;
static uint32_t s_r37_shadow_height = 0u;
static DRAM_ATTR uint32_t s_r36_dirty_block_bits[3] = {}; /* legacy R36-R40 fallback bookkeeping */
/* R41: keep dirty coverage at the native store granularity instead of
 * widening all X spans inside an 8-row block into one large rectangle.
 * 960 / 8 = 120 logical X tiles, 720 / 8 = 90 logical Y blocks. */
static constexpr uint32_t kR41TileCols = kAspectViewportWidth / 8u;
static constexpr uint32_t kR41TileRows = kAspectViewportHeight / 8u;
static constexpr uint32_t kR41TileWords = (kR41TileCols + 31u) / 32u;
static constexpr uint32_t kR41SyncRectCap = 96u;
static DRAM_ATTR uint32_t s_r41_tile_mask[kR41TileRows][kR41TileWords] = {};
typedef struct { uint16_t x0, x1, y0, y1; } r40_dirty_rect_t;
static DRAM_ATTR r40_dirty_rect_t s_r40_sync_rects[kR41SyncRectCap] = {};
static uint64_t s_r36_updates = 0u;
static uint64_t s_r36_calls = 0u;
static uint64_t s_r36_scan_us = 0u;
static uint64_t s_r36_scale_us = 0u;
static uint64_t s_r36_store_us = 0u;
static uint64_t s_r36_sync_us = 0u;
static uint64_t s_r36_blocks = 0u;
static uint64_t s_r40_block_width_sum = 0u;
static uint64_t s_r40_logical_pixels = 0u;
static uint64_t s_r41_dirty_tiles = 0u;
static uint64_t s_r41_tile_runs = 0u;
static uint64_t s_r41_sync_rects_total = 0u;
static uint64_t s_r41_span_fallbacks = 0u;
static uint64_t s_r41_sync_overflows = 0u;
static uint64_t s_r41_verify_ok = 0u;
static uint64_t s_r41_verify_skip = 0u;
static uint64_t s_r41_verify_fail = 0u;
static uint64_t s_r42_src_tile_rows = 0u;
static uint64_t s_r42_src_tiles = 0u;
static uint64_t s_r42_src_mask_fallbacks = 0u;
static uint64_t s_r37_rows_tested = 0u;
static uint64_t s_r37_rows_same = 0u;
static uint64_t s_r37_rows_changed = 0u;
static uint64_t s_r37_rows_copied = 0u;
#else
static void *s_dsi_panel = nullptr;
static uint32_t s_dsi_stride_pixels = 0;
static bool s_dsi_double_live = false;
#endif
static constexpr uint32_t kPanicCompatPitch = 512;
static constexpr uint32_t kPanicCompatHeight = 512;
static uint16_t *s_panic_compat_frame = nullptr;

/* Build 6.15h17-r4: DoubleFB already owns a full 1280x720 Back buffer in
 * PSRAM, so do not keep the old 960x720 aspect cache beside it.  Allocate the
 * legacy cache only if the DBFB path becomes unavailable at runtime.  PANIC's
 * 512x512 compatibility surface is similarly lazy because it is normally idle. */
static bool ensure_aspect_frame_allocated(void)
{
    if (s_aspect_frame) return true;
    const size_t bytes = (size_t)kAspectViewportWidth *
                         (size_t)kAspectViewportHeight * sizeof(uint16_t);

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    /* R9 PPA input/output buffers in external RAM must satisfy the P4 cache
     * alignment rules.  Prefer a DMA-capable, 128-byte aligned PSRAM surface.
     * If that allocation fails, keep the legacy M5GFX path available. */
    s_aspect_frame = static_cast<uint16_t *>(heap_caps_aligned_alloc(
        128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    s_aspect_frame_ppa_capable = s_aspect_frame != nullptr;
#endif
    if (!s_aspect_frame)
    {
        s_aspect_frame = static_cast<uint16_t *>(heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        s_aspect_frame_ppa_capable = false;
    }
    if (!s_aspect_frame)
    {
        ESP_LOGE(TAG, "PX68K_DBFB615H17R11: landscape staging framebuffer allocation FAILED");
        return false;
    }
    std::memset(s_aspect_frame, 0, bytes);
    ESP_LOGI(TAG,
             "PX68K_DBFB615H17R11: landscape staging ready bytes=%u aligned128=%u ppa_dma=%u",
             (unsigned)bytes,
             (((uintptr_t)s_aspect_frame & 127u) == 0u) ? 1u : 0u,
             s_aspect_frame_ppa_capable ? 1u : 0u);
    return true;
}

static bool ensure_panic_compat_frame_allocated(void)
{
    if (s_panic_compat_frame) return true;
    s_panic_compat_frame = static_cast<uint16_t *>(heap_caps_calloc(
        (size_t)kPanicCompatPitch * (size_t)kPanicCompatHeight, sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_panic_compat_frame)
    {
        ESP_LOGW(TAG, "PANIC compatibility framebuffer unavailable; normal PX68K presenter retained");
        return false;
    }
    ESP_LOGI(TAG, "PX68K_DBFB615H17R4: PANIC compatibility framebuffer lazy-allocated (%u bytes)",
             (unsigned)(kPanicCompatPitch * kPanicCompatHeight * sizeof(uint16_t)));
    return true;
}
static uint32_t s_panic_compat_last_w = 0;
static uint32_t s_panic_compat_last_h = 0;
static uint8_t s_panic_compat_last_mode = 0xff;
static uint8_t s_panic_compat_last_enable = 0xff;
/* Build 5.98g8: one destination-width scratch row lets CPU0 render a
 * generation-dirty line without destroying the last displayed copy.  A PIE
 * DIFF can then suppress the LCD transaction when the pixels are unchanged.
 * Keep the raw pointer because the usable row is modulo-16 aligned to
 * s_aspect_frame for the PIE wrapper. */
static uint8_t *s_aspect_line_scratch_raw = nullptr;
static uint16_t *s_aspect_line_scratch = nullptr;
static uint64_t s_vdiff_tested_src_rows = 0;
static uint64_t s_vdiff_same_src_rows = 0;
static uint64_t s_vdiff_changed_src_rows = 0;
static uint64_t s_vdiff_identical_dst_rows = 0;
static uint16_t s_aspect_xmap[kAspectMapMaxWidth] = {};
static uint16_t s_aspect_ymap[kAspectViewportHeight] = {};
static uint32_t s_aspect_map_src_w = 0;
static uint32_t s_aspect_map_dst_w = 0;
static uint32_t s_aspect_map_src_h = 0;
static uint32_t s_aspect_map_dst_h = 0;
static constexpr uint32_t kDirtyMergeGapRows = 2;
static uint32_t s_aspect_log_src_w = 0;
static uint32_t s_aspect_log_src_h = 0;

/* Build 5.98a: the WinDraw generation markers already describe the exact
 * source scanlines that were redrawn (WinDraw only marks TextDirtyLine rows).
 * Cache the last generation presented for each source line and only rebuild /
 * push LCD bands whose source generation changed.  Geometry or framebuffer
 * changes force one full viewport refresh. */
static uint32_t s_aspect_seen_generation[kTrackedLines] = {};
static const uint16_t *s_aspect_seen_frame = nullptr;
static uint32_t s_aspect_seen_src_w = 0;
static uint32_t s_aspect_seen_src_h = 0;
static uint32_t s_aspect_seen_pitch = 0;
static uint32_t s_aspect_dirty_frames = 0;
static uint32_t s_aspect_full_frames = 0;
static uint32_t s_aspect_dirty_bands = 0;
static uint32_t s_aspect_dirty_rows = 0;

/*
 * In-game touch chrome + runtime media changer
 * ---------------------------------------------
 * Intent: Match the agreed game-screen layout without covering the 960x720
 * X68000 viewport.  CPU0 owns drawing/touch.  High-level PANIC/media actions
 * are queued to CPU1 so disk mutation still happens on the emulation task.
 * Layer8 Aug/17/2026
 */
static inline bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static inline bool point_in_circle(int x, int y, int cx, int cy, int r)
{
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void draw_panel_button(int x, int y, int w, int h, const char *label,
                              uint16_t fill, uint16_t border, int text_size)
{
    M5.Display.fillRoundRect(x, y, w, h, 8, fill);
    M5.Display.drawRoundRect(x, y, w, h, 8, border);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(text_size);
    M5.Display.setTextColor(TFT_WHITE, fill);
    M5.Display.drawString(label, x + w / 2, y + h / 2);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

static void draw_folder_icon(int x, int y, uint16_t color)
{
    M5.Display.drawRect(x, y + 7, 38, 25, color);
    M5.Display.drawRect(x + 4, y, 15, 9, color);
    M5.Display.drawFastHLine(x + 1, y + 8, 36, color);
}

static void draw_speaker_icon(int x, int y, bool plus)
{
    const uint16_t c = 0xDEFB;
    M5.Display.fillRect(x, y + 10, 9, 18, c);
    M5.Display.fillTriangle(x + 9, y + 10, x + 25, y + 2, x + 25, y + 36, c);
    M5.Display.drawCircle(x + 29, y + 19, 11, c);
    M5.Display.fillRect(x + 25, y + 2, 10, 34, TFT_BLACK); /* leave a sound-wave arc */
    M5.Display.drawFastHLine(x + 43, y + 19, 16, c);
    if (plus) M5.Display.drawFastVLine(x + 51, y + 11, 16, c);
}

static void draw_round_ab(int cx, int cy, const char *label)
{
    M5.Display.fillCircle(cx, cy, 43, 0x18E3);
    M5.Display.drawCircle(cx, cy, 43, 0x8410);
    M5.Display.drawCircle(cx, cy, 42, 0x4208);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(0xDEFB, 0x18E3);
    M5.Display.drawString(label, cx, cy);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

static void draw_dpad_button(int x, int y, int w, int h, char arrow)
{
    M5.Display.fillRoundRect(x, y, w, h, 5, 0x18E3);
    M5.Display.drawRoundRect(x, y, w, h, 5, 0x5AEB);
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const uint16_t c = 0xDEFB;
    if (arrow == 'U') M5.Display.fillTriangle(cx, cy - 9, cx - 9, cy + 6, cx + 9, cy + 6, c);
    if (arrow == 'D') M5.Display.fillTriangle(cx, cy + 9, cx - 9, cy - 6, cx + 9, cy - 6, c);
    if (arrow == 'L') M5.Display.fillTriangle(cx - 9, cy, cx + 6, cy - 9, cx + 6, cy + 9, c);
    if (arrow == 'R') M5.Display.fillTriangle(cx + 9, cy, cx - 6, cy - 9, cx - 6, cy + 9, c);
}

static void draw_game_controls_unlocked(void)
{
    if (!s_game_controls_enabled) return;
    const int w = M5.Display.width();
    const int h = M5.Display.height();
    if (w < 1200 || h < 700) return;

    const int rx = w - 160;
    M5.Display.fillRect(0, 0, 160, h, TFT_BLACK);
    M5.Display.fillRect(rx, 0, 160, h, TFT_BLACK);
    M5.Display.drawFastVLine(159, 0, h, 0x3186);
    M5.Display.drawFastVLine(rx, 0, h, 0x3186);

    /* Reference layout: PANIC / FILE above a low D-pad. */
    draw_panel_button(31, 29, 98, 68, "PANIC", 0x7800, 0xF800, 2);
    M5.Display.fillRoundRect(31, 118, 98, 68, 8, 0x1082);
    M5.Display.drawRoundRect(31, 118, 98, 68, 8, 0x8410);
    draw_folder_icon(61, 127, 0xDEFB);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xDEFB, 0x1082);
    M5.Display.drawString("FILE", 80, 170);
    M5.Display.setTextDatum(textdatum_t::top_left);

    /* Build 6.12o: software keyboard launcher in the unused left-side slot. */
    draw_panel_button(31, 207, 98, 68, "KEY", 0x1082, 0x8410, 2);

    draw_dpad_button(53, 520, 54, 54, 'U');
    draw_dpad_button(17, 577, 56, 60, 'L');
    draw_dpad_button(87, 577, 56, 60, 'R');
    draw_dpad_button(53, 640, 54, 54, 'D');

    /* Reference layout: volume controls high, B/A circular buttons low. */
    /* Build 6.12d: conventional vertical volume ordering: + above, - below. */
    draw_panel_button(rx + 31, 29, 98, 62, "", 0x1082, 0x8410, 1);
    draw_speaker_icon(rx + 49, 41, true);
    draw_panel_button(rx + 31, 110, 98, 62, "", 0x1082, 0x8410, 1);
    draw_speaker_icon(rx + 49, 122, false);
    /* CPSF/MD bank-1 Start; this is not synthesized as opposing D-pad bits. */
    draw_panel_button(rx + 31, 207, 98, 68, "START", 0x1082, 0x8410, 2);
    draw_round_ab(rx + 80, 520, "B");
    draw_round_ab(rx + 80, 635, "A");
}

enum : uint32_t {
    UTIL_PANIC = 1u << 0,
    UTIL_FILE  = 1u << 1,
    UTIL_VOLM     = 1u << 2,
    UTIL_VOLP     = 1u << 3,
    UTIL_KEYBOARD = 1u << 4,
};

typedef struct
{
    uint16_t joy;
    uint32_t util;
} game_touch_map_t;

/* Build 6.12m: single source of truth for the game-side touch map. */
static game_touch_map_t map_game_touch_point(int x, int y, int display_w)
{
    game_touch_map_t m = {};
    const int rx = display_w - 160;

    if (point_in_rect(x,y,31,29,98,68)) m.util |= UTIL_PANIC;
    if (point_in_rect(x,y,31,118,98,68)) m.util |= UTIL_FILE;
    if (point_in_rect(x,y,31,207,98,68)) m.util |= UTIL_KEYBOARD;
    if (point_in_rect(x,y,53,520,54,54)) m.joy |= JOY_UP;
    if (point_in_rect(x,y,17,577,56,60)) m.joy |= JOY_LEFT;
    if (point_in_rect(x,y,87,577,56,60)) m.joy |= JOY_RIGHT;
    if (point_in_rect(x,y,53,640,54,54)) m.joy |= JOY_DOWN;

    if (point_in_rect(x,y,rx+31,29,98,62)) m.util |= UTIL_VOLP;
    if (point_in_rect(x,y,rx+31,110,98,62)) m.util |= UTIL_VOLM;
    if (point_in_rect(x,y,rx+31,207,98,68)) m.joy |= JOY_HOST_START;

    /* PX68K/libretro normal 2-button convention: B=TRG1, A=TRG2. */
    if (point_in_circle(x,y,rx+80,520,48)) m.joy |= JOY_TRG1; /* B */
    if (point_in_circle(x,y,rx+80,635,48)) m.joy |= JOY_TRG2; /* A */
    return m;
}



static uint32_t s_utility_prev_mask = 0;
/* Build 6.12d touch D-pad repeat shaping.  USB JOY remains a true continuous
 * pad. Touch gets an immediate pulse, a deliberate initial pause, then a
 * moderate repeat cadence so menu cursors do not run away. */
static uint16_t s_touch_dpad_prev = 0;
static int64_t s_touch_dpad_press_us = 0;
static bool s_media_overlay_active = false;
static bool s_media_overlay_redraw = false;
static bool s_media_overlay_touch_down = false;
static bool s_media_overlay_browser = false;
static bool s_game_touch_suppress_until_release = false;

/* R57E3 touch-transition fence.  CPU1 only publishes a semantic video
 * transition epoch + target geometry.  CPU0 consumes it before polling touch,
 * releases every synthetic touch key/JOY state, lets one matching frame cross
 * the physical presenter, then waits for an observed finger-up before touch is
 * armed again.  No guest wait and no display call occurs on CPU1. */
static volatile uint32_t s_touch_transition_epoch = 1u;
static volatile uint32_t s_touch_transition_pub_w = 0u;
static volatile uint32_t s_touch_transition_pub_h = 0u;
static volatile uint32_t s_touch_transition_pub_pitch = 0u;
static uint32_t s_touch_transition_seen_epoch = 1u;
static uint32_t s_touch_transition_target_w = 0u;
static uint32_t s_touch_transition_target_h = 0u;
static uint32_t s_touch_transition_target_pitch = 0u;
static bool s_touch_transition_active = false;
static bool s_touch_transition_present_guard = false;
static uint8_t s_touch_transition_release_stable = 0u;
static uint32_t s_touch_transition_arm_count = 0u;
static uint32_t s_touch_transition_rearm_count = 0u;
static bool s_force_game_redraw = false;
static int s_media_overlay_drive = 0; /* 0=FDD0, 1=FDD1, 2=HDD0 */
static size_t s_media_overlay_page = 0;
static int s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
static int s_runtime_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;

/* Build 6.12o buffered software keyboard -------------------------------------------------
 * CPU0 owns the overlay.  Key taps enter the same thread-safe queue used by USB
 * HID, so CPU1 still mutates the X68000 keyboard matrix only from guest_input_tick().
 * While the keyboard is visible the last game frame is intentionally frozen on
 * the LCD; the emulated machine and audio continue to run.  This avoids a large
 * keyboard redraw after every video frame and keeps CPU0 headroom predictable. */
static bool s_softkbd_active = false;
static bool s_softkbd_redraw = false;
static bool s_softkbd_touch_down = false;
static bool s_softkbd_shift = false;
static bool s_softkbd_ctrl = false;

/* Build 6.12o: compose locally, preview, then SEND.  One buffered item is one
 * X68000 key tap plus the SHIFT/CTRL state captured when it was entered. */
typedef struct {
    uint8_t scancode;
    uint8_t modifiers;
    const char *label;
} soft_input_event_t;
static constexpr size_t kSoftInputMax = 80;
static soft_input_event_t s_soft_input[kSoftInputMax] = {};
static size_t s_soft_input_count = 0;

enum : uint8_t {
    SOFTKEY_NORMAL = 0,
    SOFTKEY_SHIFT  = 1,
    SOFTKEY_CTRL   = 2,
};

typedef struct {
    const char *label;
    uint8_t scancode;
    uint8_t units;
    uint8_t kind;
} soft_key_t;

/* X68000 keyboard scan codes.  The software keyboard deliberately uses the
 * guest scan codes directly rather than translating through RetroKey/USB HID.
 * This preserves the X68000/JIS layout (including @, :, ^, OPT.1/OPT.2) and
 * guarantees that Human68k and games see the exact make/break bytes. */
static const soft_key_t kSoftRow0[] = {
    {"ESC",0x01,2,0},{"F1",0x63,1,0},{"F2",0x64,1,0},{"F3",0x65,1,0},
    {"F4",0x66,1,0},{"F5",0x67,1,0},{"F6",0x68,1,0},{"F7",0x69,1,0},
    {"F8",0x6A,1,0},{"F9",0x6B,1,0},{"F10",0x6C,1,0},{"BS",0x0F,2,0},
};
static const soft_key_t kSoftRow1[] = {
    {"1",0x02,1,0},{"2",0x03,1,0},{"3",0x04,1,0},{"4",0x05,1,0},{"5",0x06,1,0},
    {"6",0x07,1,0},{"7",0x08,1,0},{"8",0x09,1,0},{"9",0x0A,1,0},{"0",0x0B,1,0},
    {"-",0x0C,1,0},{"^",0x0D,1,0},{"\\",0x0E,1,0},{"DEL",0x37,2,0},
};
static const soft_key_t kSoftRow2[] = {
    {"TAB",0x10,2,0},{"Q",0x11,1,0},{"W",0x12,1,0},{"E",0x13,1,0},{"R",0x14,1,0},
    {"T",0x15,1,0},{"Y",0x16,1,0},{"U",0x17,1,0},{"I",0x18,1,0},{"O",0x19,1,0},
    {"P",0x1A,1,0},{"@",0x1B,1,0},{"[",0x1C,1,0},{"RET",0x1D,2,0},
};
static const soft_key_t kSoftRow3[] = {
    {"CTRL",0x71,2,SOFTKEY_CTRL},{"A",0x1E,1,0},{"S",0x1F,1,0},{"D",0x20,1,0},
    {"F",0x21,1,0},{"G",0x22,1,0},{"H",0x23,1,0},{"J",0x24,1,0},{"K",0x25,1,0},
    {"L",0x26,1,0},{";",0x27,1,0},{":",0x28,1,0},{"]",0x29,1,0},{"RET",0x1D,2,0},
};
static const soft_key_t kSoftRow4[] = {
    {"SHIFT",0x70,2,SOFTKEY_SHIFT},{"Z",0x2A,1,0},{"X",0x2B,1,0},{"C",0x2C,1,0},
    {"V",0x2D,1,0},{"B",0x2E,1,0},{"N",0x2F,1,0},{"M",0x30,1,0},{",",0x31,1,0},
    {".",0x32,1,0},{"/",0x33,1,0},{"_",0x34,1,0},{"SHIFT",0x70,2,SOFTKEY_SHIFT},
};
static const soft_key_t kSoftRow5[] = {
    {"OPT1",0x72,2,0},{"LEFT",0x3B,1,0},{"DOWN",0x3E,1,0},{"UP",0x3C,1,0},
    {"RIGHT",0x3D,1,0},{"SPACE",0x35,7,0},{"HOME",0x36,2,0},{"OPT2",0x73,2,0},
};

typedef struct { const soft_key_t *keys; size_t count; } soft_row_t;
static const soft_row_t kSoftRows[] = {
    {kSoftRow0,sizeof(kSoftRow0)/sizeof(kSoftRow0[0])},
    {kSoftRow1,sizeof(kSoftRow1)/sizeof(kSoftRow1[0])},
    {kSoftRow2,sizeof(kSoftRow2)/sizeof(kSoftRow2[0])},
    {kSoftRow3,sizeof(kSoftRow3)/sizeof(kSoftRow3[0])},
    {kSoftRow4,sizeof(kSoftRow4)/sizeof(kSoftRow4[0])},
    {kSoftRow5,sizeof(kSoftRow5)/sizeof(kSoftRow5[0])},
};

static constexpr int kSoftKbdX = 168;
static constexpr int kSoftKbdY = 266;
static constexpr int kSoftKbdW = 944;
static constexpr int kSoftKbdRowH = 68;
static constexpr int kSoftKbdGap = 4;

static int softkbd_units(const soft_row_t &row)
{
    int n=0; for (size_t i=0;i<row.count;++i) n += row.keys[i].units; return n;
}

static void softkbd_key_rect(const soft_row_t &row, size_t index, int row_index,
                             int *x, int *y, int *w, int *h)
{
    const int units=softkbd_units(row);
    const int usable=kSoftKbdW - (int)(row.count-1)*kSoftKbdGap;
    int xpos=kSoftKbdX;
    int consumed_units=0;
    for (size_t i=0;i<index;++i) {
        const int next_units=consumed_units + row.keys[i].units;
        const int x0=(usable*consumed_units)/units;
        const int x1=(usable*next_units)/units;
        xpos += (x1-x0) + kSoftKbdGap;
        consumed_units=next_units;
    }
    const int next_units=consumed_units + row.keys[index].units;
    const int x0=(usable*consumed_units)/units;
    const int x1=(usable*next_units)/units;
    *x=xpos; *y=kSoftKbdY + row_index*kSoftKbdRowH;
    *w=x1-x0; *h=kSoftKbdRowH-kSoftKbdGap;
}

static void softkbd_append_text(char *dst, size_t cap, const char *src)
{
    if (!dst || !cap || !src) return;
    const size_t used=std::strlen(dst);
    if (used + 1 >= cap) return;
    std::strncat(dst,src,cap-used-1);
}

static void softkbd_build_preview(char *out,size_t cap)
{
    if (!out || !cap) return;
    out[0]='\0';
    for (size_t i=0;i<s_soft_input_count;++i) {
        const soft_input_event_t &e=s_soft_input[i];
        const char *lab=e.label ? e.label : "?";
        char tok[20] = {};
        if (!std::strcmp(lab,"SPACE")) {
            std::strcpy(tok," ");
        } else if (!std::strcmp(lab,"RET")) {
            std::strcpy(tok,"<RET>");
        } else if (std::strlen(lab)==1 && std::isalpha((unsigned char)lab[0])) {
            char c=lab[0];
            if (!(e.modifiers & 0x01u)) c=(char)std::tolower((unsigned char)c);
            if (e.modifiers & 0x02u) std::snprintf(tok,sizeof(tok),"<C-%c>",(char)std::toupper((unsigned char)c));
            else { tok[0]=c; tok[1]='\0'; }
        } else if (std::strlen(lab)==1 && e.modifiers) {
            std::snprintf(tok,sizeof(tok),"<%s%s%s>",(e.modifiers&1u)?"S-":"",(e.modifiers&2u)?"C-":"",lab);
        } else if (std::strlen(lab)==1) {
            tok[0]=lab[0]; tok[1]='\0';
        } else {
            std::snprintf(tok,sizeof(tok),"<%s>",lab);
        }
        softkbd_append_text(out,cap,tok);
    }
}

static void draw_soft_keyboard_unlocked(void)
{
    /* Keep side controls visible; repaint only the central viewport. */
    M5.Display.fillRect(160, 0, 960, 720, 0x0841);
    M5.Display.fillRect(160, 0, 960, 54, 0x1082);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE,0x1082);
    M5.Display.drawString("SOFTWARE KEYBOARD",640,20);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Compose in INPUT BUFFER, then tap SEND",640,43);
    M5.Display.setTextDatum(textdatum_t::top_left);

    /* Input preview: deliberately separate from guest output.  Nothing is
     * injected into Human68k/game until SEND is tapped. */
    const int px=178, py=70, pw=752, ph=140;
    M5.Display.fillRoundRect(px,py,pw,ph,7,0x0000);
    M5.Display.drawRoundRect(px,py,pw,ph,7,0x5AEB);
    M5.Display.setTextColor(0xBDF7,0x0000);
    M5.Display.setTextSize(1);
    M5.Display.drawString("INPUT BUFFER",px+12,py+8);
    char preview[320]; softkbd_build_preview(preview,sizeof(preview));
    const size_t plen=std::strlen(preview);
    const char *shown=preview;
    if (plen > 84) shown=preview + (plen-84);
    char line1[44]={}, line2[44]={};
    std::strncpy(line1,shown,42); line1[42]='\0';
    if (std::strlen(shown)>42) { std::strncpy(line2,shown+42,42); line2[42]='\0'; }
    M5.Display.setTextColor(TFT_WHITE,0x0000);
    M5.Display.setTextSize(2);
    M5.Display.drawString(line1,px+14,py+44);
    M5.Display.drawString(line2,px+14,py+78);
    char cnt[32]; std::snprintf(cnt,sizeof(cnt),"%u / %u keys",(unsigned)s_soft_input_count,(unsigned)kSoftInputMax);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xBDF7,0x0000);
    M5.Display.drawString(cnt,px+14,py+118);

    const int sx=948, sy=70, sw=154, sh=140;
    const uint16_t send_fill=s_soft_input_count ? 0x03EF : 0x18E3;
    M5.Display.fillRoundRect(sx,sy,sw,sh,8,send_fill);
    M5.Display.drawRoundRect(sx,sy,sw,sh,8,s_soft_input_count?0x07FF:0x5AEB);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextColor(TFT_WHITE,send_fill);
    M5.Display.setTextSize(2);
    M5.Display.drawString("SEND",sx+sw/2,sy+sh/2-8);
    M5.Display.setTextSize(1);
    M5.Display.drawString("to X68000",sx+sw/2,sy+sh/2+22);
    M5.Display.setTextDatum(textdatum_t::top_left);

    for (size_t r=0;r<sizeof(kSoftRows)/sizeof(kSoftRows[0]);++r) {
        const soft_row_t &row=kSoftRows[r];
        for (size_t i=0;i<row.count;++i) {
            int x,y,w,h; softkbd_key_rect(row,i,(int)r,&x,&y,&w,&h);
            const soft_key_t &k=row.keys[i];
            bool active=(k.kind==SOFTKEY_SHIFT && s_softkbd_shift) ||
                        (k.kind==SOFTKEY_CTRL && s_softkbd_ctrl);
            const uint16_t fill=active ? 0x03EF : 0x18E3;
            const uint16_t border=active ? 0x07FF : 0x5AEB;
            M5.Display.fillRoundRect(x,y,w,h,5,fill);
            M5.Display.drawRoundRect(x,y,w,h,5,border);
            M5.Display.setTextDatum(textdatum_t::middle_center);
            M5.Display.setTextSize((std::strlen(k.label)>=5)?1:2);
            M5.Display.setTextColor(TFT_WHITE,fill);
            M5.Display.drawString(k.label,x+w/2,y+h/2);
        }
    }
    M5.Display.setTextDatum(textdatum_t::top_left);
    s_softkbd_redraw=false;
}

static void softkbd_buffer_key(const soft_key_t &k)
{
    /* BS edits the local composition buffer instead of being sent to the guest. */
    if (k.scancode==0x0Fu) {
        if (s_soft_input_count) --s_soft_input_count;
        s_softkbd_redraw=true;
        return;
    }
    if (s_soft_input_count >= kSoftInputMax) return;
    soft_input_event_t &e=s_soft_input[s_soft_input_count++];
    e.scancode=k.scancode;
    e.modifiers=(uint8_t)((s_softkbd_shift?1u:0u) | (s_softkbd_ctrl?2u:0u));
    e.label=k.label;
    s_softkbd_redraw=true;
}

static bool softkbd_send_buffer(void)
{
    const size_t original=s_soft_input_count;
    size_t sent=0;
    while (sent < s_soft_input_count) {
        const soft_input_event_t &e=s_soft_input[sent];
        if (!tab5_guest_input_queue_x68k_tap(e.scancode,e.modifiers)) break;
        ++sent;
    }
    if (sent) {
        const size_t remain=s_soft_input_count-sent;
        if (remain) std::memmove(s_soft_input,s_soft_input+sent,remain*sizeof(s_soft_input[0]));
        s_soft_input_count=remain;
    }
    s_softkbd_redraw=true;
    return original != 0 && s_soft_input_count == 0;
}

static void softkbd_close_to_guest(void)
{
    s_softkbd_active=false;
    s_softkbd_redraw=false;
    s_softkbd_touch_down=true;
    s_game_touch_suppress_until_release=true;
    s_force_game_redraw=true;
    s_aspect_seen_frame=nullptr;
    s_softkbd_shift=false;
    s_softkbd_ctrl=false;
}

static void softkbd_click(int x,int y)
{
    /* The physical side KEY button is also the close button while overlay owns LCD. */
    if (point_in_rect(x,y,31,207,98,68)) {
        softkbd_close_to_guest();
        return;
    }
    if (point_in_rect(x,y,948,70,154,140)) {
        /* Build 6.12q: SEND is also the natural "return to source" action.
         * Only close once the whole local buffer has been accepted by the
         * dedicated guest-send queue; a full queue leaves the remainder on
         * screen instead of silently losing keys. */
        if (softkbd_send_buffer()) softkbd_close_to_guest();
        return;
    }
    for (size_t r=0;r<sizeof(kSoftRows)/sizeof(kSoftRows[0]);++r) {
        const soft_row_t &row=kSoftRows[r];
        for (size_t i=0;i<row.count;++i) {
            int kx,ky,kw,kh; softkbd_key_rect(row,i,(int)r,&kx,&ky,&kw,&kh);
            if (!point_in_rect(x,y,kx,ky,kw,kh)) continue;
            const soft_key_t &k=row.keys[i];
            if (k.kind==SOFTKEY_SHIFT) { s_softkbd_shift=!s_softkbd_shift; s_softkbd_redraw=true; return; }
            if (k.kind==SOFTKEY_CTRL)  { s_softkbd_ctrl=!s_softkbd_ctrl; s_softkbd_redraw=true; return; }
            softkbd_buffer_key(k);
            return;
        }
    }
}

static uint16_t s_game_touch_key_prev = 0;

/* Touch controller dual role requested for X68K Tab:
 * D-pad remains JOY1 while also acting as X68000 cursor keys;
 * B remains TRG1 while also SPACE; A remains TRG2 while also RETURN.
 * Use the final shaped touch-joy state, so cursor repeat follows the same
 * deliberate touch repeat cadence instead of flooding the keyboard buffer. */
static void update_game_touch_keyboard(uint16_t joy)
{
    const uint16_t mask=(uint16_t)(JOY_UP|JOY_DOWN|JOY_LEFT|JOY_RIGHT|JOY_TRG1|JOY_TRG2);
    const uint16_t now=(uint16_t)(joy & mask);
    const uint16_t changed=(uint16_t)(now ^ s_game_touch_key_prev);
    struct Map { uint16_t bit; uint8_t scan; };
    static const Map kMap[] = {
        {JOY_UP,0x3C},{JOY_DOWN,0x3E},{JOY_LEFT,0x3B},{JOY_RIGHT,0x3D},
        {JOY_TRG1,0x35}, /* B = SPACE */
        {JOY_TRG2,0x1D}, /* A = RETURN */
    };
    for (const auto &m:kMap) {
        if (changed & m.bit)
            (void)tab5_guest_input_queue_x68k_scancode(m.scan,(now & m.bit)?1:0);
    }
    s_game_touch_key_prev=now;
}

static bool touch_transition_consume_pending(void)
{
    const uint32_t epoch = __atomic_load_n(&s_touch_transition_epoch, __ATOMIC_ACQUIRE);
    if (epoch == s_touch_transition_seen_epoch)
        return false;

    s_touch_transition_seen_epoch = epoch;
    s_touch_transition_target_w = __atomic_load_n(&s_touch_transition_pub_w, __ATOMIC_RELAXED);
    s_touch_transition_target_h = __atomic_load_n(&s_touch_transition_pub_h, __ATOMIC_RELAXED);
    s_touch_transition_target_pitch = __atomic_load_n(&s_touch_transition_pub_pitch, __ATOMIC_RELAXED);
    s_touch_transition_active = true;
    s_touch_transition_present_guard = true;
    s_touch_transition_release_stable = 0u;
    s_game_touch_suppress_until_release = true;
    s_utility_prev_mask = 0u;
    s_touch_dpad_prev = 0u;
    s_touch_dpad_press_us = 0;
    update_game_touch_keyboard(0);
    tab5_touch_joy_publish(0);
    ++s_touch_transition_arm_count;

    ESP_LOGI(TAG,
             "PX68K_TOUCHR57E3: transition fence ARM #%lu epoch=%lu target=%lux%lu pitch=%lu; touch actions held until matching present + release",
             (unsigned long)s_touch_transition_arm_count,
             (unsigned long)epoch,
             (unsigned long)s_touch_transition_target_w,
             (unsigned long)s_touch_transition_target_h,
             (unsigned long)s_touch_transition_target_pitch);
    return true;
}

static void touch_transition_present_complete(const present_request_t &req, bool success)
{
    if (!success || !s_touch_transition_active || !s_touch_transition_present_guard)
        return;
    if (s_touch_transition_target_w &&
        (req.width != s_touch_transition_target_w || req.height != s_touch_transition_target_h))
        return;

    /* A physical-present path for the new semantic screen has completed.
     * Touch is still suppressed until CPU0 observes a real release. */
    s_touch_transition_present_guard = false;
    s_touch_transition_release_stable = 0u;
}

using runtime_media_entry_t = tab5_media_ui_entry_t;
static constexpr size_t kRuntimeMediaMax = 128;
static runtime_media_entry_t *s_runtime_media = nullptr;
static size_t s_runtime_media_count = 0;
static EXT_RAM_BSS_ATTR char s_runtime_fdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static EXT_RAM_BSS_ATTR char s_runtime_fdd1[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static EXT_RAM_BSS_ATTR char s_runtime_hdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static EXT_RAM_BSS_ATTR char s_media_pending_fdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static EXT_RAM_BSS_ATTR char s_media_pending_fdd1[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static EXT_RAM_BSS_ATTR char s_media_pending_hdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static portMUX_TYPE s_runtime_media_mux = portMUX_INITIALIZER_UNLOCKED;

static void runtime_media_scan(void)
{
    if (!s_runtime_media) {
        s_runtime_media = static_cast<runtime_media_entry_t *>(heap_caps_calloc(
            kRuntimeMediaMax, sizeof(runtime_media_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_runtime_media)
            s_runtime_media = static_cast<runtime_media_entry_t *>(std::calloc(kRuntimeMediaMax, sizeof(runtime_media_entry_t)));
    }
    s_runtime_media_count = s_runtime_media ?
        tab5_media_ui_scan_sd_root(s_runtime_media, kRuntimeMediaMax) : 0;
    ESP_LOGI(TAG, "Runtime FILE scan (shared catalog): %u media file(s)", (unsigned)s_runtime_media_count);
}

static tab5_media_ui_slot_t runtime_ui_slot(void)
{
    return s_media_overlay_drive == 0 ? TAB5_MEDIA_UI_FDD0 :
           (s_media_overlay_drive == 1 ? TAB5_MEDIA_UI_FDD1 : TAB5_MEDIA_UI_HDD0);
}

static bool queue_ui_action(tab5_video_action_type_t type, const char *path)
{
    if (!s_ui_actions) return false;
    tab5_video_action_t a = {};
    a.type = type;
    if (path) std::snprintf(a.path, sizeof(a.path), "%s", path);
    return xQueueSend(s_ui_actions, &a, 0) == pdTRUE;
}

static void snapshot_runtime_media_to_pending(void)
{
    portENTER_CRITICAL(&s_runtime_media_mux);
    std::snprintf(s_media_pending_fdd0, sizeof(s_media_pending_fdd0), "%s", s_runtime_fdd0);
    std::snprintf(s_media_pending_fdd1, sizeof(s_media_pending_fdd1), "%s", s_runtime_fdd1);
    std::snprintf(s_media_pending_hdd0, sizeof(s_media_pending_hdd0), "%s", s_runtime_hdd0);
    s_media_overlay_boot_source = s_runtime_boot_source;
    portEXIT_CRITICAL(&s_runtime_media_mux);
}

static void close_media_overlay(void)
{
    s_media_overlay_active = false;
    s_media_overlay_redraw = false;
    s_media_overlay_touch_down = false;
    s_media_overlay_browser = false;
    s_utility_prev_mask = 0;
    /* Do not drop s_present_started here. Doing so reopened the runtime debug
     * status-banner path before the next game frame. Force a clean game redraw
     * separately while keeping status text suppressed. */
    s_force_game_redraw = true;
    s_aspect_seen_frame = nullptr;
    /* Never reinterpret the BACK press as a game-side control after close. */
    s_game_touch_suppress_until_release = true;
}

static void draw_runtime_media_setup_unlocked(void)
{
    const bool can_boot = s_media_overlay_boot_source == TAB5_LAUNCH_BOOT_HDD0 ?
                          s_media_pending_hdd0[0] != '\0' : s_media_pending_fdd0[0] != '\0';
    tab5_media_ui_setup_view_t view = {};
    view.back_label = nullptr;
    view.header_note = "FDD0 / FDD1 / HDD0 media changes apply immediately";
    view.floppy0 = s_media_pending_fdd0;
    view.floppy1 = s_media_pending_fdd1;
    view.hdd0 = s_media_pending_hdd0;
    view.boot_source = s_media_overlay_boot_source;
    view.bottom_left_label = "BACK";
    view.bottom_left_note = nullptr;
    view.bottom_left_enabled = 1;
    view.bottom_right_label = "(re)BOOT";
    view.bottom_right_note = nullptr;
    view.bottom_right_enabled = can_boot ? 1 : 0;
    view.footer_note = nullptr;
    tab5_media_ui_draw_setup(&view);
}

static void draw_runtime_media_browser_unlocked(void)
{
    const tab5_media_ui_slot_t slot = runtime_ui_slot();
    const size_t pages = tab5_media_ui_browser_pages(s_runtime_media, s_runtime_media_count, slot);
    if (s_media_overlay_page >= pages) s_media_overlay_page = pages - 1;
    tab5_media_ui_draw_browser(s_runtime_media, s_runtime_media_count, slot, s_media_overlay_page);
}

static void draw_media_overlay_unlocked(void)
{
    if (!s_media_overlay_active) return;
    if (s_media_overlay_browser) draw_runtime_media_browser_unlocked();
    else draw_runtime_media_setup_unlocked();
    s_media_overlay_redraw = false;
}

static char *pending_path_for_drive(int drive)
{
    return drive == 0 ? s_media_pending_fdd0 : (drive == 1 ? s_media_pending_fdd1 : s_media_pending_hdd0);
}

static bool queue_runtime_slot_change(int drive, const char *path)
{
    tab5_video_action_type_t type;
    if (path && path[0]) {
        type = drive == 0 ? TAB5_VIDEO_ACTION_MOUNT_FDD0 :
               (drive == 1 ? TAB5_VIDEO_ACTION_MOUNT_FDD1 : TAB5_VIDEO_ACTION_MOUNT_HDD0);
    } else {
        type = drive == 0 ? TAB5_VIDEO_ACTION_EJECT_FDD0 :
               (drive == 1 ? TAB5_VIDEO_ACTION_EJECT_FDD1 : TAB5_VIDEO_ACTION_EJECT_HDD0);
    }
    return queue_ui_action(type, path && path[0] ? path : nullptr);
}

static bool apply_runtime_media_selection(int drive, const char *path)
{
    if (drive < 0 || drive > 2) return false;
    const char *requested = path ? path : "";

    char current[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
    portENTER_CRITICAL(&s_runtime_media_mux);
    const char *runtime_ro = drive == 0 ? s_runtime_fdd0 :
                             (drive == 1 ? s_runtime_fdd1 : s_runtime_hdd0);
    std::snprintf(current, sizeof(current), "%s", runtime_ro);
    portEXIT_CRITICAL(&s_runtime_media_mux);
    if (!std::strcmp(current, requested)) {
        ESP_LOGI(TAG, "Runtime Media Setup immediate change: drive=%d unchanged (%s)",
                 drive, requested[0] ? requested : "<empty>");
        return true;
    }

    if (!queue_runtime_slot_change(drive, requested)) {
        ESP_LOGW(TAG, "Runtime Media Setup: immediate drive=%d change queue full", drive);
        return false;
    }

    /* The action is consumed by CPU1 at the next guest frame boundary. Mirror
     * the requested path into the UI state now; main.c publishes the actual
     * mounted path back after the action, including any mount failure. */
    portENTER_CRITICAL(&s_runtime_media_mux);
    char *runtime = drive == 0 ? s_runtime_fdd0 : (drive == 1 ? s_runtime_fdd1 : s_runtime_hdd0);
    std::snprintf(runtime, TAB5_VIDEO_MEDIA_PATH_MAX, "%s", requested);
    portEXIT_CRITICAL(&s_runtime_media_mux);

    ESP_LOGI(TAG, "Runtime Media Setup immediate change: drive=%d media=%s",
             drive, requested[0] ? requested : "<empty>");
    return true;
}

static bool queue_runtime_media_guest_reboot(void)
{
    const bool hdd_boot = s_media_overlay_boot_source == TAB5_LAUNCH_BOOT_HDD0;
    if (hdd_boot) {
        if (!s_media_pending_hdd0[0]) return false;
    } else {
        if (!s_media_pending_fdd0[0]) return false;
    }

    /* Build 6.12u: the running CPU1 task already owns the authoritative media
     * paths because CHANGE applies immediately. Queue only the desired boot
     * source; CPU1 performs a guest-only reset at the next frame boundary. */
    return queue_ui_action(TAB5_VIDEO_ACTION_REBOOT_GUEST, hdd_boot ? "HDD0" : "FDD0");
}

static void media_overlay_click(int x, int y)
{
    if (!s_media_overlay_browser) {
        switch (tab5_media_ui_setup_hit(x, y)) {
            case TAB5_MEDIA_UI_SETUP_BACK:
                /* Setup-level top-left BACK was removed in 6.12j. */
                return;
            case TAB5_MEDIA_UI_SETUP_BOOT_FDD0:
                s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                s_media_overlay_redraw = true;
                return;
            case TAB5_MEDIA_UI_SETUP_BOOT_HDD0:
                if (s_media_pending_hdd0[0]) s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_HDD0;
                s_media_overlay_redraw = true;
                return;
            case TAB5_MEDIA_UI_SETUP_CHANGE_FDD0:
                s_media_overlay_drive = 0; s_media_overlay_page = 0; s_media_overlay_browser = true; s_media_overlay_redraw = true; return;
            case TAB5_MEDIA_UI_SETUP_CHANGE_FDD1:
                s_media_overlay_drive = 1; s_media_overlay_page = 0; s_media_overlay_browser = true; s_media_overlay_redraw = true; return;
            case TAB5_MEDIA_UI_SETUP_CHANGE_HDD0:
                s_media_overlay_drive = 2; s_media_overlay_page = 0; s_media_overlay_browser = true; s_media_overlay_redraw = true; return;
            case TAB5_MEDIA_UI_SETUP_BOTTOM_LEFT:
                ESP_LOGI(TAG, "Runtime Media Setup: BACK to game");
                close_media_overlay();
                return;
            case TAB5_MEDIA_UI_SETUP_BOTTOM_RIGHT:
                if (queue_runtime_media_guest_reboot()) {
                    ESP_LOGI(TAG, "Runtime Media Setup: (re)BOOT guest reset queued (host stays alive)");
                    close_media_overlay();
                } else {
                    ESP_LOGW(TAG, "Runtime Media Setup: (re)BOOT rejected; select valid boot media");
                    s_media_overlay_redraw = true;
                }
                return;
            default:
                return;
        }
    }

    const tab5_media_ui_slot_t slot = runtime_ui_slot();
    size_t selected = 0;
    int empty = 0;
    switch (tab5_media_ui_browser_hit(s_runtime_media, s_runtime_media_count, slot,
                                      s_media_overlay_page, x, y, &selected, &empty)) {
        case TAB5_MEDIA_UI_BROWSER_BACK:
            s_media_overlay_browser = false;
            s_media_overlay_redraw = true;
            return;
        case TAB5_MEDIA_UI_BROWSER_SELECT: {
            char selected_path[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
            if (!empty) {
                const runtime_media_entry_t *e = tab5_media_ui_filtered_at(
                    s_runtime_media, s_runtime_media_count, slot, selected);
                if (!e) {
                    ESP_LOGW(TAG, "Runtime Media Setup: selected entry disappeared; rescanning");
                    runtime_media_scan();
                    s_media_overlay_redraw = true;
                    return;
                }
                std::snprintf(selected_path, sizeof(selected_path), "%s", e->path);
            }
            if (!apply_runtime_media_selection(s_media_overlay_drive, selected_path)) {
                s_media_overlay_redraw = true;
                return;
            }

            char *dst = pending_path_for_drive(s_media_overlay_drive);
            std::snprintf(dst, TAB5_VIDEO_MEDIA_PATH_MAX, "%s", selected_path);
            if (s_media_overlay_drive == 0 && dst[0]) s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
            if (s_media_overlay_drive == 2 && dst[0]) s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_HDD0;
            ESP_LOGI(TAG, "Runtime Media Setup selected/applied: drive=%d media=%s",
                     s_media_overlay_drive, dst[0] ? dst : "<empty>");
            s_media_overlay_browser = false;
            s_media_overlay_redraw = true;
            return;
        }
        case TAB5_MEDIA_UI_BROWSER_PREV:
            if (s_media_overlay_page > 0) --s_media_overlay_page;
            s_media_overlay_redraw = true;
            return;
        case TAB5_MEDIA_UI_BROWSER_NEXT:
            ++s_media_overlay_page;
            s_media_overlay_redraw = true;
            return;
        default:
            return;
    }
}

static uint16_t poll_game_controls(uint8_t sample_reason)
{
    if (!s_game_controls_enabled) return 0;
    const int w=M5.Display.width(), h=M5.Display.height();
    if (w<1200 || h<700) return 0;

    const int64_t touch_t0 = esp_timer_get_time();
    /* R57E12: update only the touch device.  M5.update() also walks generic
     * buttons/power helpers that the guest touch chrome does not use. */
    M5.Touch.update((uint32_t)(touch_t0 / 1000));
    s_touch_last_sample_us = touch_t0;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const uint32_t touch_us = (uint32_t)(esp_timer_get_time() - touch_t0);
    __atomic_add_fetch(&s_touch_update_count, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_touch_update_us, touch_us, __ATOMIC_RELAXED);
    uint32_t old_max = __atomic_load_n(&s_touch_update_max_us, __ATOMIC_RELAXED);
    while (touch_us > old_max &&
           !__atomic_compare_exchange_n(&s_touch_update_max_us, &old_max, touch_us,
                                        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (sample_reason == 1u) __atomic_add_fetch(&s_touch_irq_samples, 1u, __ATOMIC_RELAXED);
    else if (sample_reason == 2u) __atomic_add_fetch(&s_touch_active_samples, 1u, __ATOMIC_RELAXED);
    else __atomic_add_fetch(&s_touch_safety_samples, 1u, __ATOMIC_RELAXED);
#else
    (void)sample_reason;
#endif

    const size_t count=M5.Touch.getCount();
    bool any_pressed=false;
    for (size_t i=0;i<count;++i) {
        const auto td=M5.Touch.getDetail(i);
        if (td.isPressed()) { any_pressed=true; break; }
    }
    s_touch_contact_active=any_pressed;

    if (s_game_touch_suppress_until_release) {
        bool live=false;
        for (size_t i=0; i<count; ++i) {
            const auto t=M5.Touch.getDetail(i);
            if (t.isPressed()) { live=true; break; }
        }
        if (live) {
            s_touch_transition_release_stable=0u;
            return 0;
        }
        if (s_touch_transition_present_guard) {
            s_touch_transition_release_stable=0u;
            return 0;
        }
        if (s_touch_transition_active && ++s_touch_transition_release_stable < 2u)
            return 0;

        s_game_touch_suppress_until_release=false;
        s_utility_prev_mask=0;
        s_touch_dpad_prev=0;
        s_touch_dpad_press_us=0;
        if (s_touch_transition_active) {
            s_touch_transition_active=false;
            s_touch_transition_release_stable=0u;
            ++s_touch_transition_rearm_count;
            ESP_LOGI(TAG,
                     "PX68K_TOUCHR57E3: touch REARM #%lu after confirmed present + release",
                     (unsigned long)s_touch_transition_rearm_count);
        }
    }

    if (s_media_overlay_active) {
        bool have_pressed_contact=false;
        for (size_t i=0;i<count;++i) {
            const auto t=M5.Touch.getDetail(i);
            /* Only the touch-state bit means a finger is currently down.
             * wasReleased() is edge-only and leaves other non-pressed detail
             * states possible, so using !wasReleased() here can re-fire one
             * physical tap as a second click on the following sample. */
            if (!t.isPressed()) continue;
            have_pressed_contact=true;
            if (!s_media_overlay_touch_down) {
                s_media_overlay_touch_down=true;
                media_overlay_click((int)t.x,(int)t.y);
            }
            break;
        }
        if (!have_pressed_contact) s_media_overlay_touch_down=false;
        return 0;
    }

    if (s_softkbd_active) {
        bool have_pressed_contact=false;
        for (size_t i=0;i<count;++i) {
            const auto t=M5.Touch.getDetail(i);
            if (!t.isPressed()) continue;
            have_pressed_contact=true;
            if (!s_softkbd_touch_down) {
                s_softkbd_touch_down=true;
                softkbd_click((int)t.x,(int)t.y);
            }
            break;
        }
        if (!have_pressed_contact) s_softkbd_touch_down=false;
        return 0;
    }

    uint16_t joy=0;
    uint32_t util=0;
    bool have_pressed_contact=false;
    for (size_t i=0;i<count;++i) {
        const auto t=M5.Touch.getDetail(i);
        /* Build 6.12p: only isPressed() identifies a live finger.  The older
         * !wasReleased() test accepted non-pressed/no-change detail records
         * and could turn one physical tap into two UI actions. */
        if (!t.isPressed()) continue;
        have_pressed_contact=true;
        const int x=(int)t.x, y=(int)t.y;
        const game_touch_map_t mapped = map_game_touch_point(x, y, w);
        joy |= mapped.joy;
        util |= mapped.util;
    }

    /* No live contact means an unconditional release.  In particular, do not
     * carry a D-pad state across a M5Unified release detail frame. */
    if (!have_pressed_contact) { joy=0; util=0; }

    /* Touch D-pad repeat limiter.  A/B stay continuously held like a real
     * joystick, and USB JOY is untouched.  For directions: send 90 ms
     * immediately, pause until 330 ms, then repeat at 6.25 Hz with a 70 ms
     * ON pulse. This preserves taps while taming runaway menu navigation. */
    const uint16_t dpad_mask=(uint16_t)(JOY_UP|JOY_DOWN|JOY_LEFT|JOY_RIGHT);
    const uint16_t raw_dpad=(uint16_t)(joy & dpad_mask);
    joy=(uint16_t)(joy & ~dpad_mask);
    if (!raw_dpad) {
        s_touch_dpad_prev=0;
        s_touch_dpad_press_us=0;
    } else {
        const int64_t now_us=esp_timer_get_time();
        if (raw_dpad != s_touch_dpad_prev || !s_touch_dpad_press_us) {
            s_touch_dpad_prev=raw_dpad;
            s_touch_dpad_press_us=now_us;
        }
        const int64_t held_us=now_us-s_touch_dpad_press_us;
        bool gate=false;
        if (held_us < 90000) gate=true;
        else if (held_us >= 330000) {
            const int64_t phase=(held_us-330000)%160000;
            gate=phase < 70000;
        }
        if (gate) joy|=raw_dpad;
    }

    /* Apply the keyboard aliases after D-pad repeat shaping. */
    update_game_touch_keyboard(joy);

    const uint32_t rising=util & ~s_utility_prev_mask;
    s_utility_prev_mask=util;
    /* Build 6.12j: retain the proven 6.12h coarse volume ladder and touch
     * only M5Unified's master-volume value; never restart/reconfigure the
     * ES8388/I2S stream while a game is running. */
    if (rising & UTIL_VOLP) (void)tab5_audio_step_volume(+1);
    if (rising & UTIL_VOLM) (void)tab5_audio_step_volume(-1);
    if (rising & UTIL_KEYBOARD) {
        s_softkbd_active=true;
        s_softkbd_redraw=true;
        s_softkbd_touch_down=true; /* opening finger must be released first */
        s_softkbd_shift=false;
        s_softkbd_ctrl=false;
        s_soft_input_count=0;
        update_game_touch_keyboard(0);
        joy=0;
        tab5_touch_joy_publish(0);
    }
    if (rising & UTIL_PANIC) {
        /* Build 6.12s: never reboot the ESP32-P4 for the in-game PANIC button.
         * Select and stage the random PAN entirely on the host side, then pass
         * the concrete path to CPU1. CPU1 will reset only the emulated X68000
         * into Flash Human68k, preserving the already-running Tab5 audio host,
         * ES8388 and all ESP-IDF peripheral state. */
        char pan_path[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
        tab5_video_status("PANIC", "Preparing random PAN...");
        if (tab5_panic_prepare_random_runtime(pan_path, sizeof(pan_path))) {
            if (!queue_ui_action(TAB5_VIDEO_ACTION_PANIC_RANDOM, pan_path))
                ESP_LOGW(TAG, "In-game PANIC: action queue full after staging %s", pan_path);
        } else {
            ESP_LOGW(TAG, "In-game PANIC: no usable user .PAN file found");
        }
    }
    if (rising & UTIL_FILE) {
        runtime_media_scan();
        snapshot_runtime_media_to_pending();
        s_media_overlay_drive=0;
        s_media_overlay_page=0;
        s_media_overlay_browser=false;
        s_media_overlay_active=true;
        s_media_overlay_redraw=true;
        s_media_overlay_touch_down=true; /* require release before first menu click */
        joy=0;
        tab5_touch_joy_publish(0);
        ESP_LOGI(TAG,"Runtime FILE opened shared Media Setup: per-slot CHANGE applies immediately; BACK / (re)BOOT");
    }
    return joy;
}

/*
 * PANIC compatibility renderer
 * ----------------------------
 * The original standalone PanicPlayer used a direct reconstruction of the
 * X68000 packed 512-KiB GVRAM.  PANIC data such as PELSIA01 relies on the
 * 512-dot/256-colour two-page layout, while N_OHA also uses the sprite PCG
 * engine.  The normal PX68K ScrBuf renderer is retained for every non-PANIC
 * path; only PANIC mode takes this CPU0 compatibility presenter.
 * Layer8 Aug/17/2026
 */
static inline uint8_t panic_gvram_pixel_16(int page, int x, int y)
{
    page &= 3;
    const uint32_t sx = ((uint32_t)x + GrphScrollX[page]) & 0x1ffu;
    const uint32_t sy = ((uint32_t)y + GrphScrollY[page]) & 0x1ffu;
    const uint32_t off = (sy << 10) + (sx << 1) + (uint32_t)(page >> 1);
    const uint8_t raw = GVRAM[off];
    return (page & 1) ? (uint8_t)(raw >> 4) : (uint8_t)(raw & 0x0f);
}

static inline uint16_t panic_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint8_t panic_sprite_pixel(uint8_t pattern, int x, int y)
{
    const uint32_t raw = (uint32_t)pattern * 0x80u +
                         (uint32_t)(y & 15) * 4u +
                         (uint32_t)((x >> 1) & 3) +
                         ((x & 8) ? 0x40u : 0u);
    const uint8_t b = BG[raw & 0x7fffu];
    return (x & 1) ? (uint8_t)(b & 0x0f) : (uint8_t)(b >> 4);
}

static void panic_render_sprites(uint32_t draw_w, uint32_t draw_h)
{
    if (!s_panic_compat_frame) return;
    const int hstart = ((int)CRTC_Regs[0x04] << 8) | CRTC_Regs[0x05];
    const int vstart = ((int)CRTC_Regs[0x0c] << 8) | CRTC_Regs[0x0d];
    const int h_adjust = ((int)BG_Regs[0x0d] - (hstart + 4)) * 8;
    const int v_div = (BG_Regs[0x11] & 4u) ? 1 : 2;
    const int v_adjust = ((int)BG_Regs[0x0f] - vstart) / v_div;

    for (int pri = 1; pri <= 3; ++pri) {
        for (int n = 127; n >= 0; --n) {
            const uint8_t *sp = Sprite_Regs + (size_t)n * 8u;
            if ((sp[6] & 3u) != (uint8_t)pri) continue;
            const uint16_t posx = panic_le16(sp + 0) & 0x03ffu;
            const uint16_t posy = panic_le16(sp + 2) & 0x03ffu;
            const uint16_t ctrl = panic_le16(sp + 4);
            const int sx = (int)((posx + h_adjust) & 0x03ffu) - 16;
            const int sy = (int)posy + v_adjust - 16;
            if (sx >= (int)draw_w || sy >= (int)draw_h || sx + 16 <= 0 || sy + 16 <= 0) continue;

            const uint8_t pattern = (uint8_t)ctrl;
            const uint8_t palbase = (uint8_t)((ctrl >> 4) & 0xf0u);
            const bool flip_x = (ctrl & 0x4000u) != 0;
            const bool flip_y = (ctrl & 0x8000u) != 0;
            for (int py = 0; py < 16; ++py) {
                const int dy = sy + py;
                if ((unsigned)dy >= draw_h) continue;
                const int src_y = flip_y ? (15 - py) : py;
                uint16_t *dst = s_panic_compat_frame + (size_t)dy * kPanicCompatPitch;
                for (int px = 0; px < 16; ++px) {
                    const int dx = sx + px;
                    if ((unsigned)dx >= draw_w) continue;
                    const int src_x = flip_x ? (15 - px) : px;
                    const uint8_t pix = panic_sprite_pixel(pattern, src_x, src_y);
                    if (pix) dst[dx] = TextPal[(uint8_t)(palbase | pix)];
                }
            }
        }
    }
}

static bool panic_render_compat_source(const present_request_t &req, present_request_t *out)
{
    if (!s_panic_compat_enabled || !out) return false;
    if (!ensure_panic_compat_frame_allocated()) return false;
    uint32_t draw_w = req.width;
    uint32_t draw_h = req.height;
    if (!draw_w || !draw_h) return false;
    if (draw_w > 512u) draw_w = 512u;
    if (draw_h > 512u) draw_h = 512u;

    for (uint32_t y = 0; y < draw_h; ++y)
        std::memset(s_panic_compat_frame + (size_t)y * kPanicCompatPitch, 0, draw_w * sizeof(uint16_t));

    const uint8_t mode = (uint8_t)(VCReg0[1] & 3u);
    const uint8_t enable = VCReg2[1];
    bool graphics_supported = false;

    if ((mode == 1u || mode == 2u) && (enable & 0x0fu)) {
        uint32_t gx[4], gy[4];
        for (int i=0; i<4; ++i) { gx[i] = GrphScrollX[i] & 0x1ffu; gy[i] = GrphScrollY[i] & 0x1ffu; }
        const uint8_t pri = VCReg1[1];
        const bool p0_on = (enable & 0x01u) != 0;
        const bool p1_on = (enable & 0x04u) != 0;
        const bool p0_top = (pri & 3u) <= ((pri >> 4) & 3u);
        const bool p0_same_scroll = gx[0] == gx[1] && gy[0] == gy[1];
        const bool p1_same_scroll = gx[2] == gx[3] && gy[2] == gy[3];
        for (uint32_t y=0; y<draw_h; ++y) {
            uint16_t *dst = s_panic_compat_frame + (size_t)y * kPanicCompatPitch;
            const uint32_t row0 = ((y + gy[0]) & 0x1ffu) << 10;
            const uint32_t row1 = ((y + gy[1]) & 0x1ffu) << 10;
            const uint32_t row2 = ((y + gy[2]) & 0x1ffu) << 10;
            const uint32_t row3 = ((y + gy[3]) & 0x1ffu) << 10;
            for (uint32_t x=0; x<draw_w; ++x) {
                uint8_t p0=0, p1=0;
                if (p0_on) {
                    const uint32_t x0 = ((x + gx[0]) & 0x1ffu) << 1;
                    if (p0_same_scroll) p0 = GVRAM[row0 + x0];
                    else {
                        const uint32_t x1 = ((x + gx[1]) & 0x1ffu) << 1;
                        p0 = (uint8_t)((GVRAM[row0+x0] & 0x0fu) | (GVRAM[row1+x1] & 0xf0u));
                    }
                }
                uint8_t idx=0;
                if (p0_top && p0_on && p0) idx=p0;
                else {
                    if (p1_on) {
                        const uint32_t x2 = ((x + gx[2]) & 0x1ffu) << 1;
                        if (p1_same_scroll) p1 = GVRAM[row2 + x2 + 1u];
                        else {
                            const uint32_t x3 = ((x + gx[3]) & 0x1ffu) << 1;
                            p1 = (uint8_t)((GVRAM[row2+x2+1u] & 0x0fu) | (GVRAM[row3+x3+1u] & 0xf0u));
                        }
                    }
                    if (p0_top) idx = p0_on ? (p0 ? p0 : p1) : p1;
                    else idx = p1_on ? (p1 ? p1 : p0) : p0;
                }
                dst[x] = GrphPal[idx];
            }
        }
        graphics_supported = true;
    } else if (mode == 3u && (enable & 0x0fu)) {
        /* Build 6.12c: 65536-colour PANIC data must not fall through to the
         * sprite-only path.  Reproduce Grp_DrawLine16() byte-for-byte: one
         * 16-bit GVRAM pixel is converted through Pal16Adr/Pal_Regs/Pal16.
         * Keeping this on CPU0 avoids touching the proven normal WinDraw path. */
        const uint32_t gx = GrphScrollX[0] & 0x1ffu;
        for (uint32_t y=0; y<draw_h; ++y) {
            uint32_t sy = GrphScrollY[0] + y;
            if ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu) sy += y;
            sy &= 0x1ffu;
            uint16_t *dst = s_panic_compat_frame + (size_t)y * kPanicCompatPitch;
            for (uint32_t x=0; x<draw_w; ++x) {
                const uint32_t sx = (gx + x) & 0x1ffu;
                const uint32_t off = (sy << 10) + (sx << 1);
                const uint8_t lo = GVRAM[off];
                const uint8_t hi = GVRAM[off + 1u];
                if ((lo | hi) == 0u) {
                    dst[x] = 0;
                } else {
                    uint16_t pal = Pal_Regs[Pal16Adr[lo]];
                    pal |= (uint16_t)((uint16_t)Pal_Regs[Pal16Adr[hi] + 2u] << 8);
                    dst[x] = Pal16[pal];
                }
            }
        }
        graphics_supported = true;
    } else if (mode == 0u && !(VCReg0[1] & 0x04u) && (enable & 0x0fu)) {
        const uint8_t pri = VCReg1[1];
        for (uint32_t y=0; y<draw_h; ++y) {
            uint16_t *dst = s_panic_compat_frame + (size_t)y * kPanicCompatPitch;
            for (uint32_t x=0; x<draw_w; ++x) {
                uint8_t idx=0; bool have=false;
                for (int slot=3; slot>=0; --slot) {
                    if ((enable & (1u << slot)) == 0) continue;
                    const int page=(pri >> (slot*2)) & 3;
                    const uint8_t v=panic_gvram_pixel_16(page,(int)x,(int)y);
                    if (!have) { idx=v; have=true; } else if (v) idx=v;
                }
                dst[x]=GrphPal[idx];
            }
        }
        graphics_supported = true;
    }

    /* Sprite PCG is independent of the graphics-page mode and is required by
     * PANIC data such as N_OHA. Transparent sprite pixels leave graphics intact. */
    panic_render_sprites(draw_w, draw_h);

    if (s_panic_compat_last_w != draw_w || s_panic_compat_last_h != draw_h ||
        s_panic_compat_last_mode != mode || s_panic_compat_last_enable != enable) {
        ESP_LOGI(TAG,
                 "PANIC compat renderer: raw=%lux%lu VC0=%02X mode=%u VC1=%02X VC2=%02X graphics=%s",
                 (unsigned long)draw_w, (unsigned long)draw_h, (unsigned)VCReg0[1],
                 (unsigned)mode, (unsigned)VCReg1[1], (unsigned)enable,
                 graphics_supported ? "DIRECT-GVRAM" : "SPRITE/FALLBACK");
        s_panic_compat_last_w=draw_w; s_panic_compat_last_h=draw_h;
        s_panic_compat_last_mode=mode; s_panic_compat_last_enable=enable;
    }

    *out = req;
    out->frame = s_panic_compat_frame;
    out->width = draw_w;
    out->height = draw_h;
    out->pitch_pixels = kPanicCompatPitch;
    return graphics_supported || (enable & 0x10u) != 0u;
}

static inline void display_lock(void)
{
    if (s_display_mutex)
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
}

static inline void display_unlock(void)
{
    if (s_display_mutex)
        xSemaphoreGive(s_display_mutex);
}


#if defined(CONFIG_IDF_TARGET_ESP32P4)
static bool dbfb615h17_refresh_cb(esp_lcd_panel_handle_t panel,
                                  esp_lcd_dpi_panel_event_data_t *edata,
                                  void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    const uint32_t now = (uint32_t)esp_timer_get_time();
    const uint32_t prev = __atomic_exchange_n(&s_dsi_refresh_last_us, now, __ATOMIC_RELAXED);
    if (prev != 0u)
        __atomic_store_n(&s_dsi_refresh_period_us, now - prev, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_dsi_refresh_seq, 1u, __ATOMIC_RELEASE);

    BaseType_t hp = pdFALSE;
    if (s_dsi_refresh_event)
        xSemaphoreGiveFromISR(s_dsi_refresh_event, &hp);
    return hp == pdTRUE;
}

static bool dbfb615h17_gdma_done(async_memcpy_handle_t handle,
                                 async_memcpy_event_t *event,
                                 void *user_ctx)
{
    (void)handle; (void)event; (void)user_ctx;
    __atomic_store_n(&s_dsi_sync_pending, 0u, __ATOMIC_RELEASE);
    BaseType_t hp = pdFALSE;
    if (s_dsi_sync_done)
        xSemaphoreGiveFromISR(s_dsi_sync_done, &hp);
    return hp == pdTRUE;
}

static bool dbfb615h17_wait_refresh(void)
{
    if (!s_dsi_double_live || !s_dsi_swap_wait_refresh)
        return true;

    const int64_t t0 = esp_timer_get_time();
    uint32_t timeouts = 0;
    while (__atomic_load_n(&s_dsi_refresh_seq, __ATOMIC_ACQUIRE) == s_dsi_swap_refresh_seq)
    {
        if (!s_dsi_refresh_event ||
            xSemaphoreTake(s_dsi_refresh_event, pdMS_TO_TICKS(50)) != pdTRUE)
        {
            ++timeouts;
            ++s_dbfb_refresh_timeouts;
            if (timeouts == 1u)
                ESP_LOGW(TAG, "PX68K_DBFB615H17: waiting for physical DSI refresh_done after swap");
            if (timeouts >= 20u)
                return false;
        }
    }
    s_dbfb_refresh_wait_us += (uint64_t)(esp_timer_get_time() - t0);
    s_dsi_swap_wait_refresh = false;
    return true;
}

static bool dbfb615h17_wait_sync(void)
{
    if (!s_dsi_double_live || !__atomic_load_n(&s_dsi_sync_pending, __ATOMIC_ACQUIRE))
        return true;
    while (__atomic_load_n(&s_dsi_sync_pending, __ATOMIC_ACQUIRE))
    {
        if (!s_dsi_sync_done ||
            xSemaphoreTake(s_dsi_sync_done, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "PX68K_DBFB615H17: AXI-GDMA full-frame sync still pending");
        }
    }
    return true;
}

/* Build 6.15h17R6 ROTFIX
 * ------------------------
 * M5GFX's logical surface is 1280x720 after setRotation(1), but Panel_DSI
 * allocates the ESP-IDF framebuffer from the native panel geometry:
 * 720x1280 RGB565, stride 720.  The previous h17 code indexed these raw
 * pointers as 1280x720, which crossed physical scanlines at the wrong places.
 *
 * M5GFX rotation=1 maps a logical point (lx,ly) to native framebuffer:
 *     physical_x = 719 - ly
 *     physical_y = lx
 * This helper layer keeps all raw-DSI access in that native layout. */
static inline bool dbfb615h17r6_logical_rect_valid(uint32_t x0, uint32_t y0,
                                                   uint32_t x1, uint32_t y1)
{
    return x1 > x0 && y1 > y0 &&
           x1 <= kDsiPhysHeight && y1 <= kDsiPhysWidth;
}

static inline uint16_t *dbfb615h17r6_pixel_ptr(uint16_t *fb,
                                               uint32_t logical_x,
                                               uint32_t logical_y)
{
    return fb + (size_t)logical_x * kDsiPhysStridePixels +
           (size_t)(kDsiPhysWidth - 1u - logical_y);
}

static bool dbfb615h17r6_c2m_aligned(void *ptr, size_t bytes)
{
    if (!ptr || !bytes)
        return true;

    /* Match M5GFX Panel_FrameBufferBase::cacheWriteBack(): it rounds PSRAM
     * writeback ranges to 128-byte boundaries on ESP32-P4.  128 also safely
     * satisfies this project's current 64-byte L2 line setting. */
    constexpr uintptr_t kCacheLine = 128u;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t aligned_begin = begin & ~(kCacheLine - 1u);
    const uintptr_t end = begin + bytes;
    const uintptr_t aligned_end = (end + kCacheLine - 1u) & ~(kCacheLine - 1u);
    return esp_cache_msync(reinterpret_cast<void *>(aligned_begin),
                           aligned_end - aligned_begin,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M) == ESP_OK;
}

static bool dbfb615h17_copy_front_to_back_rect(uint32_t x0, uint32_t y0,
                                               uint32_t x1, uint32_t y1,
                                               bool full_sync)
{
    if (!s_dsi_double_live ||
        !dbfb615h17r6_logical_rect_valid(x0, y0, x1, y1))
        return false;
    if (!dbfb615h17_wait_refresh())
        return false;
    if (!dbfb615h17_wait_sync())
        return false;

    uint16_t *front = s_dsi_fb[s_dsi_front_idx];
    uint16_t *back  = s_dsi_fb[s_dsi_back_idx];
    if (!front || !back)
        return false;

    const int64_t t0 = esp_timer_get_time();
    size_t bytes = 0;

    /* Full sync is naturally contiguous in the native 720x1280 memory layout. */
    if (full_sync && x0 == 0u && y0 == 0u &&
        x1 == kDsiPhysHeight && y1 == kDsiPhysWidth)
    {
        bytes = (size_t)kDsiPhysWidth * kDsiPhysHeight * sizeof(uint16_t);
        tab5_pie_graphics_copy(back, front, (uint32_t)bytes);
        if (!dbfb615h17r6_c2m_aligned(back, bytes))
        {
            ESP_LOGW(TAG,
                     "PX68K_DBFB615H17R6: full native PIE coherence writeback failed bytes=%u; swap suppressed",
                     (unsigned)bytes);
            return false;
        }
    }
    else
    {
        /* Logical rectangle -> native rectangle for rotation=1:
         * native rows [x0,x1), native columns [720-y1,720-y0).
         * Each native row is contiguous, so repair remains PIE-friendly. */
        const uint32_t px0 = kDsiPhysWidth - y1;
        const uint32_t px1 = kDsiPhysWidth - y0;
        const size_t row_bytes = (size_t)(px1 - px0) * sizeof(uint16_t);
        bytes = (size_t)(x1 - x0) * row_bytes;

        for (uint32_t py = x0; py < x1; ++py)
        {
            uint16_t *src = front + (size_t)py * kDsiPhysStridePixels + px0;
            uint16_t *dst = back  + (size_t)py * kDsiPhysStridePixels + px0;
            tab5_pie_graphics_copy(dst, src, (uint32_t)row_bytes);
        }

        /* Write back only the cache lines touched by the narrow native strip.
         * A single bounding msync would unnecessarily write ~1.38 MiB even
         * when only a few logical rows changed. */
        for (uint32_t py = x0; py < x1; ++py)
        {
            uint16_t *dst = back + (size_t)py * kDsiPhysStridePixels + px0;
            if (!dbfb615h17r6_c2m_aligned(dst, row_bytes))
            {
                ESP_LOGW(TAG,
                         "PX68K_DBFB615H17R6: repair C2M failed native_row=%lu logical=(%lu,%lu)-(%lu,%lu); swap suppressed",
                         (unsigned long)py,
                         (unsigned long)x0, (unsigned long)y0,
                         (unsigned long)x1, (unsigned long)y1);
                return false;
            }
        }
    }

    s_dbfb_sync_us += (uint64_t)(esp_timer_get_time() - t0);
    s_dbfb_sync_bytes += bytes;
    ++s_dbfb_sync_count;
    ++s_dbfb_sync_fallback;
    if (full_sync) ++s_dbfb_sync_full_count;
    else ++s_dbfb_sync_repair_count;
    return true;
}

static bool dbfb615h17_prepare_back(void)
{
    if (!s_dsi_double_live)
        return false;
    if (!dbfb615h17_wait_refresh())
        return false;

    if (s_dsi_need_full_sync)
    {
        if (!dbfb615h17_copy_front_to_back_rect(
                0u, 0u, kDsiPhysHeight, kDsiPhysWidth, true))
            return false;
        s_dsi_need_full_sync = false;
        s_dsi_repair_pending = false;
        return true;
    }

    if (s_dsi_repair_pending)
    {
        const uint32_t x0 = s_dsi_repair_x0;
        const uint32_t y0 = s_dsi_repair_y0;
        const uint32_t x1 = s_dsi_repair_x1;
        const uint32_t y1 = s_dsi_repair_y1;
        if (!dbfb615h17_copy_front_to_back_rect(x0, y0, x1, y1, false))
            return false;
        s_dsi_repair_pending = false;
    }
    return true;
}

static bool dbfb615h17_flush_back_rect(uint32_t x0, uint32_t y0,
                                       uint32_t x1, uint32_t y1)
{
    if (!s_dsi_double_live ||
        !dbfb615h17r6_logical_rect_valid(x0, y0, x1, y1))
        return false;
    uint16_t *back = s_dsi_fb[s_dsi_back_idx];
    if (!back)
        return false;

    const int64_t t0 = esp_timer_get_time();
    size_t bytes = 0;
    esp_err_t er = ESP_OK;

    if (x0 == 0u && y0 == 0u &&
        x1 == kDsiPhysHeight && y1 == kDsiPhysWidth)
    {
        bytes = (size_t)kDsiPhysWidth * kDsiPhysHeight * sizeof(uint16_t);
        if (!dbfb615h17r6_c2m_aligned(back, bytes))
            er = ESP_FAIL;
    }
    else
    {
        const uint32_t px0 = kDsiPhysWidth - y1;
        const uint32_t px1 = kDsiPhysWidth - y0;
        const size_t row_bytes = (size_t)(px1 - px0) * sizeof(uint16_t);
        bytes = (size_t)(x1 - x0) * row_bytes;
        for (uint32_t py = x0; py < x1; ++py)
        {
            uint16_t *dst = back + (size_t)py * kDsiPhysStridePixels + px0;
            if (!dbfb615h17r6_c2m_aligned(dst, row_bytes))
            {
                er = ESP_FAIL;
                break;
            }
        }
    }

    s_dbfb_flush_us += (uint64_t)(esp_timer_get_time() - t0);
    s_dbfb_flush_bytes += bytes;
    ++s_dbfb_flush_count;
    if (er != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "PX68K_DBFB615H17R6: Back native-layout cache writeback failed logical=(%lu,%lu)-(%lu,%lu)",
                 (unsigned long)x0, (unsigned long)y0,
                 (unsigned long)x1, (unsigned long)y1);
        return false;
    }
    return true;
}

static bool dbfb615h17_swap_back_to_front(uint32_t changed_x0, uint32_t changed_y0,
                                          uint32_t changed_x1, uint32_t changed_y1)
{
    if (!s_dsi_double_live || !s_dsi_dpi_panel)
        return false;

    uint16_t *back = s_dsi_fb[s_dsi_back_idx];
    if (!back)
        return false;

    if (!dbfb615h17_flush_back_rect(
            changed_x0, changed_y0, changed_x1, changed_y1))
        return false;

    /* IMPORTANT: esp_lcd_panel_draw_bitmap() talks to the native DPI panel,
     * not M5GFX's rotated logical surface.  Passing 1280x720 here was another
     * h17 layout bug.  A driver-owned framebuffer pointer plus the native
     * 720x1280 bounds requests a zero-copy DMA source switch. */
    const esp_err_t er = esp_lcd_panel_draw_bitmap(
        s_dsi_dpi_panel,
        0, 0, (int)kDsiPhysWidth, (int)kDsiPhysHeight, back);
    if (er != ESP_OK)
    {
        ESP_LOGW(TAG, "PX68K_DBFB615H17R6: native framebuffer swap failed err=%d; disabling DBFB", (int)er);
        s_dsi_double_live = false;
        return false;
    }

    const uint8_t old_front = s_dsi_front_idx;
    s_dsi_front_idx = s_dsi_back_idx;
    s_dsi_back_idx = old_front;

    s_dsi_swap_refresh_seq = __atomic_load_n(&s_dsi_refresh_seq, __ATOMIC_ACQUIRE);
    s_dsi_swap_wait_refresh = true;

    s_dsi_repair_x0 = changed_x0;
    s_dsi_repair_y0 = changed_y0;
    s_dsi_repair_x1 = changed_x1;
    s_dsi_repair_y1 = changed_y1;
    s_dsi_repair_pending = changed_x1 > changed_x0 && changed_y1 > changed_y0;
    ++s_dbfb_swap_count;
    return true;
}

/* Build 6.15h17R11 PPA-ROT band-list presenter
 * -----------------------------------------------
 * R7 proved the native-layout/PPA direction but collapsed every changed row
 * to one min..max band. Two tiny changes far apart could therefore rotate
 * hundreds of untouched lines. R9 carries exact dirty-band lists across the
 * two-buffer cadence and rotates only the merged union of frame N-1 + N.
 *
 * The in-game side chrome is drawn by M5GFX into FB0. On a UI/full-sync
 * transition R9 clones that complete native front once into Back, then clears
 * only the 960x720 game viewport before PPA paints it. Side controls therefore
 * survive on both physical framebuffers instead of being erased by the old R7
 * full-screen memset. */
static bool dbfb615h17r9_ensure_ppa_client()
{
    if (s_dsi_ppa_srm)
        return true;

    ppa_client_config_t ppa_cfg = {};
    ppa_cfg.oper_type = PPA_OPERATION_SRM;
    ppa_cfg.max_pending_trans_num = 1;
    /* R33 bus-fairness A/B: R32 removed per-frame DPI FB switching and still
     * measured ~22.5 ms/f while DIRECT-FRONT was active.  The remaining PPA
     * transactions average only ~5.3 ms, so test whether 64-byte SRM bursts
     * monopolize shared PSRAM/AXI long enough to stall CPU1.  Revert only the
     * PPA burst quantum to the proven R9 32-byte setting; all R32 direct-front
     * logic and dirty-band policy remain unchanged. */
    ppa_cfg.data_burst_length = PPA_DATA_BURST_LENGTH_32;
    const esp_err_t er = ppa_register_client(&ppa_cfg, &s_dsi_ppa_srm);
    if (er != ESP_OK || !s_dsi_ppa_srm)
    {
        ESP_LOGW(TAG,
                 "PX68K_DBFB615H17R11: px68k_lcd PPA SRM client registration failed err=%d; disabling raw DBFB",
                 (int)er);
        s_dsi_ppa_srm = nullptr;
        s_dsi_ppa_live = false;
        s_dsi_double_live = false;
        return false;
    }

    ESP_LOGI(TAG,
             "PX68K_LCDR34: warmup/fallback PPA SRM client; mode=BLOCKING burst=32B maxPending=1 exactBands=%u",
             (unsigned)kDbfbPpaSubmitCap);
    return true;
}

static uint32_t dbfb615h17r9_merge_bands(const dbfb_dirty_band_t *current,
                                         uint32_t current_count,
                                         uint32_t pic_h,
                                         dbfb_dirty_band_t *out,
                                         bool force_full)
{
    if (!out || pic_h == 0u)
        return 0u;
    if (force_full)
    {
        out[0] = { 0u, (uint16_t)pic_h };
        return 1u;
    }

    dbfb_dirty_band_t tmp[kDbfbDirtyBandCap * 2] = {};
    uint32_t n = 0u;
    for (uint32_t i = 0; i < s_dsi_ppa_prev_band_count && n < kDbfbDirtyBandCap * 2; ++i)
        tmp[n++] = s_dsi_ppa_prev_bands[i];
    for (uint32_t i = 0; i < current_count && n < kDbfbDirtyBandCap * 2; ++i)
        tmp[n++] = current[i];
    if (n == 0u)
        return 0u;

    std::sort(tmp, tmp + n, [](const dbfb_dirty_band_t &a, const dbfb_dirty_band_t &b) {
        return a.y0 < b.y0 || (a.y0 == b.y0 && a.y1 < b.y1);
    });

    uint32_t m = 0u;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t y0 = std::min<uint32_t>(tmp[i].y0, pic_h);
        uint32_t y1 = std::min<uint32_t>(tmp[i].y1, pic_h);
        if (y1 <= y0)
            continue;
        if (m && y0 <= (uint32_t)out[m - 1u].y1 + kDbfbPpaMergeGapRows)
        {
            if (y1 > out[m - 1u].y1)
                out[m - 1u].y1 = (uint16_t)y1;
        }
        else
        {
            out[m++] = { (uint16_t)y0, (uint16_t)y1 };
        }
    }

    /* Keep the per-frame transaction count bounded. If the image is unusually fragmented,
     * repeatedly merge the closest pair rather than reverting to a single
     * top-to-bottom bounding rectangle. */
    while (m > kDbfbPpaSubmitCap)
    {
        uint32_t best = 0u;
        uint32_t best_gap = 0xffffffffu;
        for (uint32_t i = 0; i + 1u < m; ++i)
        {
            const uint32_t gap = out[i + 1u].y0 > out[i].y1
                ? (uint32_t)out[i + 1u].y0 - out[i].y1 : 0u;
            if (gap < best_gap)
            {
                best_gap = gap;
                best = i;
            }
        }
        out[best].y1 = out[best + 1u].y1;
        for (uint32_t i = best + 1u; i + 1u < m; ++i)
            out[i] = out[i + 1u];
        --m;
    }

    /* R11 empirical cost model: R9 averaged ~2.3 blocking PPA calls/frame.
     * The call/setup overhead dominated enough that transferring a modest clean
     * gap is cheaper than launching another SRM transaction.  If the final two
     * bands are separated by <=48 rows, collapse them to one transaction.
     * Larger gaps stay split so sparse top/bottom motion does not regress to
     * R7's huge min..max rectangle. */
    if (m == 2u)
    {
        const uint32_t gap = out[1].y0 > out[0].y1
            ? (uint32_t)out[1].y0 - out[0].y1 : 0u;
        if (gap <= 48u)
        {
            out[0].y1 = out[1].y1;
            m = 1u;
        }
    }
    return m;
}

static bool dbfb615h17r9_clear_game_window(uint16_t *back)
{
    if (!back)
        return false;
    /* Native rows correspond to logical X for rotation=1. The permanent
     * 160-pixel side bars therefore live outside native rows 160..1119. */
    constexpr uint32_t game_native_row0 = (kDsiPhysHeight - kAspectViewportWidth) / 2u;
    const size_t game_bytes = (size_t)kAspectViewportWidth *
                              (size_t)kDsiPhysWidth * sizeof(uint16_t);
    uint16_t *game = back + (size_t)game_native_row0 * kDsiPhysStridePixels;
    std::memset(game, 0, game_bytes);
    return dbfb615h17r6_c2m_aligned(game, game_bytes);
}

#if defined(__riscv)
/* a0 = first native destination row at the tile's leftmost native X
 * a1 = 8x8 transposed RGB565 tile, laid out as eight contiguous 16-byte rows.
 * Native DSI stride is fixed at 720 RGB565 pixels = 1440 bytes.  VST.128.IP
 * advances by 16 bytes; add the remaining 1424 bytes to reach the same X in
 * the next native row.  This keeps every external-RAM store a full aligned
 * 128-bit transaction rather than 64 scattered halfword stores. */
__asm__(
    ".section .iram1,\"ax\",@progbits\n"
    ".global tab5_r34_store8x8_native\n"
    ".type tab5_r34_store8x8_native, @function\n"
    ".balign 4\n"
    "tab5_r34_store8x8_native:\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a0, a0, 1424\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a0, a0, 1424\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a0, a0, 1424\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a0, a0, 1424\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a0, a0, 1424\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a0, a0, 1424\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a0, a0, 1424\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "ret\n"
    ".size tab5_r34_store8x8_native, .-tab5_r34_store8x8_native\n"
    ".previous\n"
);
extern "C" void tab5_r34_store8x8_native(uint16_t *dst, const uint16_t *tile);
#else
static void tab5_r34_store8x8_native(uint16_t *dst, const uint16_t *tile)
{
    for (uint32_t r = 0; r < 8u; ++r)
        std::memcpy(dst + (size_t)r * kDsiPhysStridePixels,
                    tile + (size_t)r * 8u, 16u);
}
#endif

/* Hot tile kernel.  Source is landscape staging.  For rotation=1 / PPA-270
 * equivalence:
 *     native_x = 719 - logical_y
 *     native_y = logical_x
 * For an 8-row source tile, ascending native X therefore consumes source Y
 * in reverse order.  The small transpose scratch is Internal DRAM and shared
 * only by the single px68k_lcd owner task. */
static IRAM_ATTR __attribute__((hot, optimize("O3"))) void
r34_swrot_tile8(uint16_t *front, const uint16_t *stage,
                uint32_t pic_w, uint32_t logical_x, uint32_t logical_y,
                uint32_t sx, uint32_t sy)
{
    uint16_t *t = s_r34_tile;
    const uint16_t *s7 = stage + (size_t)(sy + 7u) * pic_w + sx;
    const uint16_t *s6 = s7 - pic_w;
    const uint16_t *s5 = s6 - pic_w;
    const uint16_t *s4 = s5 - pic_w;
    const uint16_t *s3 = s4 - pic_w;
    const uint16_t *s2 = s3 - pic_w;
    const uint16_t *s1 = s2 - pic_w;
    const uint16_t *s0 = s1 - pic_w;

    for (uint32_t x = 0; x < 8u; ++x)
    {
        uint16_t *o = t + (size_t)x * 8u;
        o[0] = s7[x]; o[1] = s6[x]; o[2] = s5[x]; o[3] = s4[x];
        o[4] = s3[x]; o[5] = s2[x]; o[6] = s1[x]; o[7] = s0[x];
    }

    const uint32_t native_x0 = kDsiPhysWidth - (logical_y + sy + 8u);
    uint16_t *dst = front + (size_t)(logical_x + sx) * kDsiPhysStridePixels + native_x0;
    tab5_r34_store8x8_native(dst, t);
}

/* R35 stripe kernel: source PSRAM is touched only by one sequential
 * 8-row copy.  All transpose gathers thereafter hit Internal SRAM. */
static IRAM_ATTR __attribute__((hot, optimize("O3"))) void
r35_swrot_tile8_packed(uint16_t *front, const uint16_t *packed,
                       uint32_t pic_w, uint32_t logical_x, uint32_t logical_y,
                       uint32_t sx, uint32_t original_y)
{
    uint16_t *t = s_r34_tile;
    const uint16_t *s7 = packed + (size_t)7u * pic_w + sx;
    const uint16_t *s6 = s7 - pic_w;
    const uint16_t *s5 = s6 - pic_w;
    const uint16_t *s4 = s5 - pic_w;
    const uint16_t *s3 = s4 - pic_w;
    const uint16_t *s2 = s3 - pic_w;
    const uint16_t *s1 = s2 - pic_w;
    const uint16_t *s0 = s1 - pic_w;

    for (uint32_t x = 0; x < 8u; ++x)
    {
        uint16_t *o = t + (size_t)x * 8u;
        o[0] = s7[x]; o[1] = s6[x]; o[2] = s5[x]; o[3] = s4[x];
        o[4] = s3[x]; o[5] = s2[x]; o[6] = s1[x]; o[7] = s0[x];
    }

    const uint32_t native_x0 = kDsiPhysWidth - (logical_y + original_y + 8u);
    uint16_t *dst = front + (size_t)(logical_x + sx) * kDsiPhysStridePixels + native_x0;
    tab5_r34_store8x8_native(dst, t);
}

static IRAM_ATTR __attribute__((hot, optimize("O3"))) uint32_t
r35_swrot_band(uint16_t *front, const uint16_t *stage,
               uint32_t pic_w, uint32_t logical_x, uint32_t logical_y,
               uint32_t y0, uint32_t y1, uint32_t *scalar_pixels,
               uint32_t *pack_us_out, uint32_t *pack_stripes_out)
{
    uint32_t tiles = 0u;
    uint32_t scalars = 0u;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    uint32_t pack_us = 0u;
    uint32_t pack_stripes = 0u;
#else
    (void)pack_us_out;
    (void)pack_stripes_out;
#endif
    uint32_t y = y0;

    while (y < y1 && ((logical_y + y) & 7u) != 0u)
    {
        const uint32_t nx = kDsiPhysWidth - 1u - (logical_y + y);
        const uint16_t *src = stage + (size_t)y * pic_w;
        for (uint32_t x = 0; x < pic_w; ++x)
            front[(size_t)(logical_x + x) * kDsiPhysStridePixels + nx] = src[x];
        scalars += pic_w;
        ++y;
    }

    const bool stripe_ok = s_r35_pack8 &&
                           (((uintptr_t)stage & 15u) == 0u) &&
                           (((uintptr_t)s_r35_pack8 & 15u) == 0u) &&
                           ((pic_w & 7u) == 0u) &&
                           (((pic_w * sizeof(uint16_t)) & 15u) == 0u);
    if (stripe_ok)
    {
        const uint32_t stripe_bytes = pic_w * 8u * sizeof(uint16_t);
        while (y + 8u <= y1)
        {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            const int64_t p0 = esp_timer_get_time();
#endif
            tab5_pie_graphics_copy(s_r35_pack8,
                                   stage + (size_t)y * pic_w,
                                   stripe_bytes);
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            pack_us += (uint32_t)(esp_timer_get_time() - p0);
            ++pack_stripes;
#endif

            for (uint32_t x = 0; x < pic_w; x += 8u)
            {
                r35_swrot_tile8_packed(front, s_r35_pack8, pic_w,
                                       logical_x, logical_y, x, y);
                ++tiles;
            }
            y += 8u;
        }
    }

    while (y < y1)
    {
        const uint32_t nx = kDsiPhysWidth - 1u - (logical_y + y);
        const uint16_t *src = stage + (size_t)y * pic_w;
        for (uint32_t x = 0; x < pic_w; ++x)
            front[(size_t)(logical_x + x) * kDsiPhysStridePixels + nx] = src[x];
        scalars += pic_w;
        ++y;
    }

    if (scalar_pixels) *scalar_pixels += scalars;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    if (pack_us_out) *pack_us_out += pack_us;
    if (pack_stripes_out) *pack_stripes_out += pack_stripes;
#endif
    return tiles;
}

static IRAM_ATTR __attribute__((hot, optimize("O3"))) uint32_t
r34_swrot_band(uint16_t *front, const uint16_t *stage,
               uint32_t pic_w, uint32_t logical_x, uint32_t logical_y,
               uint32_t y0, uint32_t y1, uint32_t *scalar_pixels)
{
    uint32_t tiles = 0u;
    uint32_t scalars = 0u;
    uint32_t y = y0;

    /* Prefix until the destination native-X is 16-byte aligned. */
    while (y < y1 && ((logical_y + y) & 7u) != 0u)
    {
        const uint32_t nx = kDsiPhysWidth - 1u - (logical_y + y);
        const uint16_t *src = stage + (size_t)y * pic_w;
        for (uint32_t x = 0; x < pic_w; ++x)
            front[(size_t)(logical_x + x) * kDsiPhysStridePixels + nx] = src[x];
        scalars += pic_w;
        ++y;
    }

    const bool tile_ok = (((uintptr_t)stage & 15u) == 0u) &&
                         ((pic_w & 7u) == 0u) &&
                         (((pic_w * sizeof(uint16_t)) & 15u) == 0u);
    if (tile_ok)
    {
        while (y + 8u <= y1)
        {
            for (uint32_t x = 0; x < pic_w; x += 8u)
            {
                r34_swrot_tile8(front, stage, pic_w, logical_x, logical_y, x, y);
                ++tiles;
            }
            y += 8u;
        }
    }

    /* Scalar tail, or entire band if the geometry is not tile-eligible. */
    while (y < y1)
    {
        const uint32_t nx = kDsiPhysWidth - 1u - (logical_y + y);
        const uint16_t *src = stage + (size_t)y * pic_w;
        for (uint32_t x = 0; x < pic_w; ++x)
            front[(size_t)(logical_x + x) * kDsiPhysStridePixels + nx] = src[x];
        scalars += pic_w;
        ++y;
    }

    if (scalar_pixels) *scalar_pixels += scalars;
    return tiles;
}

static bool r34_sync_front_band(uint16_t *front,
                                uint32_t pic_w, uint32_t logical_x,
                                uint32_t logical_y, uint32_t y0, uint32_t y1)
{
    if (!front || y1 <= y0)
        return true;
    const uint32_t px0 = kDsiPhysWidth - (logical_y + y1);
    const uint32_t px1 = kDsiPhysWidth - (logical_y + y0);
    if (px1 <= px0)
        return true;

    /* R6 issued one msync per one of ~960 native rows.  R34 batches 64 rows
     * into one aligned writeback range.  The range includes clean cache lines
     * between rows, but only CPU-dirty lines need actual writeback; the key
     * experiment is eliminating the pathological 960-call control overhead. */
    constexpr uint32_t kSyncNativeRows = 64u;
    const uint32_t py1 = logical_x + pic_w;
    for (uint32_t py0 = logical_x; py0 < py1; py0 += kSyncNativeRows)
    {
        const uint32_t pend = std::min<uint32_t>(py0 + kSyncNativeRows, py1);
        uint16_t *begin = front + (size_t)py0 * kDsiPhysStridePixels + px0;
        uint16_t *end = front + (size_t)(pend - 1u) * kDsiPhysStridePixels + px1;
        if (!dbfb615h17r6_c2m_aligned(begin, (size_t)(end - begin) * sizeof(uint16_t)))
            return false;
    }
    return true;
}

static bool r34_swrot_selfcheck(void)
{
    uint16_t *dst = static_cast<uint16_t *>(heap_caps_aligned_alloc(
        16, (size_t)8u * kDsiPhysStridePixels * sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!dst)
        return false;

    alignas(16) uint16_t src[64] = {};
    for (uint32_t y = 0; y < 8u; ++y)
        for (uint32_t x = 0; x < 8u; ++x)
            src[(size_t)y * 8u + x] = (uint16_t)(0x1000u + y * 0x10u + x);

    std::memset(dst, 0xA5, (size_t)8u * kDsiPhysStridePixels * sizeof(uint16_t));
    /* Build exactly the same transposed tile used by the live kernel. */
    for (uint32_t x = 0; x < 8u; ++x)
    {
        uint16_t *o = s_r34_tile + (size_t)x * 8u;
        for (uint32_t j = 0; j < 8u; ++j)
            o[j] = src[(size_t)(7u - j) * 8u + x];
    }
    tab5_r34_store8x8_native(dst, s_r34_tile);

    bool ok = true;
    for (uint32_t x = 0; x < 8u && ok; ++x)
        for (uint32_t j = 0; j < 8u; ++j)
            if (dst[(size_t)x * kDsiPhysStridePixels + j] !=
                src[(size_t)(7u - j) * 8u + x])
            {
                ok = false;
                break;
            }
    heap_caps_free(dst);
    return ok;
}

static bool r34_direct_front_present(const uint16_t *stage,
                                     uint32_t pic_w, uint32_t pic_h,
                                     uint32_t logical_x, uint32_t logical_y,
                                     const dbfb_dirty_band_t *current_bands,
                                     uint32_t current_band_count,
                                     bool force_full)
{
#if !PX68K_TAB5_R34_SWROT_FRONT
    (void)stage; (void)pic_w; (void)pic_h; (void)logical_x; (void)logical_y;
    (void)current_bands; (void)current_band_count; (void)force_full;
    return false;
#else
    if (!s_r32_direct_front || !s_r34_selfcheck_ok || !stage ||
        !current_bands || current_band_count == 0u)
        return false;

    const bool geom_changed = s_dsi_ppa_geom_w != pic_w ||
                              s_dsi_ppa_geom_h != pic_h ||
                              s_dsi_ppa_geom_x != logical_x ||
                              s_dsi_ppa_geom_y != logical_y;
    const bool full = force_full || s_dsi_need_full_sync || geom_changed;
    uint16_t *front = s_dsi_fb[s_dsi_front_idx];
    if (!front)
        return false;

    /* The last owner before R34 activation is PPA/DMA.  Invalidate CPU cache
     * exactly once so a stale line can never overwrite freshly PPA-rendered
     * neighbors when CPU stores only a narrow dirty strip. */
    if (!s_r34_front_cache_primed)
    {
        uint16_t *game_native = front + (size_t)logical_x * kDsiPhysStridePixels;
        const size_t game_native_bytes = (size_t)pic_w * kDsiPhysStridePixels * sizeof(uint16_t);
        if (esp_cache_msync(game_native, game_native_bytes,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK)
        {
            ESP_LOGW(TAG, "PX68K_LCDR34: initial Front M2C invalidate failed; falling back to R32 PPA32 direct-front");
            s_r34_selfcheck_ok = false;
            return false;
        }
        s_r34_front_cache_primed = true;
    }

    /* Single in-place Front is already current: CURRENT dirty bands only. */
    s_dsi_ppa_prev_band_count = 0u;
    dbfb_dirty_band_t merged[kDbfbDirtyBandCap * 2] = {};
    const uint32_t merged_count = dbfb615h17r9_merge_bands(
        current_bands, current_band_count, pic_h, merged, full);
    if (merged_count == 0u)
        return false;

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const int64_t rot0 = esp_timer_get_time();
    uint32_t rows = 0u;
    uint32_t tiles = 0u;
    uint32_t scalar_pixels = 0u;
    uint32_t pack_us = 0u;
    uint32_t pack_stripes = 0u;
#endif
    for (uint32_t i = 0; i < merged_count; ++i)
    {
        const uint32_t y0 = merged[i].y0;
        const uint32_t y1 = merged[i].y1;
        if (y1 <= y0) continue;
#if PX68K_TAB5_R35_INTERNAL_STRIPE
        if (s_r35_pack8)
        {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            tiles += r35_swrot_band(front, stage, pic_w, logical_x, logical_y,
                                    y0, y1, &scalar_pixels, &pack_us, &pack_stripes);
#else
            (void)r35_swrot_band(front, stage, pic_w, logical_x, logical_y,
                                 y0, y1, nullptr, nullptr, nullptr);
#endif
        }
        else
#endif
        {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            tiles += r34_swrot_band(front, stage, pic_w, logical_x, logical_y,
                                    y0, y1, &scalar_pixels);
#else
            (void)r34_swrot_band(front, stage, pic_w, logical_x, logical_y,
                                 y0, y1, nullptr);
#endif
        }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        rows += y1 - y0;
#endif
    }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const uint32_t rot_us = (uint32_t)(esp_timer_get_time() - rot0);
    const int64_t sync0 = esp_timer_get_time();
#endif
    for (uint32_t i = 0; i < merged_count; ++i)
    {
        if (!r34_sync_front_band(front, pic_w, logical_x, logical_y,
                                 merged[i].y0, merged[i].y1))
        {
            ESP_LOGW(TAG, "PX68K_LCDR34: batched Front C2M failed; falling back to R32 PPA32 direct-front");
            s_r34_selfcheck_ok = false;
            s_dsi_need_full_sync = true;
            return false;
        }
    }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const uint32_t sync_us = (uint32_t)(esp_timer_get_time() - sync0);
#endif

    s_dsi_need_full_sync = false;
    s_dsi_repair_pending = false;
    s_dsi_ppa_prime_other = false;
    s_dsi_ppa_prev_band_count = 0u;
    s_dsi_ppa_geom_w = pic_w;
    s_dsi_ppa_geom_h = pic_h;
    s_dsi_ppa_geom_x = logical_x;
    s_dsi_ppa_geom_y = logical_y;

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    ++s_r34_frames;
    s_r34_rotate_us += rot_us;
    s_r34_sync_us += sync_us;
    s_r34_rows += rows;
    s_r34_tiles += tiles;
    s_r34_scalar_pixels += scalar_pixels;
#if PX68K_TAB5_R35_INTERNAL_STRIPE
    s_r35_pack_us += pack_us;
    s_r35_pack_stripes += pack_stripes;
#endif

    if ((s_r34_frames % 60u) == 0u)
    {
#if PX68K_TAB5_R35_INTERNAL_STRIPE
        if (s_r35_pack8)
        {
            ESP_LOGI(TAG,
                     "PX68K_LCDR35: PACK8 frames=%llu avgROT=%lluus avgPACK=%lluus avgSYNC=%lluus avgRows=%llu avgTiles=%llu stripes=%llu scalarPix=%llu lineScratch=%s swapsHeld=%llu front=%u",
                     (unsigned long long)s_r34_frames,
                     (unsigned long long)(s_r34_rotate_us / s_r34_frames),
                     (unsigned long long)(s_r35_pack_us / s_r34_frames),
                     (unsigned long long)(s_r34_sync_us / s_r34_frames),
                     (unsigned long long)(s_r34_rows / s_r34_frames),
                     (unsigned long long)(s_r34_tiles / s_r34_frames),
                     (unsigned long long)s_r35_pack_stripes,
                     (unsigned long long)s_r34_scalar_pixels,
                     s_r35_line_scratch_internal ? "INTERNAL" : "PSRAM",
                     (unsigned long long)s_dbfb_swap_count,
                     (unsigned)s_dsi_front_idx);
        }
        else
#endif
        {
            ESP_LOGI(TAG,
                     "PX68K_LCDR34: SWROT frames=%llu avgROT=%lluus avgSYNC=%lluus avgRows=%llu avgTiles=%llu scalarPix=%llu swapsHeld=%llu front=%u",
                     (unsigned long long)s_r34_frames,
                     (unsigned long long)(s_r34_rotate_us / s_r34_frames),
                     (unsigned long long)(s_r34_sync_us / s_r34_frames),
                     (unsigned long long)(s_r34_rows / s_r34_frames),
                     (unsigned long long)(s_r34_tiles / s_r34_frames),
                     (unsigned long long)s_r34_scalar_pixels,
                     (unsigned long long)s_dbfb_swap_count,
                     (unsigned)s_dsi_front_idx);
        }
    }
#endif
    return true;
#endif
}

static bool r32_direct_front_present(const uint16_t *stage,
                                       uint32_t pic_w, uint32_t pic_h,
                                       uint32_t logical_x, uint32_t logical_y,
                                       const dbfb_dirty_band_t *current_bands,
                                       uint32_t current_band_count,
                                       bool force_full)
{
#if !PX68K_TAB5_R32_DIRECT_FRONT
    (void)stage; (void)pic_w; (void)pic_h; (void)logical_x; (void)logical_y;
    (void)current_bands; (void)current_band_count; (void)force_full;
    return false;
#else
    if (!s_r32_direct_front || !stage || !current_bands || current_band_count == 0u)
        return false;
    if (!dbfb615h17r9_ensure_ppa_client())
        return false;

    const bool geom_changed = s_dsi_ppa_geom_w != pic_w ||
                              s_dsi_ppa_geom_h != pic_h ||
                              s_dsi_ppa_geom_x != logical_x ||
                              s_dsi_ppa_geom_y != logical_y;
    const bool full = force_full || s_dsi_need_full_sync || geom_changed;

    uint16_t *front = s_dsi_fb[s_dsi_front_idx];
    if (!front)
        return false;

    /* No A/B reuse is involved in direct-front mode.  On a geometry/UI reset,
     * clear only the game viewport in the currently scanned buffer; side
     * chrome remains intact.  This one full pass may tear, but it is rare. */
    if (full && !dbfb615h17r9_clear_game_window(front))
    {
        ESP_LOGW(TAG, "PX68K_LCDR32: direct-front game-window clear failed");
        return false;
    }

    /* The N-1 union exists only to repair alternating Back buffers.  A single
     * in-place Front is already current, so merge CURRENT bands only. */
    s_dsi_ppa_prev_band_count = 0u;
    dbfb_dirty_band_t merged[kDbfbDirtyBandCap * 2] = {};
    const uint32_t merged_count = dbfb615h17r9_merge_bands(
        current_bands, current_band_count, pic_h, merged, full);
    if (merged_count == 0u)
        return false;

    const size_t native_bytes = (size_t)kDsiPhysWidth *
                                (size_t)kDsiPhysHeight * sizeof(uint16_t);
    const int64_t t0 = esp_timer_get_time();
    uint32_t submitted = 0u;
    uint32_t submitted_rows = 0u;

    for (uint32_t i = 0; i < merged_count; ++i)
    {
        const uint32_t update_y0 = merged[i].y0;
        const uint32_t update_y1 = merged[i].y1;
        const uint32_t band_h = update_y1 - update_y0;
        if (!band_h)
            continue;

        const uint32_t native_x = kDsiPhysWidth - (logical_y + update_y1);
        const uint32_t native_y = logical_x;

        ppa_srm_oper_config_t cfg = {};
        cfg.in.buffer = stage;
        cfg.in.pic_w = pic_w;
        cfg.in.pic_h = pic_h;
        cfg.in.block_w = pic_w;
        cfg.in.block_h = band_h;
        cfg.in.block_offset_x = 0u;
        cfg.in.block_offset_y = update_y0;
        cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.out.buffer = front;
        cfg.out.buffer_size = native_bytes;
        cfg.out.pic_w = kDsiPhysWidth;
        cfg.out.pic_h = kDsiPhysHeight;
        cfg.out.block_offset_x = native_x;
        cfg.out.block_offset_y = native_y;
        cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
        cfg.scale_x = 1.0f;
        cfg.scale_y = 1.0f;
        cfg.mirror_x = false;
        cfg.mirror_y = false;
        cfg.rgb_swap = false;
        cfg.byte_swap = false;
        cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        cfg.mode = PPA_TRANS_MODE_BLOCKING;

        const esp_err_t ppa_er = ppa_do_scale_rotate_mirror(s_dsi_ppa_srm, &cfg);
        if (ppa_er != ESP_OK)
        {
            ESP_LOGW(TAG,
                     "PX68K_LCDR32: direct-front PPA failed err=%d band=%lu..%lu; falling back to DBFB",
                     (int)ppa_er, (unsigned long)update_y0, (unsigned long)update_y1);
            s_r32_direct_front = false;
            s_aspect_seen_frame = nullptr;
            s_dsi_need_full_sync = true;
            return false;
        }
        ++submitted;
        submitted_rows += band_h;
    }

    const uint32_t ppa_us = (uint32_t)(esp_timer_get_time() - t0);
    s_dsi_need_full_sync = false;
    s_dsi_repair_pending = false;
    s_dsi_ppa_prime_other = false;
    s_dsi_ppa_prev_band_count = 0u;
    s_dsi_ppa_geom_w = pic_w;
    s_dsi_ppa_geom_h = pic_h;
    s_dsi_ppa_geom_x = logical_x;
    s_dsi_ppa_geom_y = logical_y;

    s_dsi_ppa_us += ppa_us;
    s_dsi_ppa_pixels += (uint64_t)pic_w * (uint64_t)submitted_rows;
    s_dsi_ppa_rows += submitted_rows;
    s_dsi_ppa_ops += submitted;
    ++s_dsi_ppa_frames;
    ++s_r32_direct_frames;
    s_r32_direct_ppa_us += ppa_us;
    s_r32_direct_rows += submitted_rows;
    s_r32_direct_ops += submitted;

    if ((s_r32_direct_frames % 60u) == 0u)
    {
        ESP_LOGI(TAG,
                 "PX68K_LCDR34-FALLBACK: DIRECT-FRONT+PPA32 frames=%llu avgPPA=%lluus avgRows=%llu avgOps=%llu.%02llu swapsHeld=%llu front=%u",
                 (unsigned long long)s_r32_direct_frames,
                 (unsigned long long)(s_r32_direct_ppa_us / s_r32_direct_frames),
                 (unsigned long long)(s_r32_direct_rows / s_r32_direct_frames),
                 (unsigned long long)(s_r32_direct_ops / s_r32_direct_frames),
                 (unsigned long long)(((s_r32_direct_ops % s_r32_direct_frames) * 100u) / s_r32_direct_frames),
                 (unsigned long long)s_dbfb_swap_count,
                 (unsigned)s_dsi_front_idx);
    }
    return true;
#endif
}

static bool dbfb615h17r9_ppa_present(const uint16_t *stage,
                                     uint32_t pic_w, uint32_t pic_h,
                                     uint32_t logical_x, uint32_t logical_y,
                                     const dbfb_dirty_band_t *current_bands,
                                     uint32_t current_band_count,
                                     bool force_full,
                                     bool allow_direct_front)
{
    /* R56c: allow the Screen Manager's managed Back-FB path even when the
     * legacy R49 LIVE presenter has deliberately cleared s_dsi_ppa_live.
     * allow_direct_front=false is the managed transaction marker here. */
    const bool managed_backfb = !allow_direct_front;
    if (!s_dsi_double_live || (!s_dsi_ppa_live && !managed_backfb) || !stage ||
        !s_dsi_dpi_panel || !current_bands || current_band_count == 0u ||
        pic_w == 0u || pic_h == 0u ||
        logical_x + pic_w > kDsiPhysHeight ||
        logical_y + pic_h > kDsiPhysWidth)
        return false;

#if PX68K_TAB5_R32_DIRECT_FRONT
    if (allow_direct_front && s_r32_direct_front)
    {
#if PX68K_TAB5_R34_SWROT_FRONT
        if (s_r34_selfcheck_ok)
            return r34_direct_front_present(stage, pic_w, pic_h, logical_x, logical_y,
                                            current_bands, current_band_count, force_full);
#endif
        return r32_direct_front_present(stage, pic_w, pic_h, logical_x, logical_y,
                                        current_bands, current_band_count, force_full);
    }
#endif

    if (!dbfb615h17r9_ensure_ppa_client())
        return false;

    if (!dbfb615h17_wait_refresh())
        return false;

    const bool geom_changed = s_dsi_ppa_geom_w != pic_w ||
                              s_dsi_ppa_geom_h != pic_h ||
                              s_dsi_ppa_geom_x != logical_x ||
                              s_dsi_ppa_geom_y != logical_y;
    const bool pair_restart = force_full || s_dsi_need_full_sync || geom_changed;
    const bool prime_pass = s_dsi_ppa_prime_other;
    const bool full = pair_restart || prime_pass;

    uint16_t *back = s_dsi_fb[s_dsi_back_idx];
    if (!back)
        return false;

    /* Host UI / game-chrome is authored through M5GFX's FB0. Clone it once to
     * the other physical buffer before restarting the pair, then preserve the
     * side bars by clearing only the central game window. */
    if (s_dsi_need_full_sync)
    {
        if (!dbfb615h17_copy_front_to_back_rect(
                0u, 0u, kDsiPhysHeight, kDsiPhysWidth, true))
        {
            ESP_LOGW(TAG, "PX68K_DBFB615H17R11: UI/chrome Front->Back clone failed; swap suppressed");
            return false;
        }
    }
    if (full && !dbfb615h17r9_clear_game_window(back))
    {
        ESP_LOGW(TAG, "PX68K_DBFB615H17R11: central game-window clear C2M failed; swap suppressed");
        return false;
    }

    dbfb_dirty_band_t merged[kDbfbDirtyBandCap * 2] = {};
    const uint32_t merged_count = dbfb615h17r9_merge_bands(
        current_bands, current_band_count, pic_h, merged, full);
    if (merged_count == 0u)
        return false;

    const size_t native_bytes = (size_t)kDsiPhysWidth *
                                (size_t)kDsiPhysHeight * sizeof(uint16_t);
    const int64_t t0 = esp_timer_get_time();
    uint32_t submitted = 0u;
    uint32_t submitted_rows = 0u;

    for (uint32_t i = 0; i < merged_count; ++i)
    {
        const uint32_t update_y0 = merged[i].y0;
        const uint32_t update_y1 = merged[i].y1;
        const uint32_t band_h = update_y1 - update_y0;
        if (!band_h)
            continue;

        const uint32_t native_x = kDsiPhysWidth - (logical_y + update_y1);
        const uint32_t native_y = logical_x;

        ppa_srm_oper_config_t cfg = {};
        cfg.in.buffer = stage;
        cfg.in.pic_w = pic_w;
        cfg.in.pic_h = pic_h;
        cfg.in.block_w = pic_w;
        cfg.in.block_h = band_h;
        cfg.in.block_offset_x = 0u;
        cfg.in.block_offset_y = update_y0;
        cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.out.buffer = back;
        cfg.out.buffer_size = native_bytes;
        cfg.out.pic_w = kDsiPhysWidth;
        cfg.out.pic_h = kDsiPhysHeight;
        cfg.out.block_offset_x = native_x;
        cfg.out.block_offset_y = native_y;
        cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
        cfg.scale_x = 1.0f;
        cfg.scale_y = 1.0f;
        cfg.mirror_x = false;
        cfg.mirror_y = false;
        cfg.rgb_swap = false;
        cfg.byte_swap = false;
        cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        cfg.mode = PPA_TRANS_MODE_BLOCKING;

        const esp_err_t ppa_er = ppa_do_scale_rotate_mirror(s_dsi_ppa_srm, &cfg);
        if (ppa_er != ESP_OK)
        {
            ESP_LOGW(TAG,
                     "PX68K_DBFB615H17R11: PPA rotate failed err=%d band=%lu..%lu mode=blocking; disabling PPA DBFB",
                     (int)ppa_er, (unsigned long)update_y0, (unsigned long)update_y1);
            s_dsi_ppa_live = false;
            s_dsi_double_live = false;
            return false;
        }
        ++submitted;
        submitted_rows += band_h;
    }


    const uint32_t ppa_us = (uint32_t)(esp_timer_get_time() - t0);
    const esp_err_t draw_er = esp_lcd_panel_draw_bitmap(
        s_dsi_dpi_panel, 0, 0, (int)kDsiPhysWidth, (int)kDsiPhysHeight, back);
    if (draw_er != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "PX68K_DBFB615H17R11: native framebuffer swap failed err=%d; disabling PPA DBFB",
                 (int)draw_er);
        s_dsi_ppa_live = false;
        s_dsi_double_live = false;
        return false;
    }

    const uint8_t old_front = s_dsi_front_idx;
    s_dsi_front_idx = s_dsi_back_idx;
    s_dsi_back_idx = old_front;
    s_dsi_swap_refresh_seq = __atomic_load_n(&s_dsi_refresh_seq, __ATOMIC_ACQUIRE);
    s_dsi_swap_wait_refresh = true;

    if (pair_restart)
        s_dsi_ppa_prime_other = true;
    else if (prime_pass)
        s_dsi_ppa_prime_other = false;

    s_dsi_need_full_sync = false;
    s_dsi_repair_pending = false;
    s_dsi_ppa_prev_band_count = std::min<uint32_t>(current_band_count, kDbfbDirtyBandCap);
    for (uint32_t i = 0; i < s_dsi_ppa_prev_band_count; ++i)
        s_dsi_ppa_prev_bands[i] = current_bands[i];
    s_dsi_ppa_geom_w = pic_w;
    s_dsi_ppa_geom_h = pic_h;
    s_dsi_ppa_geom_x = logical_x;
    s_dsi_ppa_geom_y = logical_y;

    s_dsi_ppa_us += ppa_us;
    s_dsi_ppa_pixels += (uint64_t)pic_w * (uint64_t)submitted_rows;
    s_dsi_ppa_rows += submitted_rows;
    s_dsi_ppa_ops += submitted;
    ++s_dsi_ppa_frames;
    ++s_dbfb_swap_count;

#if PX68K_TAB5_R32_DIRECT_FRONT
    if (!s_r32_direct_front && ++s_r32_warmup_swaps >= kR32WarmupSwapFrames)
    {
        /* R47: the last warmup draw_bitmap() has only requested the next
         * framebuffer. Do not start CPU direct-front writes until a physical
         * refresh_done confirms that request has crossed the scanout boundary.
         * With eight swaps the intended steady Front is FB0. If bookkeeping
         * ever says otherwise, explicitly select FB0 once and wait again. */
        if (!dbfb615h17_wait_refresh())
        {
            ESP_LOGW(TAG, "PX68K_LCDR47: final warmup refresh wait failed; direct-front not armed");
            return true;
        }
        if (s_dsi_front_idx != 0u && s_dsi_fb[0] && s_dsi_dpi_panel)
        {
            const esp_err_t pin_er = esp_lcd_panel_draw_bitmap(
                s_dsi_dpi_panel, 0, 0, (int)kDsiPhysWidth, (int)kDsiPhysHeight, s_dsi_fb[0]);
            if (pin_er != ESP_OK)
            {
                ESP_LOGW(TAG, "PX68K_LCDR47: explicit FB0 pin failed err=%d; direct-front not armed", (int)pin_er);
                return true;
            }
            s_dsi_front_idx = 0u;
            s_dsi_back_idx = 1u;
            s_dsi_swap_refresh_seq = __atomic_load_n(&s_dsi_refresh_seq, __ATOMIC_ACQUIRE);
            s_dsi_swap_wait_refresh = true;
            if (!dbfb615h17_wait_refresh())
            {
                ESP_LOGW(TAG, "PX68K_LCDR47: explicit FB0 pin refresh wait failed; direct-front not armed");
                return true;
            }
        }
        s_dsi_front_idx = 0u;
        s_dsi_back_idx = 1u;
        s_r32_direct_front = true;
        ESP_LOGI(TAG, "PX68K_LCDR47: DIRECT-FRONT physically pinned to FB0 after refresh_done; steady draw_bitmap swaps forbidden");
        s_dsi_ppa_prev_band_count = 0u;
        s_dsi_ppa_prime_other = false;
        s_dsi_need_full_sync = true;
        s_aspect_seen_frame = nullptr;
        s_r34_front_cache_primed = false;
#if PX68K_TAB5_R34_SWROT_FRONT
        if (s_r34_selfcheck_ok && s_dsi_ppa_srm)
        {
            const esp_err_t unreg = ppa_unregister_client(s_dsi_ppa_srm);
            if (unreg == ESP_OK)
            {
                s_dsi_ppa_srm = nullptr;
                ESP_LOGI(TAG, "PX68K_LCDR34: warmup PPA client unregistered; steady-state PPA hardware ownership released");
            }
            else
            {
                ESP_LOGW(TAG, "PX68K_LCDR34: warmup PPA unregister err=%d; SWROT continues with no further PPA submissions", (int)unreg);
            }
        }
#endif
        if (s_r35_pack8)
            ESP_LOGW(TAG,
                     "PX68K_LCDR42: PRODUCER-TILE32 DIRECT-NATIVE ACTIVE after %u proven DBFB swaps; producer exact 32px sparse mask + no-op NOGEN; sparse 8x8 LCD tile runs; display canary armed; raw compare/staging/PPA/swap disabled",
                     (unsigned)kR32WarmupSwapFrames);
        else
            ESP_LOGW(TAG,
                     "PX68K_LCDR34: SWROT DIRECT-FRONT ACTIVE after %u proven DBFB swaps; steady-state PPA and per-frame FB swap disabled",
                     (unsigned)kR32WarmupSwapFrames);
    }
#endif

    if ((s_dsi_ppa_frames % 60u) == 0u)
    {
        ESP_LOGI(TAG,
                 "PX68K_DBFB615H17R11: PPA-BANDS frames=%llu ops=%llu avgOps=%llu.%02llu avgRows=%llu avgPPAwall=%lluus lastBands=%lu lastRows=%lu full=%u mode=blocking burst=64B cap=2",
                 (unsigned long long)s_dsi_ppa_frames,
                 (unsigned long long)s_dsi_ppa_ops,
                 (unsigned long long)(s_dsi_ppa_ops / s_dsi_ppa_frames),
                 (unsigned long long)(((s_dsi_ppa_ops % s_dsi_ppa_frames) * 100u) / s_dsi_ppa_frames),
                 (unsigned long long)(s_dsi_ppa_rows / s_dsi_ppa_frames),
                 (unsigned long long)(s_dsi_ppa_us / s_dsi_ppa_frames),
                 (unsigned long)submitted,
                 (unsigned long)submitted_rows,
                 full ? 1u : 0u);
    }
    return true;
}

/* Compare one scaled logical-row group with Back in native portrait memory.
 * All destination rows in a group carry the same scaled source row. */
static bool dbfb615h17r6_group_diff(const uint16_t *back,
                                    uint32_t logical_x0,
                                    uint32_t logical_y0,
                                    uint32_t logical_y1,
                                    const uint16_t *src,
                                    uint32_t width)
{
    if (!back || !src || logical_y1 <= logical_y0)
        return true;
    const uint32_t px0 = kDsiPhysWidth - logical_y1;
    const uint32_t px1 = kDsiPhysWidth - logical_y0;
    for (uint32_t dx = 0; dx < width; ++dx)
    {
        const uint16_t *p = back +
            (size_t)(logical_x0 + dx) * kDsiPhysStridePixels + px0;
        for (uint32_t px = px0; px < px1; ++px, ++p)
        {
            if (*p != src[dx])
                return true;
        }
    }
    return false;
}

/* Transpose a horizontal logical row group into the portrait-native Back FB.
 * For each logical X, the replicated logical Y rows become adjacent native X
 * pixels, so the inner write is still a tiny contiguous run (typically 2-3). */
static void dbfb615h17r6_write_group(uint16_t *back,
                                     uint32_t logical_x0,
                                     uint32_t logical_y0,
                                     uint32_t logical_y1,
                                     const uint16_t *src,
                                     uint32_t width)
{
    if (!back || !src || logical_y1 <= logical_y0)
        return;
    const uint32_t px0 = kDsiPhysWidth - logical_y1;
    const uint32_t px1 = kDsiPhysWidth - logical_y0;
    for (uint32_t dx = 0; dx < width; ++dx)
    {
        uint16_t *p = back +
            (size_t)(logical_x0 + dx) * kDsiPhysStridePixels + px0;
        std::fill(p, p + (px1 - px0), src[dx]);
    }
}

/* M5GFX's own framebuffer line table remains attached to driver FB0 and is
 * rotation-aware.  Before host UI drawing, make FB0 current, then mark FB1
 * stale so the next LIVE frame performs one native-layout full sync. */
static bool dbfb615h17_prepare_m5gfx_front(void)
{
    if (!s_dsi_double_live)
        return true;
    if (!dbfb615h17_wait_refresh() || !dbfb615h17_wait_sync())
        return false;

    if (s_dsi_front_idx != 0u)
    {
        if (!dbfb615h17_copy_front_to_back_rect(
                0u, 0u, kDsiPhysHeight, kDsiPhysWidth, true))
            return false;
        if (!dbfb615h17_swap_back_to_front(
                0u, 0u, kDsiPhysHeight, kDsiPhysWidth))
            return false;
        if (!dbfb615h17_wait_refresh())
            return false;
        s_dsi_repair_pending = false;
    }

    s_dsi_need_full_sync = true;
    s_r49_force_full_front = true;
    ++s_r49_full_repaint_requests;
    s_dsi_repair_pending = false;
    s_dsi_ppa_prev_band_count = 0u;
    s_dsi_ppa_prime_other = false;
    return true;
}
#endif

static inline uint32_t line_writer_count(uint32_t y)
{
    if (y >= kTrackedLines) return 0;
    return __atomic_load_n(&s_line_writers[y], __ATOMIC_ACQUIRE);
}

static inline uint32_t line_generation(uint32_t y)
{
    if (y >= kTrackedLines) return 0;
    return __atomic_load_n(&s_line_generation[y], __ATOMIC_ACQUIRE);
}

/* R40: consume the pending source-X union belonging to a stable generation.
 * Producer publication uses the same lock, so multiple writes are never lost. */
static bool r42_line_dirty_snapshot(uint32_t y, uint32_t expected_gen,
                                    uint32_t *tile32, uint16_t *x0, uint16_t *x1)
{
    if (y >= kTrackedLines || !tile32 || !x0 || !x1) return false;
    bool ok = false;
    portENTER_CRITICAL(&s_line_dirty_span_mux);
    const uint32_t g0 = __atomic_load_n(&s_line_generation[y], __ATOMIC_ACQUIRE);
    const uint32_t w0 = __atomic_load_n(&s_line_writers[y], __ATOMIC_ACQUIRE);
    const uint16_t a = s_line_dirty_x0[y];
    const uint16_t b = s_line_dirty_x1[y];
    const uint32_t m = s_line_dirty_tile32[y];
    const uint32_t g1 = __atomic_load_n(&s_line_generation[y], __ATOMIC_ACQUIRE);
    const uint32_t w1 = __atomic_load_n(&s_line_writers[y], __ATOMIC_ACQUIRE);
    if (w0 == 0u && w1 == 0u && g0 == expected_gen && g1 == expected_gen)
    {
        *tile32 = m; *x0 = a; *x1 = b;
        s_line_dirty_x0[y] = 0u;
        s_line_dirty_x1[y] = 0u;
        s_line_dirty_tile32[y] = 0u;
        ok = true;
    }
    portEXIT_CRITICAL(&s_line_dirty_span_mux);
    return ok;
}

static bool r40_line_dirty_span_snapshot(uint32_t y, uint32_t expected_gen,
                                         uint16_t *x0, uint16_t *x1)
{
    uint32_t ignored = 0u;
    return r42_line_dirty_snapshot(y, expected_gen, &ignored, x0, x1);
}

static inline void r40_source_span_to_dest(uint32_t src_w, uint32_t dst_w,
                                           uint32_t sx0, uint32_t sx1,
                                           uint32_t *dx0, uint32_t *dx1)
{
    if (!dx0 || !dx1 || !src_w || !dst_w) return;
    if (sx0 > src_w) sx0 = src_w;
    if (sx1 > src_w) sx1 = src_w;
    if (sx1 <= sx0) { *dx0 = 0u; *dx1 = dst_w; return; }
    uint32_t a = (uint32_t)(((uint64_t)sx0 * dst_w + src_w - 1u) / src_w);
    uint32_t b = (uint32_t)(((uint64_t)sx1 * dst_w + src_w - 1u) / src_w);
    if (a > dst_w) a = dst_w;
    if (b > dst_w) b = dst_w;
    /* 8x8 native-store kernel: expand to complete tiles. */
    a &= ~7u;
    b = (b + 7u) & ~7u;
    if (b > dst_w) b = dst_w;
    if (b <= a) { a = 0u; b = dst_w; }
    *dx0 = a; *dx1 = b;
}

#if defined(CONFIG_IDF_TARGET_ESP32P4) && PX68K_TAB5_R36_DIRECT_NATIVE && PX68K_TAB5_R37_EXACT_RAW_SHADOW
static bool r37_prepare_raw_shadow(const present_request_t &req)
{
    if (!s_aspect_frame || !req.frame || !req.pitch_pixels || !req.height)
        return false;

    const size_t backing_bytes = (size_t)kAspectViewportWidth *
                                 (size_t)kAspectViewportHeight * sizeof(uint16_t);
    const size_t need_bytes = (size_t)req.pitch_pixels *
                              (size_t)req.height * sizeof(uint16_t);
    uint8_t *raw = reinterpret_cast<uint8_t *>(s_aspect_frame);
    const uintptr_t raw_mod = (uintptr_t)raw & 15u;
    const uintptr_t src_mod = (uintptr_t)req.frame & 15u;
    const size_t adjust = (size_t)((src_mod + 16u - raw_mod) & 15u);
    if (need_bytes + adjust > backing_bytes)
        return false;

    uint16_t *shadow = reinterpret_cast<uint16_t *>(raw + adjust);
    const bool changed = shadow != s_r37_shadow ||
                         req.frame != s_r37_shadow_source ||
                         req.pitch_pixels != s_r37_shadow_pitch ||
                         req.width != s_r37_shadow_width ||
                         req.height != s_r37_shadow_height;
    if (changed)
    {
        s_r37_shadow = shadow;
        s_r37_shadow_source = req.frame;
        s_r37_shadow_pitch = req.pitch_pixels;
        s_r37_shadow_width = req.width;
        s_r37_shadow_height = req.height;
        std::memset(s_r37_shadow_valid, 0, sizeof(s_r37_shadow_valid));
        ESP_LOGI(TAG,
                 "PX68K_LCDR37: exact RAW-SHADOW ready backing=REUSED-960x720 ptr=%p bytes=%u pitch=%u src/shadow mod16=%u/%u; XespV diff/copy eligible=%u",
                 (void *)s_r37_shadow, (unsigned)need_bytes,
                 (unsigned)req.pitch_pixels,
                 (unsigned)((uintptr_t)req.frame & 15u),
                 (unsigned)((uintptr_t)s_r37_shadow & 15u),
                 ((((uintptr_t)req.frame & 15u) == ((uintptr_t)s_r37_shadow & 15u)) &&
                  (((req.pitch_pixels * sizeof(uint16_t)) & 15u) == 0u)) ? 1u : 0u);
    }
    return true;
}

static IRAM_ATTR __attribute__((hot, optimize("O3"))) bool
r36_scale_row_stable(const present_request_t &req, uint32_t sy,
                     uint16_t *dst, uint32_t out_w,
                     uint32_t *retries_total)
{
    uint32_t attempt = 0u;
    const uint16_t *src = req.frame + (size_t)sy * req.pitch_pixels;
    while (attempt < kLiveRowRetryLimit)
    {
        if (line_writer_count(sy) != 0u)
        {
            ++attempt;
            if (retries_total) ++*retries_total;
            taskYIELD();
            continue;
        }
        const uint32_t gen0 = line_generation(sy);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (uint32_t dx = 0; dx < out_w; ++dx)
            dst[dx] = src[s_aspect_xmap[dx]];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t gen1 = line_generation(sy);
        if (line_writer_count(sy) == 0u && gen0 == gen1)
            return true;
        ++attempt;
        if (retries_total) ++*retries_total;
    }
    return false;
}

static IRAM_ATTR __attribute__((hot, optimize("O3"))) bool
r40_scale_row_span_stable(const present_request_t &req, uint32_t sy,
                          uint16_t *dst, uint32_t dx0, uint32_t dx1,
                          uint32_t *retries_total)
{
    uint32_t attempt = 0u;
    const uint16_t *src = req.frame + (size_t)sy * req.pitch_pixels;
    while (attempt < kLiveRowRetryLimit)
    {
        if (line_writer_count(sy) != 0u)
        {
            ++attempt;
            if (retries_total) ++*retries_total;
            taskYIELD();
            continue;
        }
        const uint32_t gen0 = line_generation(sy);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (uint32_t dx = dx0; dx < dx1; ++dx)
            dst[dx] = src[s_aspect_xmap[dx]];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t gen1 = line_generation(sy);
        if (line_writer_count(sy) == 0u && gen0 == gen1)
            return true;
        ++attempt;
        if (retries_total) ++*retries_total;
    }
    return false;
}

static bool r40_sync_front_rect(uint16_t *front,
                                uint32_t logical_x, uint32_t logical_y,
                                uint32_t x0, uint32_t x1,
                                uint32_t y0, uint32_t y1)
{
    if (!front || x1 <= x0 || y1 <= y0) return true;
    const uint32_t px0 = kDsiPhysWidth - (logical_y + y1);
    const uint32_t px1 = kDsiPhysWidth - (logical_y + y0);
    if (px1 <= px0) return true;
    constexpr uint32_t kSyncNativeRows = 64u;
    const uint32_t py_begin = logical_x + x0;
    const uint32_t py_end = logical_x + x1;
    for (uint32_t py0 = py_begin; py0 < py_end; py0 += kSyncNativeRows)
    {
        const uint32_t pend = std::min<uint32_t>(py0 + kSyncNativeRows, py_end);
        uint16_t *begin = front + (size_t)py0 * kDsiPhysStridePixels + px0;
        uint16_t *end = front + (size_t)(pend - 1u) * kDsiPhysStridePixels + px1;
        if (!dbfb615h17r6_c2m_aligned(begin, (size_t)(end - begin) * sizeof(uint16_t)))
            return false;
    }
    return true;
}


/* R41: tile-granular dirty coverage.  R40 already proved producer X-spans
 * are much narrower than a full line (roughly 86-90 source pixels late in the
 * MDX run), but unioning all spans inside one 8-row block widened the actual
 * LCD work to roughly 478 logical pixels.  Preserve holes between spans by
 * publishing a 120-bit mask per 8-row block; the existing 8x8 transpose/store
 * kernel then touches only tiles that contain a real changed span. */
static inline void r41_tilemask_mark(uint32_t block, uint32_t x0, uint32_t x1)
{
    if (block >= kR41TileRows || x1 <= x0) return;
    if (x1 > kAspectViewportWidth) x1 = kAspectViewportWidth;
    uint32_t t0 = x0 >> 3;
    uint32_t t1 = (x1 + 7u) >> 3; /* exclusive */
    if (t1 > kR41TileCols) t1 = kR41TileCols;
    if (t1 <= t0) return;

    const uint32_t w0 = t0 >> 5;
    const uint32_t w1 = (t1 - 1u) >> 5;
    const uint32_t first = 0xffffffffu << (t0 & 31u);
    const uint32_t end_bits = t1 & 31u;
    const uint32_t last = end_bits ? ((1u << end_bits) - 1u) : 0xffffffffu;
    if (w0 == w1)
    {
        s_r41_tile_mask[block][w0] |= first & last;
        return;
    }
    s_r41_tile_mask[block][w0] |= first;
    for (uint32_t w = w0 + 1u; w < w1; ++w)
        s_r41_tile_mask[block][w] = 0xffffffffu;
    s_r41_tile_mask[block][w1] |= last;
}

static inline bool r41_tilemask_any(uint32_t block)
{
    if (block >= kR41TileRows) return false;
    uint32_t v = 0u;
    for (uint32_t w = 0u; w < kR41TileWords; ++w) v |= s_r41_tile_mask[block][w];
    return v != 0u;
}

static inline bool r41_next_tile_run(uint32_t block, uint32_t start_tile,
                                     uint32_t *run0, uint32_t *run1)
{
    if (block >= kR41TileRows || !run0 || !run1) return false;
    uint32_t t = start_tile;
    while (t < kR41TileCols &&
           (s_r41_tile_mask[block][t >> 5] & (1u << (t & 31u))) == 0u) ++t;
    if (t >= kR41TileCols) return false;
    const uint32_t a = t;
    while (t < kR41TileCols &&
           (s_r41_tile_mask[block][t >> 5] & (1u << (t & 31u))) != 0u) ++t;
    *run0 = a; *run1 = t;
    return true;
}

/* Low-rate display canary.  It checks one tile that was actually written,
 * against the current source only when every source row is still exactly the
 * generation that LCD consumed.  This catches transpose/mapping/span mistakes
 * without a full-screen readback or persistent PSRAM scan. */
static bool r41_verify_native_tile(const present_request_t &req,
                                   const uint16_t *front,
                                   uint32_t logical_x, uint32_t logical_y,
                                   uint32_t tx, uint32_t ty,
                                   uint32_t *bad_x, uint32_t *bad_y,
                                   uint16_t *expected_out, uint16_t *got_out)
{
    if (!front || !req.frame || tx + 8u > kAspectViewportWidth ||
        ty + 8u > kAspectViewportHeight) return false;

    uint32_t sy_row[8] = {};
    uint32_t gen_row[8] = {};
    for (uint32_t j = 0u; j < 8u; ++j)
    {
        const uint32_t sy = s_aspect_ymap[ty + j];
        if (sy >= kTrackedLines || line_writer_count(sy) != 0u) return false;
        const uint32_t gen = line_generation(sy);
        if (gen != s_aspect_seen_generation[sy]) return false;
        sy_row[j] = sy;
        gen_row[j] = gen;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    bool mismatch = false;
    uint32_t mx = 0u, my = 0u;
    uint16_t me = 0u, mg = 0u;
    for (uint32_t j = 0u; j < 8u && !mismatch; ++j)
    {
        const uint16_t *src = req.frame + (size_t)sy_row[j] * req.pitch_pixels;
        const uint32_t px = kDsiPhysWidth - 1u - (logical_y + ty + j);
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            const uint32_t lx = tx + i;
            const uint16_t expected = src[s_aspect_xmap[lx]];
            const uint32_t py = logical_x + lx;
            const uint16_t got = front[(size_t)py * kDsiPhysStridePixels + px];
            if (got != expected)
            {
                mismatch = true; mx = lx; my = ty + j; me = expected; mg = got; break;
            }
        }
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (uint32_t j = 0u; j < 8u; ++j)
    {
        const uint32_t sy = sy_row[j];
        if (line_writer_count(sy) != 0u || line_generation(sy) != gen_row[j] ||
            gen_row[j] != s_aspect_seen_generation[sy])
            return false;
    }
    if (mismatch)
    {
        if (bad_x) *bad_x = mx;
        if (bad_y) *bad_y = my;
        if (expected_out) *expected_out = me;
        if (got_out) *got_out = mg;
        return true; /* stable comparison completed; caller inspects mismatch */
    }
    if (bad_x) *bad_x = 0xffffffffu;
    if (bad_y) *bad_y = 0xffffffffu;
    return true;
}

static bool r38_direct_native_present(const present_request_t &req,
                                      uint32_t out_w, uint32_t out_h,
                                      uint32_t logical_x, uint32_t logical_y,
                                      bool force_full,
                                      uint32_t *retry_count,
                                      uint32_t *unstable_count)
{
    if (!s_r32_direct_front || !s_r34_selfcheck_ok || !s_r35_pack8 ||
        !req.frame || !req.width || !req.height ||
        out_w != kAspectViewportWidth || out_h != kAspectViewportHeight ||
        logical_x + out_w > kDsiPhysHeight ||
        logical_y + out_h > kDsiPhysWidth)
        return false;

    uint16_t *front = s_dsi_fb[s_dsi_front_idx];
    if (!front) return false;

    /* R49: UI/source/geometry invalidation is a mandatory repaint contract,
     * not just a generation hint.  Do not resume sparse Dirty until one
     * complete game viewport has been stored and cache-synced successfully. */
    const bool full_front = force_full || s_dsi_need_full_sync || s_r49_force_full_front;

    if (!s_r34_front_cache_primed)
    {
        uint16_t *game_native = front + (size_t)logical_x * kDsiPhysStridePixels;
        const size_t game_native_bytes = (size_t)out_w * kDsiPhysStridePixels * sizeof(uint16_t);
        if (esp_cache_msync(game_native, game_native_bytes,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK)
        {
            ESP_LOGW(TAG, "PX68K_LCDR42: initial Front M2C invalidate failed; legacy fallback retained");
            return false;
        }
        s_r34_front_cache_primed = true;
    }

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    ++s_r36_calls;
#endif
    std::memset(s_r41_tile_mask, 0, sizeof(s_r41_tile_mask));

    uint32_t retries = 0u, unstable = 0u;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    uint64_t gen_rows = 0u, gen_changed = 0u, writer_nochange = 0u;
    const int64_t scan0 = esp_timer_get_time();
#endif

    /* R41 keeps R38/R39's producer-exact generation contract, but preserves
     * the producer's X-span as a sparse 8x8 tile mask.  Multiple changed rows
     * can therefore occupy distant X regions in the same 8-row block without
     * forcing the untouched gap through scale/transpose/PSRAM store. */
    uint32_t dy = 0u;
    while (dy < out_h)
    {
        const uint32_t sy = s_aspect_ymap[dy];
        uint32_t dy_end = dy + 1u;
        while (dy_end < out_h && s_aspect_ymap[dy_end] == sy) ++dy_end;

        const uint32_t seen = s_aspect_seen_generation[sy];
        const uint32_t observed = line_generation(sy);
        if (!full_front && line_writer_count(sy) == 0u && seen == observed)
        {
            dy = dy_end;
            continue;
        }

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        ++gen_rows;
#endif
        uint32_t attempt = 0u, stable_gen = observed;
        bool stable = false;
        while (attempt < kLiveRowRetryLimit)
        {
            if (line_writer_count(sy) != 0u)
            {
                ++attempt; ++retries; taskYIELD();
                continue;
            }
            const uint32_t gen0 = line_generation(sy);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            const uint32_t gen1 = line_generation(sy);
            if (line_writer_count(sy) == 0u && gen0 == gen1)
            {
                stable = true; stable_gen = gen1; break;
            }
            ++attempt; ++retries;
        }
        if (!stable)
        {
            ++unstable;
            if (retry_count) *retry_count += retries;
            if (unstable_count) *unstable_count += unstable;
            std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
            return false;
        }

        const bool changed = full_front || stable_gen != seen;
        s_aspect_seen_generation[sy] = stable_gen;
        if (changed)
        {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            ++gen_changed;
#endif
            const uint32_t b0 = dy >> 3, b1 = (dy_end - 1u) >> 3;
            bool sparse_done = false;
            if (full_front)
            {
                /* A full refresh supersedes all pending X metadata for this stable
                 * generation. Consume it now so geometry changes cannot leak an old
                 * source-tile mask into the next steady-state update. */
                uint32_t discard_mask = 0u;
                uint16_t discard_x0 = 0u, discard_x1 = 0u;
                (void)r42_line_dirty_snapshot(sy, stable_gen, &discard_mask, &discard_x0, &discard_x1);
            }
            else
            {
                uint32_t src_mask = 0u;
                uint16_t sx0 = 0u, sx1 = 0xffffu;
                if (r42_line_dirty_snapshot(sy, stable_gen, &src_mask, &sx0, &sx1))
                {
                    /* R42 exact producer sets only useful 32px source tiles.
                     * Generic writers publish 0xffffffff as a conservative full-row sentinel. */
                    const uint32_t src_tiles = (req.width + 31u) >> 5;
                    const uint32_t useful_mask = (src_tiles >= 32u) ? 0xffffffffu :
                        (src_tiles ? ((1u << src_tiles) - 1u) : 0u);
                    if (src_mask != 0u && src_mask != 0xffffffffu &&
                        (src_mask & ~useful_mask) == 0u)
                    {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
                        ++s_r42_src_tile_rows;
#endif
                        uint32_t m = src_mask;
                        while (m)
                        {
                            const uint32_t bit = (uint32_t)__builtin_ctz(m);
                            m &= m - 1u;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
                            ++s_r42_src_tiles;
#endif
                            const uint32_t tsx0 = bit << 5;
                            const uint32_t tsx1 = std::min<uint32_t>(req.width, tsx0 + 32u);
                            uint32_t dx0 = 0u, dx1 = out_w;
                            r40_source_span_to_dest(req.width, out_w, tsx0, tsx1, &dx0, &dx1);
                            for (uint32_t b = b0; b <= b1; ++b)
                                r41_tilemask_mark(b, dx0, dx1);
                        }
                        sparse_done = true;
                    }
                    else if (src_mask == 0u && sx1 > sx0 && sx1 != 0xffffu)
                    {
                        /* Compatibility with any exact-span writer that has not adopted R42. */
                        uint32_t dx0 = 0u, dx1 = out_w;
                        r40_source_span_to_dest(req.width, out_w, sx0, sx1, &dx0, &dx1);
                        for (uint32_t b = b0; b <= b1; ++b)
                            r41_tilemask_mark(b, dx0, dx1);
                        sparse_done = true;
                    }
                    else
                    {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
                        ++s_r42_src_mask_fallbacks;
#endif
                    }
                }
                else
                {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
                    ++s_r41_span_fallbacks;
#endif
                }
            }
            if (!sparse_done)
                for (uint32_t b = b0; b <= b1; ++b)
                    r41_tilemask_mark(b, 0u, out_w);
        }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        else
        {
            ++writer_nochange;
        }
#endif
        dy = dy_end;
    }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const uint32_t scan_us = (uint32_t)(esp_timer_get_time() - scan0);
    s_r36_scan_us += scan_us;
    s_r37_rows_tested += gen_rows;
    s_r37_rows_changed += gen_changed;
    s_r37_rows_same += writer_nochange;
#endif

    if (retry_count) *retry_count += retries;
    if (unstable_count) *unstable_count += unstable;
    retries = unstable = 0u;

    uint32_t dirty_blocks = 0u;
    for (uint32_t b = 0u; b < kR41TileRows; ++b)
        if (r41_tilemask_any(b)) ++dirty_blocks;

    ++s_aspect_dirty_frames;
    if (full_front) ++s_aspect_full_frames;
    if (!dirty_blocks) return true;

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    uint32_t scale_us = 0u, store_us = 0u;
    uint64_t frame_run_width_sum = 0u, frame_logical_pixels = 0u;
    uint32_t frame_dirty_tiles = 0u, frame_tile_runs = 0u;
#endif
    std::memset(s_r40_sync_rects, 0, sizeof(s_r40_sync_rects));
    uint32_t sync_rect_count = 0u;
    bool sync_overflow = false;
    bool verify_tile_valid = false;
    uint32_t verify_tx = 0u, verify_ty = 0u;

    for (uint32_t b = 0u; b < kR41TileRows; ++b)
    {
        if (!r41_tilemask_any(b)) continue;
        const uint32_t by = b * 8u;
        uint32_t next_tile = 0u, t0 = 0u, t1 = 0u;
        while (r41_next_tile_run(b, next_tile, &t0, &t1))
        {
            next_tile = t1;
            const uint32_t bx0 = t0 * 8u;
            const uint32_t bx1 = std::min<uint32_t>(out_w, t1 * 8u);
            if (bx1 <= bx0) continue;

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            ++frame_tile_runs;
            frame_dirty_tiles += t1 - t0;
            frame_run_width_sum += bx1 - bx0;
            frame_logical_pixels += (uint64_t)(bx1 - bx0) * 8u;
#endif
            if (!verify_tile_valid)
            {
                verify_tile_valid = true;
                verify_tx = bx0;
                verify_ty = by;
            }

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            const int64_t scale0 = esp_timer_get_time();
#endif
            uint32_t prev_sy = 0xffffffffu;
            for (uint32_t j = 0u; j < 8u; ++j)
            {
                const uint32_t out_y = by + j;
                const uint32_t sy = s_aspect_ymap[out_y];
                uint16_t *dst = s_r35_pack8 + (size_t)j * out_w;
                if (j != 0u && sy == prev_sy)
                    std::memcpy(dst + bx0, dst - out_w + bx0, (bx1 - bx0) * sizeof(uint16_t));
                else if (!r40_scale_row_span_stable(req, sy, dst, bx0, bx1, &retries))
                {
                    ++unstable;
                    if (retry_count) *retry_count += retries;
                    if (unstable_count) *unstable_count += unstable;
                    std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
                    return false;
                }
                prev_sy = sy;
            }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            scale_us += (uint32_t)(esp_timer_get_time() - scale0);
#endif

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            const int64_t store0 = esp_timer_get_time();
#endif
            for (uint32_t x = bx0; x < bx1; x += 8u)
                r35_swrot_tile8_packed(front, s_r35_pack8, out_w,
                                        logical_x, logical_y, x, by);
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            store_us += (uint32_t)(esp_timer_get_time() - store0);
#endif

            /* Cache-sync rectangles can coalesce vertically when the same X
             * tile run repeats in the next 8-row block.  Unlike R40, distant
             * runs in one block remain separate. */
            bool merged = false;
            for (uint32_t i = sync_rect_count; i > 0u; --i)
            {
                r40_dirty_rect_t &r = s_r40_sync_rects[i - 1u];
                if (r.y1 == by && r.x0 == bx0 && r.x1 == bx1)
                {
                    r.y1 = (uint16_t)(by + 8u);
                    merged = true;
                    break;
                }
            }
            if (!merged)
            {
                if (sync_rect_count < kR41SyncRectCap)
                {
                    s_r40_sync_rects[sync_rect_count++] = {
                        (uint16_t)bx0, (uint16_t)bx1,
                        (uint16_t)by, (uint16_t)(by + 8u)};
                }
                else
                {
                    sync_overflow = true;
                }
            }
        }
    }

    if (retry_count) *retry_count += retries;
    if (unstable_count) *unstable_count += unstable;

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const int64_t sync0 = esp_timer_get_time();
#endif
    if (sync_overflow)
    {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        ++s_r41_sync_overflows;
#endif
        if (!r40_sync_front_rect(front, logical_x, logical_y,
                                 0u, out_w, 0u, out_h))
        {
            ESP_LOGW(TAG, "PX68K_LCDR42: full Front C2M overflow fallback failed; legacy fallback retained");
            std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
            return false;
        }
    }
    else
    {
        for (uint32_t i = 0u; i < sync_rect_count; ++i)
        {
            const r40_dirty_rect_t &r = s_r40_sync_rects[i];
            if (!r40_sync_front_rect(front, logical_x, logical_y,
                                     r.x0, r.x1, r.y0, r.y1))
            {
                ESP_LOGW(TAG, "PX68K_LCDR42: Front tile-rect C2M failed; legacy fallback retained");
                std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
                return false;
            }
        }
    }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const uint32_t sync_us = (uint32_t)(esp_timer_get_time() - sync0);
#endif

    /* One written tile every 16 dirty updates is checked end-to-end.  No
     * periodic success log is emitted: only the aggregate appears in the
     * 120-update stats line, while a stable mismatch is reported immediately. */
    const uint64_t next_update = s_r36_updates + 1u;
    if (verify_tile_valid && (next_update & 15u) == 0u)
    {
        uint32_t bad_x = 0xffffffffu, bad_y = 0xffffffffu;
        uint16_t expected = 0u, got = 0u;
        if (!r41_verify_native_tile(req, front, logical_x, logical_y,
                                    verify_tx, verify_ty,
                                    &bad_x, &bad_y, &expected, &got))
        {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            ++s_r41_verify_skip;
#endif
        }
        else if (bad_x != 0xffffffffu)
        {
            ++s_r41_verify_fail;
            if (s_r41_verify_fail <= 4u)
                ESP_LOGE(TAG,
                         "PX68K_LCDR42: DISPLAY-CANARY MISMATCH logical=%u,%u exp=%04X got=%04X tile=%u,%u fail=%llu",
                         (unsigned)bad_x, (unsigned)bad_y,
                         (unsigned)expected, (unsigned)got,
                         (unsigned)verify_tx, (unsigned)verify_ty,
                         (unsigned long long)s_r41_verify_fail);
        }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        else
        {
            ++s_r41_verify_ok;
        }
#endif
    }

    if (full_front)
    {
        s_r49_force_full_front = false;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        ++s_r49_full_repaints;
#endif
    }
    s_dsi_need_full_sync = false; s_dsi_repair_pending = false;
    s_dsi_ppa_prev_band_count = 0u; s_dsi_ppa_prime_other = false;
    s_dsi_ppa_geom_w = out_w; s_dsi_ppa_geom_h = out_h;
    s_dsi_ppa_geom_x = logical_x; s_dsi_ppa_geom_y = logical_y;

    ++s_r36_updates;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_r36_scale_us += scale_us; s_r36_store_us += store_us; s_r36_sync_us += sync_us;
    s_r36_blocks += dirty_blocks;
    s_r40_block_width_sum += frame_run_width_sum;
    s_r40_logical_pixels += frame_logical_pixels;
    s_r41_dirty_tiles += frame_dirty_tiles;
    s_r41_tile_runs += frame_tile_runs;
    s_r41_sync_rects_total += sync_overflow ? 1u : sync_rect_count;
    s_aspect_dirty_rows += (uint64_t)dirty_blocks * 8u;
    s_aspect_dirty_bands += sync_overflow ? 1u : sync_rect_count;

    if ((s_r36_updates % (PX68K_TAB5_R43_QUIET_RUNTIME ? 480u : 120u)) == 0u)
    {
        const uint64_t calls = s_r36_calls ? s_r36_calls : 1u;
        const uint64_t updates = s_r36_updates ? s_r36_updates : 1u;
        ESP_LOGI(TAG,
                 "PX68K_LCDR49: PRODUCER-TILE32 updates=%llu calls=%llu avgGENscan=%lluus avgSCALEupd=%lluus avgSTOREupd=%lluus avgSYNCupd=%lluus avgBlocks=%llu avgRuns=%llu avgTiles=%llu avgRunW=%llu logicalPix=%llu genRows=%llu genChanged=%llu srcTileRows=%llu srcTiles=%llu spanFallback=%llu maskFallback=%llu syncOv=%llu verify=%llu/%llu/%llu stage960RW=0 rawCompare=0 swapsHeld=%llu front=%u epochDrop=%llu noFlip=1 fullReq=%llu fullDone=%llu pinFail=%llu",
                 (unsigned long long)s_r36_updates, (unsigned long long)s_r36_calls,
                 (unsigned long long)(s_r36_scan_us / calls),
                 (unsigned long long)(s_r36_scale_us / updates),
                 (unsigned long long)(s_r36_store_us / updates),
                 (unsigned long long)(s_r36_sync_us / updates),
                 (unsigned long long)(s_r36_blocks / updates),
                 (unsigned long long)(s_r41_tile_runs / updates),
                 (unsigned long long)(s_r41_dirty_tiles / updates),
                 (unsigned long long)(s_r41_tile_runs ? (s_r40_block_width_sum / s_r41_tile_runs) : 0u),
                 (unsigned long long)s_r40_logical_pixels,
                 (unsigned long long)s_r37_rows_tested,
                 (unsigned long long)s_r37_rows_changed,
                 (unsigned long long)s_r42_src_tile_rows,
                 (unsigned long long)s_r42_src_tiles,
                 (unsigned long long)s_r41_span_fallbacks,
                 (unsigned long long)s_r42_src_mask_fallbacks,
                 (unsigned long long)s_r41_sync_overflows,
                 (unsigned long long)s_r41_verify_ok,
                 (unsigned long long)s_r41_verify_skip,
                 (unsigned long long)s_r41_verify_fail,
                 (unsigned long long)s_dbfb_swap_count, (unsigned)s_dsi_front_idx,
                 (unsigned long long)s_live_epoch_drops,
                 (unsigned long long)s_r49_full_repaint_requests,
                 (unsigned long long)s_r49_full_repaints,
                 (unsigned long long)s_r49_fb0_pin_failures);
    }
#endif
    return true;
}
#endif

static void push_snapshot(const present_request_t &req)
{
    if (req.slot_index >= kPresentSlots)
        return;

    present_slot_t &slot = s_slots[req.slot_index];
    const int lcd_w = M5.Display.width();
    const int lcd_h = M5.Display.height();
    const int x = (lcd_w - (int)slot.width) / 2;
    const int y = (lcd_h - (int)slot.height) / 2;

    M5.Display.pushImage(x, y, (int)slot.width, (int)slot.height, slot.pixels);
}

static bool push_live_frame(const present_request_t &req,
                            uint32_t *retry_count,
                            uint32_t *unstable_count,
                            bool force_source_scan = false)
{
    const int lcd_w = M5.Display.width();
    const int lcd_h = M5.Display.height();

    /* Normal games keep the fixed 4:3 960x720 viewport so the agreed side
     * controls always have 160px on both sides.  PANIC's old standalone
     * renderer, however, intentionally preserved the source raster aspect
     * (notably 256x256).  Keep that behaviour in PANIC mode so square data is
     * not stretched into 4:3.  Layer8 Aug/17/2026 */
    uint32_t out_w = kAspectViewportWidth;
    uint32_t out_h = kAspectViewportHeight;
    if (s_panic_compat_enabled && req.width && req.height)
    {
        out_w = kAspectViewportWidth;
        out_h = (uint32_t)(((uint64_t)out_w * req.height) / req.width);
        if (out_h > kAspectViewportHeight)
        {
            out_h = kAspectViewportHeight;
            out_w = (uint32_t)(((uint64_t)out_h * req.width) / req.height);
        }
        if (!out_w) out_w = 1;
        if (!out_h) out_h = 1;
    }
    if (!out_w || !out_h)
        return false;

    const int x = (lcd_w - (int)out_w) / 2;
    const int y0 = (lcd_h - (int)out_h) / 2;
    /* R56d: materialize presenter staging before deciding whether the request
     * has a legal direct-DBFB sink.  A managed ScreenVersion is never allowed
     * to fall back to M5GFX merely because the staging allocation happened
     * after the capability test. */
    if (!s_aspect_frame && !ensure_aspect_frame_allocated())
        return false;

    bool direct_fb = false;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    /* R56d: managed ScreenVersions are an independent Back-FB transaction.
     * R49's s_dsi_ppa_live flag describes only the legacy LIVE presenter. */
    const bool managed_backfb = (req.mode == PRESENT_MANAGED);
    direct_fb = s_dsi_double_live && (s_dsi_ppa_live || managed_backfb) &&
                s_dsi_fb[s_dsi_back_idx] && s_aspect_frame &&
                s_aspect_line_scratch &&
                s_dsi_stride_pixels == kDsiPhysStridePixels &&
                lcd_w == (int)kDsiPhysHeight &&
                lcd_h == (int)kDsiPhysWidth &&
                x >= 0 && y0 >= 0 &&
                (uint32_t)(x + (int)out_w) <= (uint32_t)lcd_w &&
                (uint32_t)(y0 + (int)out_h) <= (uint32_t)lcd_h;
    if (managed_backfb && !direct_fb)
        return false;
#endif

    auto live_dst_row = [&](uint32_t row) -> uint16_t * {
        /* R9 keeps the live image contiguous in landscape order for both the
         * PPA-ROT and legacy M5GFX paths. */
        return s_aspect_frame + (size_t)row * out_w;
    };
    uint32_t retries_total = 0;
    uint32_t unstable_total = 0;
    bool present_ok = true;

    /* Horizontal nearest-neighbour map is rebuilt only when CRTC geometry
     * changes.  This avoids integer division in the per-pixel hot loop. */
    if (s_aspect_map_src_w != req.width || s_aspect_map_dst_w != out_w)
    {
        for (uint32_t dx = 0; dx < out_w; ++dx)
        {
            uint32_t sx = (uint32_t)(((uint64_t)dx * (uint64_t)req.width) / (uint64_t)out_w);
            if (sx >= req.width) sx = req.width - 1u;
            s_aspect_xmap[dx] = (uint16_t)sx;
        }
        s_aspect_map_src_w = req.width;
        s_aspect_map_dst_w = out_w;
    }

    if (s_aspect_map_src_h != req.height || s_aspect_map_dst_h != out_h)
    {
        for (uint32_t dy = 0; dy < out_h; ++dy)
        {
            uint32_t sy = (uint32_t)(((uint64_t)dy * (uint64_t)req.height) / (uint64_t)out_h);
            if (sy >= req.height) sy = req.height - 1u;
            s_aspect_ymap[dy] = (uint16_t)sy;
        }
        s_aspect_map_src_h = req.height;
        s_aspect_map_dst_h = out_h;
    }

    /* R56 managed snapshots rotate through presenter-owned slots.  Pointer
     * identity is therefore transport identity, not screen geometry. Treating
     * every new slot pointer as a geometry change would recreate the retired
     * MASS-DIRTY/full-refresh loop.  Managed source rows are explicitly scanned
     * below, so only actual dimensions/pitch define geometry for them. */
    const bool source_identity_changed =
        (req.mode != PRESENT_MANAGED) && (s_aspect_seen_frame != req.frame);
    const bool geometry_changed =
        source_identity_changed ||
        s_aspect_seen_src_w != req.width ||
        s_aspect_seen_src_h != req.height ||
        s_aspect_seen_pitch != req.pitch_pixels;
    const bool force_full = geometry_changed || !s_aspect_dirty_frames;

    if (s_aspect_log_src_w != req.width || s_aspect_log_src_h != req.height)
    {
        ESP_LOGI(TAG,
                 "Build 5.98a 4:3 DIRTY frontend: raw=%lux%lu pitch=%lu -> LCD=%lux%lu viewport=%lux%lu; geometry change=full, steady-state=dirty bands",
                 (unsigned long)req.width, (unsigned long)req.height,
                 (unsigned long)req.pitch_pixels,
                 (unsigned long)lcd_w, (unsigned long)lcd_h,
                 (unsigned long)out_w, (unsigned long)out_h);
        s_aspect_log_src_w = req.width;
        s_aspect_log_src_h = req.height;
    }

    if (geometry_changed)
    {
        s_aspect_seen_frame = req.frame;
        s_aspect_seen_src_w = req.width;
        s_aspect_seen_src_h = req.height;
        s_aspect_seen_pitch = req.pitch_pixels;
        std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
#if defined(CONFIG_IDF_TARGET_ESP32P4) && PX68K_TAB5_R38_PRODUCER_EXACT_DIRTY
        /* producer generations are authoritative; no LCD-side raw shadow */
#endif
    }

#if defined(CONFIG_IDF_TARGET_ESP32P4) && PX68K_TAB5_R36_DIRECT_NATIVE && PX68K_TAB5_R38_PRODUCER_EXACT_DIRTY
    /* R38: producer-published exact generations eliminate all LCD-side raw
     * framebuffer compare traffic. PANIC/compat keeps the legacy path. */
    if (!force_source_scan && s_r32_direct_front && s_r34_selfcheck_ok && s_r35_pack8 &&
        r38_direct_native_present(req, out_w, out_h, (uint32_t)x, (uint32_t)y0,
                                  force_full, retry_count, unstable_count))
        return true;
#endif

    uint32_t band_start = 0xffffffffu;
    uint32_t band_end = 0;
    uint32_t clean_gap_rows = 0;
    uint32_t frame_dirty_rows = 0;
    uint32_t frame_dirty_bands = 0;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    dbfb_dirty_band_t frame_dirty_band_list[kDbfbDirtyBandCap] = {};
    uint32_t frame_dirty_band_count = 0u;
    bool frame_dirty_band_overflow = false;
#endif

    auto flush_band = [&](uint32_t start_row, uint32_t end_row) {
        if (start_row == 0xffffffffu || end_row <= start_row)
            return;
        if (!direct_fb)
        {
            M5.Display.pushImage(x, y0 + (int)start_row,
                                 (int)out_w, (int)(end_row - start_row),
                                 s_aspect_frame + (size_t)start_row * out_w);
        }
        /* DBFB path has no per-band LCD transaction.  Rows are written only
         * into the non-scanned Back buffer; one full-buffer pointer swap is
         * issued after every changed frame. */
        ++frame_dirty_bands;
        frame_dirty_rows += end_row - start_row;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
        if (direct_fb)
        {
            if (frame_dirty_band_count < kDbfbDirtyBandCap)
                frame_dirty_band_list[frame_dirty_band_count++] = {
                    (uint16_t)start_row, (uint16_t)end_row };
            else
                frame_dirty_band_overflow = true;
        }
#endif
    };

    /* A source row may be generation-dirty yet pixel-identical.  Treat that
     * case exactly like a clean generation row for LCD band coalescing. */
    auto note_clean_group = [&](uint32_t clean_start, uint32_t clean_end) {
        if (band_start != 0xffffffffu)
        {
            clean_gap_rows += clean_end - clean_start;
            if (clean_gap_rows > kDirtyMergeGapRows)
            {
                flush_band(band_start, band_end);
                band_start = 0xffffffffu;
                band_end = 0;
                clean_gap_rows = 0;
            }
        }
    };

    /* Walk destination rows in groups that reference the same source row.
     * For a 256-line source on a 720-line viewport this scales each source
     * row only once, then memcpy-replicates it into its 2-3 destination rows. */
    uint32_t dy = 0;
    while (dy < out_h)
    {
        const uint32_t sy = s_aspect_ymap[dy];

        uint32_t dy_end = dy + 1u;
        while (dy_end < out_h && s_aspect_ymap[dy_end] == sy)
            ++dy_end;

        const uint32_t observed_gen = line_generation(sy);
        /* PANIC compat pixels are rebuilt on CPU0 and do not carry WinDraw's
         * source-line generations.  Scan those rows every compat frame, but
         * keep the previous scaled LCD image so the exact PIE pixel diff can
         * suppress unchanged rows.  Do NOT turn the whole viewport into a
         * full refresh merely because WinDraw metadata is unavailable. */
        const bool source_dirty = force_full || force_source_scan ||
                                  line_writer_count(sy) != 0u ||
                                  s_aspect_seen_generation[sy] != observed_gen;

        if (!source_dirty)
        {
            note_clean_group(dy, dy_end);
            dy = dy_end;
            continue;
        }

        uint16_t *dst0 = live_dst_row(dy);
        uint16_t *render0 = s_aspect_line_scratch ? s_aspect_line_scratch : dst0;
        const uint16_t *src = req.frame + (size_t)sy * req.pitch_pixels;
        uint32_t attempt = 0;
        uint32_t stable_gen = observed_gen;
        bool stable = false;

        if (force_source_scan)
        {
            /* CPU0 just finished building the PANIC compat framebuffer, so
             * unlike WinDraw there is no concurrent writer to race here. */
            for (uint32_t dx = 0; dx < out_w; ++dx)
                render0[dx] = src[s_aspect_xmap[dx]];
            stable = true;
            stable_gen = observed_gen;
        }
        else
        {
            while (attempt < kLiveRowRetryLimit)
            {
                if (line_writer_count(sy) != 0)
                {
                    ++attempt;
                    ++retries_total;
                    taskYIELD();
                    continue;
                }

                const uint32_t gen0 = line_generation(sy);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                for (uint32_t dx = 0; dx < out_w; ++dx)
                    render0[dx] = src[s_aspect_xmap[dx]];
                __atomic_thread_fence(__ATOMIC_ACQUIRE);

                const uint32_t gen1 = line_generation(sy);
                const uint32_t writers1 = line_writer_count(sy);
                if (writers1 == 0 && gen0 == gen1)
                {
                    stable = true;
                    stable_gen = gen1;
                    break;
                }

                ++attempt;
                ++retries_total;
            }
        }

        bool pixel_changed = true;
        if (!stable)
        {
            ++unstable_total;
            /* Keep the old generation so this row is retried next frame. */
            if (render0 != dst0)
            {
                tab5_pie_graphics_copy(dst0, render0, out_w * sizeof(uint16_t));
            }
        }
        else
        {
            s_aspect_seen_generation[sy] = stable_gen;

            /* Generation dirtiness is conservative.  For DBFB R6 compare
             * against the correctly rotated native Back framebuffer instead
             * of pretending a logical LCD row is contiguous in memory. */
            if (!force_full)
            {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
                ++s_vdiff_tested_src_rows;
#endif
                if (render0 != dst0)
                {
                    pixel_changed = tab5_pie_graphics_diff(
                        render0, dst0, out_w * sizeof(uint16_t)) != 0;
                }

                if (pixel_changed)
                {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
                    ++s_vdiff_changed_src_rows;
#endif
                }
                else
                {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
                    ++s_vdiff_same_src_rows;
                    s_vdiff_identical_dst_rows += (uint64_t)(dy_end - dy);
#endif
                }
            }

            if (pixel_changed)
            {
                if (render0 != dst0)
                {
                    tab5_pie_graphics_copy(dst0, render0, out_w * sizeof(uint16_t));
                }
            }
        }

        if (!pixel_changed)
        {
            note_clean_group(dy, dy_end);
            dy = dy_end;
            continue;
        }

        /* R9 landscape staging is contiguous, so replicate scaled rows with
         * one ordinary PIE copy regardless of whether the final sink is PPA. */
        for (uint32_t copy_dy = dy + 1u; copy_dy < dy_end; ++copy_dy)
        {
            uint16_t *copy_dst = live_dst_row(copy_dy);
            tab5_pie_graphics_copy(copy_dst, dst0, out_w * sizeof(uint16_t));
        }

        if (band_start == 0xffffffffu)
            band_start = dy;
        /* If a tiny clean gap preceded this dirty group, include those cached
         * rows in the same push by simply extending band_end to dy_end. */
        band_end = dy_end;
        clean_gap_rows = 0;
        dy = dy_end;
    }

    if (band_start != 0xffffffffu)
        flush_band(band_start, band_end);

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (direct_fb && frame_dirty_rows != 0u)
    {
        const bool r9_force_full = force_full || frame_dirty_band_overflow;
        if (!dbfb615h17r9_ppa_present(
                s_aspect_frame, out_w, out_h,
                (uint32_t)x, (uint32_t)y0,
                frame_dirty_band_list, frame_dirty_band_count, r9_force_full,
                req.mode != PRESENT_MANAGED))
        {
            present_ok = false;
            /* PPA/DBFB failure falls back to the proven M5GFX path on the next
             * frame and forces a complete logical repaint. */
            s_aspect_seen_frame = nullptr;
            std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
        }
    }
#endif

    ++s_aspect_dirty_frames;
    if (force_full) ++s_aspect_full_frames;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_aspect_dirty_bands += frame_dirty_bands;
    s_aspect_dirty_rows += frame_dirty_rows;
#endif

    if (force_full)
    {
        ESP_LOGI(TAG,
                 "Build 5.98a DIRTY full refresh #%lu: %lu LCD rows in %lu band(s); subsequent frames use generation-dirty only",
                 (unsigned long)s_aspect_full_frames,
                 (unsigned long)frame_dirty_rows,
                 (unsigned long)frame_dirty_bands);
    }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    else if ((s_aspect_dirty_frames % 300u) == 0u)
    {
        const uint32_t avg_rows = s_aspect_dirty_frames
            ? (s_aspect_dirty_rows / s_aspect_dirty_frames) : 0u;
        const uint32_t avg_bands = s_aspect_dirty_frames
            ? (s_aspect_dirty_bands / s_aspect_dirty_frames) : 0u;
        ESP_LOGI(TAG,
                 "Build 5.98a DIRTY stats: frames=%lu full=%lu avgLCDrows=%lu/720 avgBands=%lu lastRows=%lu lastBands=%lu",
                 (unsigned long)s_aspect_dirty_frames,
                 (unsigned long)s_aspect_full_frames,
                 (unsigned long)avg_rows,
                 (unsigned long)avg_bands,
                 (unsigned long)frame_dirty_rows,
                 (unsigned long)frame_dirty_bands);
        ESP_LOGI(TAG,
                 "PX68K_VDIFF598G8: tested_src=%llu same=%llu changed=%llu identical_dst_rows=%llu scratch=%s",
                 (unsigned long long)s_vdiff_tested_src_rows,
                 (unsigned long long)s_vdiff_same_src_rows,
                 (unsigned long long)s_vdiff_changed_src_rows,
                 (unsigned long long)s_vdiff_identical_dst_rows,
                 s_aspect_line_scratch ? "PIE-DIFF" : "DISABLED");
#if defined(CONFIG_IDF_TARGET_ESP32P4)
        const uint32_t phys_us = __atomic_load_n(&s_dsi_refresh_period_us, __ATOMIC_RELAXED);
        const uint32_t phys_millihz = phys_us ? (uint32_t)(1000000000ULL / phys_us) : 0u;
        const uint64_t avg_sync = s_dbfb_sync_count ? (s_dbfb_sync_us / s_dbfb_sync_count) : 0u;
        ESP_LOGI(TAG,
                 "PX68K_DBFB615H17: active=%u front=%u fb0=%p fb1=%p swaps=%llu sync=%llu(full=%llu repair=%llu) avgSync=%lluus bytes=%llu pieSync=%llu flush=%llu/%lluus/%lluB refresh=%lu.%03luHz wait=%lluus timeouts=%llu",
                 s_dsi_double_live ? 1u : 0u, (unsigned)s_dsi_front_idx,
                 (void *)s_dsi_fb[0], (void *)s_dsi_fb[1],
                 (unsigned long long)s_dbfb_swap_count,
                 (unsigned long long)s_dbfb_sync_count,
                 (unsigned long long)s_dbfb_sync_full_count,
                 (unsigned long long)s_dbfb_sync_repair_count,
                 (unsigned long long)avg_sync,
                 (unsigned long long)s_dbfb_sync_bytes,
                 (unsigned long long)s_dbfb_sync_fallback,
                 (unsigned long long)s_dbfb_flush_count,
                 (unsigned long long)s_dbfb_flush_us,
                 (unsigned long long)s_dbfb_flush_bytes,
                 (unsigned long)(phys_millihz / 1000u),
                 (unsigned long)(phys_millihz % 1000u),
                 (unsigned long long)s_dbfb_refresh_wait_us,
                 (unsigned long long)s_dbfb_refresh_timeouts);
#endif
    }
#endif

    if (retry_count) *retry_count = retries_total;
    if (unstable_count) *unstable_count = unstable_total;
    return present_ok;
}

/* Build 6.15h17: keep the guest-derived host pacing, but the DBFB path also
 * gates buffer reuse on the physical DSI refresh_done event. h15 showed that
 * presenter at /2 did not reduce real work because CPU1 already submits LIVE
 * frames below that rate. The optimization is the DSI framebuffer bypass. */
static constexpr uint32_t kLiveLCDCadenceDiv = 1u;

static inline uint32_t video_pace_period_us(void)
{
    /* Same guest cadence used by the production speed meter in main.c,
     * multiplied only for the host LCD presenter. */
    const uint32_t guest_period = (CRTC_Regs[0x29] & 0x10u) ? 18031u : 16271u;
    return guest_period * kLiveLCDCadenceDiv;
}

static void video_pace_timer_cb(void *)
{
    TaskHandle_t task = s_present_task;
    if (task)
        xTaskNotifyGive(task);
}

static inline void video_pace_reset(void)
{
    s_pace_next_us = 0;
    s_pace_last_present_us = 0;
}

static void video_pace_keep_latest_live(present_request_t *req)
{
    if (!req || req->mode != PRESENT_LIVE_FB || !s_ready_requests)
        return;

    present_request_t newer = {};
    while (xQueueReceive(s_ready_requests, &newer, 0) == pdTRUE)
    {
        if (newer.mode != PRESENT_LIVE_FB)
        {
            /* Preserve mode/UI transition ordering; this queue is only two
             * deep, so returning the non-live request to the front is safe. */
            (void)xQueueSendToFront(s_ready_requests, &newer, 0);
            break;
        }
        *req = newer;
        portENTER_CRITICAL(&s_stats_mux);
        ++s_pace_coalesced_frames;
        portEXIT_CRITICAL(&s_stats_mux);
    }
}

static void video_pace_wait_live(const present_request_t &req)
{
    if ((req.mode != PRESENT_LIVE_FB && req.mode != PRESENT_MANAGED) ||
        s_panic_compat_enabled || !s_pace_timer)
    {
        video_pace_reset();
        return;
    }

    const uint32_t period = video_pace_period_us();
    if (!s_pace_active_logged)
    {
        ESP_LOGI(TAG,
                 "PX68K_LCD615H17: guest-derived host target=%uus (~%lu.%02luHz); LIVE path=%s; swaps additionally gated by physical DSI refresh_done",
                 (unsigned)period,
                 (unsigned long)(1000000u / period),
                 (unsigned long)(((100000000u / period) % 100u)),
                 s_dsi_double_live ? "DSI-DOUBLEFB" : "M5GFX-FALLBACK");
        s_pace_active_logged = true;
    }
    int64_t now = esp_timer_get_time();

    /* Re-lock phase after launcher/UI pauses, long stalls, or mode changes. */
    if (s_pace_next_us == 0 || now > s_pace_next_us + (int64_t)period * 4 ||
        s_pace_next_us > now + (int64_t)period * 4)
    {
        s_pace_next_us = now;
    }

    int64_t deadline = s_pace_next_us;
    uint32_t skipped = 0;
    while (now > deadline + (int64_t)(period / 2u))
    {
        deadline += period;
        ++skipped;
    }

    uint32_t waited = 0;
    if (now < deadline)
    {
        const uint64_t delay_us = (uint64_t)(deadline - now);
        /* Clear a stale notification defensively; this task has no other
         * direct-notification user. */
        (void)ulTaskNotifyTake(pdTRUE, 0);
        if (esp_timer_start_once(s_pace_timer, delay_us) == ESP_OK)
        {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            const int64_t after = esp_timer_get_time();
            waited = (after > now) ? (uint32_t)(after - now) : 0u;
            now = after;
        }
    }

    const uint32_t interval = (s_pace_last_present_us > 0 && now > s_pace_last_present_us)
        ? (uint32_t)(now - s_pace_last_present_us) : 0u;
    s_pace_last_present_us = now;
    s_pace_next_us = deadline + period;

    portENTER_CRITICAL(&s_stats_mux);
    if (waited)
    {
        ++s_pace_waits;
        s_pace_wait_us += waited;
    }
    s_pace_last_wait_us = waited;
    s_pace_skipped_slots += skipped;
    s_pace_last_interval_us = interval;
    portEXIT_CRITICAL(&s_stats_mux);
}

#if defined(CONFIG_IDF_TARGET_ESP32P4)
static void IRAM_ATTR r57e12_touch_irq_isr(void *)
{
    __atomic_store_n(&s_touch_irq_pending, 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_touch_irq_count, 1u, __ATOMIC_RELAXED);
}

static bool r57e12_touch_irq_init(void)
{
    /* M5Stack Tab5 ST7123 touch INT is GPIO23. M5.begin() already configured
     * the touch device/pin, so leave direction/pulls untouched and only add
     * the edge handler. Active-contact polling detects release even though the
     * IRQ is falling-edge only. */
    esp_err_t er = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "PX68K_TOUCHR57E12: gpio ISR service unavailable err=%d; sparse-poll fallback", (int)er);
        __atomic_store_n(&s_touch_irq_fallback, 1u, __ATOMIC_RELAXED);
        return false;
    }
    er = gpio_set_intr_type(GPIO_NUM_23, GPIO_INTR_NEGEDGE);
    if (er == ESP_OK) er = gpio_isr_handler_add(GPIO_NUM_23, r57e12_touch_irq_isr, nullptr);
    if (er == ESP_OK) er = gpio_intr_enable(GPIO_NUM_23);
    if (er != ESP_OK) {
        ESP_LOGW(TAG, "PX68K_TOUCHR57E12: GPIO23 IRQ attach failed err=%d; sparse-poll fallback", (int)er);
        __atomic_store_n(&s_touch_irq_fallback, 1u, __ATOMIC_RELAXED);
        return false;
    }
    ESP_LOGI(TAG, "PX68K_TOUCHR57E12: GPIO23 atomic-edge IRQ ACTIVE; Touch-only read on IRQ, active=10ms, idle safety=80ms; no guest-path M5.update()");
    return true;
}
#endif

static void video_present_task(void *)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    {
        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);
        esp_rom_printf("R56K5_TASKSELF name=px68k_lcd core=%d base=0x%08x top=0x%08x bytes=8192 hwm=%u\n",
                       (int)xPortGetCoreID(), (unsigned)r56k5_base,
                       (unsigned)(r56k5_base + 8192u),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif
    present_request_t req = {};

    for (;;)
    {
        /* R56k: one presenter loop/request is the scheduling quantum.  Putting
         * the yield at the loop boundary covers normal presents and every
         * early-continue ownership/drop path without entering a pixel copy. */
        taskYIELD();

        /* R57E12: keep a tiny bounded presenter wake so an IRQ atomic flag is
         * noticed quickly without introducing another task/stack.  Crucially,
         * a timeout no longer implies M5.update()/I2C work.  Active fingers use
         * 10 ms for cursor tracking (FreeRTOS tick=100 Hz); idle stays at 10 ms flag-check latency. */
        const TickType_t wait_ticks = pdMS_TO_TICKS(10);
        const bool have_req = xQueueReceive(s_ready_requests, &req, wait_ticks) == pdTRUE;
        bool touch_due = false;
        uint8_t touch_reason = 0u; /* 1=IRQ, 2=active follow, 3=safety/fallback */
        if (s_game_controls_enabled) {
            if (__atomic_exchange_n(&s_touch_irq_pending, 0u, __ATOMIC_ACQ_REL)) {
                touch_due = true; touch_reason = 1u;
            } else if (s_touch_contact_active ||
                       (s_game_touch_suppress_until_release && !s_touch_transition_present_guard)) {
                touch_due = true; touch_reason = 2u;
            } else {
                const int64_t now_touch_us = esp_timer_get_time();
                const int64_t safety_us = s_touch_irq_enabled ? 80000 : 20000;
                if (!s_touch_last_sample_us || now_touch_us - s_touch_last_sample_us >= safety_us) {
                    touch_due = true; touch_reason = 3u;
                }
            }
        }

        /* Build 6.12u: runtime launcher owns the panel while CPU1 is paused.
         * Drain stale snapshots but never touch LCD/touch until ownership returns. */
        if (__atomic_load_n(&s_host_ui_exclusive, __ATOMIC_ACQUIRE)) {
            if (have_req &&
                (req.mode == PRESENT_SNAPSHOT || req.mode == PRESENT_LIVE_FROZEN || req.mode == PRESENT_MANAGED) &&
                req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            if (have_req && req.mode == PRESENT_LIVE_FROZEN && s_frozen_present_done)
                (void)xSemaphoreGive(s_frozen_present_done);
            if (have_req && req.mode == PRESENT_MANAGED && req.screen_token)
                tab5_screen_manager_present_complete(req.screen_token, 0);
            continue;
        }

        /* FILE/KEY overlays already own touch + LCD and may intentionally keep
         * the guest frame hidden while its geometry changes behind them.  Defer
         * consuming a new guest transition until the overlay closes; otherwise
         * a present-required fence could deadlock an overlay that is supposed to
         * be closed by touch. */
        const bool overlay_owns_touch = s_media_overlay_active || s_softkbd_active;
        if (!overlay_owns_touch)
            (void)touch_transition_consume_pending();

        /* During the first physical present after a semantic screen change, do
         * not call M5.update() at all.  The next loop resumes touch sampling,
         * but actions remain fenced until release has actually been observed. */
        if (s_game_controls_enabled && touch_due &&
            (!s_touch_transition_present_guard || overlay_owns_touch))
            tab5_touch_joy_publish(poll_game_controls(touch_reason));

        /* FILE mode owns the LCD but not the guest.  Draw/redraw it even when
         * no video request arrived, while still draining queued snapshots. */
        if (s_media_overlay_active)
        {
            display_lock();
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            (void)dbfb615h17_prepare_m5gfx_front();
#endif
            if (s_media_overlay_redraw)
                draw_media_overlay_unlocked();
            display_unlock();
            if (have_req &&
                (req.mode == PRESENT_SNAPSHOT || req.mode == PRESENT_LIVE_FROZEN || req.mode == PRESENT_MANAGED) &&
                req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            if (have_req && req.mode == PRESENT_LIVE_FROZEN && s_frozen_present_done)
                (void)xSemaphoreGive(s_frozen_present_done);
            if (have_req && req.mode == PRESENT_MANAGED && req.screen_token)
                tab5_screen_manager_present_complete(req.screen_token, 0);
            continue;
        }

        if (s_softkbd_active)
        {
            display_lock();
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            (void)dbfb615h17_prepare_m5gfx_front();
#endif
            if (s_softkbd_redraw)
                draw_soft_keyboard_unlocked();
            display_unlock();
            if (have_req &&
                (req.mode == PRESENT_SNAPSHOT || req.mode == PRESENT_LIVE_FROZEN || req.mode == PRESENT_MANAGED) &&
                req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            if (have_req && req.mode == PRESENT_LIVE_FROZEN && s_frozen_present_done)
                (void)xSemaphoreGive(s_frozen_present_done);
            if (have_req && req.mode == PRESENT_MANAGED && req.screen_token)
                tab5_screen_manager_present_complete(req.screen_token, 0);
            continue;
        }

        if (!have_req)
            continue;

        /* Build 6.14d: when LCD work falls behind, discard only stale LIVE
         * requests and present the newest completed framebuffer.  Then align
         * CPU0 LCD work to the guest cadence; CPU1 is never delayed here. */
        video_pace_keep_latest_live(&req);
        video_pace_wait_live(req);
        /* A newer live request may arrive while the pace timer sleeps.  Keep
         * that one instead of presenting a stale queue token. */
        video_pace_keep_latest_live(&req);

        if ((req.mode == PRESENT_LIVE_FB || req.mode == PRESENT_LIVE_FROZEN) &&
            req.live_epoch != __atomic_load_n(&s_live_present_epoch, __ATOMIC_ACQUIRE))
        {
            ++s_live_epoch_drops;
            if (req.mode == PRESENT_LIVE_FROZEN) {
                if (req.slot_index < kPresentSlots)
                    (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
                if (s_frozen_present_done)
                    (void)xSemaphoreGive(s_frozen_present_done);
            }
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        uint32_t live_retries = 0;
        uint32_t live_unstable = 0;

        display_lock();
        if ((req.mode == PRESENT_LIVE_FB || req.mode == PRESENT_LIVE_FROZEN) &&
            req.live_epoch != __atomic_load_n(&s_live_present_epoch, __ATOMIC_ACQUIRE)) {
            ++s_live_epoch_drops;
            display_unlock();
            if (req.mode == PRESENT_LIVE_FROZEN) {
                if (req.slot_index < kPresentSlots)
                    (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
                if (s_frozen_present_done)
                    (void)xSemaphoreGive(s_frozen_present_done);
            }
            continue;
        }
        if (__atomic_load_n(&s_host_ui_exclusive, __ATOMIC_ACQUIRE)) {
            display_unlock();
            if ((req.mode == PRESENT_SNAPSHOT || req.mode == PRESENT_LIVE_FROZEN || req.mode == PRESENT_MANAGED) &&
                req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            if (req.mode == PRESENT_LIVE_FROZEN && s_frozen_present_done)
                (void)xSemaphoreGive(s_frozen_present_done);
            if (req.mode == PRESENT_MANAGED && req.screen_token)
                tab5_screen_manager_present_complete(req.screen_token, 0);
            continue;
        }
        if (!s_present_started || s_force_game_redraw)
        {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            (void)dbfb615h17_prepare_m5gfx_front();
#endif
            M5.Display.fillScreen(0x0000);
            draw_game_controls_unlocked();
            s_present_started = true;
            s_force_game_redraw = false;
        }
        /* R56d ownership boundary at the physical presenter:
         * managed ScreenVersions are already immutable and have a dedicated
         * low-level PPA -> Back-FB -> DSI swap transaction.  Do not enter an
         * M5GFX startWrite/endWrite transaction around that raw-panel path and
         * do not force FB0 current immediately before it.  UI/chrome drawing
         * above still uses M5GFX and marks the pair for a full sync. */
        if (req.mode != PRESENT_LIVE_FB && req.mode != PRESENT_MANAGED)
        {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            (void)dbfb615h17_prepare_m5gfx_front();
#endif
        }

        bool managed_present_ok = true;
        if (req.mode == PRESENT_MANAGED) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            managed_present_ok = s_dsi_double_live &&
                push_live_frame(req, &live_retries, &live_unstable, true);
#else
            managed_present_ok = false;
#endif
        } else {
            M5.Display.startWrite();
            if (req.mode == PRESENT_LIVE_FB || req.mode == PRESENT_LIVE_FROZEN) {
                present_request_t compat = {};
                if (req.mode == PRESENT_LIVE_FB && panic_render_compat_source(req, &compat)) {
                    /* Build 6.12g: compat pixels are freshly rebuilt on CPU0, but
                     * forcing s_aspect_seen_frame=NULL here made every PANIC frame
                     * a 640/720-row LCD full refresh and starved IDLE0 until WDT. */
                    (void)push_live_frame(compat, &live_retries, &live_unstable, true);
                } else if (req.mode == PRESENT_LIVE_FROZEN) {
                    (void)push_live_frame(req, &live_retries, &live_unstable, true);
                } else {
                    (void)push_live_frame(req, &live_retries, &live_unstable, false);
                }
            } else {
                push_snapshot(req);
            }
            M5.Display.endWrite();
        }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
        if (req.mode == PRESENT_MANAGED && managed_present_ok && s_dsi_double_live) {
            /* R56 Screen lifecycle contract: READY->VISIBLE is not acknowledged
             * when the swap is merely queued. Managed ScreenVersions always use
             * the Back framebuffer (direct-front is bypassed above), and this
             * wait confirms that this exact swap crossed the physical DSI
             * refresh boundary before Screen Manager retires the old VISIBLE. */
            managed_present_ok = dbfb615h17_wait_refresh();
        }
#endif
        display_unlock();

        touch_transition_present_complete(
            req, req.mode != PRESENT_MANAGED || managed_present_ok);

        if (req.mode == PRESENT_MANAGED) {
            static uint32_t r56d_stack_samples = 0u;
            if (r56d_stack_samples < 4u) {
                ++r56d_stack_samples;
                ESP_LOGI(TAG,
                         "PX68K_SCREEN_R56D: managed present return sample=%lu ok=%u lcdStackHighWater=%lu",
                         (unsigned long)r56d_stack_samples,
                         managed_present_ok ? 1u : 0u,
                         (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            }
        }

        const uint32_t push_us = (uint32_t)(esp_timer_get_time() - t0);
        portENTER_CRITICAL(&s_stats_mux);
        ++s_presented_frames;
        s_last_push_us = push_us;
        s_cpu0_push_total_us += push_us;
        if (req.mode == PRESENT_LIVE_FB || req.mode == PRESENT_LIVE_FROZEN || req.mode == PRESENT_MANAGED)
        {
            ++s_live_presented_frames;
            s_live_row_retries += live_retries;
            s_live_unstable_rows += live_unstable;
        }
        if (req.mode == PRESENT_LIVE_FROZEN)
            ++s_r52_frozen_presented;
        portEXIT_CRITICAL(&s_stats_mux);

        if ((req.mode == PRESENT_SNAPSHOT || req.mode == PRESENT_LIVE_FROZEN || req.mode == PRESENT_MANAGED) &&
            req.slot_index < kPresentSlots)
            (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);

        if (req.mode == PRESENT_LIVE_FROZEN && s_frozen_present_done)
            (void)xSemaphoreGive(s_frozen_present_done);
        if (req.mode == PRESENT_MANAGED && req.screen_token)
            tab5_screen_manager_present_complete(req.screen_token, managed_present_ok ? 1 : 0);

        /* Build 6.12g: PANIC can submit continuously while YM2151 also owns
         * CPU0.  One RTOS tick of blocking after a compat frame guarantees an
         * IDLE0 scheduling window instead of merely yielding to another ready
         * higher-priority worker.  At the 100-Hz system tick this is <=10 ms. */
        if (s_panic_compat_enabled && req.mode == PRESENT_LIVE_FB)
            vTaskDelay(1);

    }
}

static bool init_async_present(void)
{
    const int lcd_w = M5.Display.width();
    const int lcd_h = M5.Display.height();
    if (lcd_w <= 0 || lcd_h <= 0)
        return false;

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    /* scripts/patch_m5gfx_dsi_doublefb.py changes the pinned M5GFX Panel_DSI
     * instance to num_fbs=2 and publishes both driver-owned PSRAM buffers plus
     * the raw ESP-IDF DPI panel handle through config_detail(). */
    s_dsi_panel = static_cast<lgfx::Panel_DSI *>(M5.Display.getPanel());
    if (s_dsi_panel)
    {
        const auto dsi_cfg = s_dsi_panel->config_detail();
        s_dsi_fb[0] = reinterpret_cast<uint16_t *>(dsi_cfg.buffer);
        s_dsi_fb[1] = reinterpret_cast<uint16_t *>(dsi_cfg.buffer2);
        s_dsi_dpi_panel = dsi_cfg.px68k_dpi_panel;
        s_dsi_stride_pixels = kDsiPhysStridePixels;
    }

    s_dsi_refresh_event = xSemaphoreCreateBinary();
    s_dsi_sync_done = xSemaphoreCreateBinary();
    const bool buffers_ok = s_dsi_fb[0] && s_dsi_fb[1] &&
                            s_dsi_fb[0] != s_dsi_fb[1] && s_dsi_dpi_panel &&
                            lcd_w == 1280 && lcd_h == 720;
    bool callbacks_ok = false;
    if (buffers_ok && s_dsi_refresh_event)
    {
        esp_lcd_dpi_panel_event_callbacks_t cbs = {};
        cbs.on_refresh_done = dbfb615h17_refresh_cb;
        const esp_err_t cb_er = esp_lcd_dpi_panel_register_event_callbacks(
            s_dsi_dpi_panel, &cbs, nullptr);
        callbacks_ok = (cb_er == ESP_OK);
        if (!callbacks_ok)
            ESP_LOGW(TAG, "PX68K_DBFB615H17: refresh_done callback register failed err=%d", (int)cb_er);
    }

    /* Build 6.15h17R6: retain R5 decision to keep DBFB coherence off async GDMA.
     * The compositor keeps its independent proven AXI-GDMA path; only the
     * framebuffer repair/full-sync channel is disabled here. */
    s_dsi_sync_gdma = nullptr;

    s_dsi_double_live = buffers_ok && callbacks_ok;
    s_dsi_front_idx = 0u;
    s_dsi_back_idx = 1u;
    s_dsi_need_full_sync = true;
    s_dsi_repair_pending = false;
    s_dsi_repair_x0 = s_dsi_repair_y0 = s_dsi_repair_x1 = s_dsi_repair_y1 = 0u;
    s_dsi_swap_wait_refresh = false;
    if (s_dsi_double_live)
    {
        ESP_LOGI(TAG,
                 "PX68K_DBFB615H17R11: DSI DOUBLE-FB ARMED fb0=%p fb1=%p panel=%p logical=1280x720 rot=1 native=720x1280 stride=720 RGB565; Front=0 Back=1",
                 (void *)s_dsi_fb[0], (void *)s_dsi_fb[1], (void *)s_dsi_dpi_panel);
        ESP_LOGI(TAG,
                 "PX68K_DBFB615H17R42: warmup=PPA/DBFB x8 -> steady producer exact 32px source tile mask -> sparse 8x8 LCD tile runs -> Internal partial stripe -> IRAM/XespV native Front; display canary; UI chrome preserved");
    }
    else
    {
        ESP_LOGW(TAG,
                 "PX68K_DBFB615H17: patched double framebuffer unavailable; safe M5GFX pushImage path retained fb0=%p fb1=%p panel=%p",
                 (void *)s_dsi_fb[0], (void *)s_dsi_fb[1], (void *)s_dsi_dpi_panel);
    }
#endif

    s_slot_capacity_pixels = (size_t)lcd_w * (size_t)lcd_h;
    s_free_slots = xQueueCreate(kPresentSlots, sizeof(uint8_t));
    s_ready_requests = xQueueCreate(kPresentSlots, sizeof(present_request_t));
    s_frozen_present_done = xSemaphoreCreateBinary();
    /* R23: UI action payloads carry 512-byte paths and are cold while the game
     * runs.  Put this ~4 KiB queue in PSRAM; present/free queues remain Internal. */
    s_ui_actions = xQueueCreateWithCaps(8, sizeof(tab5_video_action_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_free_slots || !s_ready_requests || !s_ui_actions || !s_frozen_present_done)
        return false;

    for (uint8_t i = 0; i < kPresentSlots; ++i)
    {
        s_slots[i].pixels = static_cast<uint16_t *>(heap_caps_malloc(
            s_slot_capacity_pixels * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_slots[i].pixels)
            return false;
        if (xQueueSend(s_free_slots, &i, 0) != pdTRUE)
            return false;
    }

    /* R9 retains the R7 960x720 landscape staging surface.  The optimization
     * is now transaction-side: preserve exact dirty bands and avoid rotating
     * the untouched vertical distance between unrelated changes. */
    if (!ensure_aspect_frame_allocated())
        return false;
    s_panic_compat_frame = nullptr;

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (s_dsi_double_live && s_aspect_frame_ppa_capable)
    {
        /* R9 deliberately defers PPA client registration to px68k_lcd.
         * ESP-IDF recommends that each task owns its own client.  R8 created
         * the client in the init task, then consumed it from px68k_lcd and
         * depended on ISR completion tokens; the first full transaction timed
         * out on hardware.  R9 uses blocking transactions, max_pending=1, so
         * there is no callback/semaphore lifecycle at all. */
        s_dsi_ppa_live = true;
        ESP_LOGI(TAG,
                 "PX68K_DBFB615H17R42: 960x720 allocation is warmup-only after %u PPA/DBFB swaps; steady LCD does not read it",
                 (unsigned)kR32WarmupSwapFrames);
    }
    else if (s_dsi_double_live)
    {
        ESP_LOGW(TAG,
                 "PX68K_DBFB615H17R11: staging is not PPA/DMA capable; disabling raw DBFB and retaining M5GFX fallback");
        s_dsi_double_live = false;
    }
#endif

#if defined(CONFIG_IDF_TARGET_ESP32P4) && PX68K_TAB5_R35_INTERNAL_STRIPE
    /* R35 keeps only eight 960-pixel landscape rows in Internal SRAM while
     * transposing.  This turns R34's thousands of tiny PSRAM tile gathers
     * into one sequential 15-KiB read per stripe. */
    s_r35_pack8 = static_cast<uint16_t *>(heap_caps_aligned_alloc(
        16, (size_t)kAspectViewportWidth * 8u * sizeof(uint16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_r35_pack8)
        ESP_LOGI(TAG, "PX68K_LCDR42: Internal native stripe ready bytes=%u ptr=%p",
                 (unsigned)(kAspectViewportWidth * 8u * sizeof(uint16_t)),
                 (void *)s_r35_pack8);
    else
        ESP_LOGW(TAG, "PX68K_LCDR42: Internal native stripe allocation failed; R34 direct PSRAM-tile fallback retained");
#endif

    /* R35 moves the 1.9-KiB scale/diff line scratch to Internal SRAM as well.
     * It is written for every generation-dirty source row, including rows
     * later proven pixel-identical, so keeping it out of PSRAM reduces cache
     * pollution before the rotation experiment even starts. */
    s_aspect_line_scratch_raw = static_cast<uint8_t *>(heap_caps_malloc(
        (size_t)kAspectViewportWidth * sizeof(uint16_t) + 15u,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_r35_line_scratch_internal = (s_aspect_line_scratch_raw != nullptr);
    if (!s_aspect_line_scratch_raw)
        s_aspect_line_scratch_raw = static_cast<uint8_t *>(heap_caps_malloc(
            (size_t)kAspectViewportWidth * sizeof(uint16_t) + 15u,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_aspect_line_scratch_raw)
    {
        const uintptr_t raw_mod = (uintptr_t)s_aspect_line_scratch_raw & 15u;
        const uintptr_t direct_view = (uintptr_t)s_aspect_frame;
        const uintptr_t frame_mod = direct_view & 15u;
        const uintptr_t adjust = (frame_mod + 16u - raw_mod) & 15u;
        s_aspect_line_scratch = reinterpret_cast<uint16_t *>(
            s_aspect_line_scratch_raw + adjust);
        ESP_LOGI(TAG,
                 "Build 6.15h17R42 line scratch armed: bytes=%u placement=%s legacy-stage/scratch mod16=%u/%u doublefb=%u",
                 (unsigned)(kAspectViewportWidth * sizeof(uint16_t)),
                 s_r35_line_scratch_internal ? "INTERNAL" : "PSRAM-FALLBACK",
                 (unsigned)frame_mod,
                 (unsigned)((uintptr_t)s_aspect_line_scratch & 15u),
                 s_dsi_double_live ? 1u : 0u);
    }
    else
    {
        ESP_LOGW(TAG, "Build 5.98g9b LCD PIE-DIFF scratch unavailable; generation-dirty fallback retained");
    }

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    s_r34_selfcheck_ok = r34_swrot_selfcheck();
    ESP_LOGI(TAG,
             "PX68K_LCDR42: native-store self-check %s; stripe=%s lineScratch=%s; producerExactDirty=ON producerTile32=ON lcdTileMask=120x90 noOpGeneration=SUPPRESSED LCDrawCompare=OFF displayCanary=ON",
             s_r34_selfcheck_ok ? "PASS" : "FAIL -> R32 PPA32 fallback",
             s_r35_pack8 ? "INTERNAL" : "R34-FALLBACK",
             s_r35_line_scratch_internal ? "INTERNAL" : "PSRAM");

#if PX68K_TAB5_R49_FB0_NOFLIP
    /* R49: M5GFX's line table is attached to driver FB0. Select FB0 exactly
     * once, wait for a physical refresh boundary, then enter direct-native
     * mode immediately. Normal presentation never selects FB1 afterwards. */
    if (s_dsi_double_live && s_dsi_dpi_panel && s_dsi_fb[0] &&
        s_r34_selfcheck_ok && s_r35_pack8)
    {
        const uint32_t seq0 = __atomic_load_n(&s_dsi_refresh_seq, __ATOMIC_ACQUIRE);
        const esp_err_t pin_er = esp_lcd_panel_draw_bitmap(
            s_dsi_dpi_panel, 0, 0,
            (int)kDsiPhysWidth, (int)kDsiPhysHeight, s_dsi_fb[0]);
        if (pin_er == ESP_OK)
        {
            s_dsi_front_idx = 0u;
            s_dsi_back_idx = 1u;
            s_dsi_swap_refresh_seq = seq0;
            s_dsi_swap_wait_refresh = true;
            if (dbfb615h17_wait_refresh())
            {
                s_r32_direct_front = true;
                s_r32_warmup_swaps = kR32WarmupSwapFrames;
                s_dsi_ppa_live = false;
                s_dsi_need_full_sync = true;
                s_r49_force_full_front = true;
                ++s_r49_full_repaint_requests;
                s_aspect_seen_frame = nullptr;
                std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
                s_r34_front_cache_primed = false;
                ESP_LOGW(TAG,
                         "PX68K_LCDR49: FB0 NO-FLIP ACTIVE from init; FB1 scanout quarantined; first LIVE=mandatory full 960x720 repaint");
            }
            else
            {
                ++s_r49_fb0_pin_failures;
                ESP_LOGW(TAG,
                         "PX68K_LCDR49: FB0 initial refresh confirmation failed; legacy 8-swap warmup retained");
            }
        }
        else
        {
            ++s_r49_fb0_pin_failures;
            ESP_LOGW(TAG,
                     "PX68K_LCDR49: FB0 initial pin failed err=%d; legacy 8-swap warmup retained",
                     (int)pin_er);
        }
    }
#endif
#endif

    esp_timer_create_args_t pace_args = {};
    pace_args.callback = &video_pace_timer_cb;
    pace_args.name = "px68k_vpace";
    if (esp_timer_create(&pace_args, &s_pace_timer) == ESP_OK)
        ESP_LOGI(TAG, "PX68K_PACE614D: host LCD cadence timer ready; latest-live-frame pacing armed");
    else
        ESP_LOGW(TAG, "PX68K_PACE614D: cadence timer unavailable; immediate presentation retained");

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    s_touch_irq_enabled = r57e12_touch_irq_init();
#else
    s_touch_irq_enabled = false;
    __atomic_store_n(&s_touch_irq_fallback, 1u, __ATOMIC_RELAXED);
#endif

#if portNUM_PROCESSORS > 1
    const BaseType_t ok = xTaskCreatePinnedToCore(
        video_present_task, "px68k_lcd", 8192, nullptr, 3, &s_present_task, 0);
#else
    const BaseType_t ok = xTaskCreate(
        video_present_task, "px68k_lcd", 8192, nullptr, 3, &s_present_task);
#endif
    if (ok != pdPASS)
        return false;

    ESP_LOGI(TAG,
             "PX68K_SCREEN_R56D: LCD task stack=8192; managed path bypasses M5GFX write transaction and owns raw PPA/Back-FB/swap only");
    ESP_LOGI(TAG,
             "PX68K_HOST_R56K: LCD presenter prio=3 queue-blocking + IRQ-flag touch sampling boundary-yield stack=8192");
    return true;
}

extern "C" {

void tab5_video_touch_transition_begin(uint32_t width, uint32_t height, uint32_t pitch_pixels)
{
    /* CPU1 publisher: data first, epoch last with release ordering.  CPU0 is
     * the only side that touches M5Unified state or mutates the local fence. */
    __atomic_store_n(&s_touch_transition_pub_w, width, __ATOMIC_RELAXED);
    __atomic_store_n(&s_touch_transition_pub_h, height, __ATOMIC_RELAXED);
    __atomic_store_n(&s_touch_transition_pub_pitch, pitch_pixels, __ATOMIC_RELAXED);
    uint32_t e = __atomic_add_fetch(&s_touch_transition_epoch, 1u, __ATOMIC_RELEASE);
    if (!e) {
        __atomic_store_n(&s_touch_transition_epoch, 1u, __ATOMIC_RELEASE);
    }
}

void tab5_video_fb_line_write_begin(uint32_t y)
{
    if (y >= kTrackedLines) return;
    (void)__atomic_add_fetch(&s_line_writers[y], 1u, __ATOMIC_ACQ_REL);
}

void tab5_video_fb_line_write_end_changed_span(uint32_t y, int changed,
                                               uint32_t x0, uint32_t x1)
{
    if (y >= kTrackedLines) return;
    if (changed)
    {
        uint16_t nx0 = (uint16_t)std::min<uint32_t>(x0, 0xffffu);
        uint16_t nx1 = (uint16_t)std::min<uint32_t>(x1, 0xffffu);
        if (nx1 <= nx0) { nx0 = 0u; nx1 = 0xffffu; }
        portENTER_CRITICAL(&s_line_dirty_span_mux);
        s_line_dirty_tile32[y] = 0xffffffffu; /* generic/span writer => conservative full row for R42 */
        if (s_line_dirty_x1[y] <= s_line_dirty_x0[y])
        {
            s_line_dirty_x0[y] = nx0;
            s_line_dirty_x1[y] = nx1;
        }
        else
        {
            if (nx0 < s_line_dirty_x0[y]) s_line_dirty_x0[y] = nx0;
            if (nx1 > s_line_dirty_x1[y]) s_line_dirty_x1[y] = nx1;
        }
        (void)__atomic_add_fetch(&s_line_generation[y], 1u, __ATOMIC_RELEASE);
        portEXIT_CRITICAL(&s_line_dirty_span_mux);
    }
    (void)__atomic_sub_fetch(&s_line_writers[y], 1u, __ATOMIC_RELEASE);
}

void tab5_video_fb_line_write_end_changed_tiles32(uint32_t y, int changed,
                                                    uint32_t tile_mask, uint32_t width_pixels)
{
    if (y >= kTrackedLines) return;
    if (changed)
    {
        const uint32_t tiles = (width_pixels + 31u) >> 5;
        const uint32_t useful = (tiles >= 32u) ? 0xffffffffu :
            (tiles ? ((1u << tiles) - 1u) : 0u);
        uint32_t m = tile_mask & useful;
        if (!m) m = useful ? useful : 0xffffffffu; /* defensive full-row publication */
        portENTER_CRITICAL(&s_line_dirty_span_mux);
        if (s_line_dirty_tile32[y] == 0xffffffffu || m == 0xffffffffu)
            s_line_dirty_tile32[y] = 0xffffffffu;
        else
            s_line_dirty_tile32[y] |= m;
        /* Keep a conservative envelope for fallback/debug consumers. */
        uint32_t first = (uint32_t)__builtin_ctz(m);
        uint32_t last = 31u - (uint32_t)__builtin_clz(m);
        uint16_t nx0 = (uint16_t)std::min<uint32_t>(first << 5, 0xffffu);
        uint16_t nx1 = (uint16_t)std::min<uint32_t>(width_pixels, (last + 1u) << 5);
        if (s_line_dirty_x1[y] <= s_line_dirty_x0[y])
        {
            s_line_dirty_x0[y] = nx0;
            s_line_dirty_x1[y] = nx1;
        }
        else
        {
            if (nx0 < s_line_dirty_x0[y]) s_line_dirty_x0[y] = nx0;
            if (nx1 > s_line_dirty_x1[y]) s_line_dirty_x1[y] = nx1;
        }
        (void)__atomic_add_fetch(&s_line_generation[y], 1u, __ATOMIC_RELEASE);
        portEXIT_CRITICAL(&s_line_dirty_span_mux);
    }
    (void)__atomic_sub_fetch(&s_line_writers[y], 1u, __ATOMIC_RELEASE);
}

void tab5_video_fb_line_write_end_changed(uint32_t y, int changed)
{
    tab5_video_fb_line_write_end_changed_span(y, changed, 0u, 0xffffu);
}

void tab5_video_fb_line_write_end(uint32_t y)
{
    tab5_video_fb_line_write_end_changed_span(y, 1, 0u, 0xffffu);
}

void tab5_video_fb_range_write_begin(uint32_t y, uint32_t count)
{
    uint32_t end = y + count;
    if (end > kTrackedLines) end = kTrackedLines;
    for (uint32_t row = y; row < end; ++row)
        tab5_video_fb_line_write_begin(row);
}

void tab5_video_fb_range_write_end(uint32_t y, uint32_t count)
{
    uint32_t end = y + count;
    if (end > kTrackedLines) end = kTrackedLines;
    for (uint32_t row = y; row < end; ++row)
        tab5_video_fb_line_write_end(row);
}

void tab5_video_set_game_controls_enabled(int enabled)
{
    s_game_controls_enabled = enabled != 0;
    s_utility_prev_mask = 0;
    s_touch_dpad_prev = 0;
    s_touch_dpad_press_us = 0;
    if (!s_game_controls_enabled) {
        update_game_touch_keyboard(0);
        s_media_overlay_active = false;
        s_media_overlay_redraw = false;
        s_softkbd_active = false;
        s_softkbd_redraw = false;
        s_softkbd_touch_down = false;
        s_softkbd_shift = false;
        s_softkbd_ctrl = false;
        s_soft_input_count = 0;
        s_game_touch_key_prev = 0;
        /* This function is called from CPU1 for runtime ownership changes.
         * Apply zero immediately on CPU1 and tell LP to discard any older
         * CPU0 latest state.  Cold-start CPU0 calls happen before LP is active. */
        (void)tab5_guest_input_queue_touch_joypad(0);
        if (xPortGetCoreID() == 1)
            (void)tab5_lp_broker_touch_control_publish(0);
    } else {
        s_present_started = false;
        s_aspect_seen_frame = nullptr;
        tab5_video_touch_transition_begin(0u, 0u, 0u);
    }
}

int tab5_video_poll_action(tab5_video_action_t *out)
{
    if (!out || !s_ui_actions) return 0;
    std::memset(out, 0, sizeof(*out));
    return xQueueReceive(s_ui_actions, out, 0) == pdTRUE ? 1 : 0;
}

void tab5_video_set_runtime_media_paths(const char *fdd0,
                                        const char *fdd1,
                                        const char *hdd0)
{
    portENTER_CRITICAL(&s_runtime_media_mux);
    std::snprintf(s_runtime_fdd0, sizeof(s_runtime_fdd0), "%s", fdd0 ? fdd0 : "");
    std::snprintf(s_runtime_fdd1, sizeof(s_runtime_fdd1), "%s", fdd1 ? fdd1 : "");
    std::snprintf(s_runtime_hdd0, sizeof(s_runtime_hdd0), "%s", hdd0 ? hdd0 : "");
    /* While FILE is open, CPU1 is the source of truth after each immediate
     * mount/eject action. Mirror the actual result back into the shared UI so
     * a failed mount cannot leave a stale filename on screen. */
    if (s_media_overlay_active) {
        std::snprintf(s_media_pending_fdd0, sizeof(s_media_pending_fdd0), "%s", s_runtime_fdd0);
        std::snprintf(s_media_pending_fdd1, sizeof(s_media_pending_fdd1), "%s", s_runtime_fdd1);
        std::snprintf(s_media_pending_hdd0, sizeof(s_media_pending_hdd0), "%s", s_runtime_hdd0);
    }
    portEXIT_CRITICAL(&s_runtime_media_mux);
    if (s_media_overlay_active)
        s_media_overlay_redraw = true;
}

void tab5_video_set_runtime_boot_source(int boot_source)
{
    portENTER_CRITICAL(&s_runtime_media_mux);
    s_runtime_boot_source = boot_source == TAB5_LAUNCH_BOOT_HDD0 ?
                            TAB5_LAUNCH_BOOT_HDD0 : TAB5_LAUNCH_BOOT_FLOPPY0;
    portEXIT_CRITICAL(&s_runtime_media_mux);
}

static void drain_present_requests_for_host_ui(void)
{
    if (!s_ready_requests) return;
    present_request_t stale = {};
    while (xQueueReceive(s_ready_requests, &stale, 0) == pdTRUE) {
        if ((stale.mode == PRESENT_SNAPSHOT || stale.mode == PRESENT_LIVE_FROZEN) &&
            stale.slot_index < kPresentSlots && s_free_slots)
            (void)xQueueSend(s_free_slots, &stale.slot_index, 0);
        if (stale.mode == PRESENT_LIVE_FROZEN && s_frozen_present_done)
            (void)xSemaphoreGive(s_frozen_present_done);
    }
}

void tab5_video_begin_host_ui(void)
{
    /* Set ownership before waiting for the display mutex. A CPU0 presenter that
     * was already between queue receive and display_lock() re-checks this flag
     * after acquiring the mutex and discards its stale frame. */
    __atomic_store_n(&s_host_ui_exclusive, true, __ATOMIC_RELEASE);
    drain_present_requests_for_host_ui();
    display_lock();
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    (void)dbfb615h17_prepare_m5gfx_front();
#endif
    M5.Display.fillScreen(TFT_BLACK);
    s_present_started = false;
    s_force_game_redraw = false;
    s_aspect_seen_frame = nullptr;
    std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
    s_aspect_seen_src_w = s_aspect_seen_src_h = s_aspect_seen_pitch = 0;
    display_unlock();
    ESP_LOGI(TAG, "Host UI exclusive: LCD presenter paused; ESP/audio/USB remain alive");
}

void tab5_video_end_host_ui(void)
{
    drain_present_requests_for_host_ui();
    display_lock();
    s_present_started = false;
    s_force_game_redraw = true;
    s_aspect_seen_frame = nullptr;
    std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
    s_aspect_seen_src_w = s_aspect_seen_src_h = s_aspect_seen_pitch = 0;
    display_unlock();
    /* A launcher/UI finger may still be physically down here.  Publish the
     * fence before guest presentation/touch ownership resumes. */
    tab5_video_touch_transition_begin(0u, 0u, 0u);
    __atomic_store_n(&s_host_ui_exclusive, false, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Host UI exclusive released: guest LCD presentation resumed");
}

void tab5_video_set_panic_compat_enabled(int enabled)
{
    const bool was_enabled = s_panic_compat_enabled;
    const bool now_enabled = enabled != 0;
    s_panic_compat_enabled = now_enabled;
    s_panic_compat_last_w = s_panic_compat_last_h = 0;
    s_panic_compat_last_mode = s_panic_compat_last_enable = 0xff;
    if (now_enabled != was_enabled)
        tab5_video_touch_transition_begin(0u, 0u, 0u);

    /* Build 6.12t: an in-place game -> PANIC switch intentionally keeps the
     * ESP32-P4 host, LCD task and ES8388/I2S alive.  That also means the LCD
     * still contains the game's last 960x720 frame.  PANIC can start at a
     * smaller square/letterboxed viewport, so merely forcing a dirty refresh
     * leaves the old game visible around/under the new image.
     *
     * On the OFF -> ON transition, wait for any in-flight CPU0 LCD push to
     * finish, erase the physical panel, and forget every cached presentation
     * surface/generation.  The next PANIC frame is therefore a true full
     * refresh onto black, while audio/USB/FreeRTOS host services are untouched.
     * Layer8 Aug/18/2026 */
    if (now_enabled && !was_enabled)
    {
        display_lock();
#if defined(CONFIG_IDF_TARGET_ESP32P4)
        (void)dbfb615h17_prepare_m5gfx_front();
#endif
        M5.Display.fillScreen(TFT_BLACK);
        if (s_aspect_frame)
            std::memset(s_aspect_frame, 0,
                        (size_t)kAspectViewportWidth * (size_t)kAspectViewportHeight * sizeof(uint16_t));
        if (s_panic_compat_frame)
            std::memset(s_panic_compat_frame, 0,
                        (size_t)kPanicCompatPitch * (size_t)kPanicCompatHeight * sizeof(uint16_t));
        std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
        s_aspect_seen_frame = nullptr;
        s_aspect_seen_src_w = 0;
        s_aspect_seen_src_h = 0;
        s_aspect_seen_pitch = 0;
        s_aspect_map_src_w = s_aspect_map_dst_w = 0;
        s_aspect_map_src_h = s_aspect_map_dst_h = 0;
        s_aspect_log_src_w = s_aspect_log_src_h = 0;
        s_force_game_redraw = false;
        s_present_started = true;
        display_unlock();
        ESP_LOGI(TAG, "PANIC display transition: previous guest frame cleared; host LCD/audio kept alive");
    }

    if (now_enabled)
        ESP_LOGI(TAG, "PANIC compatibility presenter armed on CPU0");
}

void tab5_video_init(void)
{
    auto cfg = M5.config();
    M5.begin(cfg);

    if (M5.Display.width() < M5.Display.height())
        M5.Display.setRotation(1);

    M5.Display.setSwapBytes(true);
    M5.Display.setBrightness(255);

    s_display_mutex = xSemaphoreCreateMutex();

    display_lock();
    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(24, 24);
    M5.Display.println(X68K_TAB_APP_NAME);
    M5.Display.println(X68K_TAB_SUBTITLE);
    display_unlock();

    ESP_LOGI(TAG, "Display initialized: %d x %d", M5.Display.width(), M5.Display.height());

    s_async_ready = init_async_present();
    if (s_async_ready)
    {
        ESP_LOGI(TAG,
                 "Build 6.15h17R49 FB0-NOFLIP + FULL-REPAINT TOKEN: direct-native FB0 from init; transition/UI first LIVE=full 960x720; steady producer TILE32 sparse path retained; text snapshot slots=%u",
                 (unsigned)kPresentSlots);
    }
    else
    {
        ESP_LOGW(TAG, "Build 5.45 async LCD unavailable; synchronous fallback active");
    }
}

void tab5_video_show_message(const char *line1, const char *line2)
{
    display_lock();
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    (void)dbfb615h17_prepare_m5gfx_front();
#endif
    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(24, 24);
    if (line1) M5.Display.println(line1);
    if (line2) M5.Display.println(line2);
    display_unlock();
}

void tab5_video_status(const char *line1, const char *line2)
{
    /* Build 5.98a: status banners are useful during launcher/boot, but once
     * emulator presentation starts they become debug text painted over the
     * top of the game.  Runtime state is still reported on serial logs.
     * Check while holding the display mutex so the first emulator full-clear
     * and a late USB status message cannot race each other. */
    const int lcd_w = M5.Display.width();
    display_lock();
    if (s_present_started || s_media_overlay_active)
    {
        display_unlock();
        return;
    }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    (void)dbfb615h17_prepare_m5gfx_front();
#endif
    M5.Display.startWrite();
    M5.Display.fillRect(0, 0, lcd_w, 96, 0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(20, 12);
    if (line1) M5.Display.println(line1);
    if (line2) M5.Display.println(line2);
    M5.Display.endWrite();
    display_unlock();
}

int tab5_video_width(void) { return M5.Display.width(); }
int tab5_video_height(void) { return M5.Display.height(); }

/* Packed-snapshot path retained for the host TEXT view. */
void tab5_video_present_px68k(const uint16_t *frame,
                              uint32_t width,
                              uint32_t height,
                              uint32_t pitch_pixels)
{
    if (!frame || !width || !height || !pitch_pixels)
        return;

    const uint32_t lcd_w = (uint32_t)M5.Display.width();
    const uint32_t lcd_h = (uint32_t)M5.Display.height();
    if (width > lcd_w) width = lcd_w;
    if (height > lcd_h) height = lcd_h;

    if (s_async_ready)
    {
        uint8_t slot_index = 0;
        if (xQueueReceive(s_free_slots, &slot_index, 0) != pdTRUE)
        {
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        present_slot_t &slot = s_slots[slot_index];
        const size_t pixels = (size_t)width * (size_t)height;
        if (pixels > s_slot_capacity_pixels)
        {
            (void)xQueueSend(s_free_slots, &slot_index, 0);
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        const int64_t t0 = esp_timer_get_time();
        uint16_t *dst = slot.pixels;
        if (pitch_pixels == width)
        {
            std::memcpy(dst, frame, pixels * sizeof(uint16_t));
        }
        else
        {
            for (uint32_t row = 0; row < height; ++row)
            {
                std::memcpy(dst + (size_t)row * width,
                            frame + (size_t)row * pitch_pixels,
                            (size_t)width * sizeof(uint16_t));
            }
        }
        slot.width = width;
        slot.height = height;
        const uint32_t copy_us = (uint32_t)(esp_timer_get_time() - t0);

        present_request_t req = {};
        req.mode = PRESENT_SNAPSHOT;
        req.slot_index = slot_index;
        req.width = width;
        req.height = height;
        if (xQueueSend(s_ready_requests, &req, 0) != pdTRUE)
        {
            (void)xQueueSend(s_free_slots, &slot_index, 0);
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        portENTER_CRITICAL(&s_stats_mux);
        ++s_submitted_frames;
        s_last_copy_us = copy_us;
        portEXIT_CRITICAL(&s_stats_mux);
        return;
    }

    /* Allocation/task failure fallback: preserve the proven synchronous path. */
    const int x = ((int)lcd_w - (int)width) / 2;
    const int y = ((int)lcd_h - (int)height) / 2;
    display_lock();
    if (!s_present_started || s_force_game_redraw)
    {
        M5.Display.fillScreen(0x0000);
        if (s_game_controls_enabled) draw_game_controls_unlocked();
        s_present_started = true;
        s_force_game_redraw = false;
    }
    M5.Display.startWrite();
    if (pitch_pixels == width)
    {
        M5.Display.pushImage(x, y, (int)width, (int)height, frame);
    }
    else
    {
        for (uint32_t row = 0; row < height; ++row)
            M5.Display.pushImage(x, y + (int)row, (int)width, 1, frame + (size_t)row * pitch_pixels);
    }
    M5.Display.endWrite();
    display_unlock();
}

/* Build 5.45 zero-copy live framebuffer path. */
void tab5_video_present_px68k_live(const uint16_t *frame,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t pitch_pixels)
{
    if (!frame || !width || !height || !pitch_pixels)
        return;

    const uint32_t lcd_w = (uint32_t)M5.Display.width();
    const uint32_t lcd_h = (uint32_t)M5.Display.height();
    if (width > lcd_w) width = lcd_w;
    if (height > lcd_h) height = lcd_h;
    if (height > kTrackedLines) height = kTrackedLines;

    if (s_async_ready)
    {
        present_request_t req = {};
        req.mode = PRESENT_LIVE_FB;
        req.slot_index = 0xff;
        req.frame = frame;
        req.width = width;
        req.height = height;
        req.pitch_pixels = pitch_pixels;
        req.live_epoch = __atomic_load_n(&s_live_present_epoch, __ATOMIC_ACQUIRE);

        if (xQueueSend(s_ready_requests, &req, 0) != pdTRUE)
        {
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        portENTER_CRITICAL(&s_stats_mux);
        ++s_submitted_frames;
        s_last_copy_us = 0;
        portEXIT_CRITICAL(&s_stats_mux);
        return;
    }

    tab5_video_present_px68k(frame, width, height, pitch_pixels);
}

/* R52: immutable transition-frame publish.
 *
 * R51 guarantees ScrBuf is coherent when this function is called.  Copy that
 * exact source into one of the already-existing PSRAM presenter slots before
 * CPU1 is allowed to execute the next guest frame.  The request is a distinct
 * mode, so video_pace_keep_latest_live() cannot coalesce it into a newer
 * zero-copy LIVE token.  CPU1 then waits for CPU0 to render this exact snapshot
 * once; only after the acknowledgement may ScrBuf advance again.
 */
int tab5_video_present_px68k_live_frozen(const uint16_t *frame,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t pitch_pixels)
{
    if (!frame || !width || !height || !pitch_pixels)
        return 0;

    const uint32_t lcd_w = (uint32_t)M5.Display.width();
    const uint32_t lcd_h = (uint32_t)M5.Display.height();
    if (width > lcd_w) width = lcd_w;
    if (height > lcd_h) height = lcd_h;
    if (height > kTrackedLines) height = kTrackedLines;

    if (!s_async_ready || !s_frozen_present_done)
        return 0;

    uint8_t slot_index = 0xffu;
    if (xQueueReceive(s_free_slots, &slot_index, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        ESP_LOGW(TAG, "PX68K_R52: frozen LIVE no free snapshot slot");
        return 0;
    }

    present_slot_t &slot = s_slots[slot_index];
    const size_t pixels = (size_t)width * (size_t)height;
    if (pixels > s_slot_capacity_pixels)
    {
        (void)xQueueSend(s_free_slots, &slot_index, portMAX_DELAY);
        return 0;
    }

    /* Remove an acknowledgement left behind by a previous timed-out request. */
    while (xSemaphoreTake(s_frozen_present_done, 0) == pdTRUE) {}

    const int64_t copy_t0 = esp_timer_get_time();
    uint16_t *dst = slot.pixels;
    if (pitch_pixels == width)
    {
        std::memcpy(dst, frame, pixels * sizeof(uint16_t));
    }
    else
    {
        for (uint32_t row = 0; row < height; ++row)
        {
            std::memcpy(dst + (size_t)row * width,
                        frame + (size_t)row * pitch_pixels,
                        (size_t)width * sizeof(uint16_t));
        }
    }
    slot.width = width;
    slot.height = height;
    const uint32_t copy_us = (uint32_t)(esp_timer_get_time() - copy_t0);

    present_request_t req = {};
    req.mode = PRESENT_LIVE_FROZEN;
    req.slot_index = slot_index;
    req.frame = slot.pixels;
    req.width = width;
    req.height = height;
    req.pitch_pixels = width;
    req.live_epoch = __atomic_load_n(&s_live_present_epoch, __ATOMIC_ACQUIRE);

    if (xQueueSend(s_ready_requests, &req, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        (void)xQueueSend(s_free_slots, &slot_index, portMAX_DELAY);
        ESP_LOGW(TAG, "PX68K_R52: frozen LIVE ready queue timeout");
        return 0;
    }

    portENTER_CRITICAL(&s_stats_mux);
    ++s_submitted_frames;
    ++s_r52_frozen_submits;
    s_last_copy_us = copy_us;
    s_r52_frozen_copy_us += copy_us;
    portEXIT_CRITICAL(&s_stats_mux);

    const int64_t wait_t0 = esp_timer_get_time();
    const BaseType_t ack = xSemaphoreTake(s_frozen_present_done, pdMS_TO_TICKS(500));
    const uint32_t wait_us = (uint32_t)(esp_timer_get_time() - wait_t0);

    portENTER_CRITICAL(&s_stats_mux);
    s_r52_frozen_wait_us += wait_us;
    if (ack != pdTRUE)
        ++s_r52_frozen_timeouts;
    const uint64_t submit_count = s_r52_frozen_submits;
    const uint64_t present_count = s_r52_frozen_presented;
    const uint64_t timeout_count = s_r52_frozen_timeouts;
    portEXIT_CRITICAL(&s_stats_mux);

    if (ack != pdTRUE)
    {
        ESP_LOGE(TAG,
                 "PX68K_R52: frozen LIVE PRESENT TIMEOUT src=%lux%lu copy=%luus wait=%luus submit=%llu shown=%llu timeouts=%llu",
                 (unsigned long)width, (unsigned long)height,
                 (unsigned long)copy_us, (unsigned long)wait_us,
                 (unsigned long long)submit_count,
                 (unsigned long long)present_count,
                 (unsigned long long)timeout_count);
        return 0;
    }

    ESP_LOGI(TAG,
             "PX68K_R52: FROZEN LIVE shown src=%lux%lu copy=%luus wait=%luus submit=%llu shown=%llu",
             (unsigned long)width, (unsigned long)height,
             (unsigned long)copy_us, (unsigned long)wait_us,
             (unsigned long long)submit_count,
             (unsigned long long)present_count);
    return 1;
}

/* R56 Screen Manager immutable asynchronous publish.
 *
 * Screen Manager owns the mutable 800-pixel-pitch working surface.  This API
 * copies one completed ScreenVersion into a presenter-owned PSRAM slot and
 * returns immediately after queue acceptance.  The display task later reports
 * success/failure by opaque token; neither emulator nor renderer receives a
 * display framebuffer pointer.
 */
int tab5_video_present_px68k_managed(const uint16_t *frame,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t pitch_pixels,
                                     uint64_t screen_token)
{
    if (!frame || !width || !height || !pitch_pixels || !screen_token)
        return 0;

    const uint32_t lcd_w = (uint32_t)M5.Display.width();
    const uint32_t lcd_h = (uint32_t)M5.Display.height();
    if (width > lcd_w) width = lcd_w;
    if (height > lcd_h) height = lcd_h;
    if (height > kTrackedLines) height = kTrackedLines;

    if (!s_async_ready)
        return 0;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    /* Screen Manager's READY->VISIBLE contract requires a real Back-FB swap
     * followed by refresh_done.  Never silently downgrade managed Screens to
     * direct-front/M5GFX presentation. */
    /* R56c: s_dsi_ppa_live is the legacy LIVE-mode selector. R49 clears it
     * on purpose when FB0 direct-front is armed, so using it as managed-screen
     * admission made every R56 ScreenVersion return backpressure forever. */
    if (!s_dsi_double_live)
        return 0;
    static bool r56_managed_contract_logged = false;
    if (!r56_managed_contract_logged) {
        r56_managed_contract_logged = true;
        ESP_LOGI(TAG,
                 "PX68K_SCREEN_R56C: managed present contract ACTIVE; physical DoubleFB -> private PPA Back-FB transaction -> swap -> refresh_done; legacy R49 direct-front flag ignored");
    }
#endif

    uint8_t slot_index = 0xffu;
    /* Deliberately non-blocking.  Screen Manager decides whether to keep
     * constructing the current candidate when presenter slots are busy. */
    if (xQueueReceive(s_free_slots, &slot_index, 0) != pdTRUE)
        return 0;

    present_slot_t &slot = s_slots[slot_index];
    const size_t pixels = (size_t)width * (size_t)height;
    if (pixels > s_slot_capacity_pixels)
    {
        (void)xQueueSend(s_free_slots, &slot_index, portMAX_DELAY);
        return 0;
    }

    uint16_t *dst = slot.pixels;
    if (pitch_pixels == width)
    {
        std::memcpy(dst, frame, pixels * sizeof(uint16_t));
    }
    else
    {
        for (uint32_t row = 0; row < height; ++row)
        {
            std::memcpy(dst + (size_t)row * width,
                        frame + (size_t)row * pitch_pixels,
                        (size_t)width * sizeof(uint16_t));
        }
    }
    slot.width = width;
    slot.height = height;

    present_request_t req = {};
    req.mode = PRESENT_MANAGED;
    req.slot_index = slot_index;
    req.frame = slot.pixels;
    req.width = width;
    req.height = height;
    req.pitch_pixels = width;
    req.live_epoch = 0u; /* immutable snapshots do not participate in LIVE epoch invalidation */
    req.screen_token = screen_token;

    if (xQueueSend(s_ready_requests, &req, 0) != pdTRUE)
    {
        (void)xQueueSend(s_free_slots, &slot_index, portMAX_DELAY);
        return 0;
    }

    portENTER_CRITICAL(&s_stats_mux);
    ++s_submitted_frames;
    s_last_copy_us = 0;
    portEXIT_CRITICAL(&s_stats_mux);
    return 1;
}

uint32_t tab5_video_live_epoch_invalidate(void)
{
    uint32_t e = __atomic_add_fetch(&s_live_present_epoch, 1u, __ATOMIC_ACQ_REL);
    if (!e) {
        __atomic_store_n(&s_live_present_epoch, 1u, __ATOMIC_RELEASE);
        e = 1u;
    }
    return e;
}

uint32_t tab5_video_live_transition_begin(int clear_game_viewport)
{
    tab5_video_touch_transition_begin(0u, 0u, 0u);
    /* Increment first: a CPU0 presenter that is still pacing will fail its
     * epoch check before taking the display mutex.  Then take the mutex once
     * so any presenter already inside push_live_frame() must retire before
     * CPU1 starts the source-completion rebuild. */
    const uint32_t e = tab5_video_live_epoch_invalidate();
    display_lock();
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    s_r49_force_full_front = true;
    ++s_r49_full_repaint_requests;
    if (s_dsi_double_live) {
        /* R47 pins steady scanout to driver FB0.  M5GFX's line table also owns
         * FB0, so never select FB1 here; just make the local contract explicit. */
        if (s_dsi_front_idx != 0u)
            (void)dbfb615h17_prepare_m5gfx_front();
        if (clear_game_viewport && s_dsi_fb[0])
            (void)dbfb615h17r9_clear_game_window(s_dsi_fb[0]);
    } else if (clear_game_viewport) {
        M5.Display.fillRect(160, 0, 960, 720, TFT_BLACK);
    }
#else
    if (clear_game_viewport)
        M5.Display.fillRect(160, 0, 960, 720, TFT_BLACK);
#endif
    s_aspect_seen_frame = nullptr;
    s_aspect_seen_src_w = s_aspect_seen_src_h = s_aspect_seen_pitch = 0u;
    s_aspect_map_src_w = s_aspect_map_dst_w = 0u;
    s_aspect_map_src_h = s_aspect_map_dst_h = 0u;
    std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
    display_unlock();
    return e;
}

uint32_t tab5_video_live_epoch_current(void)
{
    return __atomic_load_n(&s_live_present_epoch, __ATOMIC_ACQUIRE);
}

void tab5_video_get_touch_irq_stats(tab5_video_touch_irq_stats_t *out)
{
    if (!out) return;
    out->irq_count = __atomic_load_n(&s_touch_irq_count, __ATOMIC_RELAXED);
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    out->update_count = __atomic_load_n(&s_touch_update_count, __ATOMIC_RELAXED);
    out->irq_samples = __atomic_load_n(&s_touch_irq_samples, __ATOMIC_RELAXED);
    out->active_samples = __atomic_load_n(&s_touch_active_samples, __ATOMIC_RELAXED);
    out->safety_samples = __atomic_load_n(&s_touch_safety_samples, __ATOMIC_RELAXED);
    out->update_total_us = __atomic_load_n(&s_touch_update_us, __ATOMIC_RELAXED);
    out->update_max_us = __atomic_load_n(&s_touch_update_max_us, __ATOMIC_RELAXED);
#else
    out->update_count = out->irq_samples = out->active_samples = out->safety_samples = 0;
    out->update_total_us = out->update_max_us = 0;
#endif
    out->contact_active = s_touch_contact_active ? 1u : 0u;
    out->irq_enabled = s_touch_irq_enabled ? 1u : 0u;
    out->fallback_poll = __atomic_load_n(&s_touch_irq_fallback, __ATOMIC_RELAXED);
}

void tab5_video_get_async_stats(tab5_video_async_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_stats_mux);
    out->submitted_frames = s_submitted_frames;
    out->presented_frames = s_presented_frames;
    out->dropped_frames = s_dropped_frames;
    out->last_copy_us = s_last_copy_us;
    out->last_push_us = s_last_push_us;
    out->live_presented_frames = s_live_presented_frames;
    out->live_row_retries = s_live_row_retries;
    out->live_unstable_rows = s_live_unstable_rows;
    out->pace_waits = s_pace_waits;
    out->pace_wait_us = s_pace_wait_us;
    out->pace_last_wait_us = s_pace_last_wait_us;
    out->pace_coalesced_frames = s_pace_coalesced_frames;
    out->pace_skipped_slots = s_pace_skipped_slots;
    out->pace_last_interval_us = s_pace_last_interval_us;
    out->cpu0_push_total_us = s_cpu0_push_total_us;
    portEXIT_CRITICAL(&s_stats_mux);
    out->queued_frames = s_ready_requests ? (uint32_t)uxQueueMessagesWaiting(s_ready_requests) : 0u;
}

} // extern "C"

extern "C" uint32_t tab5_video_stack_highwater(void)
{
    return s_present_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_present_task) : 0u;
}
