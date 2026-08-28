/*
 * Tab5 port-specific implementation.
 * Intent: M5Stack Tab5 integration: keep the X68000 guest timeline on CPU1 and host services on CPU0, while coordinating boot media, HostFS, video, audio, and USB lifecycle.
 * Layer8 Aug/17/2026
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#include "libretro.h"
#include "libretro/state.h"
#include "m68000/m68000.h"
#include "m68000/musashi/m68k.h"
#include "libretro/keyboard.h"
#include "x68k/mfp.h"
#include "x68k/dmac.h"
#include "x68k/crtc.h"
#include "x68k/bg.h"
#include "x68k/gvram.h"
#include "x68k/tvram.h"
#include "x68k/palette.h"
#include "x68k/scc.h"
#include "x68k/hostfs.h"
#include "fmgen/fmg_wrap.h"

#include "tab5_video.h"
#include "tab5_branding.h"
#include "tab5_launcher.h"
#include "tab5_panic.h"
#include "tab5_sd.h"
#include "tab5_disk_control.h"
#include "tab5_textview.h"
#include "tab5_guest_input.h"
#include "tab5_usb_keyboard.h"
#include "tab5_audio.h"
#include "tab5_compose.h"
#include "tab5_screen_manager.h"
#include "tab5_guest_bus.h"
#include "tab5_lp_broker.h"
#include "tab5_dynarec_arena.h"
#include "tab5_guest_video_state.h"
#include "tab5_video_cpu1.h"

static const char *TAG = "PX68K_TAB5";

#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif

#ifndef PX68K_TAB5_R57E63_AUDIO_AUDIT
#define PX68K_TAB5_R57E63_AUDIO_AUDIT 0
#endif

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
/* R56k3: substrate-only memory fact probe.
 * This build intentionally changes no scheduler priority, stack size, screen
 * semantics, audio semantics, USB behavior, or allocation policy.
 *
 * The previous R56k2 run reached f=600 with healthy host flow, then aborted in
 * newlib lock_init_generic before f=900.  Do not infer leak/OOM/fragmentation
 * from that alone.  Measure the allocator directly without adding stdio locks:
 *   - esp_rom_printf() bypasses newlib stdio locking;
 *   - heap_caps_get_info() records free/allocated/largest/block counts;
 *   - heap_caps_register_failed_alloc_callback() reports the exact failed
 *     request size/caps/function if the heap-cap allocator rejects a request.
 */
static volatile uint32_t s_r56k3_guest_frame = 0u;

static void tab5_r56k3_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    const char *fn = function_name ? function_name : "?";
    esp_rom_printf("\nR56K3_ALLOC_FAIL f=%u core=%d size=%u caps=0x%08x fn=%s\n",
                   (unsigned)s_r56k3_guest_frame,
                   (int)xPortGetCoreID(),
                   (unsigned)size,
                   (unsigned)caps,
                   fn);
}

static void tab5_r56k3_heap_probe_rom(uint32_t frame, bool check_integrity)
{
    multi_heap_info_t in = {0};
    multi_heap_info_t dma = {0};
    multi_heap_info_t ex = {0};
    multi_heap_info_t ps = {0};
    const uint32_t in_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;

    heap_caps_get_info(&in, in_caps);
    heap_caps_get_info(&dma, dma_caps);
    heap_caps_get_info(&ex, MALLOC_CAP_EXEC);
    heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);

    int integrity = -1;
    if (check_integrity)
        integrity = heap_caps_check_integrity_all(false) ? 1 : 0;

    esp_rom_printf(
        "R56K3_HEAP f=%u core=%d tasks=%u "
        "int{free=%u alloc=%u largest=%u min=%u ab=%u fb=%u tb=%u} "
        "dma{free=%u alloc=%u largest=%u ab=%u fb=%u} "
        "exec{free=%u largest=%u} ps{free=%u largest=%u} integrity=%d\n",
        (unsigned)frame,
        (int)xPortGetCoreID(),
        (unsigned)uxTaskGetNumberOfTasks(),
        (unsigned)in.total_free_bytes,
        (unsigned)in.total_allocated_bytes,
        (unsigned)in.largest_free_block,
        (unsigned)in.minimum_free_bytes,
        (unsigned)in.allocated_blocks,
        (unsigned)in.free_blocks,
        (unsigned)in.total_blocks,
        (unsigned)dma.total_free_bytes,
        (unsigned)dma.total_allocated_bytes,
        (unsigned)dma.largest_free_block,
        (unsigned)dma.allocated_blocks,
        (unsigned)dma.free_blocks,
        (unsigned)ex.total_free_bytes,
        (unsigned)ex.largest_free_block,
        (unsigned)ps.total_free_bytes,
        (unsigned)ps.largest_free_block,
        integrity);
}

/* R56k4: fact-only execution-stack classifier.
 * ESP32-P4 RISC-V uses a dedicated global ISR stack. Publish its exact runtime
 * range and a one-shot task stack map so a panic SP can be classified by
 * address. No task creation, priority, affinity or stack size is changed. */
extern StackType_t xIsrStack[];
extern StackType_t *xIsrStackTop;

static void tab5_r56k4_print_isr_stack_range(void)
{
    const uintptr_t lo = (uintptr_t)&xIsrStack[0];
    const uintptr_t hi = (uintptr_t)xIsrStackTop;
    esp_rom_printf("R56K4_ISRSTACK base=0x%08x top=0x%08x bytes=%u\n",
                   (unsigned)lo, (unsigned)hi,
                   (unsigned)((hi >= lo) ? (hi - lo) : 0u));
}

static void tab5_r56k4_print_task_stack_map(uint32_t frame)
{
#if configUSE_TRACE_FACILITY
    TaskStatus_t st[24];
    uint32_t total_runtime = 0;
    const UBaseType_t n = uxTaskGetSystemState(st, 24u, &total_runtime);
    esp_rom_printf("R56K4_TASKMAP_BEGIN f=%u n=%u\n", (unsigned)frame, (unsigned)n);
    for (UBaseType_t i = 0; i < n; ++i) {
        const uintptr_t base = (uintptr_t)st[i].pxStackBase;
#if defined(configRECORD_STACK_HIGH_ADDRESS) && (configRECORD_STACK_HIGH_ADDRESS == 1)
        const uintptr_t end = (uintptr_t)st[i].pxEndOfStack;
#else
        const uintptr_t end = 0u;
#endif
        esp_rom_printf("R56K4_TASKSTACK name=%s handle=0x%08x base=0x%08x end=0x%08x hwm=%u\n",
                       st[i].pcTaskName ? st[i].pcTaskName : "?",
                       (unsigned)(uintptr_t)st[i].xHandle,
                       (unsigned)base,
                       (unsigned)end,
                       (unsigned)st[i].usStackHighWaterMark);
    }
    esp_rom_printf("R56K4_TASKMAP_END f=%u\n", (unsigned)frame);
#else
    esp_rom_printf("R56K4_TASKMAP_UNAVAILABLE f=%u configUSE_TRACE_FACILITY=0\n", (unsigned)frame);
#endif
}

#endif /* PX68K_TAB5_RELEASE_DIAGNOSTICS */

extern uint32_t tab5_px68k_hotmem_bytes(void);
extern uint32_t tab5_px68k_spm_palette_bytes(void);
extern int tab5_px68k_spm_palette_ok(void);
extern void m68k_tab5_opcode_profile_set(int enabled);
extern void m68k_tab5_dispatch_profile_set(int enabled);
extern uint32_t m68k_tab5_profile_storage_bytes(void);
extern uint32_t m68k_tab5_dispatch_tcm_bytes(void);
extern uint32_t m68k_tab5_dispatch_l2_bytes(void);

#if defined(MALLOC_CAP_SPM)
#define TAB5_FASTMEM_CAP MALLOC_CAP_SPM
#define TAB5_FASTMEM_LABEL "SPM"
#elif defined(MALLOC_CAP_TCM)
#define TAB5_FASTMEM_CAP MALLOC_CAP_TCM
#define TAB5_FASTMEM_LABEL "TCM/SPM"
#else
#define TAB5_FASTMEM_CAP 0u
#define TAB5_FASTMEM_LABEL "TCM/SPM-unavailable"
#endif

static void tab5_log_memory_550(const char *phase)
{
    const uint32_t l2_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    const size_t l2_total = heap_caps_get_total_size(l2_caps);
    const size_t l2_free = heap_caps_get_free_size(l2_caps);
    const size_t l2_largest = heap_caps_get_largest_free_block(l2_caps);
    const size_t l2_min = heap_caps_get_minimum_free_size(l2_caps);
    const size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t exec_total = heap_caps_get_total_size(MALLOC_CAP_EXEC);
    const size_t exec_free = heap_caps_get_free_size(MALLOC_CAP_EXEC);
    const size_t exec_largest = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
#if TAB5_FASTMEM_CAP
    const size_t fast_total = heap_caps_get_total_size(TAB5_FASTMEM_CAP);
    const size_t fast_free = heap_caps_get_free_size(TAB5_FASTMEM_CAP);
    const size_t fast_largest = heap_caps_get_largest_free_block(TAB5_FASTMEM_CAP);
#else
    const size_t fast_total = 0, fast_free = 0, fast_largest = 0;
#endif
    ESP_LOGI(TAG,
             "MEM613[%s] L2/DMA total=%u free=%u largest=%u min=%u | EXEC total=%u free=%u largest=%u | %s total=%u free=%u largest=%u | PSRAM total=%u free=%u largest=%u",
             phase ? phase : "?",
             (unsigned)l2_total, (unsigned)l2_free, (unsigned)l2_largest, (unsigned)l2_min,
             (unsigned)exec_total, (unsigned)exec_free, (unsigned)exec_largest,
             TAB5_FASTMEM_LABEL, (unsigned)fast_total, (unsigned)fast_free, (unsigned)fast_largest,
             (unsigned)ps_total, (unsigned)ps_free, (unsigned)ps_largest);
}

#ifndef PX68K_TAB5_PERF_PROFILE
#define PX68K_TAB5_PERF_PROFILE 0
#endif
#ifndef PX68K_TAB5_DYNAREC
#define PX68K_TAB5_DYNAREC 1
#endif
#ifndef PX68K_TAB5_DYNAREC_PROD_BENCH
#define PX68K_TAB5_DYNAREC_PROD_BENCH 0
#endif
#ifndef PX68K_TAB5_DYNAREC_COST_PROFILE
#define PX68K_TAB5_DYNAREC_COST_PROFILE PX68K_TAB5_PERF_PROFILE
#endif

static bool perf_textview_render(tab5_textview_frame_t *tv,
                                 bool sample,
                                 uint32_t *elapsed_us)
{
    if (!sample)
        return tab5_textview_render(tv);

    int64_t t0 = esp_timer_get_time();
    bool ok = tab5_textview_render(tv);
    if (elapsed_us)
        *elapsed_us = (uint32_t)(esp_timer_get_time() - t0);
    return ok;
}

extern int WinX68k_LoadEmbeddedROMs(void);
extern void WinX68k_Reset(void);

extern int WinX68k_VideoProbeInit(void);
extern int WinX68k_ExecVideoProbeFrame(void);
extern uint32_t WinX68k_GetGuestClockHz(void);
extern void WinX68k_GuestPaceConfigure(uint32_t lead_max_us,
                                        uint32_t lead_keep_us,
                                        uint32_t lag_resync_us);
extern void WinX68k_GuestPaceGetStats(uint64_t *guest_cycles,
                                      uint32_t *target_hz,
                                      uint32_t *lead_max_us,
                                      uint32_t *lead_keep_us,
                                      uint32_t *wait_events,
                                      uint64_t *wait_us,
                                      uint32_t *max_wait_us,
                                      uint32_t *resyncs,
                                      uint32_t *max_lag_us,
                                      uint32_t *max_lead_us,
                                      uint32_t *check_events,
                                      uint32_t *catchup_checks,
                                      uint32_t *prefetch_events);
extern void WinDraw_R56RPathTake(uint32_t *out, uint32_t count);
extern void WinX68k_SetHostRenderEnabled(int enabled);
extern void WinX68k_PerfSetSample(int enabled);
extern void m68k_tab5_dynarec_dump(void);
extern void m68k_tab5_cmphi617_stats(unsigned int *calls, unsigned long long *loops, unsigned int *maxbatch);
extern void m68k_tab5_mdx619_stats(unsigned int *calls, unsigned long long *outer, unsigned long long *fixed_insn, unsigned int *maxbatch);
extern void m68k_tab5_mdx622_stats(unsigned int *calls, unsigned long long *loops, unsigned int *maxbatch);
extern void m68k_tab5_be01_target_snapshot(unsigned short *out_words, unsigned int max_words);
extern void WinX68k_PerfGetLast(uint32_t *frame_us,
                                uint32_t *cpu_us,
                                uint32_t *compose_us,
                                uint32_t *finalize_us);
extern void WinX68k_PerfGetDetail(uint32_t *timer_us,
                                  uint32_t *dma_us,
                                  uint32_t *line_us,
                                  uint32_t *audio_timer_us,
                                  uint32_t *input_us,
                                  uint32_t *soundmix_us,
                                  uint32_t *fdd_us);
extern void WinX68k_PerfGetDetail543(uint32_t *mfp_us, uint32_t *rtc_us,
                                     uint32_t *edge_us, uint32_t *sched_us,
                                     uint32_t *adclk_us, uint32_t *opmclk_us,
                                     uint32_t *midi_us, uint32_t *post_us);
extern void WinX68k_VideoPerfGetLast(uint32_t *grp_us, uint32_t *text_us, uint32_t *bg_us,
                                      uint32_t *blend_us, uint32_t *clear_us,
                                      uint32_t *dirty_lines, uint32_t *grp_calls,
                                      uint32_t *text_calls, uint32_t *bg_calls,
                                      uint32_t *blend_calls);
extern void WinX68k_VideoPerfGetR57E40(uint32_t *gbt_us, uint32_t *commit_us,
                                        uint32_t *gbt_calls, uint32_t *commit_calls,
                                        uint32_t *arm_count, uint32_t *active);
extern void WinX68k_VideoPerfGetR57E44(uint32_t *fused_lines, uint32_t *fallback_lines,
                                        uint32_t *cache_rebuilds, uint32_t *cache_failures);
extern void WinX68k_VideoPerfGetR57E48(uint32_t *direct_lines, uint32_t *fallback_lines,
                                        uint32_t *text_reject, uint32_t *bg_reject,
                                        uint32_t *final_reject);
extern uint32_t WinX68k_AudioProducedFrames(void);
extern void WinX68k_AudioPerfGetLast(uint32_t *adpcm_us, uint32_t *opm_us, uint32_t *mix_calls, uint32_t *mix_frames);
extern void WinX68k_AudioAsyncGetStats(uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns, uint32_t *fm_avail);
extern void WinX68k_AudioAsyncGetQueueTaxonomy(
    uint32_t *write_attempt, uint32_t *write_drop, uint32_t *write_drop_frames,
    uint32_t *render_attempt, uint32_t *render_drop, uint32_t *render_drop_frames,
    uint32_t *reset_drop, uint32_t *volume_drop, uint32_t *csm_drop,
    uint32_t *stop_drop, uint32_t *other_drop);
extern void WinX68k_AudioAsyncGetBackpressureStats(
    uint32_t *bp_events, uint32_t *wait_calls, uint32_t *timeouts,
    uint32_t *max_timeouts, uint32_t *discard_events, uint32_t *discard_frames);
extern uint32_t OPM_DebugDataWriteCount(void);
extern uint32_t OPM_DebugKeyOnCount(void);
extern uint32_t ADPCM_DebugControlWriteCount(void);
extern uint32_t ADPCM_DebugDataWriteCount(void);
extern int ADPCM_DebugPlaying(void);
extern void DSound_R57E63PCMAuditGet(uint64_t *generated_frames,
                                      uint64_t *probe_samples,
                                      uint64_t *nonzero_samples,
                                      uint32_t *peak);
extern void OPM_R57E64MixClipAuditGet(uint64_t *mixed_samples,
                                      uint64_t *clipped_samples,
                                      uint32_t *max_raw_abs);
extern uint32_t ADPCM_Tab5BufferCapacity(void);
extern uint32_t ADPCM_Tab5BufferHighWater(void);
extern uint32_t ADPCM_Tab5BufferOverflows(void);
extern int ADPCM_Tab5SpmStateOk(void);

extern uint32_t WinX68k_GetVideoWidth(void);
extern uint32_t WinX68k_GetVideoHeight(void);
extern uint32_t WinX68k_GetVideoPitchPixels(void);
extern uint32_t WinX68k_GetDirtyLineCount(void);
extern uint32_t WinX68k_GetRasterHeight(void);
extern void WinX68k_MarkAllVideoDirty(void);
extern void WinX68k_MarkVideoLineDirty(uint32_t y);
extern void tab5_cpu1_video_frame_boundary_sync(void);

extern int WinX68k_MountFloppy(int drive, const char *path);
extern int WinX68k_StandaloneInit(void);
extern int WinX68k_FloppyReady(int drive);
extern int WinX68k_MountSCSIHD(int target, const char *path, int readonly);
extern int WinX68k_SCSIHDReady(int target);
extern int WinX68k_SCSIHDProbe(int target, uint32_t *hash_out);
extern int WinX68k_SCSIHDProbeLayout(int target, uint32_t *partition_count);
extern int WinX68k_SCSIArmDirectBoot(int target);
extern uint32_t WinX68k_SCSIDebugIOCSCalls(void);
extern uint32_t WinX68k_SCSIDebugReads(void);
extern uint32_t WinX68k_SCSIDebugInstallerCalls(void);
extern uint32_t WinX68k_SCSIDebugInitCalls(void);
extern uint32_t WinX68k_SCSIDebugDriverInstalls(void);
extern uint32_t WinX68k_SCSIDebugPartitionCount(void);
extern uint32_t WinX68k_SCSIDebugIOCSVector(void);

typedef enum
{
    TAB5_BUDGET_NORMAL = 0,
    TAB5_BUDGET_AUDIO_GUARD = 1,
    TAB5_BUDGET_AUDIO_CRITICAL = 2
} tab5_budget_mode_t;

typedef struct
{
    tab5_budget_mode_t mode;
    uint32_t transitions;
    uint32_t normal_frames;
    uint32_t guard_frames;
    uint32_t critical_frames;
    uint32_t render_skips;
    uint32_t present_skips;
    uint32_t text_skips;
    uint32_t high_pace_events;
    uint32_t high_pace_ms;
    uint32_t critical_forced_renders;
    uint32_t idle_relief_events;
    uint32_t idle_relief_forced;
    uint32_t idle_relief_deferred;
    uint32_t idle_relief_ms;
    uint32_t preexec_q;
    uint32_t preexec_q_effective;
} tab5_budget_stats_t;

/* Build 5.63 Frame Budget Manager.
 *
 * Guest-visible X68000 work is never skipped.  Only host rendering/LCD work
 * is shed when the audio reserve approaches its real-time deadline.  Hysteresis
 * prevents oscillation.  CRITICAL no longer means a black/frozen LCD: one full
 * host render is forced every 6 guest frames, matching the normal composite
 * presentation cadence once the text renderer has activated.
 *
 * A nearly-full ring applies a tiny wall-clock pace so a fast guest cannot
 * overflow the producer ring while the speaker drains. */
static tab5_budget_mode_t tab5_budget_next_mode(tab5_budget_mode_t mode,
                                                uint32_t queued,
                                                uint32_t submitted)
{
    enum {
        Q_CRITICAL_ENTER = 1024u,
        Q_GUARD_ENTER = 3072u,
        Q_CRITICAL_EXIT = 4096u,
        Q_NORMAL_EXIT = 6144u,
        Q_POLICY_ARM = 4096u
    };

    if (submitted < Q_POLICY_ARM)
        return TAB5_BUDGET_NORMAL;

    switch (mode)
    {
        case TAB5_BUDGET_AUDIO_CRITICAL:
            return (queued >= Q_CRITICAL_EXIT) ? TAB5_BUDGET_AUDIO_GUARD
                                               : TAB5_BUDGET_AUDIO_CRITICAL;
        case TAB5_BUDGET_AUDIO_GUARD:
            if (queued < Q_CRITICAL_ENTER)
                return TAB5_BUDGET_AUDIO_CRITICAL;
            if (queued >= Q_NORMAL_EXIT)
                return TAB5_BUDGET_NORMAL;
            return TAB5_BUDGET_AUDIO_GUARD;
        default:
            if (queued < Q_CRITICAL_ENTER)
                return TAB5_BUDGET_AUDIO_CRITICAL;
            if (queued < Q_GUARD_ENTER)
                return TAB5_BUDGET_AUDIO_GUARD;
            return TAB5_BUDGET_NORMAL;
    }
}

static const char *tab5_budget_mode_name(tab5_budget_mode_t mode)
{
    switch (mode)
    {
        case TAB5_BUDGET_AUDIO_GUARD: return "GUARD";
        case TAB5_BUDGET_AUDIO_CRITICAL: return "CRIT";
        default: return "NORMAL";
    }
}

static void halt_forever(void)
{
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

/* R56: composite Screen pixels are not read directly by CPU1 diagnostics. */

typedef struct
{
    char xdf_path[512];
    char human_path[512];
    char b_xdf_path[512];
    char diskmag_path[512];
    char a_boot_path[512];
    char hds_path[512];
    tab5_launcher_config_t launcher_cfg;
    bool audio_host_ready;
    bool compose_host_ready;
    bool usb_host_ready;
} px68k_run_context_t;

static px68k_run_context_t s_run_ctx;
static TaskHandle_t s_guest_task = NULL;
static void px68k_emulation_task(void *arg);

typedef struct
{
    bool direct_hdd_boot;
    bool hds_layout_ok;
    bool b_inserted;
    bool hdd_deferred_fdds_pending;
} tab5_guest_boot_result_t;

/* Build 6.12u: common in-process X68000 reboot path used by runtime FILE
 * (re)BOOT and by PANIC -> GUI launcher selection.  The ESP32-P4 host side
 * (M5Unified, ES8388/I2S, USB, LCD task and CPU0 audio workers) remains alive.
 * Only guest devices/CPU state are reset and selected media are reattached. */
static bool tab5_guest_apply_boot_config(px68k_run_context_t *ctx,
                                         const tab5_launcher_config_t *requested,
                                         tab5_guest_boot_result_t *result,
                                         const char *reason)
{
    if (!ctx || !requested || !result)
        return false;

    tab5_launcher_config_t cfg = *requested;
    bool hds_layout_ok = false;

    if (cfg.mode == TAB5_LAUNCH_MODE_PANIC)
    {
        cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
        snprintf(cfg.floppy0, sizeof(cfg.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
        cfg.floppy1[0] = '\0';
        cfg.hdd0[0] = '\0';
    }

    const bool direct_hdd = cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0;

    if (cfg.boot_source == TAB5_LAUNCH_BOOT_FLOPPY0 && !cfg.floppy0[0])
    {
        ESP_LOGE(TAG, "%s: guest reboot rejected: FDD0 boot selected with no media",
                 reason ? reason : "guest reboot");
        return false;
    }
    if (cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0 && !cfg.hdd0[0])
    {
        ESP_LOGE(TAG, "%s: guest reboot rejected: HDD0 boot selected with no media",
                 reason ? reason : "guest reboot");
        return false;
    }

    tab5_guest_input_cancel_text();
    (void)tab5_disk_eject(0);
    (void)tab5_disk_eject(1);
    (void)tab5_hdd_eject(0);

    ESP_LOGI(TAG, "%s: WinX68k_Reset only; host audio/LCD/USB preserved",
             reason ? reason : "guest reboot");
    tab5_video_touch_transition_begin(0u, 0u, 0u);
    tab5_guest_video_state_reset();
    tab5_screen_video_reset();
    WinX68k_Reset();

    ctx->launcher_cfg = cfg;
    snprintf(ctx->xdf_path, sizeof(ctx->xdf_path), "%s", cfg.floppy0);
    snprintf(ctx->b_xdf_path, sizeof(ctx->b_xdf_path), "%s", cfg.floppy1);
    snprintf(ctx->hds_path, sizeof(ctx->hds_path), "%s", cfg.hdd0);

    if (ctx->hds_path[0])
    {
        if (!WinX68k_MountSCSIHD(0, ctx->hds_path, 0))
        {
            ESP_LOGE(TAG, "%s: HDD0 attach failed: %s",
                     reason ? reason : "guest reboot", ctx->hds_path);
            return false;
        }
        uint32_t partitions = 0;
        hds_layout_ok = WinX68k_SCSIHDProbeLayout(0, &partitions) != 0;
        ESP_LOGI(TAG, "%s: HDD0 attached %s layout=%s partitions=%lu",
                 reason ? reason : "guest reboot", ctx->hds_path,
                 hds_layout_ok ? "OK" : "unrecognized",
                 (unsigned long)partitions);
    }

    if (direct_hdd)
    {
        if (!ctx->hds_path[0] || !hds_layout_ok || !WinX68k_SCSIArmDirectBoot(0))
        {
            ESP_LOGE(TAG, "%s: HDD0 direct boot arm failed",
                     reason ? reason : "guest reboot");
            return false;
        }
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->hds_path);
    }
    else
    {
        if (!WinX68k_MountFloppy(0, ctx->xdf_path))
        {
            ESP_LOGE(TAG, "%s: FDD0 mount failed: %s",
                     reason ? reason : "guest reboot", ctx->xdf_path);
            return false;
        }
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->xdf_path);

        if (ctx->b_xdf_path[0] && !WinX68k_MountFloppy(1, ctx->b_xdf_path))
        {
            ESP_LOGW(TAG, "%s: FDD1 mount failed, leaving B: empty: %s",
                     reason ? reason : "guest reboot", ctx->b_xdf_path);
            ctx->b_xdf_path[0] = '\0';
            ctx->launcher_cfg.floppy1[0] = '\0';
        }
    }

    result->direct_hdd_boot = direct_hdd;
    result->hds_layout_ok = hds_layout_ok;
    result->b_inserted = !direct_hdd && ctx->b_xdf_path[0] != '\0';
    result->hdd_deferred_fdds_pending = direct_hdd &&
                                        (ctx->xdf_path[0] || ctx->b_xdf_path[0]);

    tab5_video_set_runtime_media_paths(ctx->xdf_path, ctx->b_xdf_path, ctx->hds_path);
    tab5_video_set_runtime_boot_source(ctx->launcher_cfg.boot_source);

    ESP_LOGI(TAG,
             "%s complete: mode=%s source=%s A=%s B=%s HDD0=%s; ESP host not restarted",
             reason ? reason : "guest reboot",
             ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC ? "PANIC" : "PX68K",
             direct_hdd ? "HDD0" : "FDD0",
             ctx->a_boot_path[0] ? ctx->a_boot_path : "<empty>",
             ctx->b_xdf_path[0] ? ctx->b_xdf_path : "<empty>",
             ctx->hds_path[0] ? ctx->hds_path : "<empty>");
    return true;
}

/* R56h1: CPU0 is intentionally a real-time host-services core (YM2151,
 * audio feeder/mix, compositor, LCD, USB).  Do NOT delete IDLE0 directly:
 * ESP-IDF's TWDT idle hook would remain installed and would then call
 * esp_task_wdt_reset() from an unsubscribed task, flooding the log with
 * "task not found".  Reconfigure TWDT's idle_core_mask instead so IDF
 * atomically unregisters the old idle hooks and subscribes CPU1 only.
 * Interrupt watchdog (IWDT) remains unchanged. */
static void tab5_r56h_apply_cpu0_rt_twdt_policy(void)
{
#if defined(CONFIG_ESP_TASK_WDT_EN) && CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_config_t cfg = {
        .timeout_ms = (uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000u,
        .idle_core_mask = 0u,
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    cfg.idle_core_mask |= (1u << 1); /* keep CPU1 IDLE under TWDT */
#endif

    const esp_err_t rc = esp_task_wdt_reconfigure(&cfg);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG,
                 "PX68K_R56H1: TWDT reconfigured idleMask=0x%lX timeout=%lums panic=%d; IDLE0 hook removed, CPU1-IDLE + IWDT retained",
                 (unsigned long)cfg.idle_core_mask,
                 (unsigned long)cfg.timeout_ms,
                 cfg.trigger_panic ? 1 : 0);
    } else {
        ESP_LOGW(TAG,
                 "PX68K_R56H1: TWDT reconfigure failed err=%d; leaving IDF policy unchanged",
                 (int)rc);
    }
#else
    ESP_LOGI(TAG, "PX68K_R56H1: TWDT disabled by sdkconfig; no runtime change");
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "PX68K_R56S5G_BOOTPROOF: early app_main reached; this binary is the R56s5f app-flash build");
    px68k_run_context_t *ctx = &s_run_ctx;
    memset(ctx, 0, sizeof(*ctx));

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    const esp_err_t r56k3_hook_rc =
        heap_caps_register_failed_alloc_callback(tab5_r56k3_alloc_failed);
    ESP_LOGI(TAG,
             "PX68K_R56K3: allocator fact probe ACTIVE hook_rc=%d; ROM-only heap snapshots; NO stack/scheduler/feature change",
             (int)r56k3_hook_rc);
    tab5_r56k4_print_isr_stack_range();
    ESP_LOGI(TAG, "PX68K_R56K4: execution-stack classifier ACTIVE; ISR range + f600 task stack map; NO policy change");
    ESP_LOGI(TAG, "PX68K_R56K5: self-stack range probe ACTIVE; per-task pxTaskGetStackStart; NO policy change");
#endif

/* Intent: Core ownership rule: CPU1 advances the X68000 timeline; CPU0 handles host-only services so host work cannot stall or duplicate guest execution.  Layer8 Aug/17/2026 */
    /* Build 5.47: keep peripheral/host initialization on CPU0 so peripheral
     * driver work stays with the ESP-IDF/system side.  Only after all host
     * services exist do we launch the X68000 time-axis as a dedicated CPU1
     * task. */
    tab5_video_init();
    /* PX68K_R56S1_LAUNCHER_HOST_UI
     * Keep cold-start Media Setup + picker on the M5GFX/FB0 ownership
     * path for the entire launcher session. R49 quarantines FB1 scanout;
     * begin_host_ui() prepares the safe M5GFX front and prevents the LCD
     * presenter from racing native UI draws. */
    tab5_video_begin_host_ui();
    tab5_video_show_message(X68K_TAB_APP_NAME, X68K_TAB_SUBTITLE);

    if (!tab5_sd_mount_and_find_xdf(ctx->human_path, sizeof(ctx->human_path)))
    {
        ESP_LOGE(TAG, "SD/XDF discovery FAILED");
        halt_forever();
    }

    /* Intent: Remove transport files from interrupted/older PANIC sessions before
     * the user sees the SD library. This also cleans legacy PLAY.PAN.
     * Layer8 Aug/17/2026 */
    tab5_panic_cleanup_staging();

    /* Build 6.12u: no normal UI path restarts the ESP32-P4.  The launcher is
     * authoritative at cold start; a PANIC staging failure simply returns to
     * the same native launcher instead of rebooting M5Unified/ES8388/I2S. */
    for (;;)
    {
        if (!tab5_launcher_run(ctx->human_path, &ctx->launcher_cfg))
        {
            ESP_LOGE(TAG, "Launcher failed");
            halt_forever();
        }

        if (ctx->launcher_cfg.mode != TAB5_LAUNCH_MODE_PANIC)
            break;

        if (tab5_panic_prepare_runtime(ctx->launcher_cfg.panic_path))
        {
            ESP_LOGI(TAG, "PANIC mode selected: %s", ctx->launcher_cfg.panic_path);
            break;
        }

        ESP_LOGE(TAG, "PANIC runtime preparation failed: %s", ctx->launcher_cfg.panic_path);
        tab5_panic_cleanup_staging();
        tab5_video_show_message("PANIC setup failed", "Returning to X68K Tab GUI...");
        vTaskDelay(pdMS_TO_TICKS(900));
    }

    /*
     * Intent: Normal PX68K games own the 160px side bars as touch JOY1.
     * PANIC mode keeps the whole touch surface for tap=SPACE / corner-hold.
     * Layer8 Aug/17/2026
     */
    tab5_video_set_game_controls_enabled(ctx->launcher_cfg.mode != TAB5_LAUNCH_MODE_PANIC);
    tab5_video_set_panic_compat_enabled(ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC);

    snprintf(ctx->xdf_path, sizeof(ctx->xdf_path), "%s", ctx->launcher_cfg.floppy0);
    snprintf(ctx->b_xdf_path, sizeof(ctx->b_xdf_path), "%s", ctx->launcher_cfg.floppy1);
    snprintf(ctx->hds_path, sizeof(ctx->hds_path), "%s", ctx->launcher_cfg.hdd0);
    if (ctx->launcher_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0)
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->hds_path);
    else
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->xdf_path);
    ESP_LOGI(TAG, "Launcher boot selection: mode=%s source=%s FDD0=%s FDD1=%s HDD0=%s",
             ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC ? "PANIC" : "PX68K",
             ctx->launcher_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0 ? "HDD0" : "FLOPPY0",
             ctx->xdf_path[0] ? ctx->xdf_path : "<empty>",
             ctx->b_xdf_path[0] ? ctx->b_xdf_path : "<empty>",
             ctx->hds_path[0] ? ctx->hds_path : "<empty>");

    /* R56s1: launcher selection is complete; return panel ownership before
     * starting audio/compose/USB workers and the guest. */
    tab5_video_end_host_ui();
    tab5_log_memory_550("host-preworkers");
#if PX68K_TAB5_DYNAREC
    const int tab5_dyn_probe_ok = tab5_dynarec_arena_probe();
    m68k_tab5_dynarec_bind(tab5_dyn_probe_ok ? tab5_dynarec_arena_base() : NULL,
                            tab5_dyn_probe_ok ? (unsigned int)tab5_dynarec_arena_bytes() : 0u,
                            tab5_dyn_probe_ok ? tab5_dynarec_arena_sync : NULL);
    tab5_log_memory_550("dynarena-static32k");
#else
    ESP_LOGI(TAG, "PX68K_CPU615H23: R22 CPU path retained; memory hierarchy pass only (cold SRAM->PSRAM, FM/ADPCM->Internal, hot palettes/YM ctrl->SPM)");
    tab5_log_memory_550("dynarena-disabled");
#endif
    tab5_r56h_apply_cpu0_rt_twdt_policy();
    ctx->audio_host_ready = tab5_audio_init() != 0;
    /* R23: reserve the compositor arena before guest/FM initialization.  The
     * FM PCM FIFO is now a fixed 16 KiB Internal-SRAM object; 48 KiB of cold SD
     * catalogs plus dead JIT metadata were moved out first, so compositor headroom
     * is preserved without shrinking the CPU dispatch caches. */
    /* Build 5.53a: after M5 speaker DMA is allocated, reserve one contiguous
     * Internal L2/DMA arena before text/USB/task allocations fragment it. */
    (void)tab5_compose_reserve_arena();
    ctx->compose_host_ready = tab5_compose_init() != 0;
    ESP_LOGI(TAG, "CPU0 host compositor: %s",
             ctx->compose_host_ready ? "R57E56 STUTTERFIX ACTIVE - CPU1 guest/MDX real-time priority" : "UNAVAILABLE - NW18 CPU1 exact fallback");

    /* R56: Screen Manager must exist before WinDraw_Init() on CPU1.  It is the
     * sole owner/writer of displayable ScreenVersion state; emulator/renderers
     * only exchange video facts, opaque render tickets and result messages. */
    if (!tab5_screen_manager_init()) {
        ESP_LOGE(TAG, "PX68K_SCREEN_R56: Screen Manager init FAILED");
        halt_forever();
    }
    ESP_LOGI(TAG, "PX68K_SCREEN_R56: ownership/timeline manager READY before guest start");
    ESP_LOGI(TAG,
             "PX68K_R57E2: CPU0 host scheduler contract ACTIVE YM/audio/screen/compose/LCD/R57bus=prio3 speaker=prio4; bounded bus drain + complete-work boundary yields");

    if (!tab5_textview_init())
        ESP_LOGW(TAG, "Core text view unavailable; composite view only");

    tab5_guest_input_init();
    tab5_guest_input_set_interval_frames(12u);

    /* LPFAB R2 starts only after guest-input state has been reset.  The CPU0
     * touch presenter therefore uses the proven direct fallback during the
     * launcher/host-init phase, and no pre-init touch state can leak through
     * the LP latest slot into the first guest frame. */
    if (!tab5_lp_broker_init())
        ESP_LOGW(TAG, "LPFAB_R10 unavailable; R57 + direct TouchJoy fallback retained");

    ctx->usb_host_ready = tab5_usb_keyboard_start() != 0;
    if (ctx->usb_host_ready)
    {
        ESP_LOGI(TAG, "USB keyboard/mouse/JoyPAD host ready on Type-A (CPU0 host side)");
        tab5_video_status("USB-A HID host ready",
                          "Keyboard / mouse / JoyPAD supported");
    }
    else
    {
        ESP_LOGW(TAG, "USB HID host unavailable; core regression continues");
        tab5_video_status("USB HID host unavailable",
                          "PX68K boot will continue");
    }

    tab5_log_memory_550("host-postworkers");

#if portNUM_PROCESSORS > 1
    BaseType_t ok = xTaskCreatePinnedToCore(px68k_emulation_task, "px68k_guest",
                                            16384, ctx, 1, &s_guest_task, 1);
#else
    BaseType_t ok = xTaskCreate(px68k_emulation_task, "px68k_guest",
                                16384, ctx, 1, &s_guest_task);
#endif
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Unable to create CPU1 X68000 guest task");
        halt_forever();
    }

    ESP_LOGI(TAG, "Build 6.15h17R26-device-exact host init complete (DoubleFB + exact PPA dirty-band union + restored virtual Joypad chrome)");
    vTaskDelete(NULL);
}

static void px68k_emulation_task(void *arg)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    {
        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);
        esp_rom_printf("R56K5_TASKSELF name=px68k_guest core=%d base=0x%08x top=0x%08x bytes=16384 hwm=%u\n",
                       (int)xPortGetCoreID(), (unsigned)r56k5_base,
                       (unsigned)(r56k5_base + 16384u),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif
    px68k_run_context_t *ctx = (px68k_run_context_t *)arg;
    if (!ctx)
        vTaskDelete(NULL);

#define xdf_path         (ctx->xdf_path)
#define human_path       (ctx->human_path)
#define b_xdf_path       (ctx->b_xdf_path)
#define diskmag_path     (ctx->diskmag_path)
#define a_boot_path      (ctx->a_boot_path)
#define hds_path         (ctx->hds_path)
#define audio_host_ready (ctx->audio_host_ready)
#define usb_host_ready   (ctx->usb_host_ready)
#define panic_mode       (ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC)

    bool direct_hdd_boot = ctx->launcher_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0;

    ESP_LOGI(TAG, "Build 6.15h17R26-device-exact guest task started: R25-measured DMA3 + MFP exact device paths armed; CPU path unchanged");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, " X68K Tab - Build 6.15h17R26-device-exact measured DMA3/MFP exact paths + R23 SRAM hierarchy + ST7123 ~62Hz");
    ESP_LOGI(TAG, " P4 video hot working-set in internal SRAM: %u bytes", (unsigned)tab5_px68k_hotmem_bytes());
    ESP_LOGI(TAG, " Build 5.91 baseline + Flash Human68k Quick Boot + HDS/SCSI 5.94c");
    ESP_LOGI(TAG, "=======================================");

    ESP_LOGI(TAG, "Human68k Quick Boot: %s (project-root human302.xdf in dedicated flash partition)", TAB5_FLASH_HUMAN_PATH);
    if (human_path[0])
        ESP_LOGI(TAG, "SD Human302 candidate (not auto-inserted): %s", human_path);
    else
        ESP_LOGI(TAG, "SD Human302 candidate: <none>; Flash Human68k remains bootable");
    ESP_LOGI(TAG, "Build 6.15h17R26-device-exact: R25 measured modes specialized exactly; DMA3 OCR32/DCR80/SCR04 + MFP B/10 C/500; CPU execution unchanged");
    #if PX68K_TAB5_RELEASE_DIAGNOSTICS
    ESP_LOGI(TAG, "LP core: LPFAB_R10 SAFE BASE retained; research FM semantic sampler ENABLED");
#else
    ESP_LOGI(TAG, "LP core: LPFAB_R10 SAFE BASE retained; research FM semantic sampler compiled OUT");
#endif
    ESP_LOGI(TAG, "Root XDF catalog: %u image(s)",
             (unsigned)tab5_sd_xdf_count());
    ESP_LOGI(TAG, "Root boot-media catalog: %u image(s) (.XDF/.DIM)",
             (unsigned)tab5_sd_floppy_count());
    ESP_LOGI(TAG,
             "PX68K_MEMR26: CPU-L1/L2=UNCHANGED; R23 cold-data/FM policy retained; FM-PCM=%uB INTERNAL-only; Core1-ADPCM=%uB INTERNAL; R25 trace tables removed",
             (unsigned)OPM_AsyncRingBytes(),
             (unsigned)(ADPCM_Tab5BufferCapacity() * 2u * sizeof(int16_t)));
    ESP_LOGI(TAG,
             "PX68K_SPMR26: palette-hot=%uB spm=%d FM-control-spm=%d ADPCM-state-spm=%d",
             (unsigned)tab5_px68k_spm_palette_bytes(),
             tab5_px68k_spm_palette_ok(), OPM_AsyncSpmControlOk(), ADPCM_Tab5SpmStateOk());

    if (tab5_disk_find_named("DISKMAG1.XDF", diskmag_path, sizeof(diskmag_path)))
    {
        ESP_LOGI(TAG, "Alternate boot XDF ready for F8: %s", diskmag_path);
    }
    else
    {
        diskmag_path[0] = '\0';
        ESP_LOGW(TAG, "DISKMAG1.XDF not found; F8 alternate boot disabled");
    }

    tab5_video_show_message(panic_mode ? "PANIC Player" : (direct_hdd_boot ? "X68K Tab HDD0 boot" : "X68K Tab boot media"),
                            panic_mode ? ctx->launcher_cfg.panic_path : (direct_hdd_boot ? hds_path : xdf_path));

    tab5_log_memory_550("guest-pre-px68k");
    ESP_LOGI(TAG,
             "PSRAM free before PX68K: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (!retro_load_game(NULL))
    {
        ESP_LOGE(TAG, "retro_load_game(NULL) FAILED");
        halt_forever();
    }

    ESP_LOGI(TAG, "PX68K allocation: OK");

    ESP_LOGI(TAG, "Musashi init BEGIN");
    m68000_init();
    ESP_LOGI(TAG, "Musashi init: OK; Build 5.62 FDC+GPIP/fill fast-forward + host arithmetic sweep + DISPATCH256; Musashi context=%u bytes",
             (unsigned)m68k_context_size());
    tab5_log_memory_550("guest-post-musashi");

    ESP_LOGI(TAG, "Embedded ROM load BEGIN");

    if (!WinX68k_LoadEmbeddedROMs())
    {
        ESP_LOGE(TAG, "Embedded ROM load FAILED");
        halt_forever();
    }

    ESP_LOGI(TAG, "Embedded ROM load: OK");

    if (!WinX68k_StandaloneInit())
    {
        ESP_LOGE(TAG, "WinX68k_StandaloneInit FAILED");
        halt_forever();
    }

    /*
     * Build 5.7a: WinDraw_Init() MUST run before WinX68k_Reset().
     *
     * WinX68k_Reset() calls Pal_Init(), and Pal_Init()/Pal_SetColor() build
     * PX68K's Pal16[] conversion table from WinDraw_Pal16R/G/B.  Those RGB565
     * masks are established by WinDraw_Init().  The old standalone order did
     * Reset first, so native TextPal/GrphPal/composite colors were generated
     * with uninitialized pixel-format masks.  The old monochrome proof view
     * hid this because 0x0000/0xffff are byte-order/palette invariant.
     *
     * Match PX68K pmain(): WinDraw_Init -> WinX68k_Reset.
     */
    if (!WinX68k_VideoProbeInit())
    {
        ESP_LOGE(TAG, "WinX68k_VideoProbeInit FAILED");
        halt_forever();
    }

    ESP_LOGI(TAG,
             "WinDraw private renderer initialized BEFORE reset: size=%lux%lu pitch=%lu; managed Screen pointer is not exposed to CPU1",
             (unsigned long)WinX68k_GetVideoWidth(),
             (unsigned long)WinX68k_GetVideoHeight(),
             (unsigned long)WinX68k_GetVideoPitchPixels());

    ESP_LOGI(TAG, "WinX68k_Reset BEGIN");
    tab5_video_touch_transition_begin(0u, 0u, 0u);
    tab5_guest_video_state_reset();
    tab5_screen_video_reset();
    WinX68k_Reset();
    ESP_LOGI(TAG, "WinX68k_Reset END");

    bool hds_layout_ok = false;
    if (hds_path[0])
    {
        /* Build 5.94b keeps the proven 5.15 high-level SCSI IOCS bridge, but
         * can now make HDD0 an explicit boot source.  Attachment still occurs
         * after reset and before the first guest instruction. */
        if (!WinX68k_MountSCSIHD(0, hds_path, 0))
        {
            ESP_LOGE(TAG, "HDS SCSI0 attach FAILED: %s", hds_path);
            tab5_video_status("HDS attach failed", hds_path);
        }
        else
        {
            ESP_LOGI(TAG, "*** HDS SCSI0 ATTACHED READ-WRITE: %s ready=%d ***",
                     hds_path, WinX68k_SCSIHDReady(0));

            uint32_t hds_lba0_hash = 0;
            if (WinX68k_SCSIHDProbe(0, &hds_lba0_hash))
            {
                ESP_LOGI(TAG, "*** HDD0 HOST READ OK: LBA0 hash=%08lX ***",
                         (unsigned long)hds_lba0_hash);
            }
            else
            {
                ESP_LOGE(TAG, "*** HDD0 HOST READ FAILED: %s ***", hds_path);
            }

            uint32_t hds_partitions = 0;
            if (WinX68k_SCSIHDProbeLayout(0, &hds_partitions))
            {
                hds_layout_ok = true;
                ESP_LOGI(TAG, "*** HDD0 HOST LAYOUT OK: X68SCSI1 + X68K table; usable Human68k partitions=%lu ***",
                         (unsigned long)hds_partitions);
                tab5_video_status("HDD0 host layout OK",
                                  direct_hdd_boot ? "Arming native IPL HD0 boot"
                                                  : "Waiting for Human68k device installer");
            }
            else
            {
                ESP_LOGE(TAG, "*** HDD0 HOST LAYOUT INVALID: expected X68SCSI1/X68K SCSI disk ***");
                tab5_video_status("HDD0 layout invalid",
                                  "Not a recognized X68000 SCSI disk");
            }

            /*
             * Do NOT force IOCS vector $7D4 here.  Human68k correctly chooses
             * the external fake SCSI ROM entry at $EA004A.  Build 5.15a's
             * $FC002A restore was backwards and is intentionally removed.
             */
            ESP_LOGI(TAG, "HDD0 IOCS vector before Human68k enumeration: $%08lX (informational)",
                     (unsigned long)WinX68k_SCSIDebugIOCSVector());
        }
    }

    if (direct_hdd_boot)
    {
        if (!hds_path[0] || !hds_layout_ok || !WinX68k_SCSIArmDirectBoot(0))
        {
            ESP_LOGE(TAG, "HDD0 boot arm FAILED: HDS=%s layout=%d ready=%d",
                     hds_path[0] ? hds_path : "<empty>", hds_layout_ok ? 1 : 0,
                     WinX68k_SCSIHDReady(0));
            tab5_video_status("HDD0 boot failed",
                              "Select a valid X68SCSI1 .HDS image");
            halt_forever();
        }
        ESP_LOGI(TAG, "*** HDD0 BOOT ARMED: normal IPL -> HD0 priority $8000 -> X68000 HDD boot path ***");
        tab5_video_status("HDD0 boot armed", "Normal IPL -> HD0 -> HDS");
    }

    /* Build 5.96b: when HDD0 is the explicit boot source, keep both FDDs
     * physically absent during the IPL boot-device decision.  Some IPL paths
     * still prefer an inserted floppy even with SRAM boot priority set to HD0.
     * The launcher-selected FDD media are hot-inserted only after the HDD disk
     * IPL is confirmed executing from RAM. */
    if (direct_hdd_boot)
    {
        ESP_LOGI(TAG,
                 "HDD0 boot priority guard: FDD0/FDD1 held empty until HDD IPL enters RAM");
        if (xdf_path[0])
            ESP_LOGI(TAG, "HDD0 deferred FDD0 media: %s", xdf_path);
    }
    else if (xdf_path[0])
    {
        ESP_LOGI(TAG, "Post-reset FDD0 media insert: %s", xdf_path);
        if (!WinX68k_MountFloppy(0, xdf_path))
        {
            ESP_LOGE(TAG, "Post-reset FDD0 mount FAILED: %s", xdf_path);
            halt_forever();
        }
        ESP_LOGI(TAG,
                 "FDD0(A:) attached after reset; ready_now=%d",
                 WinX68k_FloppyReady(0));
    }
    else
    {
        ESP_LOGE(TAG, "FDD0 boot selected but no floppy image is configured");
        halt_forever();
    }

    if (direct_hdd_boot)
        snprintf(a_boot_path, sizeof(a_boot_path), "%s", hds_path);
    else
        snprintf(a_boot_path, sizeof(a_boot_path), "%s", xdf_path);

    /* Build 5.16/5.96b: FDD1 comes from the launcher, but direct HDD boot
     * defers insertion until the HDD IPL has won boot selection. */
    if (direct_hdd_boot)
    {
        if (b_xdf_path[0])
            ESP_LOGI(TAG, "HDD0 deferred FDD1 media: %s", b_xdf_path);
        else
            ESP_LOGI(TAG, "FDD1(B:) left empty by launcher");
    }
    else if (b_xdf_path[0])
    {
        if (WinX68k_MountFloppy(1, b_xdf_path))
        {
            ESP_LOGI(TAG, "FDD1(B:) launcher media attached: %s", b_xdf_path);
            tab5_video_status("B: launcher disk inserted", b_xdf_path);
        }
        else
        {
            ESP_LOGE(TAG, "FDD1 launcher mount FAILED: %s", b_xdf_path);
            b_xdf_path[0] = '\0';
        }
    }
    else
    {
        ESP_LOGI(TAG, "FDD1(B:) left empty by launcher");
    }

    /* Runtime FILE screen starts from the actual launcher-selected media.
     * Per-slot CHANGE hot-swaps immediately; (re)BOOT performs guest-only reset. */
    tab5_video_set_runtime_media_paths(xdf_path, b_xdf_path, hds_path);
    tab5_video_set_runtime_boot_source(ctx->launcher_cfg.boot_source);

    ESP_LOGI(TAG,
             "RESET PC=$%08lX A7=$%08lX SR=$%04lX",
             (unsigned long)m68k_get_reg(NULL, M68K_REG_PC),
             (unsigned long)m68k_get_reg(NULL, M68K_REG_A7),
             (unsigned long)m68k_get_reg(NULL, M68K_REG_SR));

    ESP_LOGI(TAG, "=======================================");
    if (panic_mode)
    {
        ESP_LOGI(TAG, " PANIC Player mode - Human68k bootstrap");
    }
    else
    {
        ESP_LOGI(TAG, " Human68k + USB keyboard + manual console mode");
    }
    ESP_LOGI(TAG, "=======================================");

    bool ram_execution_seen = false;
    bool textview_active = false;
    uint32_t panic_text_ready_frame = 0;
    uint32_t panic_hostfs_ready_frame = 0;
    uint32_t panic_touch_enable_frame = 0;
    uint32_t panic_command_frame = 0;
    uint32_t panic_command_gfx_base = 0;
    uint32_t panic_command_pal_base = 0;
    uint32_t panic_command_bat_open_base = 0;
    uint32_t panic_command_player_open_base = 0;
    uint32_t panic_command_pan_open_base = 0;
    bool panic_command_sent = false;
    bool panic_direct_fallback_sent = false;
    /* Build 5.63: the real emulator output is the default.  COLOR TEXT is now
     * an F9 diagnostic view rather than a boot-time mode the user must leave. */
    bool composite_view = true;
#if PX68K_TAB5_DIAG_VERBOSE
#endif
    tab5_textview_frame_t tv = {0};

    bool hds_enumeration_logged_in_ram = false;

    /* Build 5.1 legacy diagnosis: retained, but compiled quiet by default. */
#if PX68K_TAB5_DIAG_VERBOSE
    uint32_t diag_prev_usb_events = 0;
    uint32_t diag_prev_udr_reads = 0;
    uint32_t diag_prev_scroll_y = 0xffffffffu;
    uint32_t diag_prev_raster_copy = CRTC_DebugRasterCopyCount();
#endif

    /* Build 5.6: B: runtime media + write-protect state. */
    bool b_inserted = !direct_hdd_boot && b_xdf_path[0] != '\0';
    bool b_write_protected = false;
    bool hdd_deferred_fdds_pending = direct_hdd_boot && (xdf_path[0] || b_xdf_path[0]);

    /* Build 5.9: host-controlled A: boot disk switch. */
    bool a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);

    /* Build 5.10: announce only the first real USB mouse report. */
    bool mouse_activity_announced = false;
    bool mouse_guest_consumed_announced = false;
    uint32_t mouse_scc_packets_at_first_report = 0;
    uint32_t mouse_scc_reads_at_first_report = 0;

    /* Build 5.32: standard generic USB HID gamepad -> X68000 JOY1. */
    bool joypad_candidate_announced = false;
    bool joypad_activity_announced = false;

    /* Build 5.8: graphics activity watch, armed after Human text appears. */
    bool gfx_watch_armed = false;
    bool gfx_activity_seen = false;
    uint32_t gfx_base_writes = 0;
    uint32_t gfx_base_pal_writes = 0;
    uint32_t gfx_base_fast_clear = 0;

    /* R57E3: semantic source geometry published to the CPU0 touch fence. */
    uint32_t touch_video_w = 0u;
    uint32_t touch_video_h = 0u;
    uint32_t touch_video_pitch = 0u;

#define TAB5_REARM_GUEST_OBSERVERS() do { \
        ram_execution_seen = false; \
        textview_active = false; \
        composite_view = true; \
        panic_text_ready_frame = 0; \
        panic_hostfs_ready_frame = 0; \
        panic_touch_enable_frame = 0; \
        panic_command_frame = 0; \
        panic_command_gfx_base = 0; \
        panic_command_pal_base = 0; \
        panic_command_bat_open_base = 0; \
        panic_command_player_open_base = 0; \
        panic_command_pan_open_base = 0; \
        panic_command_sent = false; \
        panic_direct_fallback_sent = false; \
        hds_enumeration_logged_in_ram = false; \
        gfx_watch_armed = false; \
        gfx_activity_seen = false; \
        gfx_base_writes = 0; \
        gfx_base_pal_writes = 0; \
        gfx_base_fast_clear = 0; \
        memset(&tv, 0, sizeof(tv)); \
    } while (0)

    ESP_LOGI(TAG, "A: boot controls: F7=next .XDF/.DIM + reset, F8=Human68k / DISKMAG1");
    ESP_LOGI(TAG, "Video control: F9=color text / PX68K composite");
    ESP_LOGI(TAG, "B: controls: F10=write-protect, F11=eject/reinsert, F12=next XDF");
    ESP_LOGI(TAG, "B: write test: COPY A:COMMAND.X B:PXWTEST.X then F11 eject/reinsert and DIR B:");
    if (hds_path[0])
        ESP_LOGI(TAG, "HDD0: SCSI0=%s READ-WRITE; mode=%s", hds_path,
                 direct_hdd_boot ? "DIRECT BOOT" :
                 (!strcmp(a_boot_path, TAB5_FLASH_HUMAN_PATH) ? "Flash Human68k FDD0 enumeration" : "mounted data disk"));
#if PX68K_TAB5_DIAG_VERBOSE
    ESP_LOGI(TAG, "Diagnostics: VERBOSE (legacy FDC/KEYPIPE/TEXT_RC enabled)");
    ESP_LOGI(TAG, "Console: manual HID only; retired automatic CLS/DIR regression injector removed");
#else
    ESP_LOGI(TAG, "Diagnostics: QUIET (set PX68K_TAB5_DIAG_VERBOSE=1 to restore legacy traces)");
    ESP_LOGI(TAG, "PX68K_R56Q1: memory-safe sparse exec/CPU0 attribution; legacy WinX68k perf path remains QUIET-gated");
    ESP_LOGI(TAG, "PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only");
    ESP_LOGI(TAG, "PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored");
    ESP_LOGI(TAG, "PX68K_R56S1: cold launcher Host-UI FB0 ownership fence active");
    ESP_LOGI(TAG, "PX68K_R56S2: media browser PSRAM-string staging bridge active");
    ESP_LOGI(TAG, "PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload");
    ESP_LOGI(TAG, "PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active");
    ESP_LOGI(TAG, "PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active");
    ESP_LOGI(TAG, "PX68K_R56S5F: validator forced to ordinary queue by brace-scoped function patch; burst after PASS");
    ESP_LOGW(TAG, "PX68K_R56S5K: visible TEXT host-BT quarantined after false one-line PASS; R56s4 exact BT authoritative");
    ESP_LOGI(TAG, "PX68K_R57A: CPU1 guest/render posted-write journal ACTIVE; R56s5k correctness path retained");
    ESP_LOGI(TAG, "PX68K_R57A2: dedicated CPU0 journal consumer ACTIVE; CPU1 posted writes never wait; R56s5k correctness retained");
    ESP_LOGI(TAG, "PX68K_R57B: CPU0 BG/Sprite render-shadow mirror ACTIVE; ownership switch deferred to raster-token phase");
    ESP_LOGI(TAG, "PX68K_R57C: ordered raster-token boundaries ACTIVE; renderer ownership still correctness-fenced");
    ESP_LOGI(TAG, "PX68K_R57D: class-certified visible TEXT + ordered CPU0 BG/Sprite shadow ownership ACTIVE; CPU1 never waits");
    ESP_LOGI(TAG, "PX68K_R57E2: CPU1 final-video detach ACTIVE; priority-safe shadow consumer prio3 + bounded drain512; 512KiB CPU0 TVRAM shadow + 1KiB CPU1 fallback line");
    ESP_LOGI(TAG, "PX68K_R57E3: touch transition fence ACTIVE; geometry/reset publish is nonblocking, CPU0 rearms only after physical present + release");
    ESP_LOGI(TAG, "PX68K_R57E4: Screen Manager rotating fair admission ACTIVE; max inflight remains 32; R57E3 touch fence retained");
    ESP_LOGI(TAG, "PX68K_R57E5: CPU0 packed-TEXT shadow ACTIVE; certification requires host + packed-shadow exactness; mismatch self-repairs on CPU0, no permanent CPU1 fallback");
    ESP_LOGI(TAG, "PX68K_R57E6: latest-wins 32px tile mailbox + common logical guestSeq ACTIVE; CPU1 never waits for physical refresh; carry dirty belongs next ScreenVersion");
    ESP_LOGI(TAG, "PX68K_R57E7: mailbox memory placement fix superseded by R57E8 double-PSRAM banks; CPU1 stack headroom retained");
    ESP_LOGI(TAG, "PX68K_R57E8: lock-free DOUBLE PSRAM dirty-tile mailbox ACTIVE; CPU0 ownership switch is one atomic active_idx; CPU1 never waits; guestSeq remains common logical order");
    ESP_LOGI(TAG, "PX68K_R57E9: adaptive mailbox rescue ACTIVE; physical ownership cut + up to 3 extra light-load rescue cuts; CPU1 remains refresh-blind and CPU0 never waits");
    ESP_LOGI(TAG, "PX68K_R57E10: audio-safe edge-latched rescue ACTIVE; Screen Manager evaluates each clean->dirty mailbox edge at most once; LP input ABI planning uses archived proven LP bring-up but LP execution remains disabled in this build");
    ESP_LOGI(TAG, "PX68K_R57E11: Audio Deadline Shield + adaptive16 mailbox rescue ACTIVE; GUARD sheds 1/4 physical presents, CRIT sheds 1/2; rescue limit follows audio reserve 0/1/3/7/15");
    ESP_LOGI(TAG, "PX68K_R57E20/BAT167M1: BAT166M0 CPU1 video module retained; legacy/special final RGB565 priority/transparency compositor moved to CPU0 via immutable snapshots; guest layer generation unchanged; LP/R57/CPU fast paths frozen; exact CPU1 fallback on queue pressure");
    ESP_LOGI(TAG, "PX68K_R57E48/BAT177NW18 FINAL BASE: NW18 exact compact compositor retained only for semantic/path reject, not host queue pressure");
    ESP_LOGI(TAG, "PX68K_R57E68/RELEASE_QUIET: R57E67 10MHz phase-reservoir governor + bounded catch-up + stronger audio recovery retained");
    ESP_LOGI(TAG, "R57E68: R57E67 governor behavior retained; checks ~2ms, reservoir=6000us keep=3000us lag-credit=20000us");
    ESP_LOGI(TAG, "R57E68 quiet: Timer/PCM/mix/PACE periodic audit compiled OUT; RAM/IPL prefetch policy unchanged; MMIO never prefetched");
    ESP_LOGI(TAG, "PX68K_R57E57/AUDIOCONT: 512f feeder + prio4 retained; TEXT override allowed only in NORMAL audio mode");
    ESP_LOGI(TAG, "PX68K_CPUHOT_R57E48: NW17 IRAM12 retained unchanged; no further CPU experiment in FINAL build");
    ESP_LOGI(TAG, "PX68K_R57E44/BAT177NW14: video baseline retained byte-for-byte in renderer paths; exact common 65K GRP decode + GBT selector fusion ACTIVE");
    ESP_LOGI(TAG, "PX68K_DIRTY_R57E44: NW13 BG fuse retained; 65K GRP materialization deferred only for exact common CPU1 GBT rows");
    #if PX68K_TAB5_RELEASE_DIAGNOSTICS
    ESP_LOGI(TAG, "PX68K_CPU1COST_R57E48: recoverable render-frame profiler ENABLED");
#else
    ESP_LOGI(TAG, "PX68K_RELEASE_R57E48: periodic telemetry/profilers compiled OUT; warnings/errors and boot self-checks retained");
#endif
    ESP_LOGI(TAG, "Console: manual HID only; retired automatic CLS/DIR regression injector removed");
    if (panic_mode)
        ESP_LOGI(TAG, "PANIC controls: tap screen=SPACE, hold upper-left 1.2s=return to GUI; PAN=%s", ctx->launcher_cfg.panic_path);
    ESP_LOGI(TAG, "Mouse: USB HID Boot Mouse -> PX68K SCC (left/right + relative motion)");
    ESP_LOGI(TAG, "JoyPAD: standard USB HID generic -> X68000 JOY1 CPSF/MD (X/Y or Hat + learned B1..B6,L,R; 2-button compatible)");
#endif
#if PX68K_TAB5_PERF_PROFILE
    ESP_LOGI(TAG, "CPU613C: profiler ACTIVE; timing sample=1/600 frames, opcode+hot-PC/back-edge sample=1/1200 frames");
#elif PX68K_TAB5_DYNAREC_PROD_BENCH
    ESP_LOGI(TAG, "CPU613C14R: production benchmark mode: heavyweight CPU/opcode/render profiler OFF");
#else
    ESP_LOGI(TAG, "CPU613C14R: production runtime; periodic CPU/render benchmark telemetry OFF");
#endif
#if PX68K_TAB5_DYNAREC
    ESP_LOGI(TAG, "CPU613C14R: native-loop JIT v2.2R: c14 target gate OFF; c13 specialized-first direct L0 + epoch memo; <=96/48/32 fragments; 32KB IRAM");
    ESP_LOGI(TAG, "CPU613C14R: dispatch=%luB TCM + %luB internal DRAM dynarena=%luB dyntables=%luB costprobe=%s",
             (unsigned long)m68k_tab5_dispatch_tcm_bytes(),
             (unsigned long)m68k_tab5_dispatch_l2_bytes(),
             (unsigned long)tab5_dynarec_arena_bytes(),
             (unsigned long)m68k_tab5_dynarec_metadata_bytes(),
#if PX68K_TAB5_DYNAREC_COST_PROFILE
             "ON");
#else
             "OFF");
#endif
#endif
#if PX68K_TAB5_PERF_PROFILE
    ESP_LOGI(TAG, "Raster: fixed PIE-128 for validated aligned >=64B GVRAM snapshots; unaligned/small runs use memcpy");
    ESP_LOGI(TAG, "Fetch/data: main opcode + extension/immediate + ordinary RAM/IPL operands inline; post-op stream/poll/DBF classification fused into dispatch metadata; device regions use authoritative wrappers");
    ESP_LOGI(TAG, "Polling: stable ordinary-RAM MOVE/AND, BTST, CMPI.W back-edge loops fast-forward only to the current scheduler boundary");
    ESP_LOGI(TAG, "DBF: pure self-loops plus short repeated MOVE.W/L store bodies batch only within the current CPU slice; final exit remains normal Musashi");
    ESP_LOGI(TAG, "MOVE.W stream: semantics frozen at 5.79; eligible DREG->GVRAM repeat chunks use P4 PIE-128, RAM/TVRAM/IPL copy remains scalar");
    ESP_LOGI(TAG, "P4 stream: validated repeat path uses fixed PIE-128; copy path remains scalar");
#endif
    ESP_LOGI(TAG, "MFP timer cache: exact 5.63 semantics retained; Timer-A TACR bit3 exclusion preserved");
    ESP_LOGI(TAG, "Audio 6.00: CPU1 guest-timed ADPCM; CPU0 ADPCM pull + FM final mix + jitter/prebuffer + speaker; 44.1/22.05kHz pitch-safe");
    ESP_LOGI(TAG, "Speaker volume target: 33/255 (~13%%); guest FM/ADPCM/PCM8 amplitude unchanged");
    ESP_LOGI(TAG, "OPM backend: CPU1 lightweight timer/status + CPU0 YM2151-only @44.1kHz (5.42 direct algorithm hot-path)");
    ESP_LOGI(TAG, "P4 core split R57E68: CPU1 R57E67 10MHz phase reservoir retained; runtime measurement/logging compiled OUT; CPU0 flow nonblocking");
    ESP_LOGI(TAG, "Tab5 audio host: %s", audio_host_ready ? "READY" : "UNAVAILABLE");

#if PX68K_TAB5_PERF_PROFILE
    int64_t perf_prev_end_us = 0;
    uint32_t perf_prev_frame = 0;
#endif
#if PX68K_TAB5_DYNAREC_PROD_BENCH
    int64_t dynprod_prev_end_us = 0;
    uint32_t dynprod_prev_frame = 0;
#endif
    tab5_budget_stats_t budget = {0};
    budget.mode = TAB5_BUDGET_NORMAL;

#if PX68K_TAB5_PERF_PROFILE
    /* Diagnostic-only audio wall-rate telemetry. */
    uint32_t audio_generated_total = 0;
    uint32_t audio_rate_prev_generated = 0;
    uint32_t audio_rate_prev_submitted = 0;
    uint32_t audio_rate_prev_played = 0;
    uint32_t audio_rate_prev_under = 0;
    int64_t audio_rate_prev_us = esp_timer_get_time();
#endif

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    /* Recoverable NW14-NW18 research telemetry. Release builds compile it out. */
    bool mdx615e_diag_active = false;
    uint32_t mdx615e_prev_gvram_writes = 0;
    uint32_t mdx615e_prev_pal_writes = 0;
    int64_t cpu0bill_prev_wall_us = esp_timer_get_time();
    uint32_t cpu0bill_prev_fm_us = 0, cpu0bill_prev_fm_calls = 0, cpu0bill_prev_fm_frames = 0;
    uint32_t cpu0bill_prev_comp_us = 0;
    uint32_t cpu0bill_prev_lcd_us = 0, cpu0bill_prev_mix_us = 0, cpu0bill_prev_spk_us = 0;
#endif

    /* Build 6.13c14r2: WDT/IDLE service remains deadline driven.  c11 recorded
     * one IDLE1 Task-WDT warning with the previous timeout/2 hard interval.
     * Use timeout/3 so a missed one-tick relief still leaves another full
     * opportunity before the watchdog deadline; this is far cheaper than the
     * retired every-4-frame delay and does not alter guest timing. */
    int64_t idle_relief_prev_us = esp_timer_get_time();
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
    int64_t idle_relief_hard_us = ((int64_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000000LL) / 3LL;
#else
    int64_t idle_relief_hard_us = 1500000LL;
#endif
    if (idle_relief_hard_us < 500000LL)
        idle_relief_hard_us = 500000LL;
    const int64_t idle_relief_soft_us = idle_relief_hard_us / 3LL;

    /* R57E67: keep the exact 200-cycle device scheduler, but make the
     * wall-clock controller a phase reservoir rather than a near-zero clamp.
     * Check only every ~2 ms of guest execution.  A 6-ms future reservoir
     * absorbs predictable frame/host work before any wait is issued.  Up to
     * 20 ms of lag remains bounded catch-up credit instead of being erased.
     * Long-term slope remains the configured X68000 clock. */
    WinX68k_GuestPaceConfigure(6000u, 3000u, 20000u);

    /* R57E66 TEXT priority: actual changed TVRAM bytes advance this epoch.
     * It is only a presentation hint; R57 ordered shadow remains authoritative.
     * Keep a short hold so console bursts are not hidden by frame shedding. */
    uint32_t r57e66_text_epoch = TVRAM_Tab5TextWriteEpoch();
    uint32_t r57e66_text_hold = 0u;

    /* R57E57: when the final audio reserve is genuinely close to starvation,
     * temporarily spend the host budget on guest/audio rather than pixels.
     * Hysteresis prevents chatter. A sparse rescue render keeps the display
     * visibly alive if a very heavy MDX passage lasts for many frames. */
    int r57e57_audio_recharge = 0;
    uint32_t r57e57_recharge_enters = 0;
    uint32_t r57e57_recharge_frames = 0;
    uint32_t r57e57_recharge_rescue = 0;

#if PX68K_TAB5_R57E63_AUDIO_AUDIT
    uint64_t r57e63_prev_guest_clocks = 0;
    uint32_t r57e63_prev_tb_expires = 0;
    uint32_t r57e63_prev_tb_irq_sets = 0;
    uint32_t r57e63_prev_adpcm_data = 0;
    uint32_t r57e63_prev_dma_hits = 0;
    uint64_t r57e64_prev_mix_samples = 0;
    uint64_t r57e64_prev_mix_clips = 0;
    int64_t r57e63_prev_wall_us = esp_timer_get_time();
#endif

    /* R56: SOURCE/MASS-DIRTY transition heuristics are retired.  Screen
     * lifecycle is owned by tab5_screen_manager and driven by guest-sequenced
     * line/frame facts plus explicit host present opportunities. */

    for (uint32_t frame = 1; ; ++frame)
    {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        s_r56k3_guest_frame = frame;
#endif
        bool perf_sample = false;
        bool cpu_diag_sample = false;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        bool mdx615e_perf_sample = false;
#endif
        int64_t perf_outer_start = 0;
        uint32_t perf_text_us = 0;
        uint32_t perf_lcd_us = 0;
        uint32_t perf_yield_us = 0;

        /* BAT177NW1: no CPU1 readback from Screen Manager.  Guest dirtiness
         * is owned entirely by the CPU1 video module; host lag is downstream. */

        tab5_audio_stats_t budget_audio = {0};
        if (audio_host_ready)
            tab5_audio_get_stats(&budget_audio);

        budget.preexec_q = budget_audio.queued_frames;
        budget.preexec_q_effective = budget_audio.queued_frames + budget_audio.speaker_queued_frames;
        /* R57E11: fact-only audio reserve hint.  Screen Manager uses this to
         * scale optional mailbox rescue 0/1/3/7/15; CPU1 never waits. */
        tab5_screen_manager_audio_reserve_hint(budget_audio.queued_frames,
                                               budget_audio.speaker_queued_frames,
                                               budget_audio.submitted_frames);

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        if (!mdx615e_diag_active && OPM_DebugDataWriteCount() >= 32u)
        {
            mdx615e_diag_active = true;
            mdx615e_prev_gvram_writes = GVRAM_DebugWriteCount();
            mdx615e_prev_pal_writes = Pal_DebugGrphWriteCount();
            {
                tab5_compose_stats_t cs0 = {0};
                tab5_audio_stats_t as0 = {0};
                tab5_video_async_stats_t vs0 = {0};
                uint32_t fm0 = 0, fmc0 = 0, fmf0 = 0;
                tab5_compose_get_stats(&cs0);
                tab5_audio_get_stats(&as0);
                tab5_video_get_async_stats(&vs0);
                OPM_AsyncWorkGet(&fm0, &fmc0, &fmf0);
                cpu0bill_prev_wall_us = esp_timer_get_time();
                cpu0bill_prev_fm_us = fm0;
                cpu0bill_prev_fm_calls = fmc0;
                cpu0bill_prev_fm_frames = fmf0;
                cpu0bill_prev_comp_us = cs0.gbt65k_cpu0_work_us;
                cpu0bill_prev_lcd_us = vs0.cpu0_push_total_us;
                cpu0bill_prev_mix_us = as0.cpu0_mix_work_us;
                cpu0bill_prev_spk_us = as0.cpu0_speaker_work_us;
            }
            ESP_LOGI(TAG,
                     "PX68K_SCREEN_R56: old MASS-DIRTY/SOURCE barrier lifecycle retired; Screen Manager guest_seq/render_ticket/present_token state machine authoritative");
        }
#endif


        const tab5_budget_mode_t next_budget_mode =
            tab5_budget_next_mode(budget.mode,
                                  budget.preexec_q_effective,
                                  budget_audio.submitted_frames);
        if (next_budget_mode != budget.mode)
        {
            budget.mode = next_budget_mode;
            ++budget.transitions;
        }
        /* Build 5.98g12: NORMAL keeps full 44.1-kHz quality.  The existing
         * queue-hysteretic GUARD/CRIT states request a pitch-safe 22.05-kHz
         * output path (2:1 pair-average decimation), never clock stretching. */
        if (audio_host_ready)
            tab5_audio_set_high_load_22k(budget.mode != TAB5_BUDGET_NORMAL);

        /* BAT161: retain BAT160 interactive/file-manager refresh only when the
         * audio reserve is already in the safest high-water band.  Heavy MDX
         * normally sits far below this threshold, so GUARD/CRIT and the usual
         * composite cadence remain untouched there. */
        const bool lowload_present_boost =
            (!textview_active &&
             budget.mode == TAB5_BUDGET_NORMAL &&
             budget.preexec_q_effective >= 16384u &&
             budget_audio.submitted_frames >= 4096u);

        /* R57E54 frame-level visual governor.  Do not spend CPU1 time
         * rediscovering a full line queue hundreds of times per frame.  When
         * CPU0 visual work is backed up, reduce whole-frame production to
         * roughly 30/15 fps while guest/audio time continues every frame. */
        const uint32_t visual_pending = tab5_compose_gbt65k_pending();
        uint32_t visual_div = 1u;
        if (visual_pending >= 14u)
            visual_div = 4u;
        else if (visual_pending >= 8u || budget_audio.queued_frames >= 12288u)
            visual_div = 2u;

        /* R57E57 audio-reserve recharge.  The R57E56 log proved the final
         * residual stutter is true speaker underflow while R57 and pacing are
         * healthy.  Enter before the ring reaches the cliff, render only one
         * rescue frame out of 12, and leave once roughly 90 ms of effective
         * PCM reserve has been rebuilt. */
        if (budget_audio.submitted_frames >= 4096u) {
            if (!r57e57_audio_recharge && budget.preexec_q_effective < 1536u) {
                r57e57_audio_recharge = 1;
                ++r57e57_recharge_enters;
            } else if (r57e57_audio_recharge && budget.preexec_q_effective >= 4096u) {
                r57e57_audio_recharge = 0;
            }
        }

        const uint32_t text_epoch_now = TVRAM_Tab5TextWriteEpoch();
        if (text_epoch_now != r57e66_text_epoch)
        {
            r57e66_text_epoch = text_epoch_now;
            r57e66_text_hold = 4u;
        }
        bool budget_render = true;
        if (r57e57_audio_recharge)
        {
            ++r57e57_recharge_frames;
            budget_render = ((frame % 12u) == 0u);
            if (budget_render)
                ++r57e57_recharge_rescue;
        }
        else if (budget.mode == TAB5_BUDGET_AUDIO_CRITICAL)
        {
            ++budget.critical_frames;
            budget_render = ((frame % 6u) == 0u);
            if (budget_render)
                ++budget.critical_forced_renders;
        }
        else if (budget.mode == TAB5_BUDGET_AUDIO_GUARD)
        {
            ++budget.guard_frames;
            const uint32_t div = (visual_div < 2u) ? 2u : visual_div;
            budget_render = ((frame % div) == 0u);
        }
        else
        {
            ++budget.normal_frames;
            budget_render = ((frame % visual_div) == 0u);
        }

        /* R57E66: keep the Human68k TEXT responsiveness win, but do not let
         * MDX/status TVRAM traffic override audio protection.  TEXT can force
         * an otherwise skipped frame only in NORMAL audio mode with a healthy
         * effective reserve.  GUARD, CRITICAL and recharge retain their sparse
         * visual cadence and therefore spend CPU0 on audio first. */
        if (r57e66_text_hold && !budget_render)
        {
            const bool text_force =
                (budget.mode == TAB5_BUDGET_NORMAL) &&
                !r57e57_audio_recharge &&
                (budget.preexec_q_effective >= 1536u);

            if (text_force)
            {
                budget_render = true;
            }
        }
        if (r57e66_text_hold)
            --r57e66_text_hold;

        /* BAT177NW1: geometry invalidation is CPU1 video-state owned.
         * There is deliberately no Screen Manager readback/force-render gate. */

        if (!budget_render)
            ++budget.render_skips;

        /* R57E66: no queue-derived delay and no frame-end busy wait.
         * Device scheduling remains 200-cycle exact; pacing itself is sampled
         * only at the coarse guest-time quantum inside the core loop. */

        tab5_compose_gbt65k_begin_frame(budget_render ? 1 : 0, (int)budget.mode);
        WinX68k_SetHostRenderEnabled(budget_render ? 1 : 0);

        /* BAT177NW14/R57E44: sample a frame that is guaranteed to render in
         * all budget modes.  CRIT renders every 6th guest frame, GUARD every
         * even frame, so f%600==294 satisfies both while staying away from the
         * R56Q1 wall sample at ==300.  NW6 used ==299 and therefore sampled
         * render=0 under CRIT, hiding compose/final cost. */
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        mdx615e_perf_sample = mdx615e_diag_active && ((frame % 600u) == 294u);
        if (mdx615e_perf_sample)
            WinX68k_PerfSetSample(1);

#endif

#if PX68K_TAB5_PERF_PROFILE
        perf_sample = ((frame % 600u) == 300u);
        /* Build 5.63: exact opcode/cache/fill profiling runs on a different frame
         * from PERF so cpu= remains comparable with prior builds.  One frame
         * every 1200 is enough to identify the real hot working set. */
        cpu_diag_sample = ((frame % 1200u) == 600u);
        perf_outer_start = perf_sample ? esp_timer_get_time() : 0;
        WinX68k_PerfSetSample(perf_sample ? 1 : 0);
        if (cpu_diag_sample) {
            m68k_tab5_opcode_profile_set(1);
            m68k_tab5_dispatch_profile_set(1);
        }
#endif
        /* R56q1: deliberately do NOT wake the legacy WinX68k_Perf* graph.
         * One outer wall sample every 600 guest frames is enough to compare
         * directly with the historical MDXPERF core= number, with no large
         * profiler/static-data reachability change. */
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        const bool r56q1_sample =
            mdx615e_diag_active && ((frame % 600u) == 300u);
        const int64_t r56q1_exec_t0 = r56q1_sample ? esp_timer_get_time() : 0;
#endif
        int cycles = WinX68k_ExecVideoProbeFrame();
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        const uint32_t r56q1_exec_us = r56q1_sample
            ? (uint32_t)(esp_timer_get_time() - r56q1_exec_t0) : 0u;
#endif
#if PX68K_TAB5_PERF_PROFILE
        if (cpu_diag_sample) {
            m68k_tab5_opcode_profile_set(0);
            m68k_tab5_dispatch_profile_set(0);
            /* Exclude this intentionally instrumented frame from the next
             * avg= wall-speed window. */
            perf_prev_end_us = esp_timer_get_time();
            perf_prev_frame = frame;
        }
#endif
#if !PX68K_TAB5_DIAG_VERBOSE
        (void)cycles;
#endif

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        if (r56q1_sample)
        {
            tab5_compose_stats_t qs = {0};
            tab5_audio_stats_t qa = {0};
            tab5_video_async_stats_t qv = {0};
            uint32_t q_fm = 0, q_fmcalls = 0, q_fmframes = 0;
            tab5_compose_get_stats(&qs);
            tab5_audio_get_stats(&qa);
            tab5_video_get_async_stats(&qv);
            OPM_AsyncWorkGet(&q_fm, &q_fmcalls, &q_fmframes);

            const int64_t q_now_us = esp_timer_get_time();
            const uint32_t q_wall = (uint32_t)(q_now_us - cpu0bill_prev_wall_us);
            const uint32_t q_dfm = q_fm - cpu0bill_prev_fm_us;
            const uint32_t q_dcomp = qs.gbt65k_cpu0_work_us - cpu0bill_prev_comp_us;
            const uint32_t q_dlcd = qv.cpu0_push_total_us - cpu0bill_prev_lcd_us;
            const uint32_t q_dmix = qa.cpu0_mix_work_us - cpu0bill_prev_mix_us;
            const uint32_t q_dspk = qa.cpu0_speaker_work_us - cpu0bill_prev_spk_us;
            const uint32_t q_dfmcalls = q_fmcalls - cpu0bill_prev_fm_calls;
            const uint32_t q_dfmframes = q_fmframes - cpu0bill_prev_fm_frames;
#define R56Q1_PCT(v) ((unsigned long)(q_wall ? ((uint64_t)(v) * 100u / q_wall) : 0u))
            ESP_LOGI(TAG,
                     "R56Q1_SAFE f=%lu exec=%luus cycles=%d budget=%s q=%lu render=%u pace{div=%lu boost=%u} "
                     "c0{wall=%lu fm=%lu/%lu%% comp=%lu/%lu%% lcd=%lu/%lu%% mix=%lu/%lu%% spk=%lu/%lu%% calls=%lu frames=%lu} "
                     "render{lines=%lu/%lu hit=%lu miss=%lu rebuild=%lu build=%luus last=%luus qfull=%lu waits=%lu pmax=%lu stale=%lu skip=%lu}",
                     (unsigned long)frame,
                     (unsigned long)r56q1_exec_us,
                     cycles,
                     tab5_budget_mode_name(budget.mode),
                     (unsigned long)budget.preexec_q_effective,
                     budget_render ? 1u : 0u,
                     (unsigned long)(textview_active ? 6u : (lowload_present_boost ? 6u : 12u)),
                     lowload_present_boost ? 1u : 0u,
                     (unsigned long)q_wall,
                     (unsigned long)q_dfm, R56Q1_PCT(q_dfm),
                     (unsigned long)q_dcomp, R56Q1_PCT(q_dcomp),
                     (unsigned long)q_dlcd, R56Q1_PCT(q_dlcd),
                     (unsigned long)q_dmix, R56Q1_PCT(q_dmix),
                     (unsigned long)q_dspk, R56Q1_PCT(q_dspk),
                     (unsigned long)q_dfmcalls, (unsigned long)q_dfmframes,
                     (unsigned long)qs.gbt65k_submitted_lines,
                     (unsigned long)qs.gbt65k_completed_lines,
                     (unsigned long)qs.gbt65k_cache_hits,
                     (unsigned long)qs.gbt65k_cache_misses,
                     (unsigned long)qs.gbt65k_cache_rebuilds,
                     (unsigned long)qs.last_gbt65k_build_us,
                     (unsigned long)qs.last_gbt65k_render_us,
                     (unsigned long)qs.queue_full,
                     (unsigned long)qs.frame_waits,
                     (unsigned long)qs.max_pending,
                     (unsigned long)qs.gbt65k_stale_dropped,
                     (unsigned long)qs.gbt65k_window_skipped);
#undef R56Q1_PCT
            {
                uint32_t rp[19] = {0};
                WinDraw_R56RPathTake(rp, 19u);
                ESP_LOGI(TAG,
                         "R56R_PATH f=%lu lat=%lu rej=%lu mode{16=%lu 256=%lu 65=%lu} "
                         "cpu0{65=%lu gbt=%lu grp8=%lu two=%lu} cpu1{two=%lu legacy=%lu gbtFast=%lu pie=%lu} "
                         "legacyMode{16=%lu 256=%lu 65=%lu} 65fail{q=%lu reject=%lu} cpu0Legacy=%lu",
                         (unsigned long)frame,
                         (unsigned long)rp[0], (unsigned long)rp[1],
                         (unsigned long)rp[2], (unsigned long)rp[3], (unsigned long)rp[4],
                         (unsigned long)rp[5], (unsigned long)rp[6],
                         (unsigned long)rp[7], (unsigned long)rp[8],
                         (unsigned long)rp[9], (unsigned long)rp[10],
                         (unsigned long)rp[17], (unsigned long)rp[18],
                         (unsigned long)rp[11], (unsigned long)rp[12], (unsigned long)rp[13],
                         (unsigned long)rp[14], (unsigned long)rp[15], (unsigned long)rp[16]);
            }
            {
                uint32_t da[TAB5_DIRTY_ALL_N] = {0};
                uint32_t crtc_irq = 0u, crtc_ctl = 0u;
                uint32_t bg_pat = 0u, bg_m0 = 0u, bg_m1 = 0u, bg_lines = 0u, bg_skip = 0u;
                uint32_t bg_inv_rebuild = 0u, bg_inv_visits = 0u;
                uint32_t spr_mask_rebuild = 0u, spr_mask_queries = 0u;
                uint32_t pal_g = 0u, pal_t = 0u, pal_c = 0u;
                uint32_t pal_same = 0u, pal_unused = 0u, pal_visual = 0u;
                uint32_t pal_bank[16] = {0};
                uint32_t text_fast_lines = 0u, text_fallback_lines = 0u, text_fast_blocks = 0u;
                uint32_t bg1_first = 0u, bg1_fallback = 0u, bg0_first = 0u, bg0_fallback = 0u;
                uint32_t spr_y_rebuild = 0u, spr_y_probes = 0u, spr_y_items = 0u;
                uint32_t bg_fuse_lines = 0u, bg_fuse_fallback = 0u, bg_fuse_blocks = 0u;
                TVRAM_DebugDirtyAllTake(da, TAB5_DIRTY_ALL_N);
                CRTC_Tab5DirtyPruneStatsTake(&crtc_irq, &crtc_ctl);
                BG_Tab5DirtyPruneStatsTake(&bg_pat, &bg_m0, &bg_m1, &bg_lines, &bg_skip,
                                           &bg_inv_rebuild, &bg_inv_visits,
                                           &spr_mask_rebuild, &spr_mask_queries);
                Pal_Tab5DirtyStatsTake(&pal_g, &pal_t, &pal_c);
                Pal_Tab5PruneStatsTake(&pal_same, &pal_unused, &pal_visual, pal_bank);
                TVRAM_Tab5TextFastStatsTake(&text_fast_lines, &text_fallback_lines, &text_fast_blocks);
                BG_Tab5RenderFastStatsTake(&bg1_first, &bg1_fallback, &bg0_first, &bg0_fallback,
                                           &spr_y_rebuild, &spr_y_probes, &spr_y_items);
                BG_Tab5FuseStatsTake(&bg_fuse_lines, &bg_fuse_fallback, &bg_fuse_blocks);
                ESP_LOGI(TAG,
                         "DIRTYALL_R57E44 f=%lu all{other=%lu crtc=%lu vctrl=%lu pal=%lu bgreg=%lu bgmemFull=%lu fastclr=%lu} prune{crtcIRQ=%lu crtcCTL=%lu bgPatFull=%lu bgMap0=%lu bgMap1=%lu localLines=%lu inactive=%lu invRebuild=%lu invVisits=%lu} palSrc{g=%lu t=%lu contrast=%lu} palPrune{same=%lu unused=%lu visual=%lu} sprMask{rebuild=%lu query=%lu} textFast{lines=%lu fallback=%lu blocks=%lu} bgFast{bg1First=%lu bg1Fallback=%lu bg0First=%lu bg0Fallback=%lu yRebuild=%lu yProbe=%lu yItems=%lu} bgFuse{lines=%lu fallback=%lu blocks=%lu} palBank{%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu}",
                         (unsigned long)frame,
                         (unsigned long)da[TAB5_DIRTY_ALL_OTHER],
                         (unsigned long)da[TAB5_DIRTY_ALL_CRTC],
                         (unsigned long)da[TAB5_DIRTY_ALL_VCTRL],
                         (unsigned long)da[TAB5_DIRTY_ALL_PALETTE],
                         (unsigned long)da[TAB5_DIRTY_ALL_BG_REG],
                         (unsigned long)da[TAB5_DIRTY_ALL_BG_MEM],
                         (unsigned long)da[TAB5_DIRTY_ALL_FASTCLR],
                         (unsigned long)crtc_irq, (unsigned long)crtc_ctl,
                         (unsigned long)bg_pat, (unsigned long)bg_m0,
                         (unsigned long)bg_m1, (unsigned long)bg_lines,
                         (unsigned long)bg_skip,
                         (unsigned long)bg_inv_rebuild, (unsigned long)bg_inv_visits,
                         (unsigned long)pal_g, (unsigned long)pal_t,
                         (unsigned long)pal_c,
                         (unsigned long)pal_same, (unsigned long)pal_unused,
                         (unsigned long)pal_visual,
                         (unsigned long)spr_mask_rebuild, (unsigned long)spr_mask_queries,
                         (unsigned long)text_fast_lines, (unsigned long)text_fallback_lines,
                         (unsigned long)text_fast_blocks,
                         (unsigned long)bg1_first, (unsigned long)bg1_fallback,
                         (unsigned long)bg0_first, (unsigned long)bg0_fallback,
                         (unsigned long)spr_y_rebuild, (unsigned long)spr_y_probes,
                         (unsigned long)spr_y_items,
                         (unsigned long)bg_fuse_lines, (unsigned long)bg_fuse_fallback,
                         (unsigned long)bg_fuse_blocks,
                         (unsigned long)pal_bank[0], (unsigned long)pal_bank[1],
                         (unsigned long)pal_bank[2], (unsigned long)pal_bank[3],
                         (unsigned long)pal_bank[4], (unsigned long)pal_bank[5],
                         (unsigned long)pal_bank[6], (unsigned long)pal_bank[7],
                         (unsigned long)pal_bank[8], (unsigned long)pal_bank[9],
                         (unsigned long)pal_bank[10], (unsigned long)pal_bank[11],
                         (unsigned long)pal_bank[12], (unsigned long)pal_bank[13],
                         (unsigned long)pal_bank[14], (unsigned long)pal_bank[15]);
            {
                uint32_t ff=0, fb=0, cr=0, cf=0;
                WinX68k_VideoPerfGetR57E44(&ff, &fb, &cr, &cf);
                ESP_LOGI(TAG, "GRPGBT_R57E44 f=%lu fuse=%lu fallback=%lu cache{rebuild=%lu fail=%lu}",
                         (unsigned long)frame, (unsigned long)ff, (unsigned long)fb,
                         (unsigned long)cr, (unsigned long)cf);
            }
            {
                uint32_t dl=0, fl=0, tr=0, br=0, fr=0;
                WinX68k_VideoPerfGetR57E48(&dl, &fl, &tr, &br, &fr);
                ESP_LOGI(TAG,
                         "FINALFUSE_R57E48 f=%lu direct=%lu fallback=%lu reject{text=%lu bg=%lu final=%lu}",
                         (unsigned long)frame, (unsigned long)dl, (unsigned long)fl,
                         (unsigned long)tr, (unsigned long)br, (unsigned long)fr);
            }
            }
            {
                tab5_video_touch_irq_stats_t ts = {0};
                OPMAsyncDetailProfile fp = {0};
                tab5_video_get_touch_irq_stats(&ts);
                OPM_AsyncDetailProfileGet(&fp);
                const uint32_t favg = ts.update_count ? (ts.update_total_us / ts.update_count) : 0u;
                ESP_LOGI(TAG,
                         "TOUCH_R57E12 f=%lu irq=%lu upd=%lu src{irq=%lu active=%lu safety=%lu} touchUS{sum=%lu avg=%lu max=%lu} contact=%lu irqOn=%lu fallback=%lu",
                         (unsigned long)frame,
                         (unsigned long)ts.irq_count, (unsigned long)ts.update_count,
                         (unsigned long)ts.irq_samples, (unsigned long)ts.active_samples,
                         (unsigned long)ts.safety_samples, (unsigned long)ts.update_total_us,
                         (unsigned long)favg, (unsigned long)ts.update_max_us,
                         (unsigned long)ts.contact_active, (unsigned long)ts.irq_enabled,
                         (unsigned long)ts.fallback_poll);
                uint32_t falgo[8] = {0};
                OPM_AsyncSemanticAlgoGet(falgo);
                ESP_LOGI(TAG,
                         "FMOPSEM_R57E13F f=%lu samples=%lu zeroMod=%lu sums{activeCh=%lu activeOp=%lu audibleOp=%lu phaseOnlyOp=%lu fb0Ch=%lu noiseSamples=%lu} algo{%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu} NOTE=1of64-render-call cold state sample; hot=ZERO-MOD-IRAM generic=FLASH-COLD",
                         (unsigned long)frame, (unsigned long)fp.sampled_frames,
                         (unsigned long)fp.post_cycles,
                         (unsigned long)fp.total_cycles,
                         (unsigned long)fp.envelope_cycles,
                         (unsigned long)fp.lfo_noise_cycles,
                         (unsigned long)fp.channel_prep_cycles,
                         (unsigned long)fp.operator_cycles,
                         (unsigned long)fp.routing_pan_cycles,
                         (unsigned long)falgo[0], (unsigned long)falgo[1],
                         (unsigned long)falgo[2], (unsigned long)falgo[3],
                         (unsigned long)falgo[4], (unsigned long)falgo[5],
                         (unsigned long)falgo[6], (unsigned long)falgo[7]);
            }
            cpu0bill_prev_wall_us = q_now_us;
            cpu0bill_prev_fm_us = q_fm;
            cpu0bill_prev_fm_calls = q_fmcalls;
            cpu0bill_prev_fm_frames = q_fmframes;
            cpu0bill_prev_comp_us = qs.gbt65k_cpu0_work_us;
            cpu0bill_prev_lcd_us = qv.cpu0_push_total_us;
            cpu0bill_prev_mix_us = qa.cpu0_mix_work_us;
            cpu0bill_prev_spk_us = qa.cpu0_speaker_work_us;
        }

        if (mdx615e_perf_sample)
        {
            uint32_t core_us = 0, cpu_us = 0, compose_us = 0, finalize_us = 0;
            uint32_t timer_us = 0, dma_us = 0, line_us = 0;
            uint32_t audio_timer_us = 0, input_us = 0, soundmix_us = 0, fdd_us = 0;
            uint32_t mfp_us = 0, rtc_us = 0, edge_us = 0, sched_us = 0;
            uint32_t adclk_us = 0, opmclk_us = 0, midi_us = 0, post_us = 0;
            WinX68k_PerfGetLast(&core_us, &cpu_us, &compose_us, &finalize_us);
            WinX68k_PerfGetDetail(&timer_us, &dma_us, &line_us,
                                  &audio_timer_us, &input_us, &soundmix_us, &fdd_us);
            WinX68k_PerfGetDetail543(&mfp_us, &rtc_us, &edge_us, &sched_us,
                                     &adclk_us, &opmclk_us, &midi_us, &post_us);
            const uint32_t dev_us =
                (core_us > cpu_us + compose_us + finalize_us)
                    ? core_us - cpu_us - compose_us - finalize_us
                    : 0u;
            uint32_t grp_us = 0, text_us = 0, bg_us = 0, blend_us = 0, clear_us = 0;
            uint32_t dirty_lines = 0, grp_calls = 0, text_calls = 0, bg_calls = 0, blend_calls = 0;
            WinX68k_VideoPerfGetLast(&grp_us, &text_us, &bg_us, &blend_us, &clear_us,
                                     &dirty_lines, &grp_calls, &text_calls, &bg_calls, &blend_calls);
            uint32_t gbt_us = 0, commit_us = 0, gbt_calls = 0, commit_calls = 0;
            uint32_t wd_arm = 0, wd_active = 0;
            WinX68k_VideoPerfGetR57E40(&gbt_us, &commit_us, &gbt_calls, &commit_calls,
                                       &wd_arm, &wd_active);
            const uint32_t gv_now = GVRAM_DebugWriteCount();
            const uint32_t pal_now = Pal_DebugGrphWriteCount();
            const uint32_t gv_delta = gv_now - mdx615e_prev_gvram_writes;
            const uint32_t pal_delta = pal_now - mdx615e_prev_pal_writes;
            mdx615e_prev_gvram_writes = gv_now;
            mdx615e_prev_pal_writes = pal_now;
            {
                const char *rn[4] = {"cpu", "compose", "final", "dev"};
                uint32_t rv[4] = {cpu_us, compose_us, finalize_us, dev_us};
                for (unsigned i = 0; i < 3u; ++i) {
                    for (unsigned j = i + 1u; j < 4u; ++j) {
                        if (rv[j] > rv[i]) {
                            uint32_t tv = rv[i]; rv[i] = rv[j]; rv[j] = tv;
                            const char *tn = rn[i]; rn[i] = rn[j]; rn[j] = tn;
                        }
                    }
                }
                ESP_LOGI(TAG,
                         "CPU1RANK_R57E48 f=%lu 1=%s:%luus 2=%s:%luus 3=%s:%luus 4=%s:%luus",
                         (unsigned long)frame,
                         rn[0], (unsigned long)rv[0], rn[1], (unsigned long)rv[1],
                         rn[2], (unsigned long)rv[2], rn[3], (unsigned long)rv[3]);
            }
            {
                const char *ln[7] = {"grp", "text", "bg", "legacy", "gbt", "commit", "clear"};
                uint32_t lv[7] = {grp_us, text_us, bg_us, blend_us, gbt_us, commit_us, clear_us};
                for (unsigned i = 0; i < 6u; ++i) {
                    for (unsigned j = i + 1u; j < 7u; ++j) {
                        if (lv[j] > lv[i]) {
                            uint32_t tv = lv[i]; lv[i] = lv[j]; lv[j] = tv;
                            const char *tn = ln[i]; ln[i] = ln[j]; ln[j] = tn;
                        }
                    }
                }
                const uint32_t layer_sum = grp_us + text_us + bg_us + blend_us +
                                           gbt_us + commit_us + clear_us;
                const uint32_t control_us = compose_us > layer_sum ? compose_us - layer_sum : 0u;
                ESP_LOGI(TAG,
                         "CPU1LAYER_R57E48 f=%lu 1=%s:%luus 2=%s:%luus 3=%s:%luus 4=%s:%luus 5=%s:%luus control=%luus dirty=%lu calls{g/t/b/l/gbt/c=%lu/%lu/%lu/%lu/%lu/%lu} wd{arm=%lu active=%lu}",
                         (unsigned long)frame,
                         ln[0], (unsigned long)lv[0], ln[1], (unsigned long)lv[1],
                         ln[2], (unsigned long)lv[2], ln[3], (unsigned long)lv[3],
                         ln[4], (unsigned long)lv[4], (unsigned long)control_us,
                         (unsigned long)dirty_lines,
                         (unsigned long)grp_calls, (unsigned long)text_calls,
                         (unsigned long)bg_calls, (unsigned long)blend_calls,
                         (unsigned long)gbt_calls, (unsigned long)commit_calls,
                         (unsigned long)wd_arm, (unsigned long)wd_active);
                ESP_LOGI(TAG,
                         "CPU1PIPE_R57E48 f=%lu raw{grp=%lu text=%lu bg=%lu legacy=%lu gbt=%lu commit=%lu clear=%lu control=%lu} compose=%lu",
                         (unsigned long)frame, (unsigned long)grp_us,
                         (unsigned long)text_us, (unsigned long)bg_us,
                         (unsigned long)blend_us, (unsigned long)gbt_us,
                         (unsigned long)commit_us, (unsigned long)clear_us,
                         (unsigned long)control_us, (unsigned long)compose_us);
            }
            ESP_LOGI(TAG,
                     "CPU1COST_R57E48 f=%lu budget=%s q=%lu render=%u core=%luus cpu=%lu compose=%lu final=%lu dev=%lu "
                     "gfx{grp=%lu text=%lu bg=%lu blend=%lu gbt=%lu commit=%lu clear=%lu dirty=%lu calls=%lu/%lu/%lu/%lu/%lu/%lu} "
                     "guestgfx{gvrw=%lu palw=%lu} devx{mfp=%lu rtc=%lu dma=%lu line=%lu adclk=%lu opmclk=%lu midi=%lu snd=%lu input=%lu sched=%lu}",
                     (unsigned long)frame,
                     tab5_budget_mode_name(budget.mode),
                     (unsigned long)budget.preexec_q_effective,
                     budget_render ? 1u : 0u,
                     (unsigned long)core_us, (unsigned long)cpu_us,
                     (unsigned long)compose_us, (unsigned long)finalize_us,
                     (unsigned long)dev_us,
                     (unsigned long)grp_us, (unsigned long)text_us,
                     (unsigned long)bg_us, (unsigned long)blend_us,
                     (unsigned long)gbt_us, (unsigned long)commit_us,
                     (unsigned long)clear_us, (unsigned long)dirty_lines,
                     (unsigned long)grp_calls, (unsigned long)text_calls,
                     (unsigned long)bg_calls, (unsigned long)blend_calls,
                     (unsigned long)gbt_calls, (unsigned long)commit_calls,
                     (unsigned long)gv_delta, (unsigned long)pal_delta,
                     (unsigned long)mfp_us, (unsigned long)rtc_us,
                     (unsigned long)dma_us, (unsigned long)line_us,
                     (unsigned long)adclk_us, (unsigned long)opmclk_us,
                     (unsigned long)midi_us, (unsigned long)soundmix_us,
                     (unsigned long)input_us, (unsigned long)sched_us);
            WinX68k_PerfSetSample(0);
        }

        if ((!PX68K_TAB5_R43_QUIET_RUNTIME) && mdx615e_diag_active && ((frame % 240u) == 0u))
        {
            tab5_compose_stats_t rs = {0};
            tab5_compose_get_stats(&rs);
            ESP_LOGI(TAG,
                     "RENDER615H17 f=%lu 65k=%lu/%lu hit=%lu miss=%lu rebuild=%lu build=%luus render=%luus burst=%lu/%lu full=%lu slots=%lu pie=%lu scalar=%lu stale=%lu skip=%lu bands=%lu epoch=%lu qfull=%lu waits=%lu pmax=%lu gvbar=%lu/%lu",
                     (unsigned long)frame,
                     (unsigned long)rs.gbt65k_submitted_lines,
                     (unsigned long)rs.gbt65k_completed_lines,
                     (unsigned long)rs.gbt65k_cache_hits,
                     (unsigned long)rs.gbt65k_cache_misses,
                     (unsigned long)rs.gbt65k_cache_rebuilds,
                     (unsigned long)rs.last_gbt65k_build_us,
                     (unsigned long)rs.last_gbt65k_render_us,
                     (unsigned long)rs.gbt65k_burst_pending,
                     (unsigned long)rs.gbt65k_burst_max_pending,
                     (unsigned long)rs.gbt65k_burst_full,
                     (unsigned long)rs.gbt65k_burst_slots,
                     (unsigned long)rs.gbt65k_pie_lines,
                     (unsigned long)rs.gbt65k_scalar_lines,
                     (unsigned long)rs.gbt65k_stale_dropped,
                     (unsigned long)rs.gbt65k_window_skipped,
                     (unsigned long)rs.gbt65k_admit_bands,
                     (unsigned long)rs.gbt65k_frame_epoch,
                     (unsigned long)rs.queue_full,
                     (unsigned long)rs.frame_waits,
                     (unsigned long)rs.max_pending,
                     (unsigned long)rs.gvram_barrier_calls,
                     (unsigned long)rs.gvram_barrier_waits);
        }

        if ((!PX68K_TAB5_R43_QUIET_RUNTIME) && mdx615e_diag_active && ((frame % 240u) == 0u))
        {
            tab5_compose_stats_t cs = {0};
            tab5_audio_stats_t as = {0};
            tab5_video_async_stats_t vs = {0};
            uint32_t fm_us = 0, fm_calls = 0, fm_frames = 0;
            tab5_compose_get_stats(&cs);
            tab5_audio_get_stats(&as);
            tab5_video_get_async_stats(&vs);
            OPM_AsyncWorkGet(&fm_us, &fm_calls, &fm_frames);
            const int64_t now_us = esp_timer_get_time();
            const uint32_t wall = (uint32_t)(now_us - cpu0bill_prev_wall_us);
            const uint32_t d_fm = fm_us - cpu0bill_prev_fm_us;
            const uint32_t d_fmcalls = fm_calls - cpu0bill_prev_fm_calls;
            const uint32_t d_fmframes = fm_frames - cpu0bill_prev_fm_frames;
            const uint32_t d_comp = cs.gbt65k_cpu0_work_us - cpu0bill_prev_comp_us;
            const uint32_t d_lcd = vs.cpu0_push_total_us - cpu0bill_prev_lcd_us;
            const uint32_t d_mix = as.cpu0_mix_work_us - cpu0bill_prev_mix_us;
            const uint32_t d_spk = as.cpu0_speaker_work_us - cpu0bill_prev_spk_us;
            ESP_LOGI(TAG,
                     "CPU0COST615H17 wall=%luus FM=%lu(%lu%%) compWall=%lu(%lu%%) LCD=%lu(%lu%%) mix=%lu(%lu%%) spk=%lu(%lu%%) fmcalls=%lu fmframes=%lu NOTE=window-deltas,task-wall-overlap",
                     (unsigned long)wall,
                     (unsigned long)d_fm, (unsigned long)(wall ? ((uint64_t)d_fm * 100u / wall) : 0u),
                     (unsigned long)d_comp, (unsigned long)(wall ? ((uint64_t)d_comp * 100u / wall) : 0u),
                     (unsigned long)d_lcd, (unsigned long)(wall ? ((uint64_t)d_lcd * 100u / wall) : 0u),
                     (unsigned long)d_mix, (unsigned long)(wall ? ((uint64_t)d_mix * 100u / wall) : 0u),
                     (unsigned long)d_spk, (unsigned long)(wall ? ((uint64_t)d_spk * 100u / wall) : 0u),
                     (unsigned long)d_fmcalls, (unsigned long)d_fmframes);
            cpu0bill_prev_wall_us = now_us;
            cpu0bill_prev_fm_us = fm_us;
            cpu0bill_prev_fm_calls = fm_calls;
            cpu0bill_prev_fm_frames = fm_frames;
            cpu0bill_prev_comp_us = cs.gbt65k_cpu0_work_us;
            cpu0bill_prev_lcd_us = vs.cpu0_push_total_us;
            cpu0bill_prev_mix_us = as.cpu0_mix_work_us;
            cpu0bill_prev_spk_us = as.cpu0_speaker_work_us;
        }

#endif /* PX68K_TAB5_RELEASE_DIAGNOSTICS */

        /* Build 5.98g12: CPU1 no longer extracts PCM and never performs the
         * final ADPCM+FM saturation mix.  DSound_FlushPending() above only
         * publishes guest-timed ADPCM plus the matching async-FM render work;
         * CPU0's audio worker pulls/mixes it.  CPU1 does one counter read and
         * one task notification per guest frame. */
#if PX68K_TAB5_PERF_PROFILE
        audio_generated_total = WinX68k_AudioProducedFrames();
#endif
        if (audio_host_ready)
            tab5_audio_kick();

        uint32_t w = WinX68k_GetVideoWidth();
        uint32_t h = WinX68k_GetVideoHeight();
        uint32_t pitch = WinX68k_GetVideoPitchPixels();
        uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

        /* R57E3: publish the semantic geometry transition before Screen Manager
         * starts constructing the new screen.  CPU1 never waits; CPU0 sees the
         * epoch before its next IRQ/active/safety touch sample and quarantines any finger
         * that straddles the transition. */
        if (w != touch_video_w || h != touch_video_h || pitch != touch_video_pitch) {
            tab5_video_touch_transition_begin(w, h, pitch);
            touch_video_w = w;
            touch_video_h = h;
            touch_video_pitch = pitch;
        }

        /* BAT172E0: CPU1 semantic video state is independent of Screen/LCD.
         * It observes only guest facts and stamps later render requests.
         * The Screen Manager still keeps BAT167M1 behavior; epoch is diagnostic. */
        tab5_guest_video_state_frame_boundary(w, h, pitch, VCReg0[1], CRTC_Regs[0x29]);
        tab5_cpu1_video_frame_boundary_sync();

#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        /* Focused release-candidate audit. 300 guest frames is sparse enough
         * not to perturb MDX timing, while still showing timer and PCM changes
         * as arrangements switch between FM-only and PCM/ADPCM passages. */
        if ((frame % 300u) == 0u)
        {
            uint64_t guest_clocks = 0;
            uint32_t timer_calls = 0, tb_writes = 0, tc_writes = 0;
            uint32_t tb_starts = 0, tb_stops = 0, tb_expires = 0;
            uint32_t tb_irq_sets = 0, tb_clears = 0;
            uint32_t last_tb = 0, last_tc = 0, tb_period_us = 0, tb_count_us = 0;
            OPM_R57E63TimerAuditGet(&guest_clocks, &timer_calls,
                                    &tb_writes, &tc_writes,
                                    &tb_starts, &tb_stops,
                                    &tb_expires, &tb_irq_sets, &tb_clears,
                                    &last_tb, &last_tc,
                                    &tb_period_us, &tb_count_us);

            uint64_t pcm_frames = 0, pcm_probe = 0, pcm_nonzero = 0;
            uint32_t pcm_peak = 0;
            DSound_R57E63PCMAuditGet(&pcm_frames, &pcm_probe, &pcm_nonzero, &pcm_peak);

            uint64_t mix_samples = 0, mix_clips = 0;
            uint32_t mix_max_raw = 0;
            OPM_R57E64MixClipAuditGet(&mix_samples, &mix_clips, &mix_max_raw);

            uint32_t dma_hits = 0, dma_fallbacks = 0;
            DMA_Tab5ADPCMFastStats(&dma_hits, &dma_fallbacks);

            tab5_audio_stats_t aud = {0};
            if (audio_host_ready) tab5_audio_get_stats(&aud);

            const int64_t now_us = esp_timer_get_time();
            const uint64_t dclk = guest_clocks - r57e63_prev_guest_clocks;
            const uint32_t dexp = tb_expires - r57e63_prev_tb_expires;
            const uint32_t dirq = tb_irq_sets - r57e63_prev_tb_irq_sets;
            const uint32_t dadpcm = ADPCM_DebugDataWriteCount() - r57e63_prev_adpcm_data;
            const uint32_t ddma = dma_hits - r57e63_prev_dma_hits;
            const uint64_t wall_us = (uint64_t)(now_us - r57e63_prev_wall_us);
            const uint32_t guest_khz =
                wall_us ? (uint32_t)((dclk * 1000ULL) / wall_us) : 0u;
            const uint32_t expected_tb_clocks = tb_period_us * 10u;
            const uint32_t observed_tb_clocks =
                dexp ? (uint32_t)(dclk / (uint64_t)dexp) : 0u;
            const uint64_t dmix_samples = mix_samples - r57e64_prev_mix_samples;
            const uint64_t dmix_clips = mix_clips - r57e64_prev_mix_clips;
            const uint32_t clip_ppm = dmix_samples
                ? (uint32_t)((dmix_clips * 1000000ULL) / dmix_samples) : 0u;

            ESP_LOGI(TAG,
                     "R57E67_TIMER f=%lu guest=%lluck d=%lluck wall=%lluus rate=%lu.%03luMHz calls=%lu TB{reg=%02lX ctrl=%02lX period=%luus remain=%luus writes=%lu tc=%lu start=%lu stop=%lu exp=%lu(+%lu) irq=%lu(+%lu) clear=%lu expClk=%lu obsClk=%lu}",
                     (unsigned long)frame,
                     (unsigned long long)guest_clocks,
                     (unsigned long long)dclk,
                     (unsigned long long)wall_us,
                     (unsigned long)(guest_khz / 1000u),
                     (unsigned long)(guest_khz % 1000u),
                     (unsigned long)timer_calls,
                     (unsigned long)last_tb,
                     (unsigned long)last_tc,
                     (unsigned long)tb_period_us,
                     (unsigned long)tb_count_us,
                     (unsigned long)tb_writes,
                     (unsigned long)tc_writes,
                     (unsigned long)tb_starts,
                     (unsigned long)tb_stops,
                     (unsigned long)tb_expires,
                     (unsigned long)dexp,
                     (unsigned long)tb_irq_sets,
                     (unsigned long)dirq,
                     (unsigned long)tb_clears,
                     (unsigned long)expected_tb_clocks,
                     (unsigned long)observed_tb_clocks);

            ESP_LOGI(TAG,
                     "R57E67_PCM f=%lu ADPCM{ctl=%lu data=%lu(+%lu) playing=%d dma=%lu(+%lu)/%lu gen=%llu probe=%llu nz=%llu peak=%lu} AUDIO{producer=%luHz speaker=%luHz servo22k=%lu q=%lu spkq=%lu under=%lu drop=%lu} MIX{samples=%llu(+%llu) clip=%llu(+%llu) ppm=%lu maxRaw=%lu}",
                     (unsigned long)frame,
                     (unsigned long)ADPCM_DebugControlWriteCount(),
                     (unsigned long)ADPCM_DebugDataWriteCount(),
                     (unsigned long)dadpcm,
                     ADPCM_DebugPlaying(),
                     (unsigned long)dma_hits,
                     (unsigned long)ddma,
                     (unsigned long)dma_fallbacks,
                     (unsigned long long)pcm_frames,
                     (unsigned long long)pcm_probe,
                     (unsigned long long)pcm_nonzero,
                     (unsigned long)pcm_peak,
                     (unsigned long)aud.producer_rate_hz,
                     (unsigned long)aud.speaker_rate_hz,
                     (unsigned long)aud.rate_servo_active,
                     (unsigned long)aud.queued_frames,
                     (unsigned long)aud.speaker_queued_frames,
                     (unsigned long)aud.underflow_events,
                     (unsigned long)aud.dropped_frames,
                     (unsigned long long)mix_samples,
                     (unsigned long long)dmix_samples,
                     (unsigned long long)mix_clips,
                     (unsigned long long)dmix_clips,
                     (unsigned long)clip_ppm,
                     (unsigned long)mix_max_raw);

            uint64_t pace_cycles = 0, pace_wait_us = 0;
            uint32_t pace_hz = 0, pace_lead = 0, pace_keep = 0;
            uint32_t pace_waits = 0, pace_max_wait = 0, pace_resyncs = 0;
            uint32_t pace_max_lag = 0, pace_max_lead = 0, pace_checks = 0, pace_catch = 0, pace_prefetch = 0;
            WinX68k_GuestPaceGetStats(&pace_cycles, &pace_hz, &pace_lead, &pace_keep,
                                      &pace_waits, &pace_wait_us, &pace_max_wait,
                                      &pace_resyncs, &pace_max_lag, &pace_max_lead,
                                      &pace_checks, &pace_catch, &pace_prefetch);
            ESP_LOGI(TAG,
                     "R57E67_PACE f=%lu target=%luHz reservoir=%lu/%luus cycles=%llu wait=%lu/%lluus maxWait=%luus check=%lu catch=%lu prefetch=%lu resync=%lu maxLead=%luus maxLag=%luus TEXT{epoch=%lu hold=%lu force=%lu active=%lu} audioQ=%lu+%lu",
                     (unsigned long)frame,
                     (unsigned long)pace_hz,
                     (unsigned long)pace_lead,
                     (unsigned long)pace_keep,
                     (unsigned long long)pace_cycles,
                     (unsigned long)pace_waits,
                     (unsigned long long)pace_wait_us,
                     (unsigned long)pace_max_wait,
                     (unsigned long)pace_checks,
                     (unsigned long)pace_catch,
                     (unsigned long)pace_prefetch,
                     (unsigned long)pace_resyncs,
                     (unsigned long)pace_max_lead,
                     (unsigned long)pace_max_lag,
                     (unsigned long)r57e66_text_epoch,
                     (unsigned long)r57e66_text_hold,
                     0ul,
                     0ul,
                     (unsigned long)aud.queued_frames,
                     (unsigned long)aud.speaker_queued_frames);

            r57e63_prev_guest_clocks = guest_clocks;
            r57e63_prev_tb_expires = tb_expires;
            r57e63_prev_tb_irq_sets = tb_irq_sets;
            r57e63_prev_adpcm_data = ADPCM_DebugDataWriteCount();
            r57e63_prev_dma_hits = dma_hits;
            r57e64_prev_mix_samples = mix_samples;
            r57e64_prev_mix_clips = mix_clips;
            r57e63_prev_wall_us = now_us;
        }
#else
        /* R57E62 production freeze: periodic performance telemetry removed. */
#endif

        /* R56 guest video fact.  This records guest ordering/geometry only; it
         * does NOT mean present and it does not inspect dirty-count magnitude. */
        tab5_screen_video_frame_boundary(w, h, pitch);

        if (!ram_execution_seen &&
            pc >= 0x00002000u &&
            pc <  0x00C00000u)
        {
            ram_execution_seen = true;

            if (hds_path[0] && !hds_enumeration_logged_in_ram)
            {
                hds_enumeration_logged_in_ram = true;
                ESP_LOGI(TAG,
                         "*** HDD0 RAM EXEC: vec=$%08lX init_calls=%lu installer_calls=%lu installs=%lu partitions=%lu ***",
                         (unsigned long)WinX68k_SCSIDebugIOCSVector(),
                         (unsigned long)WinX68k_SCSIDebugInitCalls(),
                         (unsigned long)WinX68k_SCSIDebugInstallerCalls(),
                         (unsigned long)WinX68k_SCSIDebugDriverInstalls(),
                         (unsigned long)WinX68k_SCSIDebugPartitionCount());
            }

            ESP_LOGI(TAG,
                     "*** RAM EXECUTION REACHED: PC=$%08lX frame=%lu ***",
                     (unsigned long)pc,
                     (unsigned long)frame);

            if (direct_hdd_boot && hdd_deferred_fdds_pending)
            {
                ESP_LOGI(TAG,
                         "*** HDD0 IPL CONFIRMED: hot-inserting deferred FDD media now ***");

                if (xdf_path[0])
                {
                    if (WinX68k_MountFloppy(0, xdf_path))
                    {
                        ESP_LOGI(TAG,
                                 "HDD0 post-boot FDD0(A:) inserted: %s ready_now=%d",
                                 xdf_path, WinX68k_FloppyReady(0));
                    }
                    else
                    {
                        ESP_LOGE(TAG, "HDD0 post-boot FDD0 mount FAILED: %s", xdf_path);
                    }
                }

                if (b_xdf_path[0])
                {
                    if (WinX68k_MountFloppy(1, b_xdf_path))
                    {
                        b_inserted = true;
                        ESP_LOGI(TAG,
                                 "HDD0 post-boot FDD1(B:) inserted: %s ready_now=%d",
                                 b_xdf_path, WinX68k_FloppyReady(1));
                    }
                    else
                    {
                        b_inserted = false;
                        ESP_LOGE(TAG, "HDD0 post-boot FDD1 mount FAILED: %s", b_xdf_path);
                    }
                }

                hdd_deferred_fdds_pending = false;
                tab5_video_status("HDD0 boot confirmed",
                                  "Selected floppy media inserted after HDD IPL");
            }

            if (strcmp(a_boot_path, TAB5_FLASH_HUMAN_PATH) != 0 &&
                (!human_path[0] || strcmp(a_boot_path, human_path) != 0))
            {
                tab5_video_status("Alternate boot code running",
                                  "Watching native graphics activity");

                gfx_base_writes = GVRAM_DebugWriteCount();
                gfx_base_pal_writes = Pal_DebugGrphWriteCount();
                gfx_base_fast_clear = GVRAM_DebugFastClearCount();
                gfx_activity_seen = false;
                gfx_watch_armed = true;

                ESP_LOGI(TAG,
                         "Graphics watch armed for alternate boot (%s): GVRAM=%lu GrphPal=%lu FastClr=%lu",
                         a_boot_path,
                         (unsigned long)gfx_base_writes,
                         (unsigned long)gfx_base_pal_writes,
                         (unsigned long)gfx_base_fast_clear);
            }
            else
            {
                tab5_video_status(
                    "Human68k code loaded",
                    "Waiting for console text...");
            }
        }

        const uint32_t present_div = textview_active ? 6u : (lowload_present_boost ? 6u : 12u);
        const bool do_present =
            (frame <= 12u) || ((frame % present_div) == 0u);

        /* Build 5.45: while viewing COMPOSITE the expensive host text renderer
         * is diagnostic-only.  Refresh it once per second instead of every
         * LCD present; switching back to TEXT catches up on the next frame. */
        const bool do_text_sample_raw =
            (!textview_active || !composite_view)
                ? (do_present || ((frame % 60u) == 0u))
                : ((frame % 60u) == 0u);
        const bool do_text_sample = budget_render && do_text_sample_raw;
        if (!budget_render && do_text_sample_raw)
            ++budget.text_skips;

        if (do_text_sample && perf_textview_render(&tv, perf_sample,
                                                   &perf_text_us))
        {
            if (!textview_active &&
                ram_execution_seen &&
                tv.nonzero_pixels > 128u)
            {
                textview_active = true;
                if (panic_mode && !panic_text_ready_frame) panic_text_ready_frame = frame;

                ESP_LOGI(TAG,
                         "*** CORE COLOR TEXT VIEW ACTIVE: NZ=%lu HASH=%08lX SY=%lu PAL=%d ***",
                         (unsigned long)tv.nonzero_pixels,
                         (unsigned long)tv.hash,
                         (unsigned long)tv.scroll_y,
                         tv.palette_active);

                tab5_video_status(
                    "Human68k screen active",
                    "F9 toggles PX68K composite");

                if (!gfx_watch_armed)
                {
                    gfx_base_writes = GVRAM_DebugWriteCount();
                    gfx_base_pal_writes = Pal_DebugGrphWriteCount();
                    gfx_base_fast_clear = GVRAM_DebugFastClearCount();
                                gfx_watch_armed = true;

                    ESP_LOGI(TAG,
                             "Graphics watch armed: GVRAM=%lu GrphPal=%lu FastClr=%lu",
                             (unsigned long)gfx_base_writes,
                             (unsigned long)gfx_base_pal_writes,
                             (unsigned long)gfx_base_fast_clear);
                }
            }
        }

        /* RC: historical automatic CLS/DIR and C:/D: guest probes were OFF since
         * 5.96b and had no transition into their state machines.  Manual HID
         * input remains the sole console path. */

        /* LPFAB R2: CPU1 is the sole consumer of the brokered TouchJoy LATEST
         * state.  Apply it immediately before the existing guest-input tick so
         * guest-visible joystick ownership stays on the emulation task. */
        {
            uint16_t lp_touch_joy = 0u;
            if (tab5_lp_broker_touch_latest_take(&lp_touch_joy))
                (void)tab5_guest_input_queue_touch_joypad(lp_touch_joy);
        }

        /* Drain USB real-time events, then optional automatic text, on this task only. */
        tab5_guest_input_tick(frame);

        /* In-game side-bar actions.  CPU0 owns touch/UI; CPU1 applies disk
         * mutations here between guest frames so PX68K media state is never
         * changed concurrently with emulation.  FILE hot-swaps and ejects do
         * not reset or alter the boot source.  Layer8 Aug/17/2026 */
        if (!panic_mode)
        {
            tab5_video_action_t ui_action = {0};
            while (tab5_video_poll_action(&ui_action))
            {
                int ok = 0;
                switch (ui_action.type)
                {
                    case TAB5_VIDEO_ACTION_PANIC_RANDOM:
                    {
                        /* Build 6.12t: game->PANIC is an in-process X68000 guest
                         * reboot, NOT an ESP32-P4 reboot.  The old host-reset
                         * path tore down M5Unified/ES8388/I2S and produced the
                         * visible white hardware reboot reported on Tab5.
                         *
                         * CPU0 already selected and staged P68K.X/P68K.PAN. At
                         * this guest-frame boundary, switch A: to the dedicated
                         * Flash Human68k image and reset only PX68K.  The host
                         * speaker/audio feeder remains alive continuously. */
                        if (!ui_action.path[0]) {
                            ESP_LOGE(TAG, "In-game PANIC live switch rejected: missing staged PAN path");
                            break;
                        }

                        ESP_LOGI(TAG, "In-game PANIC live switch: guest-only reset, ESP/ES8388 preserved; PAN=%s",
                                 ui_action.path);
                        tab5_guest_input_cancel_text();
                        tab5_video_status("PANIC", "Switching X68000 to Human68k...");

                        /* Match launcher PANIC media state without restarting the host. */
                        (void)tab5_disk_eject(0);
                        (void)tab5_disk_eject(1);
                        if (hds_path[0]) (void)tab5_hdd_eject(0);

                        /* Preserve the live host audio stream here. WinX68k_Reset()
                         * resets guest OPM/ADPCM state via DSound_Stop/Play, but
                         * leaves the physical Tab5 speaker and CPU0 feeder up. */
                        tab5_video_touch_transition_begin(0u, 0u, 0u);
                        tab5_guest_video_state_reset();
                        tab5_screen_video_reset();
                        WinX68k_Reset();
                        if (!WinX68k_MountFloppy(0, TAB5_FLASH_HUMAN_PATH)) {
                            ESP_LOGE(TAG, "In-game PANIC live switch: Flash Human68k mount FAILED");
                            tab5_video_status("PANIC start failed", "Flash Human68k mount failed");
                            break;
                        }

                        snprintf(ctx->launcher_cfg.floppy0, sizeof(ctx->launcher_cfg.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                        ctx->launcher_cfg.floppy1[0] = '\0';
                        ctx->launcher_cfg.hdd0[0] = '\0';
                        snprintf(ctx->launcher_cfg.panic_path, sizeof(ctx->launcher_cfg.panic_path), "%s", ui_action.path);
                        ctx->launcher_cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                        ctx->launcher_cfg.mode = TAB5_LAUNCH_MODE_PANIC;

                        snprintf(xdf_path, 512, "%s", TAB5_FLASH_HUMAN_PATH);
                        b_xdf_path[0] = '\0';
                        hds_path[0] = '\0';
                        snprintf(a_boot_path, 512, "%s", TAB5_FLASH_HUMAN_PATH);
                        a_diskmag_boot = false;
                        b_inserted = false;
                        b_write_protected = false;
                        direct_hdd_boot = false;
                        hds_layout_ok = false;
                        hdd_deferred_fdds_pending = false;

                        /* Re-arm the normal PANIC bootstrap state machine from
                         * its initial Human68k boot state. */
                        TAB5_REARM_GUEST_OBSERVERS();

                        tab5_video_set_runtime_media_paths(xdf_path, b_xdf_path, hds_path);
                        tab5_video_set_runtime_boot_source(TAB5_LAUNCH_BOOT_FLOPPY0);
                        tab5_video_set_game_controls_enabled(0);
                        tab5_video_set_panic_compat_enabled(1);
                        tab5_panic_reset_touch();

                        ESP_LOGI(TAG, "In-game PANIC live switch complete: PC=$%08lX A:=%s; no ESP restart",
                                 (unsigned long)m68k_get_reg(NULL, M68K_REG_PC), TAB5_FLASH_HUMAN_PATH);
                        break;
                    }
                    case TAB5_VIDEO_ACTION_MOUNT_FDD0:
                        ok = tab5_disk_mount_path(0, ui_action.path);
                        if (ok) {
                            snprintf(xdf_path, 512, "%s", ui_action.path);
                            snprintf(a_boot_path, 512, "%s", ui_action.path);
                            a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD0 %s: %s", ok ? "inserted" : "FAILED", ui_action.path);
                        break;
                    case TAB5_VIDEO_ACTION_MOUNT_FDD1:
                        ok = tab5_disk_mount_path(1, ui_action.path);
                        if (ok) {
                            snprintf(b_xdf_path, 512, "%s", ui_action.path);
                            b_inserted = true;
                            b_write_protected = false;
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD1 %s: %s", ok ? "inserted" : "FAILED", ui_action.path);
                        break;
                    case TAB5_VIDEO_ACTION_EJECT_FDD0:
                        ok = tab5_disk_eject(0);
                        if (ok) {
                            xdf_path[0] = '\0';
                            a_boot_path[0] = '\0';
                            a_diskmag_boot = false;
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD0 eject: %s", ok ? "OK" : "FAILED");
                        break;
                    case TAB5_VIDEO_ACTION_EJECT_FDD1:
                        ok = tab5_disk_eject(1);
                        if (ok) {
                            b_xdf_path[0] = '\0';
                            b_inserted = false;
                            b_write_protected = false;
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD1 eject: %s", ok ? "OK" : "FAILED");
                        break;
                    case TAB5_VIDEO_ACTION_MOUNT_HDD0:
                        ok = tab5_hdd_mount_path(0, ui_action.path);
                        if (ok) snprintf(hds_path, 512, "%s", ui_action.path);
                        ESP_LOGI(TAG, "Runtime FILE HDD0 %s: %s ready=%d", ok ? "inserted" : "FAILED",
                                 ui_action.path, WinX68k_SCSIHDReady(0));
                        break;
                    case TAB5_VIDEO_ACTION_EJECT_HDD0:
                        ok = tab5_hdd_eject(0);
                        if (ok) hds_path[0] = '\0';
                        ESP_LOGI(TAG, "Runtime FILE HDD0 eject: %s", ok ? "OK" : "FAILED");
                        break;
                    case TAB5_VIDEO_ACTION_REBOOT_GUEST:
                    {
                        tab5_launcher_config_t reboot_cfg = ctx->launcher_cfg;
                        tab5_guest_boot_result_t boot_result = {0};
                        snprintf(reboot_cfg.floppy0, sizeof(reboot_cfg.floppy0), "%s", xdf_path);
                        snprintf(reboot_cfg.floppy1, sizeof(reboot_cfg.floppy1), "%s", b_xdf_path);
                        snprintf(reboot_cfg.hdd0, sizeof(reboot_cfg.hdd0), "%s", hds_path);
                        reboot_cfg.panic_path[0] = '\0';
                        reboot_cfg.mode = TAB5_LAUNCH_MODE_PX68K;
                        reboot_cfg.boot_source = !strcmp(ui_action.path, "HDD0") ?
                                                 TAB5_LAUNCH_BOOT_HDD0 : TAB5_LAUNCH_BOOT_FLOPPY0;

                        ESP_LOGI(TAG,
                                 "Runtime FILE (re)BOOT: guest-only source=%s; ESP/ES8388/I2S stay alive",
                                 reboot_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0 ? "HDD0" : "FDD0");
                        tab5_video_set_game_controls_enabled(0);
                        tab5_video_set_panic_compat_enabled(0);

                        if (!tab5_guest_apply_boot_config(ctx, &reboot_cfg, &boot_result,
                                                         "Runtime FILE (re)BOOT"))
                        {
                            tab5_launcher_config_t recovery = {0};
                            snprintf(recovery.floppy0, sizeof(recovery.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                            recovery.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                            recovery.mode = TAB5_LAUNCH_MODE_PX68K;
                            ESP_LOGW(TAG, "Runtime FILE (re)BOOT failed; recovering to Flash Human68k without host reset");
                            if (!tab5_guest_apply_boot_config(ctx, &recovery, &boot_result,
                                                             "Runtime FILE recovery"))
                            {
                                ESP_LOGE(TAG, "Runtime FILE recovery failed; guest stopped with host still alive");
                                tab5_video_show_message("Guest boot failed", "Host remains alive; reboot device manually");
                                break;
                            }
                        }

                        direct_hdd_boot = boot_result.direct_hdd_boot;
                        hds_layout_ok = boot_result.hds_layout_ok;
                        b_inserted = boot_result.b_inserted;
                        b_write_protected = false;
                        hdd_deferred_fdds_pending = boot_result.hdd_deferred_fdds_pending;
                        a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                        TAB5_REARM_GUEST_OBSERVERS();
                        tab5_panic_reset_touch();
                        tab5_video_set_game_controls_enabled(1);
                        tab5_video_set_panic_compat_enabled(0);
                        ESP_LOGI(TAG, "Runtime FILE (re)BOOT complete: no ESP restart");
                        break;
                    }
                    default:
                        break;
                }
                tab5_video_set_runtime_media_paths(xdf_path, b_xdf_path, hds_path);
            }
        }

        /*
         * PANIC one-shot bootstrap.  Keep the proven BAT-first sequence and
         * direct retry policy.  R57E62 additionally accepts HostFS-ready after
         * RAM execution when the host color-text detector never becomes active.
         * No direct guest-memory probing is used.
         */
        if (panic_mode && !panic_command_sent && ram_execution_seen &&
            !panic_hostfs_ready_frame)
        {
            const int ready_drive = HostFS_DebugDrive();
            if (ready_drive >= 0 && ready_drive < 26) {
                panic_hostfs_ready_frame = frame;
                ESP_LOGI(TAG,
                         "PANIC HostFS-ready fallback armed: drive=%c: frame=%lu; command eligible in 120 frames",
                         (char)('A' + ready_drive), (unsigned long)frame);
            }
        }

        const bool panic_text_trigger =
            textview_active && panic_text_ready_frame &&
            frame >= panic_text_ready_frame + 90u;
        const bool panic_hostfs_trigger =
            panic_hostfs_ready_frame &&
            frame >= panic_hostfs_ready_frame + 120u;

        if (panic_mode && !panic_command_sent &&
            (panic_text_trigger || panic_hostfs_trigger) &&
            !tab5_guest_input_busy())
        {
            const int host_drive = HostFS_DebugDrive();
            if (host_drive >= 0 && host_drive < 26)
            {
                const char drive = (char)('A' + host_drive);
                char command[40];
                const bool batch_ready = tab5_panic_prepare_batch(drive) != 0;
                if (batch_ready)
                    snprintf(command, sizeof(command), "%c:\\P68K.BAT\r", drive);
                else
                    snprintf(command, sizeof(command), "%c:\\P68K.X %c:\\P68K.PAN\r", drive, drive);

                ESP_LOGI(TAG, "PANIC audio launch: preserving live PCM/FM stream (6.11b behavior; no host flush)");
                tab5_guest_input_set_interval_frames(3u);
                if (tab5_guest_input_queue_text(command))
                {
                    panic_command_sent = true;
                    panic_command_frame = frame;
                    panic_command_gfx_base = GVRAM_DebugWriteCount();
                    panic_command_pal_base = Pal_DebugGrphWriteCount();
                    panic_command_bat_open_base = HostFS_DebugPanicBatchOpens();
                    panic_command_player_open_base = HostFS_DebugPanicPlayerOpens();
                    panic_command_pan_open_base = HostFS_DebugPanicPanOpens();
                    panic_touch_enable_frame = frame + 240u;
                    tab5_panic_reset_touch();
                    ESP_LOGI(TAG, "*** PANIC AUTO START: drive=%c: via=%s trigger=%s selected=%s ***",
                             drive, batch_ready ? "P68K.BAT" : "direct P68K.X",
                             panic_text_trigger ? "TEXT" : "HOSTFS-FALLBACK",
                             ctx->launcher_cfg.panic_path);
                    tab5_video_status("PANIC.X starting", "Tap=SPACE / hold upper-left=menu");
                }
            }
        }

        /* Build 6.12q: the old fallback used only GVRAM/palette activity as
         * its launch proof.  PANIC.X can be alive on its own title/initial
         * screen before those counters move; in that case typing the direct
         * command 10 seconds later becomes ordinary key input to PANIC.X,
         * skipping the title and potentially disturbing its audio init.
         *
         * HostFS now counts successful opens of P68K.X/P68K.PAN.  If either
         * was opened after our command, the guest really dispatched the
         * player and the fallback MUST stay silent.  Only a genuinely
         * undispatched BAT retains the proven direct retry. */
        if (panic_mode && panic_command_sent && !panic_direct_fallback_sent &&
            frame >= panic_command_frame + 600u && !tab5_guest_input_busy())
        {
            const uint32_t bat_opens = HostFS_DebugPanicBatchOpens();
            const uint32_t player_opens = HostFS_DebugPanicPlayerOpens();
            const uint32_t pan_opens = HostFS_DebugPanicPanOpens();
            const bool player_dispatched =
                player_opens > panic_command_player_open_base ||
                pan_opens > panic_command_pan_open_base;

            if (player_dispatched)
            {
                panic_direct_fallback_sent = true;
                ESP_LOGI(TAG,
                         "PANIC launch confirmed by HostFS opens; direct retry suppressed: BAT=%lu(+%lu) X=%lu(+%lu) PAN=%lu(+%lu)",
                         (unsigned long)bat_opens,
                         (unsigned long)(bat_opens - panic_command_bat_open_base),
                         (unsigned long)player_opens,
                         (unsigned long)(player_opens - panic_command_player_open_base),
                         (unsigned long)pan_opens,
                         (unsigned long)(pan_opens - panic_command_pan_open_base));
            }
            else if (GVRAM_DebugWriteCount() <= panic_command_gfx_base + 32u &&
                     Pal_DebugGrphWriteCount() <= panic_command_pal_base + 16u)
            {
                const int host_drive = HostFS_DebugDrive();
                if (host_drive >= 0 && host_drive < 26)
                {
                    const char drive = (char)('A' + host_drive);
                    char direct[40];
                    snprintf(direct, sizeof(direct), "%c:\\P68K.X %c:\\P68K.PAN\r", drive, drive);
                    tab5_guest_input_set_interval_frames(3u);
                    if (tab5_guest_input_queue_text(direct))
                    {
                        panic_direct_fallback_sent = true;
                        panic_touch_enable_frame = frame + 240u;
                        ESP_LOGW(TAG, "PANIC BAT did not open player after 10s; one direct P68K.X retry queued on %c: BATopens=%lu Xopens=%lu PANopens=%lu input_sent=%lu keyq=%lu delivered=%lu drop=%lu unmapped=%lu",
                                 drive,
                                 (unsigned long)bat_opens,
                                 (unsigned long)player_opens,
                                 (unsigned long)pan_opens,
                                 (unsigned long)tab5_guest_input_sent_chars(),
                                 (unsigned long)Keyboard_DebugQueued(),
                                 (unsigned long)Keyboard_DebugIntDelivered(),
                                 (unsigned long)Keyboard_DebugQueueDropped(),
                                 (unsigned long)Keyboard_DebugUnmapped());
                    }
                }
            }
        }

        /* R57E62 production freeze: one-shot PANIC audio diagnostics removed. */

        if (panic_mode && panic_command_sent && frame >= panic_touch_enable_frame && (frame % 3u) == 0u)
        {
            const int touch_action = tab5_panic_poll_playback_touch();
            if (touch_action == 1)
            {
                (void)tab5_guest_input_queue_key(RETROK_SPACE, 1);
                (void)tab5_guest_input_queue_key(RETROK_SPACE, 0);
            }
            else if (touch_action == 2)
            {
                tab5_launcher_config_t next_cfg = {0};
                tab5_guest_boot_result_t boot_result = {0};
                bool launcher_ok = false;

                ESP_LOGI(TAG, "PANIC return-to-GUI requested: entering native launcher without ESP restart");
                tab5_guest_input_cancel_text();
                tab5_video_set_game_controls_enabled(0);
                tab5_video_set_panic_compat_enabled(0);
                tab5_video_begin_host_ui();
                tab5_panic_cleanup_staging();

                for (;;)
                {
                    if (!tab5_launcher_run(human_path, &next_cfg))
                    {
                        ESP_LOGE(TAG, "Runtime launcher failed; falling back to Flash Human68k");
                        memset(&next_cfg, 0, sizeof(next_cfg));
                        snprintf(next_cfg.floppy0, sizeof(next_cfg.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                        next_cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                        next_cfg.mode = TAB5_LAUNCH_MODE_PX68K;
                        launcher_ok = true;
                        break;
                    }

                    if (next_cfg.mode != TAB5_LAUNCH_MODE_PANIC)
                    {
                        launcher_ok = true;
                        break;
                    }

                    if (tab5_panic_prepare_runtime(next_cfg.panic_path))
                    {
                        launcher_ok = true;
                        break;
                    }

                    ESP_LOGE(TAG, "Runtime launcher PANIC staging failed: %s", next_cfg.panic_path);
                    tab5_panic_cleanup_staging();
                    tab5_video_show_message("PANIC setup failed", "Returning to X68K Tab GUI...");
                    vTaskDelay(pdMS_TO_TICKS(900));
                }

                if (launcher_ok && !tab5_guest_apply_boot_config(ctx, &next_cfg, &boot_result,
                                                                  "PANIC -> GUI selection"))
                {
                    tab5_launcher_config_t recovery = {0};
                    snprintf(recovery.floppy0, sizeof(recovery.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                    recovery.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                    recovery.mode = TAB5_LAUNCH_MODE_PX68K;
                    ESP_LOGW(TAG, "PANIC -> GUI selected boot failed; recovering Flash Human68k without host reset");
                    launcher_ok = tab5_guest_apply_boot_config(ctx, &recovery, &boot_result,
                                                               "PANIC -> GUI recovery");
                }

                if (!launcher_ok)
                {
                    tab5_video_end_host_ui();
                    tab5_video_show_message("Guest boot failed", "ESP host/audio are still alive");
                    ESP_LOGE(TAG, "PANIC -> GUI recovery failed; no ESP restart performed");
                    break;
                }

                direct_hdd_boot = boot_result.direct_hdd_boot;
                hds_layout_ok = boot_result.hds_layout_ok;
                b_inserted = boot_result.b_inserted;
                b_write_protected = false;
                hdd_deferred_fdds_pending = boot_result.hdd_deferred_fdds_pending;
                a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                TAB5_REARM_GUEST_OBSERVERS();
                tab5_panic_reset_touch();

                tab5_video_end_host_ui();
                tab5_video_set_game_controls_enabled(panic_mode ? 0 : 1);
                tab5_video_set_panic_compat_enabled(panic_mode ? 1 : 0);

                ESP_LOGI(TAG,
                         "PANIC -> GUI complete: selected mode=%s source=%s; ESP/M5/ES8388/I2S never restarted",
                         panic_mode ? "PANIC" : "PX68K",
                         direct_hdd_boot ? "HDD0" : "FDD0");
            }
        }

        if (!mouse_activity_announced && tab5_usb_mouse_event_count() > 0u)
        {
            mouse_activity_announced = true;
            mouse_scc_packets_at_first_report = SCC_DebugMousePackets();
            mouse_scc_reads_at_first_report = SCC_DebugMouseBytesRead();
            ESP_LOGI(TAG,
                     "*** X68000 MOUSE ACTIVE: USB reports=%lu SCC packets=%lu bytes=%lu ***",
                     (unsigned long)tab5_usb_mouse_event_count(),
                     (unsigned long)mouse_scc_packets_at_first_report,
                     (unsigned long)mouse_scc_reads_at_first_report);
            tab5_video_status("X68000 mouse active",
                              "USB mouse -> PX68K SCC");
        }

        if (mouse_activity_announced && !mouse_guest_consumed_announced &&
            (SCC_DebugMousePackets() > mouse_scc_packets_at_first_report ||
             SCC_DebugMouseBytesRead() > mouse_scc_reads_at_first_report))
        {
            mouse_guest_consumed_announced = true;
            ESP_LOGI(TAG,
                     "*** X68000 MOUSE CONSUMED: SCC packets=%lu bytes=%lu ***",
                     (unsigned long)SCC_DebugMousePackets(),
                     (unsigned long)SCC_DebugMouseBytesRead());
        }

        if (!joypad_candidate_announced && tab5_usb_joypad_report_count() > 0u)
        {
            joypad_candidate_announced = true;
            ESP_LOGI(TAG,
                     "*** USB JOYPAD REPORTS ACTIVE: reports=%lu recognized=%d -> X68000 JOY1 ***",
                     (unsigned long)tab5_usb_joypad_report_count(),
                     tab5_usb_joypad_recognized());
            tab5_video_status("USB JoyPAD detected",
                              "Auto-mapping to X68000 JOY1");
        }

        if (!joypad_activity_announced && tab5_usb_joypad_event_count() > 0u)
        {
            joypad_activity_announced = true;
            ESP_LOGI(TAG,
                     "*** X68000 JOY1 ACTIVE: USB events=%lu state=%04X ***",
                     (unsigned long)tab5_usb_joypad_event_count(),
                     (unsigned)tab5_guest_input_joypad_state());
            tab5_video_status("X68000 JOY1 active",
                              "USB JoyPAD direction/button received");
        }

        /*
         * Build 5.5/5.10 host hotkeys are consumed in the USB layer but executed
         * here, on the emulation task, so PX68K/FDD state remains single-threaded.
         */
        {
            const uint32_t hotkeys = tab5_usb_keyboard_take_hotkeys();

            if (hotkeys & TAB5_USB_HOTKEY_A_BOOT_NEXT)
            {
                char target[512];

                if (!tab5_disk_next_boot_media(a_boot_path, target, sizeof(target)))
                {
                    ESP_LOGW(TAG, "A: F7 boot-media catalog is empty");
                    tab5_video_status("A: boot selector unavailable", "No .XDF/.DIM in SD root");
                }
                else
                {
                    /* Never keep the same writable image mounted in both drives. */
                    if (b_inserted && b_xdf_path[0] && !strcmp(b_xdf_path, target))
                    {
                        if (tab5_disk_eject(1))
                        {
                            b_inserted = false;
                            ESP_LOGI(TAG, "B: auto-ejected because selected F7 boot media is becoming A:");
                        }
                    }

                    tab5_guest_input_cancel_text();
                    (void)tab5_disk_eject(0);

                    ESP_LOGI(TAG, "*** A: BOOT NEXT (F7): guest reset for %s; host audio preserved ***", target);
                    tab5_video_touch_transition_begin(0u, 0u, 0u);
                    tab5_guest_video_state_reset();
                    tab5_screen_video_reset();
                    WinX68k_Reset();

                    if (WinX68k_MountFloppy(0, target))
                    {
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", target);
                        a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                        gfx_activity_seen = false;
                        gfx_base_writes = 0;
                        gfx_base_pal_writes = 0;
                        gfx_base_fast_clear = 0;
                        memset(&tv, 0, sizeof(tv));

                        ESP_LOGI(TAG, "*** A: BOOT MEDIA (F7): %s ***", a_boot_path);
                        tab5_video_status("Booting next SD image", a_boot_path);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "A: F7 mount failed after reset: %s", target);
                        (void)WinX68k_MountFloppy(0, TAB5_FLASH_HUMAN_PATH);
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", TAB5_FLASH_HUMAN_PATH);
                        a_diskmag_boot = false;
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                        gfx_activity_seen = false;
                        memset(&tv, 0, sizeof(tv));
                        tab5_video_status("A: boot switch failed", "Recovered HUMAN302.XDF");
                    }
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_A_BOOT_TOGGLE)
            {
                const char *target = a_diskmag_boot ? TAB5_FLASH_HUMAN_PATH : diskmag_path;

                if (!target || !target[0])
                {
                    ESP_LOGW(TAG, "A: F8 alternate boot unavailable (DISKMAG1.XDF not found)");
                    tab5_video_status("A: boot switch unavailable", "DISKMAG1.XDF not found");
                }
                else
                {
                    /* Never keep the same writable XDF mounted in both drives. */
                    if (!a_diskmag_boot && b_inserted &&
                        b_xdf_path[0] && !strcmp(b_xdf_path, diskmag_path))
                    {
                        if (tab5_disk_eject(1))
                        {
                            b_inserted = false;
                            ESP_LOGI(TAG, "B: auto-ejected because DISKMAG1 is becoming A: boot media");
                        }
                    }

                    tab5_guest_input_cancel_text();
                    (void)tab5_disk_eject(0);

                    ESP_LOGI(TAG, "*** A: BOOT SWITCH (F8): guest reset for %s; host audio preserved ***", target);
                    tab5_video_touch_transition_begin(0u, 0u, 0u);
                    tab5_guest_video_state_reset();
                    tab5_screen_video_reset();
                    WinX68k_Reset();

                    if (WinX68k_MountFloppy(0, target))
                    {
                        a_diskmag_boot = !a_diskmag_boot;
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", target);
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                        gfx_activity_seen = false;
                        gfx_base_writes = 0;
                        gfx_base_pal_writes = 0;
                        gfx_base_fast_clear = 0;
                        memset(&tv, 0, sizeof(tv));

                        ESP_LOGI(TAG, "*** A: BOOT MEDIA (F8): %s ***", target);
                        tab5_video_status(a_diskmag_boot ? "Booting DISKMAG1.XDF" : "Booting HUMAN302.XDF",
                                          "PX68K composite view");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "A: F8 mount failed after reset: %s", target);
                        /* Recover the known Human68k boot media instead of leaving A: empty. */
                        (void)WinX68k_MountFloppy(0, TAB5_FLASH_HUMAN_PATH);
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", TAB5_FLASH_HUMAN_PATH);
                        a_diskmag_boot = false;
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                        gfx_activity_seen = false;
                        memset(&tv, 0, sizeof(tv));
                        tab5_video_status("A: boot switch failed", "Recovered HUMAN302.XDF");
                    }
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_VIDEO_TOGGLE)
            {
                tab5_video_touch_transition_begin(0u, 0u, 0u);
                composite_view = !composite_view;
                ESP_LOGI(TAG, "*** VIDEO VIEW (F9): %s ***",
                         composite_view ? "PX68K COMPOSITE" : "COLOR TEXT");
                tab5_video_status(composite_view ? "PX68K composite view" : "PX68K color text view",
                                  composite_view ? "F9=color text" : "F9=composite");
            }

            if (hotkeys & TAB5_USB_HOTKEY_B_WP)
            {
                if (!b_inserted)
                {
                    ESP_LOGW(TAG, "B: F10 ignored while ejected");
                }
                else
                {
                    const bool requested = !b_write_protected;
                    if (tab5_disk_set_write_protect(1, requested ? 1 : 0))
                    {
                        b_write_protected = requested;
                        ESP_LOGI(TAG, "*** B: WRITE PROTECT %s (F10, safe remount) ***",
                                 b_write_protected ? "ON" : "OFF");
                        tab5_video_status(b_write_protected ? "B: write protected" : "B: writable",
                                          b_xdf_path[0] ? b_xdf_path : "No disk");
                    }
                    else
                    {
                        ESP_LOGW(TAG, "B: F10 write-protect change failed");
                    }
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_B_TOGGLE)
            {
                if (b_inserted)
                {
                    if (tab5_disk_eject(1))
                    {
                        b_inserted = false;
                        ESP_LOGI(TAG, "*** B: EJECTED (F11), selected=%s ***",
                                 b_xdf_path[0] ? b_xdf_path : "(none)");
                        tab5_video_status("B: ejected", "F11=insert, F12=next disk");
                    }
                }
                else if (b_xdf_path[0] && WinX68k_MountFloppy(1, b_xdf_path))
                {
                    b_inserted = true;
                    b_write_protected = false;
                    ESP_LOGI(TAG, "*** B: REINSERTED (F11): %s ***", b_xdf_path);
                    tab5_video_status("B: inserted", b_xdf_path);
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_B_NEXT)
            {
                char next_path[512];

                if (b_inserted)
                {
                    (void)tab5_disk_eject(1);
                    b_inserted = false;
                }

                if (tab5_disk_mount_next_other(1,
                                               a_boot_path,
                                               b_xdf_path,
                                               next_path,
                                               sizeof(next_path)))
                {
                    snprintf(b_xdf_path, sizeof(b_xdf_path), "%s", next_path);
                    b_inserted = true;
                    b_write_protected = false;
                    ESP_LOGI(TAG, "*** B: NEXT XDF (F12): %s ***", b_xdf_path);
                    tab5_video_status("B: next disk inserted", b_xdf_path);
                }
                else
                {
                    ESP_LOGW(TAG, "B: F12 could not select another XDF");
                    tab5_video_status("B: disk change failed", "No alternate XDF");
                }
            }
        }

#if PX68K_TAB5_DIAG_GRAPHICS
        if (gfx_watch_armed)
        {
            const uint32_t gw = GVRAM_DebugWriteCount();
            const uint32_t gp = Pal_DebugGrphWriteCount();
            const uint32_t fc = GVRAM_DebugFastClearCount();

            if (!gfx_activity_seen &&
                (gw != gfx_base_writes ||
                 gp != gfx_base_pal_writes ||
                 fc != gfx_base_fast_clear))
            {
                gfx_activity_seen = true;

                ESP_LOGI(TAG,
                         "*** GRAPHICS ACTIVITY: GVRAM +%lu GrphPal +%lu FastClr +%lu "
                         "mode=%u CRTC28=%02X CRTC29=%02X last=$C%05lX<-%02X ***",
                         (unsigned long)(gw - gfx_base_writes),
                         (unsigned long)(gp - gfx_base_pal_writes),
                         (unsigned long)(fc - gfx_base_fast_clear),
                         (unsigned)GVRAM_DebugLastMode(),
                         (unsigned)CRTC_Regs[0x28],
                         (unsigned)CRTC_Regs[0x29],
                         (unsigned long)GVRAM_DebugLastAddr(),
                         (unsigned)GVRAM_DebugLastData());

                tab5_video_status("X68000 graphics activity",
                                  "Native PX68K composite path active");
            }
        }
#endif

#if PX68K_TAB5_DIAG_VERBOSE
        /*
         * Build 5.1 diagnostic: log only when a real USB key event reaches our
         * queue or when Human68k reads the MFP UDR.  This distinguishes an
         * input-path failure from a text-scroll/display failure without flooding
         * the serial port during normal emulation.
         */
        {
            const uint32_t diag_usb_events = tab5_usb_keyboard_event_count();
            const uint32_t diag_udr_reads = MFP_DebugUDRReads();

            if (diag_usb_events != diag_prev_usb_events ||
                diag_udr_reads != diag_prev_udr_reads)
            {
                ESP_LOGI(TAG,
                         "KEYPIPE f=%lu USB_EV=%lu RTQ=%u "
                         "KD/KU=%lu/%lu KQ=%u/%u KIF=%u "
                         "ENQ=%lu KDROP=%lu UNMAP=%lu KINT=%lu "
                         "KLAST=%02X INTLAST=%02X "
                         "IRQ=%lu/%lu/%lu UDR=%lu RSR=%lu UDRLAST=%02X "
                         "MFP=%02X/%02X/%02X/%02X SY=%lu TH=%08lX TXMIS=%lu RC=%lu",
                         (unsigned long)frame,
                         (unsigned long)diag_usb_events,
                         (unsigned)tab5_guest_input_realtime_pending(),
                         (unsigned long)Keyboard_DebugKeyDownCalls(),
                         (unsigned long)Keyboard_DebugKeyUpCalls(),
                         (unsigned)KeyBufRP,
                         (unsigned)KeyBufWP,
                         (unsigned)KeyIntFlag,
                         (unsigned long)Keyboard_DebugQueued(),
                         (unsigned long)Keyboard_DebugQueueDropped(),
                         (unsigned long)Keyboard_DebugUnmapped(),
                         (unsigned long)Keyboard_DebugIntDelivered(),
                         (unsigned)Keyboard_DebugLastQueued(),
                         (unsigned)Keyboard_DebugLastDelivered(),
                         (unsigned long)MFP_DebugKeyboardIRQCalls(),
                         (unsigned long)MFP_DebugKeyboardIRQEnabled(),
                         (unsigned long)MFP_DebugKeyboardIRQDisabled(),
                         (unsigned long)diag_udr_reads,
                         (unsigned long)MFP_DebugRSRReads(),
                         (unsigned)MFP_DebugLastUDR(),
                         (unsigned)MFP[MFP_IERA],
                         (unsigned)MFP[MFP_IMRA],
                         (unsigned)MFP[MFP_IPRA],
                         (unsigned)MFP[MFP_ISRA],
                         (unsigned long)(tv.pixels ? tv.scroll_y : 0u),
                         (unsigned long)(tv.pixels ? tv.hash : 0u),
                         (unsigned long)(tv.pixels ? tv.expanded_mismatch_pixels : 0u),
                         (unsigned long)CRTC_DebugRasterCopyCount());

                diag_prev_usb_events = diag_usb_events;
                diag_prev_udr_reads = diag_udr_reads;
            }
        }

        {
            const uint32_t rc_now = CRTC_DebugRasterCopyCount();
            if (rc_now != diag_prev_raster_copy)
            {
                ESP_LOGI(TAG,
                         "TEXT_RC f=%lu count=%lu (+%lu) src=%02X dst=%02X "
                         "planes=%X mode=%02X TH=%08lX TXMIS=%lu",
                         (unsigned long)frame,
                         (unsigned long)rc_now,
                         (unsigned long)(rc_now - diag_prev_raster_copy),
                         (unsigned)CRTC_DebugRasterCopySrc(),
                         (unsigned)CRTC_DebugRasterCopyDst(),
                         (unsigned)CRTC_DebugRasterCopyPlanes(),
                         (unsigned)CRTC_DebugRasterCopyMode(),
                         (unsigned long)(tv.pixels ? tv.hash : 0u),
                         (unsigned long)(tv.pixels ? tv.expanded_mismatch_pixels : 0u));
            }
            diag_prev_raster_copy = rc_now;
        }

        if (tv.pixels && tv.scroll_y != diag_prev_scroll_y)
        {
            if (diag_prev_scroll_y != 0xffffffffu)
            {
                ESP_LOGI(TAG,
                         "TEXT_SCROLL f=%lu SY=%lu->%lu TH=%08lX "
                         "USB_EV=%lu KQ=%u/%u KIF=%u UDR=%lu",
                         (unsigned long)frame,
                         (unsigned long)diag_prev_scroll_y,
                         (unsigned long)tv.scroll_y,
                         (unsigned long)tv.hash,
                         (unsigned long)tab5_usb_keyboard_event_count(),
                         (unsigned)KeyBufRP,
                         (unsigned)KeyBufWP,
                         (unsigned)KeyIntFlag,
                         (unsigned long)MFP_DebugUDRReads());
            }
            diag_prev_scroll_y = tv.scroll_y;
        }
#endif /* PX68K_TAB5_DIAG_VERBOSE */

        bool budget_present = budget_render;
        if (do_present && frame > 12u && budget_render)
        {
            const uint32_t present_slot = frame / present_div;
            if (budget.mode == TAB5_BUDGET_AUDIO_CRITICAL)
            {
                /* R57E11 Audio Deadline Shield: keep only every second normal
                 * physical present while reserve is critical.  Logical render
                 * and mailbox state continue latest-wins in the background. */
                budget_present = ((present_slot & 1u) == 0u);
            }
            else if (budget.mode == TAB5_BUDGET_AUDIO_GUARD)
            {
                /* Mild pressure: shed one of four physical presents. */
                budget_present = ((present_slot & 3u) != 3u);
            }
        }

        const bool do_present_budget = do_present && budget_present;
        if (do_present && !budget_present)
            ++budget.present_skips;

        if (do_present_budget)
        {
#if PX68K_TAB5_PERF_PROFILE
            int64_t perf_lcd_start = perf_sample ? esp_timer_get_time() : 0;
#endif
            if (textview_active && tv.pixels && !composite_view)
            {
                tab5_video_present_px68k(
                    tv.pixels,
                    tv.width,
                    tv.height,
                    tv.pitch_pixels);
            }
            else
            {
                /* R56: host policy offers an opportunity only.  It never names
                 * a ScreenVersion and never passes a framebuffer pointer.
                 * Screen Manager seals only after every opaque render ticket
                 * for the selected guest-sequence cutoff has completed. */
                tab5_screen_present_opportunity();
            }
#if PX68K_TAB5_PERF_PROFILE
            if (perf_sample)
                perf_lcd_us = (uint32_t)(esp_timer_get_time() - perf_lcd_start);
#endif
        }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        /* R56k3 diagnostic window: the R56k2 failure occurred between f=600
         * and f=900.  Sample without malloc/newlib stdio every 60 guest frames
         * from f=480 through f=1200.  Integrity traversal is limited to every
         * 120 frames to keep diagnostic perturbation bounded. */
        if (frame >= 480u && frame <= 1200u && (frame % 60u) == 0u)
            tab5_r56k3_heap_probe_rom(frame, (frame % 120u) == 0u);
        if (frame == 600u)
            tab5_r56k4_print_task_stack_map(frame);

        if ((frame % 600u) == 0u) {
            tab5_screen_manager_stats_t ss = {0};
            tab5_screen_manager_get_stats(&ss);
            ESP_LOGI(TAG,
                     "SCREEN_R57E11 f=%lu orderSeq=%llu guestFrame=%llu cand=%llu visible=%llu pending=%lu/%lu carry=%lu open=%lu seal=%lu geomReset=%lu ready=%llu retired=%llu dropped=%llu result=%llu noop=%llu retry=%llu/%llu stale=%llu p=%llu/%llu backpressure=%llu carryCap=%llu carrySame=%llu carryFair=%llu fairWrap=%llu mail{post=%llu dup=%llu sup=%llu claim=%llu lines=%llu tiles=%llu maxSeq=%llu p=%llu swap=%llu defer=%llu wrRetry=%llu rescue=%llu/%llu skipBusy=%llu skipBudget=%llu edgeWake=%llu limit=%lu aq=%lu epoch=%lu used=%lu active=%lu pend=0x%lx in=%lu/%lu} ticketDiag{old=%llu seq=%llu age=%llu y=%lu rq=%lu free=%lu} geomDup=%llu rtlog{coll=%llu unk=%llu rej=%llu ooo=%llu}",
                     (unsigned long)frame,
                     (unsigned long long)ss.guest_seq,
                     (unsigned long long)ss.guest_frame,
                     (unsigned long long)ss.candidate_id,
                     (unsigned long long)ss.visible_id,
                     (unsigned long)ss.pending_tickets,
                     (unsigned long)ss.max_inflight,
                     (unsigned long)ss.deferred_lines,
                     (unsigned long)ss.admission_open,
                     (unsigned long)ss.seal_requested,
                     (unsigned long)ss.geometry_reset_pending,
                     (unsigned long long)ss.ready_count,
                     (unsigned long long)ss.retired_count,
                     (unsigned long long)ss.dropped_count,
                     (unsigned long long)ss.line_results,
                     (unsigned long long)ss.line_noops,
                     (unsigned long long)ss.retry_delivered,
                     (unsigned long long)ss.retry_requests,
                     (unsigned long long)ss.stale_results,
                     (unsigned long long)ss.present_completions,
                     (unsigned long long)ss.present_submits,
                     (unsigned long long)ss.present_backpressure,
                     (unsigned long long)ss.admission_deferred_capacity,
                     (unsigned long long)ss.admission_deferred_same_line,
                     (unsigned long long)ss.admission_deferred_fair,
                     (unsigned long long)ss.fair_wraps,
                     (unsigned long long)ss.mailbox_posts,
                     (unsigned long long)ss.mailbox_duplicate_posts,
                     (unsigned long long)ss.mailbox_offer_suppressed,
                     (unsigned long long)ss.mailbox_refresh_claims,
                     (unsigned long long)ss.mailbox_refresh_lines,
                     (unsigned long long)ss.mailbox_refresh_tiles,
                     (unsigned long long)ss.mailbox_last_claim_max_seq,
                     (unsigned long long)ss.mailbox_last_claim_present_token,
                     (unsigned long long)ss.mailbox_buffer_swaps,
                     (unsigned long long)ss.mailbox_swap_deferred,
                     (unsigned long long)ss.mailbox_writer_retries,
                     (unsigned long long)ss.mailbox_rescue_swaps,
                     (unsigned long long)ss.mailbox_rescue_attempts,
                     (unsigned long long)ss.mailbox_rescue_skip_busy,
                     (unsigned long long)ss.mailbox_rescue_skip_budget,
                     (unsigned long long)ss.mailbox_edge_wakes,
                     (unsigned long)ss.mailbox_rescue_limit,
                     (unsigned long)ss.audio_reserve_frames,
                     (unsigned long)ss.mailbox_offer_epoch,
                     (unsigned long)ss.mailbox_rescues_since_refresh,
                     (unsigned long)ss.mailbox_active_index,
                     (unsigned long)ss.mailbox_pending_mask,
                     (unsigned long)ss.mailbox_inflight0,
                     (unsigned long)ss.mailbox_inflight1,
                     (unsigned long long)ss.oldest_pending_ticket,
                     (unsigned long long)ss.oldest_pending_guest_seq,
                     (unsigned long long)ss.oldest_pending_age_seq,
                     (unsigned long)ss.oldest_pending_y,
                     (unsigned long)ss.result_q_depth,
                     (unsigned long)ss.result_free_depth,
                     (unsigned long long)ss.geometry_duplicate_suppressed,
                     (unsigned long long)ss.tasklog_record_collisions,
                     (unsigned long long)ss.tasklog_present_unknown,
                     (unsigned long long)ss.tasklog_present_rejected,
                     (unsigned long long)ss.tasklog_present_out_of_order);

            tab5_guest_video_state_stats_t gvs = {0};
            tab5_guest_video_state_get_stats(&gvs);
            ESP_LOGI(TAG,
                     "VIDEOEPOCH_R57E27 f=%lu guest{epoch=%lu visual=%lu hard=%lu vischg=%lu lastHard=0x%lx hardOr=0x%lx visOr=0x%lx geom=%lux%lu/%lu vm=%02x cm=%02x} screen{cand=%lu-%lu vis=%lu-%lu trans=%llu mixCommit=%llu pend=%lu target=%lu seal{n=%llu bad=%llu qdrop=%llu good=%llu last=%llu/%lu-%lu/e%lu/mis%lu/unk%lu} visible{bad=%llu last=%llu/e%lu/mis%lu/unk%lu}} POLICY=HARD_EPOCH_QUARANTINE",
                     (unsigned long)frame,
                     (unsigned long)gvs.video_epoch,
                     (unsigned long)gvs.visual_seq,
                     (unsigned long)gvs.hard_transitions,
                     (unsigned long)gvs.visual_transitions,
                     (unsigned long)gvs.last_hard_cause,
                     (unsigned long)gvs.hard_cause_or,
                     (unsigned long)gvs.visual_cause_or,
                     (unsigned long)gvs.width,
                     (unsigned long)gvs.height,
                     (unsigned long)gvs.pitch,
                     (unsigned)gvs.vctrl_mode,
                     (unsigned)gvs.crtc_mode,
                     (unsigned long)ss.candidate_video_epoch_min,
                     (unsigned long)ss.candidate_video_epoch_max,
                     (unsigned long)ss.candidate_visual_seq_min,
                     (unsigned long)ss.candidate_visual_seq_max,
                     (unsigned long long)ss.epoch_transitions,
                     (unsigned long long)ss.epoch_mixed_commits,
                     (unsigned long)ss.epoch_transition_pending,
                     (unsigned long)ss.epoch_transition_target,
                     (unsigned long long)ss.epoch_transition_seals,
                     (unsigned long long)ss.epoch_transition_bad_seals,
                     (unsigned long long)ss.epoch_quarantine_drops,
                     (unsigned long long)ss.epoch_quarantine_coherent_seals,
                     (unsigned long long)ss.epoch_last_transition_seal_id,
                     (unsigned long)ss.epoch_last_transition_seal_min,
                     (unsigned long)ss.epoch_last_transition_seal_max,
                     (unsigned long)ss.epoch_last_transition_seal_epoch,
                     (unsigned long)ss.epoch_last_transition_mismatch_lines,
                     (unsigned long)ss.epoch_last_transition_unknown_lines,
                     (unsigned long long)ss.epoch_transition_bad_visible,
                     (unsigned long long)ss.epoch_last_bad_visible_id,
                     (unsigned long)ss.epoch_last_bad_visible_epoch,
                     (unsigned long)ss.epoch_last_bad_visible_mismatch_lines,
                     (unsigned long)ss.epoch_last_bad_visible_unknown_lines);

            {
                tab5_cpu1_video_publish_stats_t cps = {0};
                tab5_video_flow_stats_t vfs = {0};
                tab5_cpu1_video_get_publish_stats(&cps);
                tab5_video_flow_get_stats(&vfs);
                ESP_LOGI(TAG,
                         "CPU1NW_R57E48 f=%lu exact=%llu accel=%llu nw{commit=%llu offload=%llu} frontier{guest=%llu compose=%llu screen=%llu visible=%llu lagGC=%lld lagGS=%lld lagGV=%lld} flow{dirty=%lu max=%lu post=%llu race=%llu requeue=%llu cpu0=%llu} CONTRACT=CPU1_NO_WAIT_EXACT_FINAL_BGINV_PALBANK_PRUNE_TEXTFAST_BGFUSE_GRPGBT65FUSE_CPUHOTIRAM12_FINALFUSE",
                         (unsigned long)frame,
                         (unsigned long long)cps.exact_renders,
                         (unsigned long long)cps.accelerated_renders,
                         (unsigned long long)cps.no_wait_commits,
                         (unsigned long long)cps.no_wait_offloads,
                         (unsigned long long)vfs.guest_frontier,
                         (unsigned long long)vfs.compose_frontier,
                         (unsigned long long)vfs.screen_commit_frontier,
                         (unsigned long long)vfs.visible_frontier,
                         (long long)(vfs.guest_frontier - vfs.compose_frontier),
                         (long long)(vfs.guest_frontier - vfs.screen_commit_frontier),
                         (long long)(vfs.guest_frontier - vfs.visible_frontier),
                         (unsigned long)vfs.dirty_lines,
                         (unsigned long)vfs.dirty_max,
                         (unsigned long long)vfs.dirty_posts,
                         (unsigned long long)vfs.snapshot_races,
                         (unsigned long long)vfs.requeues,
                         (unsigned long long)vfs.cpu0_final_commits);
            }

            /* R56k substrate-only health sampler.  No pixel/sprite/audio
             * feature diagnosis here: just scheduler queues, stacks and heap. */
            {
                tab5_audio_stats_t hs = {0};
                uint32_t fm_q = 0u, fm_drop = 0u, fm_over = 0u, fm_avail = 0u;
                uint32_t fm_w_att = 0u, fm_w_drop = 0u, fm_w_frames = 0u;
                uint32_t fm_r_att = 0u, fm_r_drop = 0u, fm_r_frames = 0u;
                uint32_t fm_reset_drop = 0u, fm_vol_drop = 0u, fm_csm_drop = 0u;
                uint32_t fm_stop_drop = 0u, fm_other_drop = 0u;
                uint32_t fm_bp_events = 0u, fm_bp_wait_calls = 0u, fm_bp_timeouts = 0u;
                uint32_t fm_bp_max = 0u, fm_discard_events = 0u, fm_discard_frames = 0u;
                uint32_t usb_lib_hw = 0u, usb_ctl_hw = 0u;
                tab5_audio_get_stats(&hs);
                WinX68k_AudioAsyncGetStats(&fm_q, &fm_drop, &fm_over, &fm_avail);
                WinX68k_AudioAsyncGetQueueTaxonomy(
                    &fm_w_att, &fm_w_drop, &fm_w_frames,
                    &fm_r_att, &fm_r_drop, &fm_r_frames,
                    &fm_reset_drop, &fm_vol_drop, &fm_csm_drop,
                    &fm_stop_drop, &fm_other_drop);
                WinX68k_AudioAsyncGetBackpressureStats(
                    &fm_bp_events, &fm_bp_wait_calls, &fm_bp_timeouts,
                    &fm_bp_max, &fm_discard_events, &fm_discard_frames);
                tab5_usb_keyboard_stack_highwater(&usb_lib_hw, &usb_ctl_hw);
                const uint32_t int_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                const uint32_t int_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                const uint32_t int_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                const uint32_t exec_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_EXEC);
                ESP_LOGI(TAG,
                         "R56K_HOST f=%lu stack{guest=%u ym=%u aud=%u screen=%u comp=%u lcd=%u usb=%u/%u} heap{int=%lu largest=%lu min=%lu exec=%lu} flow{fmq=%lu avail=%lu drop=%lu over=%lu hostq=%lu spq=%lu under=%lu fail=%lu} fmev{W=%lu/%lu/%lu R=%lu/%lu/%lu ctl=%lu/%lu/%lu/%lu other=%lu} r56l{bp=%lu/%lu/%lu/max%lu discard=%lu/%lu}",
                         (unsigned long)frame,
                         s_guest_task ? (unsigned)uxTaskGetStackHighWaterMark(s_guest_task) : 0u,
                         (unsigned)OPM_AsyncStackHighWater(),
                         (unsigned)tab5_audio_stack_highwater(),
                         (unsigned)tab5_screen_manager_stack_highwater(),
                         (unsigned)tab5_compose_stack_highwater(),
                         (unsigned)tab5_video_stack_highwater(),
                         (unsigned)usb_lib_hw, (unsigned)usb_ctl_hw,
                         (unsigned long)int_free, (unsigned long)int_largest,
                         (unsigned long)int_min, (unsigned long)exec_free,
                         (unsigned long)fm_q, (unsigned long)fm_avail,
                         (unsigned long)fm_drop, (unsigned long)fm_over,
                         (unsigned long)hs.queued_frames,
                         (unsigned long)hs.speaker_queued_frames,
                         (unsigned long)hs.underflow_events,
                         (unsigned long)hs.play_failures,
                         (unsigned long)fm_w_att, (unsigned long)fm_w_drop,
                         (unsigned long)fm_w_frames,
                         (unsigned long)fm_r_att, (unsigned long)fm_r_drop,
                         (unsigned long)fm_r_frames,
                         (unsigned long)fm_reset_drop, (unsigned long)fm_vol_drop,
                         (unsigned long)fm_csm_drop, (unsigned long)fm_stop_drop,
                         (unsigned long)fm_other_drop,
                         (unsigned long)fm_bp_events, (unsigned long)fm_bp_wait_calls,
                         (unsigned long)fm_bp_timeouts, (unsigned long)fm_bp_max,
                         (unsigned long)fm_discard_events, (unsigned long)fm_discard_frames);

                /* R57E2: CPU1-side fact probe for priority inversion / frozen
                 * shadow holds. This is deliberately logged from CPU1, never
                 * from the CPU0 compositor/journal worker. */
                tab5_guest_bus_health_t bh = {0};
                tab5_guest_bus_get_health(&bh);
                ESP_LOGI(TAG,
                         "R57E2_BUS f=%lu seq{%lu/%lu/%lu/%lu} q{depth=%lu max=%lu full=%lu drop=%lu gap=%lu inv=%lu} hold{h=%lu t=%lu reached=%lu claim=%lu busy=%lu cancel=%lu rel=%lu wait=%lu zskip=%lu} raster=%lu",
                         (unsigned long)frame,
                         (unsigned long)bh.attempt_seq,
                         (unsigned long)bh.prod_seq,
                         (unsigned long)bh.cons_seq,
                         (unsigned long)bh.shadow_seq,
                         (unsigned long)bh.depth,
                         (unsigned long)bh.max_depth,
                         (unsigned long)bh.ring_full,
                         (unsigned long)bh.dropped,
                         (unsigned long)bh.seq_gap,
                         (unsigned long)bh.invalid,
                         (unsigned long)bh.hold_head,
                         (unsigned long)bh.hold_tail,
                         (unsigned long)bh.hold_reached,
                         (unsigned long)bh.hold_claims,
                         (unsigned long)bh.hold_busy,
                         (unsigned long)bh.hold_cancel,
                         (unsigned long)bh.hold_release,
                         (unsigned long)bh.hold_waits,
                         (unsigned long)bh.hold_zero_skip,
                         (unsigned long)bh.raster_tokens);
            }
        }

#endif /* PX68K_TAB5_RELEASE_DIAGNOSTICS */

#if PX68K_TAB5_DIAG_VERBOSE
        /* R56: CPU1 diagnostics no longer read Screen Manager pixel memory.
         * Screen lifecycle/timeline counters are reported separately by
         * SCREEN_R56; RUN FRAME stays guest/input/text diagnostics only. */
        const bool log_frame =
            (frame <= 12u) ||
            (frame <= 600u && (frame % 60u) == 0u) ||
            (frame > 600u && (frame % 300u) == 0u);

        if (log_frame)
        {
            ESP_LOGI(TAG,
                     "RUN FRAME %06lu cycles=%d "
                     "PC=$%08lX SR=$%04lX "
                     "TXT_NZ=%lu TXT_HASH=%08lX SY=%lu PAL=%d TXMIS=%lu RC=%lu "
                     "view=%s KBD=%u/%u KIF=%u UDR=%lu Q=%u RT=%u USB=%d EV=%lu DROP=%lu",
                     (unsigned long)frame,
                     cycles,
                     (unsigned long)pc,
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_SR),
                     (unsigned long)(tv.pixels ? tv.nonzero_pixels : 0u),
                     (unsigned long)(tv.pixels ? tv.hash : 0u),
                     (unsigned long)(tv.pixels ? tv.scroll_y : 0u),
                     (int)(tv.pixels ? tv.palette_active : 0),
                     (unsigned long)(tv.pixels ? tv.expanded_mismatch_pixels : 0u),
                     (unsigned long)CRTC_DebugRasterCopyCount(),
                     (textview_active && !composite_view) ? "COLOR_TEXT" : "COMPOSITE",
                     (unsigned)KeyBufRP,
                     (unsigned)KeyBufWP,
                     (unsigned)KeyIntFlag,
                     (unsigned long)MFP_DebugUDRReads(),
                     (unsigned)tab5_guest_input_pending(),
                     (unsigned)tab5_guest_input_realtime_pending(),
                     tab5_usb_keyboard_connected(),
                     (unsigned long)tab5_usb_keyboard_event_count(),
                     (unsigned long)tab5_guest_input_realtime_dropped());
        }

#endif /* PX68K_TAB5_DIAG_VERBOSE */

        if (do_present_budget && (frame % 120u) == 0u)
        {
            char line2[112];

            snprintf(line2, sizeof(line2),
                     "USB=%s EV=%lu DROP=%lu XDF=%u f%lu",
                     tab5_usb_keyboard_connected() ? "KBD" :
                         (usb_host_ready ? "WAIT" : "OFF"),
                     (unsigned long)tab5_usb_keyboard_event_count(),
                     (unsigned long)tab5_guest_input_realtime_dropped(),
                     (unsigned)tab5_sd_xdf_count(),
                     (unsigned long)frame);

            tab5_video_status(tab5_usb_keyboard_connected() ? "USB keyboard active" : "Human68k running",
                              line2);
        }

        if (frame == 600u)
        {
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            ESP_LOGI(TAG,
                     "*** Human/FDD/keyboard/mouse/JoyPAD/composite baseline stable; R56k CPU0 quiet logging active ***");
#endif
            /* R56k: CPU0 realtime workers must not discover/allocate newlib
             * stdio locks under sustained load.  Keep warnings/errors, but
             * move steady health reporting to the CPU1 600-frame sampler. */
            esp_log_level_set("TAB5_SCREEN", ESP_LOG_WARN);
            esp_log_level_set("TAB5_COMPOSE", ESP_LOG_WARN);
            esp_log_level_set("TAB5_VIDEO", ESP_LOG_WARN);
            esp_log_level_set("TAB5_AUDIO", ESP_LOG_WARN);
        }

        /* Build 5.63 deadline-aware IDLE/WDT service.  A soft opportunity is
         * used only when the audio producer ring has >= 6144 frames reserve
         * (or audio has not armed yet).  If that opportunity keeps being
         * deferred, the hard deadline wins so CPU1 IDLE still gets runtime. */
        {
            const int64_t now_us = esp_timer_get_time();
            const int64_t idle_age_us = now_us - idle_relief_prev_us;
            bool audio_safe_for_idle = !audio_host_ready;
            if (audio_host_ready)
            {
                tab5_audio_stats_t idle_audio = {0};
                tab5_audio_get_stats(&idle_audio);
                const uint32_t idle_effective_q = idle_audio.queued_frames + idle_audio.speaker_queued_frames;
                audio_safe_for_idle = (idle_audio.submitted_frames < 4096u) ||
                                      (idle_effective_q >= 6144u);
            }

            const bool hard_due = idle_age_us >= idle_relief_hard_us;
            const bool soft_due = idle_age_us >= idle_relief_soft_us;
            if (hard_due || (soft_due && audio_safe_for_idle))
            {
#if PX68K_TAB5_PERF_PROFILE
                int64_t perf_yield_start = perf_sample ? now_us : 0;
#endif
                if (hard_due && !audio_safe_for_idle)
                    ++budget.idle_relief_forced;
                vTaskDelay(1);
                const int64_t after_idle_us = esp_timer_get_time();
                const uint32_t idle_ms = (uint32_t)((after_idle_us - now_us + 999LL) / 1000LL);
                idle_relief_prev_us = after_idle_us;
                ++budget.idle_relief_events;
                budget.idle_relief_ms += idle_ms;
#if PX68K_TAB5_PERF_PROFILE
                if (perf_sample)
                    perf_yield_us = (uint32_t)(after_idle_us - perf_yield_start);
#endif
            }
            else if (soft_due && !audio_safe_for_idle)
            {
                ++budget.idle_relief_deferred;
            }
        }

#if PX68K_TAB5_DYNAREC_PROD_BENCH
        /* c13 clean wall benchmark: two esp_timer reads per 600-frame window,
         * no per-frame CPU/opcode/render instrumentation.  Use the same f=300,
         * 900, 1500... cadence as the research PERF logs.  Reset the window
         * after printing so UART output is excluded from the next average. */
        if ((frame % 600u) == 300u)
        {
            const int64_t dynprod_now_us = esp_timer_get_time();
            uint32_t avg_wall_us = 0u, speed_pct = 0u, fps_x10 = 0u;
            const uint32_t target_us = (CRTC_Regs[0x29] & 0x10) ? 18031u : 16271u;
            if (dynprod_prev_end_us != 0 && frame > dynprod_prev_frame)
            {
                const int64_t dt = dynprod_now_us - dynprod_prev_end_us;
                if (dt > 0)
                {
                    avg_wall_us = (uint32_t)(dt / (int64_t)(frame - dynprod_prev_frame));
                    if (avg_wall_us)
                    {
                        speed_pct = (target_us * 100u) / avg_wall_us;
                        fps_x10 = 10000000u / avg_wall_us;
                    }
                }
            }
            unsigned int cmphi_calls = 0u, cmphi_max = 0u;
            unsigned long long cmphi_loops = 0u;
            unsigned int mdx_calls = 0u, mdx_max = 0u;
            unsigned long long mdx_outer = 0u, mdx_insn = 0u;
            unsigned int zr_calls = 0u, zr_max = 0u;
            unsigned long long zr_loops = 0u;
            m68k_tab5_cmphi617_stats(&cmphi_calls, &cmphi_loops, &cmphi_max);
            m68k_tab5_mdx619_stats(&mdx_calls, &mdx_outer, &mdx_insn, &mdx_max);
            m68k_tab5_mdx622_stats(&zr_calls, &zr_loops, &zr_max);
#if PX68K_TAB5_R43_QUIET_RUNTIME
            /* PX68K_R56P: QUIET exact-executor counters visible; fact-only.
             * The stats calls above already execute in R43 quiet mode;
             * R56p only exposes their already-collected cumulative values. */
            ESP_LOGI(TAG,
                     "CPU613R43 QUIET f=%lu avg=%luus/f fps=%lu.%lu speed=%lu%% "
                     "MDXQ{CMPHI=%u/%llu/max%u MDX52=%u/%llu/%llu/max%u ZRUN=%u/%llu/max%u}",
                     (unsigned long)frame, (unsigned long)avg_wall_us,
                     (unsigned long)(fps_x10/10u), (unsigned long)(fps_x10%10u),
                     (unsigned long)speed_pct,
                     cmphi_calls, cmphi_loops, cmphi_max,
                     mdx_calls, mdx_outer, mdx_insn, mdx_max,
                     zr_calls, zr_loops, zr_max);
#else
            ESP_LOGI(TAG,
                     "CPU613C14R PROD f=%lu avg=%luus/f fps=%lu.%lu speed=%lu%% JIT=%s profiler=OFF costprobe=OFF CMPHI=%u/%llu/max%u MDX52=%u/%llu/%llu/max%u ZRUN=%u/%llu/max%u",
                     (unsigned long)frame,(unsigned long)avg_wall_us,
                     (unsigned long)(fps_x10/10u),(unsigned long)(fps_x10%10u),
                     (unsigned long)speed_pct, PX68K_TAB5_DYNAREC ? "ON" : "OFF",
                     cmphi_calls, cmphi_loops, cmphi_max,
                     mdx_calls, mdx_outer, mdx_insn, mdx_max,
                     zr_calls, zr_loops, zr_max);
#endif
            /* BAT162P: one-shot RAM code capture of the expensive BE01 call target.
             * No dispatch hook is installed; this runs only at the normal log boundary.
             * UART time is excluded when dynprod_prev_end_us is reset below. */
            if (frame == 900u) {
                unsigned short w[64];
                unsigned int row, col;
                m68k_tab5_be01_target_snapshot(w, 64u);
                ESP_LOGI(TAG, "BAT162P_TARGET $19D7F0-$19D86F exact RAM words follow (128B)");
                for (row = 0u; row < 8u; ++row) {
                    char line[160];
                    int n = snprintf(line, sizeof(line), "BE01T %06lX:",
                                     (unsigned long)(0x0019d7f0u + row * 16u));
                    for (col = 0u; col < 8u && n > 0 && n < (int)sizeof(line); ++col)
                        n += snprintf(line + n, sizeof(line) - (size_t)n, " %04X",
                                      (unsigned)w[row * 8u + col]);
                    ESP_LOGI(TAG, "%s", line);
                }
            }

            /* c13: keep the targeted f=2100 dump so the SFXVI $368760 block
             * is captured before its later observed signature invalidation.
             * The timestamp is reset after the dump, so UART time is excluded
             * from the next wall window. */
            if ((!PX68K_TAB5_R43_QUIET_RUNTIME) &&
                (frame == 2100u || (frame >= 1500u && ((frame - 300u) % 1200u) == 0u))) {
                tab5_compose_stats_t rs = {0};
                tab5_video_async_stats_t vs = {0};
#if PX68K_TAB5_DYNAREC
                m68k_tab5_dynarec_dump();
#endif
                tab5_compose_get_stats(&rs);
                tab5_video_get_async_stats(&vs);
                {
                    uint32_t usb_lib_hw = 0u, usb_ctl_hw = 0u;
                    tab5_usb_keyboard_stack_highwater(&usb_lib_hw, &usb_ctl_hw);
                    ESP_LOGI(TAG,
                             "STACKR26 f=%lu min-free-bytes guest=%u/16384 fm=%u/6144 audio=%u/4096 compose=%u/4096 lcd=%u/8192 usb-lib=%u/4096 usb-ctl=%u/4096 | ADPCM-SRAM high=%u/%u overflow=%u FM-internal=%u FM-SPM=%u ADPCM-SPM=%u PAL-SPM=%u",
                             (unsigned long)frame,
                             s_guest_task ? (unsigned)uxTaskGetStackHighWaterMark(s_guest_task) : 0u,
                             (unsigned)OPM_AsyncStackHighWater(),
                             (unsigned)tab5_audio_stack_highwater(),
                             (unsigned)tab5_compose_stack_highwater(),
                             (unsigned)tab5_video_stack_highwater(),
                             (unsigned)usb_lib_hw, (unsigned)usb_ctl_hw,
                             (unsigned)ADPCM_Tab5BufferHighWater(),
                             (unsigned)ADPCM_Tab5BufferCapacity(),
                             (unsigned)ADPCM_Tab5BufferOverflows(),
                             (unsigned)OPM_AsyncInternalOnly(),
                             (unsigned)OPM_AsyncSpmControlOk(),
                             (unsigned)ADPCM_Tab5SpmStateOk(),
                             (unsigned)tab5_px68k_spm_palette_ok());
                    {
                        uint32_t dma3_hits = 0u, dma3_fallbacks = 0u;
                        uint32_t mfp_bc = 0u, mfp_fb = 0u;
                        DMA_Tab5ADPCMFastStats(&dma3_hits, &dma3_fallbacks);
                        MFP_Tab5TimerFastStats(&mfp_bc, &mfp_fb);
                        ESP_LOGI(TAG,
                                 "DEVHOTR26 f=%lu DMA3EXACT=%lu/%lu MFPBC=%lu/%lu",
                                 (unsigned long)frame,
                                 (unsigned long)dma3_hits, (unsigned long)dma3_fallbacks,
                                 (unsigned long)mfp_bc, (unsigned long)mfp_fb);
                    }
                }
                ESP_LOGI(TAG,
                         "RENDER615 f=%lu gbt=%lu/%lu cache=%lu/%lu hit=%lu miss=%lu rebuild=%lu build=%luus cache-render=%luus raw=%lu/%lu gdmaL=%lu fail=%lu barrier=%lu/%lu/%luus qfull=%lu pmax=%lu pace=%lu/%luus last=%luus coal=%lu skip=%lu int=%luus vq=%lu vdrop=%lu",
                         (unsigned long)frame,
                         (unsigned long)rs.gbt_submitted_lines,
                         (unsigned long)rs.gbt_completed_lines,
                         (unsigned long)rs.scroll_cache_submitted_lines,
                         (unsigned long)rs.scroll_cache_completed_lines,
                         (unsigned long)rs.scroll_cache_hits,
                         (unsigned long)rs.scroll_cache_misses,
                         (unsigned long)rs.scroll_cache_rebuilds,
                         (unsigned long)rs.last_scroll_cache_build_us,
                         (unsigned long)rs.last_scroll_cache_render_us,
                         (unsigned long)rs.gbt_raw_submitted_lines,
                         (unsigned long)rs.gbt_raw_completed_lines,
                         (unsigned long)rs.gbt_raw_dma_lines,
                         (unsigned long)rs.gbt_raw_dma_submit_fail,
                         (unsigned long)rs.gvram_barrier_calls,
                         (unsigned long)rs.gvram_barrier_waits,
                         (unsigned long)rs.gvram_barrier_us,
                         (unsigned long)rs.queue_full,
                         (unsigned long)rs.max_pending,
                         (unsigned long)vs.pace_waits,
                         (unsigned long)vs.pace_wait_us,
                         (unsigned long)vs.pace_last_wait_us,
                         (unsigned long)vs.pace_coalesced_frames,
                         (unsigned long)vs.pace_skipped_slots,
                         (unsigned long)vs.pace_last_interval_us,
                         (unsigned long)vs.queued_frames,
                         (unsigned long)vs.dropped_frames);
            }

            dynprod_prev_end_us = esp_timer_get_time();
            dynprod_prev_frame = frame;
        }
#endif

        /* R57E66: no frame-end wait.  Coarse pacing/catch-up accounting is
         * performed inside the guest frame without touching device semantics. */


#if PX68K_TAB5_PERF_PROFILE
        if (perf_sample)
        {
            uint32_t core_us = 0, cpu_us = 0, compose_us = 0, finalize_us = 0;
            uint32_t timer_us = 0, dma_us = 0, line_us = 0;
            uint32_t audio_timer_us = 0, input_us = 0, soundmix_us = 0, fdd_us = 0;
            uint32_t mfp_us = 0, rtc_us = 0, edge_us = 0, sched_us = 0;
            uint32_t adclk_us = 0, opmclk_us = 0, midi_us = 0, post_us = 0;
            uint32_t adpcm_us = 0, opm_us = 0;
            uint32_t mix_calls = 0, mix_frames = 0;
            uint32_t fm_qdepth = 0, fm_event_drops = 0, fm_ring_overruns = 0, fm_avail = 0;
            uint32_t vg_grp_us = 0, vg_text_us = 0, vg_bg_us = 0, vg_blend_us = 0, vg_clear_us = 0;
            uint32_t vg_dirty = 0, vg_grp_calls = 0, vg_text_calls = 0, vg_bg_calls = 0, vg_blend_calls = 0;
            tab5_audio_stats_t audio_stats = {0};
            tab5_video_async_stats_t video_stats = {0};
            tab5_compose_stats_t compose_stats = {0};
            WinX68k_PerfGetLast(&core_us, &cpu_us, &compose_us, &finalize_us);
            WinX68k_PerfGetDetail(&timer_us, &dma_us, &line_us,
                                  &audio_timer_us, &input_us, &soundmix_us, &fdd_us);
            WinX68k_PerfGetDetail543(&mfp_us, &rtc_us, &edge_us, &sched_us,
                                     &adclk_us, &opmclk_us, &midi_us, &post_us);
            WinX68k_AudioPerfGetLast(&adpcm_us, &opm_us, &mix_calls, &mix_frames);
            WinX68k_AudioAsyncGetStats(&fm_qdepth, &fm_event_drops, &fm_ring_overruns, &fm_avail);
            WinX68k_VideoPerfGetLast(&vg_grp_us, &vg_text_us, &vg_bg_us, &vg_blend_us, &vg_clear_us,
                                     &vg_dirty, &vg_grp_calls, &vg_text_calls, &vg_bg_calls, &vg_blend_calls);
            tab5_audio_get_stats(&audio_stats);
            tab5_video_get_async_stats(&video_stats);
            tab5_compose_get_stats(&compose_stats);

            const uint32_t dev_us =
                (core_us > cpu_us + compose_us + finalize_us)
                    ? core_us - cpu_us - compose_us - finalize_us
                    : 0u;
            const uint32_t outer_us =
                (uint32_t)(esp_timer_get_time() - perf_outer_start);
            const uint32_t known_us = core_us + perf_text_us + perf_lcd_us + perf_yield_us;
            const uint32_t misc_us = (outer_us > known_us) ? outer_us - known_us : 0u;
            const uint32_t present_div_perf = present_div;
            const uint32_t text_div_perf = (textview_active && composite_view) ? 60u : present_div_perf;
            const uint32_t text_avg_us = perf_text_us / text_div_perf;
            const uint32_t lcd_avg_us = perf_lcd_us / present_div_perf;
            /* 5.63 idle relief is wall-clock periodic, not every 4 frames.
             * Do not smear a rare one-tick housekeeping window over every frame. */
            const uint32_t yield_avg_us = 0u;
            const uint32_t target_us =
                (CRTC_Regs[0x29] & 0x10) ? 18031u : 16271u;
            const uint32_t dev_detail_known_us =
                mfp_us + rtc_us + dma_us + edge_us + sched_us + line_us +
                adclk_us + opmclk_us + midi_us + input_us + soundmix_us +
                fdd_us + post_us;
            const uint32_t dev_other_us =
                (dev_us > dev_detail_known_us) ? (dev_us - dev_detail_known_us) : 0u;

            uint32_t avg_wall_us = 0;
            uint32_t speed_pct = 0;
            uint32_t fps_x10 = 0;
            if (perf_prev_end_us != 0 && frame > perf_prev_frame)
            {
                const int64_t wall_delta64 = esp_timer_get_time() - perf_prev_end_us;
                const uint32_t wall_delta_us =
                    (wall_delta64 > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)wall_delta64;
                avg_wall_us = wall_delta_us / (uint32_t)(frame - perf_prev_frame);
                if (avg_wall_us)
                {
                    speed_pct = (target_us * 100u) / avg_wall_us;
                    fps_x10 = 10000000u / avg_wall_us;
                }
            }

            /* Build 5.63: measure audio producer/accept/play rates over wall
             * time.  This is diagnostic-only (once per PERF window), so the
             * 64-bit multiply/divide cannot affect the hot emulation path. */
            uint32_t audio_gen_hz = 0, audio_sub_hz = 0, audio_play_hz = 0;
            uint32_t audio_gen_pct = 0, audio_under_delta = 0;
            {
                const int64_t audio_now_us = esp_timer_get_time();
                const int64_t audio_dt64 = audio_now_us - audio_rate_prev_us;
                if (audio_dt64 > 0)
                {
                    const uint32_t gen_delta = audio_generated_total - audio_rate_prev_generated;
                    const uint32_t sub_delta = audio_stats.submitted_frames - audio_rate_prev_submitted;
                    const uint32_t play_delta = audio_stats.played_frames - audio_rate_prev_played;
                    audio_under_delta = audio_stats.underflow_events - audio_rate_prev_under;
                    audio_gen_hz = (uint32_t)(((uint64_t)gen_delta * 1000000ULL) / (uint64_t)audio_dt64);
                    audio_sub_hz = (uint32_t)(((uint64_t)sub_delta * 1000000ULL) / (uint64_t)audio_dt64);
                    audio_play_hz = (uint32_t)(((uint64_t)play_delta * 1000000ULL) / (uint64_t)audio_dt64);
                    audio_gen_pct = (audio_gen_hz * 100u) / 44100u;
                }
                audio_rate_prev_us = audio_now_us;
                audio_rate_prev_generated = audio_generated_total;
                audio_rate_prev_submitted = audio_stats.submitted_frames;
                audio_rate_prev_played = audio_stats.played_frames;
                audio_rate_prev_under = audio_stats.underflow_events;
            }

            ESP_LOGI(TAG,
                     "PERF f=%lu media=%s view=%s avg=%luus/f fps=%lu.%lu speed=%lu%% "
                     "sample: core=%luus cpu=%lu compose=%lu dev=%lu final=%lu "
                     "host:text=%lu lcd=%lu misc=%lu yield=%lu "
                     "amort:text=%lu lcd=%lu yield=%lu "
                     "dev:timer=%lu dma=%lu line=%lu audclk=%lu input=%lu mix=%lu fdd=%lu "
                     "dev543:mfp=%lu rtc=%lu edge=%lu sched=%lu adclk=%lu opmclk=%lu midi=%lu post=%lu other=%lu "
                     "gfx:grp=%lu text=%lu bgsp=%lu blend=%lu clear=%lu dirty=%lu gc=%lu tc=%lu bc=%lu xc=%lu "
                     "v1:copy=%lu push=%lu queued=%lu submitted=%lu presented=%lu drop=%lu live=%lu retry=%lu unstable=%lu "
                     "hostcmp:copy=%lu blend=%lu rawcopy=%lu rawrender=%lu sub=%lu done=%lu rawsub=%lu rawdone=%lu fallback=%lu qfull=%lu waits=%lu max=%lu "
                     "mb:tcm=%lu bytes=%lu slots=%lu slotb=%lu pool=%lu notify=%lu rempty=%lu fempty=%lu "
                     "a553:arena=%lu used=%lu spare=%lu bgsub=%lu bgdone=%lu bgrender=%lu bar=%lu/%lu barus=%lu "
                     "audio:opm=%lu adpcm=%lu calls=%lu frames=%lu fmq=%lu fmdrop=%lu fmover=%lu fmavail=%lu "
                     "q=%lu spq=%lu drop=%lu play=%lu under=%lu fail=%lu qmin=%lu qmax=%lu prime=%lu/%lu low=%lu hold=%lu qempty=%lu pbi=%lu fullwait=%lu "
                     "arate=%lu prod=%lu q22=%lu rc=%lu budget=%s qpre=%lu/%lu N/G/C=%lu/%lu/%lu trans=%lu rskip=%lu pskip=%lu tskip=%lu pace=%lu/%lums "
                     "minlcd=%lu idle=%lu/%lums force=%lu defer=%lu audiohz:g/s/p=%lu/%lu/%lu gen=%lu%% u+%lu",
                     (unsigned long)frame,
                     a_boot_path,
                     (textview_active && !composite_view) ? "TEXT" : "COMPOSITE",
                     (unsigned long)avg_wall_us,
                     (unsigned long)(fps_x10 / 10u),
                     (unsigned long)(fps_x10 % 10u),
                     (unsigned long)speed_pct,
                     (unsigned long)core_us,
                     (unsigned long)cpu_us,
                     (unsigned long)compose_us,
                     (unsigned long)dev_us,
                     (unsigned long)finalize_us,
                     (unsigned long)perf_text_us,
                     (unsigned long)perf_lcd_us,
                     (unsigned long)misc_us,
                     (unsigned long)perf_yield_us,
                     (unsigned long)text_avg_us,
                     (unsigned long)lcd_avg_us,
                     (unsigned long)yield_avg_us,
                     (unsigned long)timer_us,
                     (unsigned long)dma_us,
                     (unsigned long)line_us,
                     (unsigned long)audio_timer_us,
                     (unsigned long)input_us,
                     (unsigned long)soundmix_us,
                     (unsigned long)fdd_us,
                     (unsigned long)mfp_us,
                     (unsigned long)rtc_us,
                     (unsigned long)edge_us,
                     (unsigned long)sched_us,
                     (unsigned long)adclk_us,
                     (unsigned long)opmclk_us,
                     (unsigned long)midi_us,
                     (unsigned long)post_us,
                     (unsigned long)dev_other_us,
                     (unsigned long)vg_grp_us,
                     (unsigned long)vg_text_us,
                     (unsigned long)vg_bg_us,
                     (unsigned long)vg_blend_us,
                     (unsigned long)vg_clear_us,
                     (unsigned long)vg_dirty,
                     (unsigned long)vg_grp_calls,
                     (unsigned long)vg_text_calls,
                     (unsigned long)vg_bg_calls,
                     (unsigned long)vg_blend_calls,
                     (unsigned long)video_stats.last_copy_us,
                     (unsigned long)video_stats.last_push_us,
                     (unsigned long)video_stats.queued_frames,
                     (unsigned long)video_stats.submitted_frames,
                     (unsigned long)video_stats.presented_frames,
                     (unsigned long)video_stats.dropped_frames,
                     (unsigned long)video_stats.live_presented_frames,
                     (unsigned long)video_stats.live_row_retries,
                     (unsigned long)video_stats.live_unstable_rows,
                     (unsigned long)compose_stats.last_copy_us,
                     (unsigned long)compose_stats.last_blend_us,
                     (unsigned long)compose_stats.last_grp8_copy_us,
                     (unsigned long)compose_stats.last_grp8_render_us,
                     (unsigned long)compose_stats.submitted_lines,
                     (unsigned long)compose_stats.completed_lines,
                     (unsigned long)compose_stats.grp8_submitted_lines,
                     (unsigned long)compose_stats.grp8_completed_lines,
                     (unsigned long)compose_stats.fallback_lines,
                     (unsigned long)compose_stats.queue_full,
                     (unsigned long)compose_stats.frame_waits,
                     (unsigned long)compose_stats.max_pending,
                     (unsigned long)compose_stats.mailbox_tcm,
                     (unsigned long)compose_stats.mailbox_bytes,
                     (unsigned long)compose_stats.slot_count,
                     (unsigned long)compose_stats.slot_bytes,
                     (unsigned long)compose_stats.pool_bytes,
                     (unsigned long)compose_stats.notify_count,
                     (unsigned long)compose_stats.ready_empty,
                     (unsigned long)compose_stats.free_empty,
                     (unsigned long)compose_stats.arena_bytes,
                     (unsigned long)compose_stats.arena_used,
                     (unsigned long)compose_stats.arena_spare,
                     (unsigned long)compose_stats.bgsp_submitted_lines,
                     (unsigned long)compose_stats.bgsp_completed_lines,
                     (unsigned long)compose_stats.last_bgsp_render_us,
                     (unsigned long)compose_stats.bg_barrier_calls,
                     (unsigned long)compose_stats.bg_barrier_waits,
                     (unsigned long)compose_stats.bg_barrier_us,
                     (unsigned long)opm_us,
                     (unsigned long)adpcm_us,
                     (unsigned long)mix_calls,
                     (unsigned long)mix_frames,
                     (unsigned long)fm_qdepth,
                     (unsigned long)fm_event_drops,
                     (unsigned long)fm_ring_overruns,
                     (unsigned long)fm_avail,
                     (unsigned long)audio_stats.queued_frames,
                     (unsigned long)audio_stats.speaker_queued_frames,
                     (unsigned long)audio_stats.dropped_frames,
                     (unsigned long)audio_stats.played_frames,
                     (unsigned long)audio_stats.underflow_events,
                     (unsigned long)audio_stats.play_failures,
                     (unsigned long)audio_stats.min_queued_frames,
                     (unsigned long)audio_stats.max_queued_frames,
                     (unsigned long)audio_stats.prebuffer_resumes,
                     (unsigned long)audio_stats.prebuffer_waits,
                     (unsigned long)audio_stats.low_water_hits,
                     (unsigned long)audio_stats.partial_holds,
                     (unsigned long)audio_stats.queue_empty_events,
                     (unsigned long)audio_stats.play_buffers_internal,
                     (unsigned long)audio_stats.speaker_full_waits,
                     (unsigned long)audio_stats.speaker_rate_hz,
                     (unsigned long)audio_stats.producer_rate_hz,
                     (unsigned long)audio_stats.rate_servo_active,
                     (unsigned long)audio_stats.rate_changes,
                     tab5_budget_mode_name(budget.mode),
                     (unsigned long)budget.preexec_q,
                     (unsigned long)budget.preexec_q_effective,
                     (unsigned long)budget.normal_frames,
                     (unsigned long)budget.guard_frames,
                     (unsigned long)budget.critical_frames,
                     (unsigned long)budget.transitions,
                     (unsigned long)budget.render_skips,
                     (unsigned long)budget.present_skips,
                     (unsigned long)budget.text_skips,
                     (unsigned long)budget.high_pace_events,
                     (unsigned long)budget.high_pace_ms,
                     (unsigned long)budget.critical_forced_renders,
                     (unsigned long)budget.idle_relief_events,
                     (unsigned long)budget.idle_relief_ms,
                     (unsigned long)budget.idle_relief_forced,
                     (unsigned long)budget.idle_relief_deferred,
                     (unsigned long)audio_gen_hz,
                     (unsigned long)audio_sub_hz,
                     (unsigned long)audio_play_hz,
                     (unsigned long)audio_gen_pct,
                     (unsigned long)audio_under_delta);

            {
                const uint32_t m68k_share_x10 = core_us ? (cpu_us * 1000u) / core_us : 0u;
                const uint32_t dev_share_x10 = core_us ? (dev_us * 1000u) / core_us : 0u;
                const uint32_t render_us = compose_us + finalize_us;
                const uint32_t render_share_x10 = core_us ? (render_us * 1000u) / core_us : 0u;
                const size_t bench_exec_free = heap_caps_get_free_size(MALLOC_CAP_EXEC);
                const size_t bench_exec_largest = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
                const size_t bench_l2_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
                const size_t bench_l2_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
                const size_t bench_ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                ESP_LOGI(TAG,
                         "CPU613C NATIVEJIT f=%lu wall=%luus fps=%lu.%lu speed=%lu%% | CPU1sample core=%luus m68k=%luus(%lu.%lu%%) dev=%luus(%lu.%lu%%) render=%luus(%lu.%lu%%) adpcmflush=%luus | MEM exec_free=%u largest=%u l2dma_free=%u largest=%u psram_free=%u | ACCEL dispatch=%lu+%luB hotmem=%luB profiler=%luB dynarena=%luB dyntables=%luB dynexec=%d dynprobe=%d",
                         (unsigned long)frame,
                         (unsigned long)avg_wall_us,
                         (unsigned long)(fps_x10 / 10u),
                         (unsigned long)(fps_x10 % 10u),
                         (unsigned long)speed_pct,
                         (unsigned long)core_us,
                         (unsigned long)cpu_us,
                         (unsigned long)(m68k_share_x10 / 10u),
                         (unsigned long)(m68k_share_x10 % 10u),
                         (unsigned long)dev_us,
                         (unsigned long)(dev_share_x10 / 10u),
                         (unsigned long)(dev_share_x10 % 10u),
                         (unsigned long)render_us,
                         (unsigned long)(render_share_x10 / 10u),
                         (unsigned long)(render_share_x10 % 10u),
                         (unsigned long)soundmix_us,
                         (unsigned)bench_exec_free,
                         (unsigned)bench_exec_largest,
                         (unsigned)bench_l2_free,
                         (unsigned)bench_l2_largest,
                         (unsigned)bench_ps_free,
                         (unsigned long)m68k_tab5_dispatch_tcm_bytes(),
                         (unsigned long)m68k_tab5_dispatch_l2_bytes(),
                         (unsigned long)tab5_px68k_hotmem_bytes(),
                         (unsigned long)m68k_tab5_profile_storage_bytes(),
                         (unsigned long)tab5_dynarec_arena_bytes(),
                         (unsigned long)m68k_tab5_dynarec_metadata_bytes(),
                         tab5_dynarec_arena_is_executable(),
                         tab5_dynarec_arena_probe_ok());
            }

            ESP_LOGI(TAG,
                     "P4BLEND3 f=%lu host-common calls C/PIE=%lu/%lu pixels=%lu/%lu alignfb=%lu fail=%lu backend[b0/b1/b2]=%lu/%lu/%lu",
                     (unsigned long)frame,
                     (unsigned long)compose_stats.p4_blend_scalar_calls,
                     (unsigned long)compose_stats.p4_blend_pie_calls,
                     (unsigned long)compose_stats.p4_blend_scalar_pixels,
                     (unsigned long)compose_stats.p4_blend_pie_pixels,
                     (unsigned long)compose_stats.p4_blend_align_fallbacks,
                     (unsigned long)compose_stats.p4_blend_failures,
                     (unsigned long)compose_stats.p4_blend_backend0,
                     (unsigned long)compose_stats.p4_blend_backend1,
                     (unsigned long)compose_stats.p4_blend_backend2);


            perf_prev_end_us = esp_timer_get_time();
            perf_prev_frame = frame;
        }
#endif
    }

#undef TAB5_REARM_GUEST_OBSERVERS
#undef panic_mode
#undef xdf_path
#undef human_path
#undef b_xdf_path
#undef diskmag_path
#undef a_boot_path
#undef hds_path
#undef audio_host_ready
#undef usb_host_ready
}
