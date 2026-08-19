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
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "libretro.h"
#include "libretro/state.h"
#include "m68000/m68000.h"
#include "m68000/musashi/m68k.h"
#include "libretro/keyboard.h"
#include "x68k/mfp.h"
#include "x68k/crtc.h"
#include "x68k/gvram.h"
#include "x68k/palette.h"
#include "x68k/scc.h"
#include "x68k/hostfs.h"

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
#include "tab5_dynarec_arena.h"

static const char *TAG = "PX68K_TAB5";
extern uint32_t tab5_px68k_hotmem_bytes(void);
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
extern void WinX68k_SetHostRenderEnabled(int enabled);
extern void WinX68k_PerfSetSample(int enabled);
extern void m68k_tab5_dynarec_dump(void);
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
extern uint32_t WinX68k_AudioProducedFrames(void);
extern void WinX68k_AudioPerfGetLast(uint32_t *adpcm_us, uint32_t *opm_us, uint32_t *mix_calls, uint32_t *mix_frames);
extern void WinX68k_AudioAsyncGetStats(uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns, uint32_t *fm_avail);
extern uint32_t OPM_DebugDataWriteCount(void);
extern uint32_t OPM_DebugKeyOnCount(void);
extern uint32_t ADPCM_DebugControlWriteCount(void);
extern uint32_t ADPCM_DebugDataWriteCount(void);
extern int ADPCM_DebugPlaying(void);

extern const uint16_t *WinX68k_GetVideoBuffer(void);
extern uint32_t WinX68k_GetVideoWidth(void);
extern uint32_t WinX68k_GetVideoHeight(void);
extern uint32_t WinX68k_GetVideoPitchPixels(void);

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

#if PX68K_TAB5_DIAG_VERBOSE
static uint32_t frame_nonzero(const uint16_t *fb,
                              uint32_t w,
                              uint32_t h,
                              uint32_t pitch)
{
    uint32_t count = 0;

    if (!fb)
        return 0;

    for (uint32_t y = 0; y < h; ++y)
    {
        const uint16_t *row = fb + y * pitch;

        for (uint32_t x = 0; x < w; ++x)
        {
            if (row[x])
                ++count;
        }
    }

    return count;
}

static uint32_t frame_hash(const uint16_t *fb,
                           uint32_t w,
                           uint32_t h,
                           uint32_t pitch)
{
    uint32_t hash = 2166136261u;

    if (!fb)
        return 0;

    for (uint32_t y = 0; y < h; ++y)
    {
        const uint16_t *row = fb + y * pitch;

        for (uint32_t x = 0; x < w; ++x)
        {
            uint16_t p = row[x];

            hash ^= (uint8_t)(p & 0xFF);
            hash *= 16777619u;

            hash ^= (uint8_t)(p >> 8);
            hash *= 16777619u;
        }
    }

    return hash;
}

#endif

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

void app_main(void)
{
    px68k_run_context_t *ctx = &s_run_ctx;
    memset(ctx, 0, sizeof(*ctx));

/* Intent: Core ownership rule: CPU1 advances the X68000 timeline; CPU0 handles host-only services so host work cannot stall or duplicate guest execution.  Layer8 Aug/17/2026 */
    /* Build 5.47: keep peripheral/host initialization on CPU0 so peripheral
     * driver work stays with the ESP-IDF/system side.  Only after all host
     * services exist do we launch the X68000 time-axis as a dedicated CPU1
     * task. */
    tab5_video_init();
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

    tab5_log_memory_550("host-preworkers");
    const int tab5_dyn_probe_ok = tab5_dynarec_arena_probe();
    m68k_tab5_dynarec_bind(tab5_dyn_probe_ok ? tab5_dynarec_arena_base() : NULL,
                            tab5_dyn_probe_ok ? (unsigned int)tab5_dynarec_arena_bytes() : 0u,
                            tab5_dyn_probe_ok ? tab5_dynarec_arena_sync : NULL);
    tab5_log_memory_550("dynarena-static32k");
    ctx->audio_host_ready = tab5_audio_init() != 0;
    /* Build 5.53a: after M5 speaker DMA is allocated, reserve one contiguous
     * Internal L2/DMA arena before text/USB/task allocations fragment it. */
    (void)tab5_compose_reserve_arena();
    ctx->compose_host_ready = tab5_compose_init() != 0;
    ESP_LOGI(TAG, "CPU0 host compositor: %s",
             ctx->compose_host_ready ? "READY" : "UNAVAILABLE - guest CPU fallback");

    if (!tab5_textview_init())
        ESP_LOGW(TAG, "Core text view unavailable; composite view only");

    tab5_guest_input_init();
    tab5_guest_input_set_interval_frames(12u);

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
                                            16384, ctx, 1, NULL, 1);
#else
    BaseType_t ok = xTaskCreate(px68k_emulation_task, "px68k_guest",
                                16384, ctx, 1, NULL);
#endif
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Unable to create CPU1 X68000 guest task");
        halt_forever();
    }

    ESP_LOGI(TAG, "Build 6.15 Production host init complete on CPU0; validated scroll-cache + CRTC-paced LCD presenter");
    vTaskDelete(NULL);
}

static void px68k_emulation_task(void *arg)
{
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

    ESP_LOGI(TAG, "Build 6.15 Production guest task started: X68000 time-axis pinned to CPU1; host presentation remains decoupled");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, " X68K Tab - Build 6.15 Production");
    ESP_LOGI(TAG, " P4 video hot working-set in internal SRAM: %u bytes", (unsigned)tab5_px68k_hotmem_bytes());
    ESP_LOGI(TAG, " Build 5.91 baseline + Flash Human68k Quick Boot + HDS/SCSI 5.94c");
    ESP_LOGI(TAG, "=======================================");

    ESP_LOGI(TAG, "Human68k Quick Boot: %s (project-root human302.xdf in dedicated flash partition)", TAB5_FLASH_HUMAN_PATH);
    if (human_path[0])
        ESP_LOGI(TAG, "SD Human302 candidate (not auto-inserted): %s", human_path);
    else
        ESP_LOGI(TAG, "SD Human302 candidate: <none>; Flash Human68k remains bootable");
    ESP_LOGI(TAG, "Build 6.15 Production: persistent GRP8 scroll cache + CRTC-paced latest-live LCD presentation");
    ESP_LOGI(TAG, "Root XDF catalog: %u image(s)",
             (unsigned)tab5_sd_xdf_count());
    ESP_LOGI(TAG, "Root boot-media catalog: %u image(s) (.XDF/.DIM)",
             (unsigned)tab5_sd_floppy_count());

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
             "WinDraw video initialized BEFORE reset: ptr=%p size=%lux%lu pitch=%lu",
             WinX68k_GetVideoBuffer(),
             (unsigned long)WinX68k_GetVideoWidth(),
             (unsigned long)WinX68k_GetVideoHeight(),
             (unsigned long)WinX68k_GetVideoPitchPixels());

    ESP_LOGI(TAG, "WinX68k_Reset BEGIN");
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
    uint32_t panic_touch_enable_frame = 0;
    uint32_t panic_command_frame = 0;
    uint32_t panic_command_gfx_base = 0;
    uint32_t panic_command_pal_base = 0;
    uint32_t panic_command_bat_open_base = 0;
    uint32_t panic_command_player_open_base = 0;
    uint32_t panic_command_pan_open_base = 0;
    uint32_t panic_opm_write_base = 0;
    uint32_t panic_opm_keyon_base = 0;
    uint32_t panic_adpcm_ctl_base = 0;
    uint32_t panic_adpcm_data_base = 0;
    uint32_t panic_audio_produced_base = 0;
    bool panic_audio_diag_5s = false;
    bool panic_audio_diag_15s = false;
    bool panic_command_sent = false;
    bool panic_direct_fallback_sent = false;
    /* Build 5.63: the real emulator output is the default.  COLOR TEXT is now
     * an F9 diagnostic view rather than a boot-time mode the user must leave. */
    bool composite_view = true;
#if PX68K_TAB5_DIAG_VERBOSE
    uint32_t measured_nz = 0;
    uint32_t measured_hash = 0;
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

#define TAB5_REARM_GUEST_OBSERVERS() do { \
        ram_execution_seen = false; \
        textview_active = false; \
        composite_view = true; \
        panic_text_ready_frame = 0; \
        panic_touch_enable_frame = 0; \
        panic_command_frame = 0; \
        panic_command_gfx_base = 0; \
        panic_command_pal_base = 0; \
        panic_command_bat_open_base = 0; \
        panic_command_player_open_base = 0; \
        panic_command_pan_open_base = 0; \
        panic_opm_write_base = 0; \
        panic_opm_keyon_base = 0; \
        panic_adpcm_ctl_base = 0; \
        panic_adpcm_data_base = 0; \
        panic_audio_produced_base = 0; \
        panic_audio_diag_5s = false; \
        panic_audio_diag_15s = false; \
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
    ESP_LOGI(TAG, "P4 core split: CPU1 X68000 time-axis (68000/guest timing/ADPCM generation only); CPU0 host workers (ADPCM pull+FM final mix/speaker + compose/LCD/USB)");
    ESP_LOGI(TAG, "Tab5 audio host: %s", audio_host_ready ? "READY" : "UNAVAILABLE");

    int64_t perf_prev_end_us = 0;
    uint32_t perf_prev_frame = 0;
#if PX68K_TAB5_DYNAREC_PROD_BENCH
    int64_t dynprod_prev_end_us = 0;
    uint32_t dynprod_prev_frame = 0;
#endif
    tab5_budget_stats_t budget = {0};
    budget.mode = TAB5_BUDGET_NORMAL;

    /* Build 5.63 audio clock telemetry.  DSound produces exactly from guest
     * X68000 time; the speaker consumes in wall time.  Comparing these two
     * rates tells us directly whether a starvation is scheduling jitter or a
     * chronically slow guest time axis.  32-bit counters last > 24h at 44.1k. */
    uint32_t audio_generated_total = 0;
    uint32_t audio_rate_prev_generated = 0;
    uint32_t audio_rate_prev_submitted = 0;
    uint32_t audio_rate_prev_played = 0;
    uint32_t audio_rate_prev_under = 0;
    int64_t audio_rate_prev_us = esp_timer_get_time();

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

    for (uint32_t frame = 1; ; ++frame)
    {
        bool perf_sample = false;
        bool cpu_diag_sample = false;
        int64_t perf_outer_start = 0;
        uint32_t perf_text_us = 0;
        uint32_t perf_lcd_us = 0;
        uint32_t perf_yield_us = 0;
        tab5_audio_stats_t budget_audio = {0};
        if (audio_host_ready)
            tab5_audio_get_stats(&budget_audio);

        budget.preexec_q = budget_audio.queued_frames;
        budget.preexec_q_effective = budget_audio.queued_frames + budget_audio.speaker_queued_frames;
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

        bool budget_render = true;
        if (budget.mode == TAB5_BUDGET_AUDIO_CRITICAL)
        {
            ++budget.critical_frames;
            /* Build 5.63: CRITICAL protects audio, but never freezes the LCD.
             * Force one full host render every 6 guest frames.  Once the text
             * renderer is active this aligns with the normal composite present
             * cadence (~10 Hz at a 60-Hz guest), so each scheduled LCD update
             * has a fresh framebuffer. */
            budget_render = ((frame % 6u) == 0u);
            if (budget_render)
                ++budget.critical_forced_renders;
        }
        else if (budget.mode == TAB5_BUDGET_AUDIO_GUARD)
        {
            ++budget.guard_frames;
            /* Half-rate host rendering while preserving every guest frame. */
            budget_render = ((frame & 1u) == 0u);
        }
        else
        {
            ++budget.normal_frames;
        }

        if (!budget_render)
            ++budget.render_skips;

        /* High-water backpressure prevents the opposite failure mode seen in
         * 5.61: a burst fills all 32768 frames, then producer PCM is dropped. */
        if (audio_host_ready && budget_audio.submitted_frames >= 4096u)
        {
            uint32_t pace_ms = 0;
            if (budget_audio.queued_frames >= 28672u)
                pace_ms = 2u;
            else if (budget_audio.queued_frames >= 24576u)
                pace_ms = 1u;
            if (pace_ms)
            {
                ++budget.high_pace_events;
                budget.high_pace_ms += pace_ms;
                vTaskDelay(pdMS_TO_TICKS(pace_ms));
            }
        }

        WinX68k_SetHostRenderEnabled(budget_render ? 1 : 0);
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
        int cycles = WinX68k_ExecVideoProbeFrame();
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

        /* Build 5.98g12: CPU1 no longer extracts PCM and never performs the
         * final ADPCM+FM saturation mix.  DSound_FlushPending() above only
         * publishes guest-timed ADPCM plus the matching async-FM render work;
         * CPU0's audio worker pulls/mixes it.  CPU1 does one counter read and
         * one task notification per guest frame. */
        audio_generated_total = WinX68k_AudioProducedFrames();
        if (audio_host_ready)
            tab5_audio_kick();

        const uint16_t *fb = WinX68k_GetVideoBuffer();
        uint32_t w = WinX68k_GetVideoWidth();
        uint32_t h = WinX68k_GetVideoHeight();
        uint32_t pitch = WinX68k_GetVideoPitchPixels();
        uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

#if PX68K_TAB5_DIAG_VERBOSE
        /* Development-only HDD register snapshot. */
        if (hds_path[0] && (frame == 300u || frame == 600u))
        {
            ESP_LOGI(TAG,
                     "*** HDD0 BOOT SNAP f=%lu PC=$%08lX SR=$%04lX D0=%08lX D1=%08lX D2=%08lX D3=%08lX D4=%08lX D5=%08lX A1=%08lX IOCS=%lu READ=%lu init=%lu installer=%lu installs=%lu parts=%lu vec=$%08lX ***",
                     (unsigned long)frame,
                     (unsigned long)pc,
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_SR),
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_D0),
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_D1),
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_D2),
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_D3),
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_D4),
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_D5),
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_A1),
                     (unsigned long)WinX68k_SCSIDebugIOCSCalls(),
                     (unsigned long)WinX68k_SCSIDebugReads(),
                     (unsigned long)WinX68k_SCSIDebugInitCalls(),
                     (unsigned long)WinX68k_SCSIDebugInstallerCalls(),
                     (unsigned long)WinX68k_SCSIDebugDriverInstalls(),
                     (unsigned long)WinX68k_SCSIDebugPartitionCount(),
                     (unsigned long)WinX68k_SCSIDebugIOCSVector());
        }
#endif

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

        const uint32_t present_div = textview_active ? 6u : 12u;
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
         * PANIC one-shot bootstrap.  Build 6.12c deliberately restores the
         * hardware-proven 6.11b sequence: create/execute P68K.BAT first, then
         * issue one direct P68K.X retry after 10 seconds only if no graphics
         * activity appeared.  6.12a/b changed this to an early direct-only
         * command and regressed PANIC startup on the real Tab5.
         */
        if (panic_mode && !panic_command_sent && textview_active && panic_text_ready_frame &&
            frame >= panic_text_ready_frame + 90u && !tab5_guest_input_busy())
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

                /* Build 6.12r: restore the proven 6.11b PANIC audio launch
                 * semantics.  Do NOT stop/flush the Tab5 speaker channel here.
                 * PANIC.X changes OPM/ADPCM state on the guest timeline, and the
                 * continuously running CPU0 feeder must remain attached to that
                 * stream.  The 6.12d pre-launch host flush was not present in the
                 * known-good 6.11b PELSIA path. */
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
                    panic_opm_write_base = OPM_DebugDataWriteCount();
                    panic_opm_keyon_base = OPM_DebugKeyOnCount();
                    panic_adpcm_ctl_base = ADPCM_DebugControlWriteCount();
                    panic_adpcm_data_base = ADPCM_DebugDataWriteCount();
                    panic_audio_produced_base = WinX68k_AudioProducedFrames();
                    panic_audio_diag_5s = false;
                    panic_audio_diag_15s = false;
                    panic_touch_enable_frame = frame + 240u;
                    tab5_panic_reset_touch();
                    ESP_LOGI(TAG, "*** PANIC AUTO START: drive=%c: via=%s selected=%s ***",
                             drive, batch_ready ? "P68K.BAT" : "direct P68K.X",
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

        /* Build 6.12r: one-shot PANIC audio source/host diagnostics.  This is
         * intentionally sparse (5 s and 15 s) so it can remain enabled while
         * isolating silent PAN files such as PELSIA01.PAN. */
        if (panic_mode && panic_command_sent &&
            ((!panic_audio_diag_5s && frame >= panic_command_frame + 300u) ||
             (!panic_audio_diag_15s && frame >= panic_command_frame + 900u)))
        {
            const bool late = frame >= panic_command_frame + 900u;
            tab5_audio_stats_t as = {0};
            uint32_t fm_q = 0, fm_drop = 0, fm_over = 0, fm_avail = 0;
            tab5_audio_get_stats(&as);
            WinX68k_AudioAsyncGetStats(&fm_q, &fm_drop, &fm_over, &fm_avail);
            ESP_LOGI(TAG,
                     "PANIC AUDIO DIAG %s: OPMwrites=+%lu keyon=+%lu ADPCMctl=+%lu ADPCMdata=+%lu playing=%d produced=+%lu host{sub=%lu play=%lu q=%lu spkq=%lu fail=%lu under=%lu} FM{q=%lu avail=%lu drop=%lu over=%lu}",
                     late ? "15s" : "5s",
                     (unsigned long)(OPM_DebugDataWriteCount() - panic_opm_write_base),
                     (unsigned long)(OPM_DebugKeyOnCount() - panic_opm_keyon_base),
                     (unsigned long)(ADPCM_DebugControlWriteCount() - panic_adpcm_ctl_base),
                     (unsigned long)(ADPCM_DebugDataWriteCount() - panic_adpcm_data_base),
                     ADPCM_DebugPlaying(),
                     (unsigned long)(WinX68k_AudioProducedFrames() - panic_audio_produced_base),
                     (unsigned long)as.submitted_frames,
                     (unsigned long)as.played_frames,
                     (unsigned long)as.queued_frames,
                     (unsigned long)as.speaker_queued_frames,
                     (unsigned long)as.play_failures,
                     (unsigned long)as.underflow_events,
                     (unsigned long)fm_q,
                     (unsigned long)fm_avail,
                     (unsigned long)fm_drop,
                     (unsigned long)fm_over);
            if (late) panic_audio_diag_15s = true;
            else panic_audio_diag_5s = true;
        }

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

        const bool do_present_budget = do_present && budget_render;
        if (do_present && !budget_render)
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
                /* Build 5.45: live PX68K framebuffer goes straight to CPU0 host LCD worker.
                 * No guest-side PSRAM snapshot copy. */
                tab5_video_present_px68k_live(fb, w, h, pitch);
            }
#if PX68K_TAB5_PERF_PROFILE
            if (perf_sample)
                perf_lcd_us = (uint32_t)(esp_timer_get_time() - perf_lcd_start);
#endif
        }

#if PX68K_TAB5_DIAG_VERBOSE
        /* Composite hashing and RUN FRAME summaries are legacy diagnostics. */
        const bool measure_frame =
            (frame <= 12u) ||
            (!textview_active && ((frame % 60u) == 0u)) ||
            (textview_active && composite_view && ((frame % 60u) == 0u)) ||
            (textview_active && !composite_view && ((frame % 300u) == 0u));

        if (measure_frame)
        {
            measured_nz = frame_nonzero(fb, w, h, pitch);
            measured_hash = frame_hash(fb, w, h, pitch);
        }

        const bool log_frame =
            (frame <= 12u) ||
            (frame <= 600u && (frame % 60u) == 0u) ||
            (frame > 600u && (frame % 300u) == 0u);

        if (log_frame)
        {
            ESP_LOGI(TAG,
                     "RUN FRAME %06lu cycles=%d "
                     "PC=$%08lX SR=$%04lX "
                     "CMP_NZ=%lu CMP_HASH=%08lX "
                     "TXT_NZ=%lu TXT_HASH=%08lX SY=%lu PAL=%d TXMIS=%lu RC=%lu "
                     "view=%s KBD=%u/%u KIF=%u UDR=%lu Q=%u RT=%u USB=%d EV=%lu DROP=%lu",
                     (unsigned long)frame,
                     cycles,
                     (unsigned long)pc,
                     (unsigned long)m68k_get_reg(NULL, M68K_REG_SR),
                     (unsigned long)measured_nz,
                     (unsigned long)measured_hash,
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
            ESP_LOGI(TAG,
                     "*** Human/FDD/keyboard/mouse/JoyPAD/composite baseline stable; quiet mode active ***");
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
            ESP_LOGI(TAG,
                     "CPU613C14R PROD f=%lu avg=%luus/f fps=%lu.%lu speed=%lu%% JIT=%s profiler=OFF costprobe=OFF",
                     (unsigned long)frame,(unsigned long)avg_wall_us,
                     (unsigned long)(fps_x10/10u),(unsigned long)(fps_x10%10u),
                     (unsigned long)speed_pct, PX68K_TAB5_DYNAREC ? "ON" : "OFF");
            /* c13: keep the targeted f=2100 dump so the SFXVI $368760 block
             * is captured before its later observed signature invalidation.
             * The timestamp is reset after the dump, so UART time is excluded
             * from the next wall window. */
            if (frame == 2100u || (frame >= 1500u && ((frame - 300u) % 1200u) == 0u)) {
                tab5_compose_stats_t rs = {0};
                tab5_video_async_stats_t vs = {0};
                m68k_tab5_dynarec_dump();
                tab5_compose_get_stats(&rs);
                tab5_video_get_async_stats(&vs);
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
            const uint32_t present_div_perf = textview_active ? 6u : 12u;
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
