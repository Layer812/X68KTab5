/*
 * Tab5 port-specific implementation.
 * Intent: Tab5 audio backend: CPU1 preserves guest ADPCM/DMA timing and publishes ordered ADPCM commands; CPU0 decodes MSM6258, generates PCM, adds asynchronous YM2151 output, applies pitch-safe rate policy, and feeds the speaker.
 * Layer8 Aug/17/2026
 */
#include "tab5_audio.h"

/* RP_SAM2695_VOLBTN_R1: desired-state request only; UART stays on CPU0 MIDI worker. */
extern "C" void rp_midi_uart_adjust_master_volume(int direction);


#include <M5Unified.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstring>
#include <climits>
#include "esp_rom_sys.h"

extern "C" int WinX68k_AudioHostFramesAvail(void);
extern "C" int WinX68k_AudioHostReadFrames(int16_t *dst, int max_frames);
extern "C" void WinX68k_AudioSetSourceRate(uint32_t rate);
extern "C" void DSound_SetHostSourceRateRed11025(void);
extern "C" uint32_t WinX68k_AudioGetSourceRate(void);
extern "C" void WinX68k_AudioSetHostSourceReadyCallback(void (*cb)(void));
extern "C" void WinX68k_AudioAsyncGetStats(uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns, uint32_t *fm_avail);
extern "C" void OPM_R140P4D2MixClipGet(uint64_t *samples, uint64_t *clips);
extern "C" void OPM_R140P4D2SilentGet(uint32_t *generated_frames, uint32_t *opm_writes);
extern "C" void OPM_R140P4D7DemandGet(uint32_t *requested_frames, uint32_t *accepted_frames, uint32_t *rendered_frames, uint32_t *queue_depth, uint32_t *queue_max);
extern "C" void OPM_R140P4D7WindowBegin(void);
extern "C" void DSound_AbsTimelineAuditGet(uint32_t *guest_tick,uint32_t *fm_cursor,uint32_t *ad_cursor,
                                             uint32_t *seq,uint32_t *opm_pushes,uint32_t *qdepth,uint32_t *qmax,
                                             uint32_t *fault,uint32_t *drops,uint32_t *fm_render_frames,
                                             uint32_t *fm_render_work_us,uint32_t *ad_render_frames,uint32_t *ad_render_work_us);
extern "C" void DSound_AbsTimelineAuditWindowBegin(void);
extern "C" void DSound_AbsTimelineAuditGetEx(uint32_t *fm_wait_us,uint32_t *fm_wait_events,
                                                uint32_t *ad_wait_us,uint32_t *ad_wait_events,
                                                uint32_t *events_consumed,uint32_t *commit_pushes,
                                                uint32_t *fm_ctl_pushes,uint32_t *ad_data_pushes,uint32_t *ad_ctl_pushes);

static const char *TAG = "TAB5_AUDIO";

#ifndef PX68K_TAB5_AUDIO_RATE
#define PX68K_TAB5_AUDIO_RATE 44100
#endif
#ifndef PX68K_TAB5_AUDIO_VOLUME
#define PX68K_TAB5_AUDIO_VOLUME 33
#endif

/*
 * Build 5.62 audio-continuity + real-time budget policy.
 *
 * Intent: Keep guest-visible ADPCM/DMA state progression on CPU1 while moving MSM6258 waveform/PCM generation and final mixing to CPU0.  Layer8 Aug/17/2026
 * R57E107X: CPU1 remains authoritative for guest timing + DMA and now
 * emits an ordered ADPCM command stream only.  This CPU0 worker decodes and
 * renders MSM6258 PCM, adds the already-async CPU0 YM2151 ring, then
 * buffers the completed stereo PCM before handing it to M5Unified.
 *
 * M5Unified has two queued wav slots per virtual channel.  Sending a partial
 * tail whenever the queue becomes empty made short producer jitter worse:
 * the ring was drained to zero immediately and a harmless short gap became a
 * real speaker underflow.  Keep full 1024-frame chunks, build a small reserve
 * before first playback/recovery, and let producer notifications wake us.
 */
static constexpr size_t kRingFrames = 32768; /* R13: restore 128 KiB host ring; keep in PSRAM to preserve compositor Internal SRAM */
static constexpr size_t kRingMask = kRingFrames - 1;
/* R57E76 AUDIO DEADLINE:
 * 512 source frames are only 11.61 ms at 44.1 kHz.  The feeder's historical
 * one-tick sleep is 10 ms on this build, leaving almost no scheduling margin.
 * Restore a 1024-source-frame speaker slot (23.22 ms) while keeping the
 * established cold-start/resume reserve in absolute source-frame units. */
/* R57E77: R76 proved the feeder can keep up, but a 1024-source-frame
 * refill quantum makes a slow producer wait for too much data before each
 * speaker submission.  Restore the proven 512-frame quantum while RETAINING
 * R76's eager two-slot fill (no 10-ms sleep after the first slot). */
static constexpr size_t kChunkFrames = 512;
/* P12R6A3 TURBO: true 22.05-kHz uses half as many source frames for the
 * same wall-clock refill quantum.  These are the proven R120A time-equivalent
 * values; no 44.1->22.05 pair-average compatibility shortcut is used. */
static constexpr size_t kTurboChunkFrames = 256;
static constexpr size_t kRedTurboChunkFrames = 128;
static constexpr size_t kRedTurboLogicalRingFrames = 8192; /* ~743 ms, same max wall-time as NORMAL 32768@44.1k */
/* P12R1 HF2 / P12R6A4 time-equivalent reserves in each native source domain. */
static constexpr size_t kStartupWatermarkFrames = 3 * kChunkFrames; /* 1536 = 34.8 ms @44.1k */
static constexpr size_t kResumeWatermarkFrames  = 2 * kChunkFrames; /* 1024 = 23.2 ms @44.1k */
static constexpr size_t kTurboStartupWatermarkFrames = 3 * kTurboChunkFrames; /* 768 = 34.8 ms @22.05k */
static constexpr size_t kTurboResumeWatermarkFrames  = 2 * kTurboChunkFrames; /* 512 = 23.2 ms @22.05k */
static constexpr size_t kRedTurboStartupWatermarkFrames = 3 * kRedTurboChunkFrames; /* 384 = 34.8 ms @11.025k */
static constexpr size_t kRedTurboResumeWatermarkFrames  = 2 * kRedTurboChunkFrames; /* 256 = 23.2 ms @11.025k */
static constexpr size_t kPlayBuffers = 3;
static constexpr int kSpeakerChannel = 0;
static_assert((kRingFrames & (kRingFrames - 1)) == 0, "audio ring must be power-of-two");

static int16_t *s_ring = nullptr;
static int16_t *s_source_pull = nullptr;
static int16_t *s_play[kPlayBuffers] = { nullptr, nullptr, nullptr };
static size_t s_rd = 0;
static size_t s_wr = 0;
static size_t s_count = 0;
static TaskHandle_t s_task = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started = false;
static uint32_t s_reset_seq = 0;

static volatile uint32_t s_submitted = 0;
static uint32_t s_playbuf_internal = 0;
static volatile uint32_t s_speaker_queued_frames = 0;
/* HF3: CPU0 publishes only the budget occupancy value CPU1 needs.  The actual
 * ring count remains protected for the rare explicit reset/flush boundary, but
 * guest policy never takes the CPU0 audio mux. */
static volatile uint32_t s_budget_queued_frames = 0;

/* P12R6A4 three-stage product audio state.
 * 0=NORMAL 44.1k, 1=GREEN true22.05k, 2=RED true11.025k.
 * Cross-core mode observation is atomic; there is no steady-state audio mux. */
static volatile uint32_t s_audio_profile = 0u;

static inline uint32_t p12r6a4_profile_rate(uint32_t profile)
{
    return profile >= 2u ? (PX68K_TAB5_AUDIO_RATE / 4u)
                         : (profile == 1u ? (PX68K_TAB5_AUDIO_RATE / 2u)
                                          : PX68K_TAB5_AUDIO_RATE);
}
static inline size_t p12r6a4_profile_chunk(uint32_t profile)
{
    return profile >= 2u ? kRedTurboChunkFrames
                         : (profile == 1u ? kTurboChunkFrames : kChunkFrames);
}

static void audio_enqueue_mixed(const int16_t *samples, size_t frames);

/* R140P4: both source domains notify this feeder directly.  Task
 * notifications are counting/coalescing events, so a completion that arrives
 * just before the feeder blocks is retained and consumed immediately. */
static void audio_source_ready_cb(void)
{
    TaskHandle_t task = s_task;
    if (task)
        xTaskNotifyGive(task);
}

static void pump_host_source(void)
{
    if (!s_started || !s_source_pull)
        return;

/* Intent: Drain published source audio even when the speaker jitter buffer is full; drop only completed host PCM so back-pressure never reaches guest audio generation.  Layer8 Aug/17/2026 */
    /* Build 5.98g12a: preserve the pre-g12 pressure-relief semantics after
     * moving final mixing to CPU0.  CPU1 used to drain the guest ADPCM+FM
     * source first and only then submit to this host ring; when the host ring
     * was full, the CPU0 jitter-buffer enqueue dropped the completed PCM but the source
     * rings still advanced.  g12 accidentally stopped pulling whenever this
     * ring was full, so the async FM ring backed up and ASYNC_OPM_RENDER
     * requests were rejected (fmover).
     *
     * Always drain up to four 1024-frame source chunks per service pass and
     * let the CPU0-local enqueue perform the established destination-side drop
     * accounting.  This keeps ADPCM and FM consumption locked together and
     * prevents back-pressure from crossing into the emulated audio sources. */
    for (unsigned batch = 0; batch < 8; ++batch)
    {
        const uint32_t profile = __atomic_load_n(&s_audio_profile, __ATOMIC_ACQUIRE);
        const size_t ask = p12r6a4_profile_chunk(profile);
        const int got = WinX68k_AudioHostReadFrames(s_source_pull, (int)ask);
if (got <= 0)
            break;

        /* This call now runs on CPU0.  It preserves all established rate,
         * queue, nonzero, and drop accounting while CPU1 only sends a wakeup. */
        audio_enqueue_mixed(s_source_pull, (size_t)got);
        if ((size_t)got < ask)
            break;

        /* Eight bounded recovery passes; rotate equal-priority audio workers
         * after every complete 512-frame 44.1-kHz source chunk. */
        taskYIELD();
    }
}

/* P12R1 production quiet: R140P4S5 continuous serial audit and its
 * CPU0 timing/min-max instrumentation are intentionally not compiled. */

static void audio_task(void *)
{

    size_t play_index = 0;
    bool primed = false;
    bool playback_started = false;
    uint32_t seen_reset_seq = 0;

    for (;;)
    {
        if (!s_started)
        {
            vTaskDelay(1);
            continue;
        }

        /* CPU0 owns final extraction + FM saturation mix from this point. */
        pump_host_source();

        uint32_t reset_seq;
        size_t available;
        portENTER_CRITICAL(&s_mux);
        reset_seq = s_reset_seq;
        available = s_count;
        portEXIT_CRITICAL(&s_mux);
        const uint32_t profile = __atomic_load_n(&s_audio_profile, __ATOMIC_ACQUIRE);
        const size_t active_chunk = p12r6a4_profile_chunk(profile);

        if (reset_seq != seen_reset_seq)
        {
            seen_reset_seq = reset_seq;
            primed = false;
            playback_started = false;
        }

        /*
         * M5Unified keeps two wav slots per virtual channel.  Never call
         * playRaw() while both are occupied: _set_next_wav() may otherwise
         * wait for a slot and keep CPU0 runnable for too long.
         */
        const size_t speaker_queue = M5.Speaker.isPlaying(kSpeakerChannel);
        __atomic_store_n(&s_speaker_queued_frames,
                         (uint32_t)(speaker_queue * active_chunk), __ATOMIC_RELEASE);
        if (speaker_queue >= 2)
        {
            /*
             * Build 5.66: this must be a real blocked wait, not a millisecond
             * conversion that may round down to zero when configTICK_RATE_HZ
             * is low.  A zero-tick delay turns the feeder into a priority-3
             * busy poll of M5.Speaker.isPlaying(), starving IDLE0 and tripping
             * the task watchdog once guest PCM production reaches real time.
             *
             * One FreeRTOS tick is intentionally used here.  While two 1024
             * frame speaker slots are queued we already have about 46 ms of
             * PCM committed at 44.1 kHz, so a single tick cannot starve audio
             * on the current target and guarantees an IDLE scheduling window.
             */
            vTaskDelay(1);
            continue;
        }

        /*
         * Startup and real starvation recovery are deliberately different
         * from normal refill.  Once primed, one queued speaker chunk plus a
         * producer notification is enough; do not add latency on every dip.
         */
        if (!primed)
        {
            const size_t needed =
                profile >= 2u ? (playback_started ? kRedTurboResumeWatermarkFrames
                                                  : kRedTurboStartupWatermarkFrames)
                              : (profile == 1u
                                     ? (playback_started ? kTurboResumeWatermarkFrames
                                                         : kTurboStartupWatermarkFrames)
                                     : (playback_started ? kResumeWatermarkFrames
                                                         : kStartupWatermarkFrames));
            if (available < needed)
            {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }

            primed = true;
        }

        size_t take = 0;
        portENTER_CRITICAL(&s_mux);
        if (s_count >= active_chunk)
        {
            take = active_chunk;
            int16_t *dst = s_play[play_index];
            const size_t first = std::min(take, kRingFrames - s_rd);
            std::memcpy(dst, &s_ring[s_rd * 2], first * 2 * sizeof(int16_t));
            if (first < take)
                std::memcpy(dst + first * 2, s_ring, (take - first) * 2 * sizeof(int16_t));
            s_rd = (s_rd + take) & kRingMask;
            s_count -= take;
            __atomic_store_n(&s_budget_queued_frames, (uint32_t)s_count, __ATOMIC_RELEASE);
        }
        else if (s_count)
        {
            /* Build 5.61: preserve the tail instead of draining it early. */
        }
        portEXIT_CRITICAL(&s_mux);

        if (!take)
        {
            if (speaker_queue == 0 && playback_started)
            {
                /* A real speaker starvation: rebuild reserve before restart. */
                primed = false;
            }

            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* P12R6A4: speaker consumes the exact same native source domain that
         * CPU0 just rendered: NORMAL=44.1k, GREEN=22.05k, RED=11.025k. */
        const uint32_t play_rate = p12r6a4_profile_rate(profile);
        size_t play_frames = take;
        const bool play_ok=M5.Speaker.playRaw(s_play[play_index],
                               play_frames * 2,
                               play_rate,
                               true,
                               1,
                               kSpeakerChannel,
                               false);
        if (play_ok)
        {
            playback_started = true;
            ++play_index;
            if (play_index == kPlayBuffers)
                play_index = 0;
        }
        else
        {
        }


        /* R57E77: retain R76 eager second-slot refill with the original
         * 512-source-frame quantum.  Two queued slots now represent about
         * 23.2 ms of source time, but a refill only waits for 11.6 ms of new
         * source data rather than 23.2 ms.  The full two-slot path remains the
         * only place that sleeps one FreeRTOS tick. */
        taskYIELD();
        continue;
    }
}

extern "C" int tab5_audio_init(void)
{
if (s_started)
        return 1;

    if (!M5.Speaker.isEnabled())
    {
        ESP_LOGW(TAG, "Tab5 speaker is not enabled by M5Unified");
        return 0;
    }

    /* R13: keep the large final host ring in PSRAM.  R12 proved that consuming
     * 64 KiB of scarce Internal SRAM here fragments the heap enough to disable
     * the 81,920-byte CPU0 compositor arena, which is far more expensive overall. */
    const bool ring_internal = false;
    s_ring = static_cast<int16_t *>(heap_caps_aligned_alloc(
        64, kRingFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_ring)
    {
        ESP_LOGE(TAG, "audio ring allocation failed");
        return 0;
    }
    ESP_LOGI(TAG,
             "PX68K_AUDIOLOCAL_R13: Host ring bytes=%u frames=%u placement=%s ptr=%p internalFree=%u largest=%u",
             (unsigned)(kRingFrames * 2 * sizeof(int16_t)),
             (unsigned)kRingFrames,
             ring_internal ? "INTERNAL" : "PSRAM-FALLBACK",
             (void *)s_ring,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    /* g12 CPU0 pull/mix scratch.  Keep it internal so the final ADPCM+FM
     * saturation pass does not add another PSRAM read/modify/write stream. */
    s_source_pull = static_cast<int16_t *>(heap_caps_malloc(
        kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_source_pull)
        s_source_pull = static_cast<int16_t *>(heap_caps_malloc(
            kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_source_pull)
    {
        ESP_LOGE(TAG, "CPU0 host-mix scratch allocation failed");
        return 0;
    }

    /* Build 5.62: keep the large producer ring in PSRAM, but place the tiny
     * speaker staging buffers in internal SRAM.  Graphics is the dominant
     * PSRAM consumer during heavy scenes; removing the final 12 KiB audio
     * staging traffic from that contention path protects the real-time
     * speaker feeder.  Fall back to PSRAM rather than failing startup. */
    for (size_t i = 0; i < kPlayBuffers; ++i)
    {
        s_play[i] = static_cast<int16_t *>(heap_caps_malloc(
            kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (s_play[i])
        {
            ++s_playbuf_internal;
            continue;
        }

        s_play[i] = static_cast<int16_t *>(heap_caps_malloc(
            kChunkFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_play[i])
        {
            ESP_LOGE(TAG, "play buffer allocation failed at %u", (unsigned)i);
            return 0;
        }
    }

    /* Build 6.12f: Tab5 external-audio cold recovery.
     *
     * The ES8388 codec and PI4IO-controlled speaker amplifier can survive an
     * ESP32-P4 software reset in a bad state.  The official M5Unified Tab5
     * speaker callback rewrites the ES8388 register set and toggles SPK_EN
     * whenever Speaker.end()/begin() disables/enables the device.  Give the
     * external devices a real OFF interval before re-enabling them.
     *
     * Keep the proven 6.11b audio path otherwise byte-for-byte in behavior:
     * 44.1 kHz stereo, the same ring/mix/playRaw path and 33/255 master level.
     */
    ESP_LOGI(TAG, "Build 6.12f Tab5 audio cold recovery: Speaker OFF -> 50ms -> ES8388/AMP re-init");
    M5.Speaker.stop();
    M5.Speaker.end();
    vTaskDelay(pdMS_TO_TICKS(50));

    auto cfg = M5.Speaker.config();
    cfg.sample_rate = PX68K_TAB5_AUDIO_RATE;
    cfg.stereo = true;
    cfg.dma_buf_len = 256;
    cfg.dma_buf_count = 8;
    /* Speaker DMA/service must outrank compositor work on CPU0. */
    cfg.task_priority = 4;
#if portNUM_PROCESSORS > 1
    cfg.task_pinned_core = 0;
#endif
    M5.Speaker.config(cfg);

    if (!M5.Speaker.begin())
    {
        ESP_LOGE(TAG, "M5Unified speaker begin failed after cold recovery");
        return 0;
    }

    /* Allow ES8388 clocks/VREF and the external amplifier to settle before
     * the feeder starts queueing PCM.  Channel gains are forced to their
     * baseline values as an extra guard against stale runtime state. */
    vTaskDelay(pdMS_TO_TICKS(20));
    M5.Speaker.setAllChannelVolume(255);
    M5.Speaker.setChannelVolume(0, 255);
    M5.Speaker.setVolume(PX68K_TAB5_AUDIO_VOLUME);

    const auto verify_cfg = M5.Speaker.config();
    ESP_LOGI(TAG,
             "Tab5 speaker recovered: running=%d enabled=%d pins MCK/BCK/WS/DOUT=%d/%d/%d/%d rate=%u",
             M5.Speaker.isRunning() ? 1 : 0,
             M5.Speaker.isEnabled() ? 1 : 0,
             verify_cfg.pin_mck, verify_cfg.pin_bck, verify_cfg.pin_ws, verify_cfg.pin_data_out,
             (unsigned)verify_cfg.sample_rate);
    ESP_LOGI(TAG, "Speaker master volume applied: %u/255",
             (unsigned)M5.Speaker.getVolume());


    ESP_LOGI(TAG,
             "Build 5.89a feeder wait: tick_hz=%u tick_ms=%u queue-full=1 tick (never 0)",
             (unsigned)configTICK_RATE_HZ,
             (unsigned)portTICK_PERIOD_MS);
    __atomic_store_n(&s_audio_profile, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_budget_queued_frames, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_speaker_queued_frames, 0u, __ATOMIC_RELEASE);
    WinX68k_AudioSetSourceRate(PX68K_TAB5_AUDIO_RATE);
    ESP_LOGI(TAG, "PX68K_PRODUCT_P12R6A3_AUDIO: NORMAL=44.1kHz chunk512 reserve1536/1024; TURBO=true22.05kHz chunk256 reserve768/512; guest clocks unchanged");

#if portNUM_PROCESSORS > 1
    const BaseType_t task_res = xTaskCreatePinnedToCore(
        audio_task, "px68k_audio", 4096, nullptr, 4, &s_task, 0);
#else
    const BaseType_t task_res = xTaskCreate(
        audio_task, "px68k_audio", 4096, nullptr, 4, &s_task);
#endif
    if (task_res != pdPASS)
    {
        ESP_LOGE(TAG, "audio task create failed");
        s_task = nullptr;
        return 0;
    }

    s_started = true;
    WinX68k_AudioSetHostSourceReadyCallback(audio_source_ready_cb);
    ESP_LOGI(TAG,
             "PX68K_AUDIO_R140P4: COMPLETION-DRIVEN source wake ACTIVE; FM publish + ADPCM render event wake feeder, 10ms source polling retired");
    ESP_LOGI(TAG,
             "PX68K_AUDIO_R57E77: 512-frame eager feeder ACTIVE; R76 1024 quantum retired; MDX quantum=256; CPU0 YM2151 prio=4 feeder=4 speaker=4; rate=%u ring=%u chunk=%u startup=%u resume=%u playbuf_internal=%u/%u",
             (unsigned)PX68K_TAB5_AUDIO_RATE,
             (unsigned)kRingFrames,
             (unsigned)kChunkFrames,
             (unsigned)kStartupWatermarkFrames,
             (unsigned)kResumeWatermarkFrames,
             (unsigned)s_playbuf_internal,
             (unsigned)kPlayBuffers);

    ESP_LOGI(TAG, "PX68K_AUDIO_R57E118X3: CPU0 RT FAIRNESS ACTIVE YM=4 feeder=4 speaker=4; source-pull yields each full512; buffers/rates unchanged");
    ESP_LOGI(TAG, "PX68K_AUDIO_R57E91: 512-frame behavior frozen; continuity recorder compiled OUT for production");
    ESP_LOGI(TAG, "X68P4_R140N2R6 AUDIO LOW-LATENCY ACTIVE N/A startup=1536(34.8ms) resume=1024(23.2ms) chunk=512 two-slot retained; source-rate proof lives at guest-frame loop");
    ESP_LOGI(TAG, "PX68K_AUDIO_R57E94: OUTPCM/submit/M5 continuity probes compiled OUT; functional audio path unchanged");
    return 1;
}

extern "C" void tab5_audio_set_turbo_profile(uint32_t profile)
{
    if (!s_started)
        return;

    if (profile > 2u)
        profile = 2u;

    const uint32_t old_profile =
        __atomic_exchange_n(&s_audio_profile, profile, __ATOMIC_ACQ_REL);
    if (old_profile == profile)
        return;

    /* P12R6A4 follows the proven R120A explicit UI transition: switch the
     * complete native source domain, then discard only completed host/speaker
     * PCM. Guest DMA/timers are never flushed or retimed. */
    if (profile >= 2u)
        DSound_SetHostSourceRateRed11025();
    else
        WinX68k_AudioSetSourceRate(p12r6a4_profile_rate(profile));
    M5.Speaker.stop(kSpeakerChannel);
    portENTER_CRITICAL(&s_mux);
    s_rd = s_wr = s_count = 0;
    ++s_reset_seq;
    __atomic_store_n(&s_budget_queued_frames, 0u, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_mux);
    __atomic_store_n(&s_speaker_queued_frames, 0u, __ATOMIC_RELEASE);
    if (s_task)
        xTaskNotifyGive(s_task);

    ESP_LOGI(TAG,
             "PX68K_TURBO_P12R6A4_AUDIO: profile=%u source/YM/ADPCM/host/speaker=%uHz",
             (unsigned)profile, (unsigned)p12r6a4_profile_rate(profile));
}

extern "C" void tab5_audio_set_high_load_22k(int enable)
{
    tab5_audio_set_turbo_profile(enable ? 1u : 0u);
}

static void audio_enqueue_mixed(const int16_t *samples, size_t frames)
{
if (!s_started || !samples || !frames)
        return;

    /* R57E92: upstream staging scan retired; exact observation is at playRaw(). */



    size_t accepted = 0;
    const uint32_t profile = __atomic_load_n(&s_audio_profile, __ATOMIC_ACQUIRE);
    const size_t logical_capacity = profile >= 2u ? kRedTurboLogicalRingFrames : kRingFrames;
    portENTER_CRITICAL(&s_mux);
    const size_t bounded_count = std::min(s_count, logical_capacity);
    const size_t free_frames = logical_capacity - bounded_count;
    accepted = std::min(frames, free_frames);
    if (accepted)
    {
        const size_t first = std::min(accepted, kRingFrames - s_wr);
        std::memcpy(&s_ring[s_wr * 2], samples, first * 2 * sizeof(int16_t));
        if (first < accepted)
            std::memcpy(s_ring, samples + first * 2, (accepted - first) * 2 * sizeof(int16_t));
        s_wr = (s_wr + accepted) & kRingMask;
        s_count += accepted;
    }
    __atomic_store_n(&s_budget_queued_frames, (uint32_t)s_count, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_mux);
    if (accepted)
        __atomic_add_fetch(&s_submitted, (uint32_t)accepted, __ATOMIC_RELAXED);

    /* Producer and consumer are this same CPU0 feeder task.  Do not create a
     * self-notification here; source readiness is signaled at FM/ADPCM
     * completion boundaries instead. */
}

extern "C" void tab5_audio_flush(void)
{
    if (!s_started)
        return;

    M5.Speaker.stop(kSpeakerChannel);
    portENTER_CRITICAL(&s_mux);
    s_rd = s_wr = s_count = 0;
    ++s_reset_seq;
    __atomic_store_n(&s_budget_queued_frames, 0u, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&s_mux);
    __atomic_store_n(&s_speaker_queued_frames, 0u, __ATOMIC_RELEASE);
    if (s_task)
        xTaskNotifyGive(s_task);

    WinX68k_AudioSetSourceRate(
        p12r6a4_profile_rate(__atomic_load_n(&s_audio_profile, __ATOMIC_ACQUIRE)));
}

extern "C" void tab5_audio_get_stats(tab5_audio_stats_t *out)
{
    if (!out)
        return;

    /* HF3 CPU1 NO-WAIT: CPU0 publishes the three budget facts atomically.
     * Never take the audio ring mux from the guest timeline. */
    std::memset(out, 0, sizeof(*out));
    out->queued_frames = __atomic_load_n(&s_budget_queued_frames, __ATOMIC_ACQUIRE);
    out->submitted_frames = __atomic_load_n(&s_submitted, __ATOMIC_ACQUIRE);
    out->speaker_queued_frames = __atomic_load_n(&s_speaker_queued_frames, __ATOMIC_ACQUIRE);
}

extern "C" int tab5_audio_step_volume(int direction)
{
    /* RP_SAM2695_VOLBTN_R1: host desired-state update only; UART remains CPU0-owned. */
    if (direction != 0)
        rp_midi_uart_adjust_master_volume(direction);

    static const uint8_t kLevels[] = { 0, 16, 33, 64, 96, 128, 160, 192, 224, 255 };
    int current = (int)M5.Speaker.getVolume();
    int target = current;
    if (direction > 0) {
        for (uint8_t v : kLevels) {
            if ((int)v > current) { target = (int)v; break; }
        }
    } else if (direction < 0) {
        for (size_t i = sizeof(kLevels) / sizeof(kLevels[0]); i-- > 0;) {
            if ((int)kLevels[i] < current) { target = (int)kLevels[i]; break; }
        }
    }
    if (target != current) M5.Speaker.setVolume((uint8_t)target);
    ESP_LOGI(TAG, "In-game volume step: %d -> %d /255 (master only; stream untouched)", current, target);
    return target;
}

extern "C" int tab5_audio_get_volume(void)
{
    return (int)M5.Speaker.getVolume();
}

