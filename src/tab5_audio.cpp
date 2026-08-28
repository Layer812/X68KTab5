/*
 * Tab5 port-specific implementation.
 * Intent: Tab5 audio backend: preserve guest timing on CPU1 while CPU0 pulls published ADPCM, adds asynchronous YM2151 output, applies pitch-safe rate policy, and feeds the speaker.
 * Layer8 Aug/17/2026
 */
#include "tab5_audio.h"

#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif
#ifndef PX68K_TAB5_R57E63_AUDIO_AUDIT
#define PX68K_TAB5_R57E63_AUDIO_AUDIT 0
#endif

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
extern "C" void WinX68k_AudioAsyncGetStats(uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns, uint32_t *fm_avail);
extern "C" uint32_t WinX68k_AudioProducedFrames(void);
extern "C" uint32_t OPM_DebugDataWriteCount(void);

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
 * Intent: Move only host-side final mixing to CPU0; never move guest ADPCM state progression away from CPU1.  Layer8 Aug/17/2026
 * Build 5.98g12: CPU1 remains authoritative for guest timing + ADPCM
 * generation, but no longer extracts or final-mixes PCM.  This CPU0 worker
 * pulls published ADPCM PCM, adds the already-async CPU0 YM2151 ring, then
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
static constexpr size_t kChunkFrames = 512;
/* R57E57: halve feeder granularity so a real starvation recovers in ~23 ms
 * instead of ~46 ms, while keeping the original ~70 ms cold-start reserve. */
static constexpr size_t kStartupWatermarkFrames = 6 * kChunkFrames; /* 69.7 ms */
/* R57E67: recovery needs more than a single 23-ms cushion when guest
 * production is hovering around real time.  Resume with ~46 ms in hand;
 * normal steady playback latency is unchanged. */
static constexpr size_t kResumeWatermarkFrames = 4 * kChunkFrames;  /* 46.4 ms */
static constexpr size_t kLowWatermarkFrames = kChunkFrames;
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
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
static bool s_nonzero_announced = false;
#endif
static bool s_host_mix_announced = false;
static uint32_t s_reset_seq = 0;

static uint32_t s_submitted = 0;
static uint32_t s_dropped = 0;
static uint32_t s_played = 0;
static uint32_t s_underflow = 0;
static uint32_t s_play_fail = 0;
static uint32_t s_prebuffer_waits = 0;
static uint32_t s_prebuffer_resumes = 0;
static uint32_t s_low_water_hits = 0;
static uint32_t s_partial_holds = 0;
static uint32_t s_queue_empty_events = 0;
static uint32_t s_min_queued = UINT32_MAX;
static uint32_t s_max_queued = 0;
static uint32_t s_playbuf_internal = 0;
static uint32_t s_speaker_queued_frames = 0;
static uint32_t s_speaker_full_waits = 0;
/* Build 5.98g9b: producer-rate estimator + pitch-safe 44.1/22.05 kHz quality switch.
 * R57E68 keeps only the functional speaker-rate state in release; producer
 * wall-rate exists solely when focused audit/research diagnostics are enabled. */
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
static uint32_t s_producer_rate_hz = PX68K_TAB5_AUDIO_RATE;
#endif
static uint32_t s_speaker_rate_hz = PX68K_TAB5_AUDIO_RATE;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
static uint32_t s_cpu0_mix_work_us = 0;
static uint32_t s_cpu0_speaker_work_us = 0;
#endif
static uint32_t s_rate_changes = 0;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
static uint32_t s_rate_window_frames = 0;
static int64_t s_rate_window_start_us = 0;
static bool s_rate_valid = false;
#endif
static bool s_quality_22k_requested = false;
static bool s_quality_22k_active = false;

static void audio_enqueue_mixed(const int16_t *samples, size_t frames);

static inline void note_queued_locked(size_t count)
{
    const uint32_t q = (uint32_t)count;
    if (q < s_min_queued)
        s_min_queued = q;
    if (q > s_max_queued)
        s_max_queued = q;
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
        const size_t ask = kChunkFrames;
        const int got = WinX68k_AudioHostReadFrames(s_source_pull, (int)ask);
if (got <= 0)
            break;

        if (!s_host_mix_announced)
        {
            s_host_mix_announced = true;
            ESP_LOGI(TAG, "*** CPU0 FINAL AUDIO MIX ACTIVE: ADPCM pull + async FM saturation mix moved off CPU1 ***");
        }

        /* This call now runs on CPU0.  It preserves all established rate,
         * queue, nonzero, and drop accounting while CPU1 only sends a wakeup. */
        audio_enqueue_mixed(s_source_pull, (size_t)got);
        if ((size_t)got < ask)
            break;
    }
}

static void audio_task(void *)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    {
        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);
        esp_rom_printf("R56K5_TASKSELF name=px68k_audio core=%d base=0x%08x top=0x%08x bytes=4096 hwm=%u\\n",
                       (int)xPortGetCoreID(), (unsigned)r56k5_base,
                       (unsigned)(r56k5_base + 4096u),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif

    size_t play_index = 0;
    bool primed = false;
    bool playback_started = false;
    uint32_t seen_reset_seq = 0;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    int64_t diag_prev_us = 0;
    uint32_t diag_prev_guest_frames = 0;
    uint32_t diag_prev_opm_writes = 0;
#endif

    for (;;)
    {
        if (!s_started)
        {
            vTaskDelay(1);
            continue;
        }

        /* CPU0 owns final extraction + FM saturation mix from this point. */
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        const int64_t mix_work_t0 = esp_timer_get_time();
#endif
        pump_host_source();
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        s_cpu0_mix_work_us += (uint32_t)(esp_timer_get_time() - mix_work_t0);
#endif

        uint32_t reset_seq;
        bool quality_22k_requested;
        size_t available;
        portENTER_CRITICAL(&s_mux);
        reset_seq = s_reset_seq;
        available = s_count;
        quality_22k_requested = s_quality_22k_requested;
        portEXIT_CRITICAL(&s_mux);

        if (reset_seq != seen_reset_seq)
        {
            seen_reset_seq = reset_seq;
            primed = false;
            playback_started = false;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
            diag_prev_us = 0;
            diag_prev_guest_frames = WinX68k_AudioProducedFrames();
            diag_prev_opm_writes = OPM_DebugDataWriteCount();
#endif
        }

        /* Build 6.15b: sparse MDX/audio-only telemetry plus OPM-write rate. Production CPU/render
         * profiling remains OFF; one line every two seconds distinguishes a
         * slow guest producer from FM queue pressure and speaker starvation. */
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        if ((!PX68K_TAB5_R43_QUIET_RUNTIME) && s_host_mix_announced)
        {
            const int64_t now_us = esp_timer_get_time();
            if (!diag_prev_us)
            {
                diag_prev_us = now_us;
                diag_prev_guest_frames = WinX68k_AudioProducedFrames();
                diag_prev_opm_writes = OPM_DebugDataWriteCount();
            }
            const int64_t diag_dt = now_us - diag_prev_us;
            if (diag_dt >= 2000000LL)
            {
                const uint32_t guest_now = WinX68k_AudioProducedFrames();
                const uint32_t guest_delta = guest_now - diag_prev_guest_frames;
                const uint32_t guest_rate = (uint32_t)(((uint64_t)guest_delta * 1000000ULL) / (uint64_t)diag_dt);
                const uint32_t opm_now = OPM_DebugDataWriteCount();
                const uint32_t opm_delta = opm_now - diag_prev_opm_writes;
                const uint32_t opm_rate = (uint32_t)(((uint64_t)opm_delta * 1000000ULL) / (uint64_t)diag_dt);
                uint32_t fm_q = 0, fm_drop = 0, fm_over = 0, fm_avail = 0;
                WinX68k_AudioAsyncGetStats(&fm_q, &fm_drop, &fm_over, &fm_avail);
                uint32_t q, spq, prod, sprate, q22, under, drop, fail, fullwait;
                portENTER_CRITICAL(&s_mux);
                q = (uint32_t)s_count;
                spq = s_speaker_queued_frames;
                prod = s_producer_rate_hz;
                sprate = s_speaker_rate_hz;
                q22 = s_quality_22k_active ? 1u : 0u;
                under = s_underflow;
                drop = s_dropped;
                fail = s_play_fail;
                fullwait = s_speaker_full_waits;
                portEXIT_CRITICAL(&s_mux);
                ESP_LOGI(TAG,
                         "AUDIO615H17 MDX guest=%luHz host=%luHz out=%luHz opmw=%lu/s q22=%lu q=%lu spq=%lu under=%lu drop=%lu fail=%lu fullwait=%lu FM{q=%lu avail=%lu drop=%lu over=%lu}",
                         (unsigned long)guest_rate, (unsigned long)prod,
                         (unsigned long)sprate, (unsigned long)opm_rate,
                         (unsigned long)q22,
                         (unsigned long)q, (unsigned long)spq,
                         (unsigned long)under, (unsigned long)drop,
                         (unsigned long)fail, (unsigned long)fullwait,
                         (unsigned long)fm_q, (unsigned long)fm_avail,
                         (unsigned long)fm_drop, (unsigned long)fm_over);
                diag_prev_us = now_us;
                diag_prev_guest_frames = guest_now;
                diag_prev_opm_writes = opm_now;
            }
        }
#endif

        /*
         * M5Unified keeps two wav slots per virtual channel.  Never call
         * playRaw() while both are occupied: _set_next_wav() may otherwise
         * wait for a slot and keep CPU0 runnable for too long.
         */
        const size_t speaker_queue = M5.Speaker.isPlaying(kSpeakerChannel);
        portENTER_CRITICAL(&s_mux);
        s_speaker_queued_frames = (uint32_t)(speaker_queue * kChunkFrames);
        portEXIT_CRITICAL(&s_mux);
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
            portENTER_CRITICAL(&s_mux);
            ++s_speaker_full_waits;
            portEXIT_CRITICAL(&s_mux);
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
            const size_t needed = playback_started
                                      ? kResumeWatermarkFrames
                                      : kStartupWatermarkFrames;
            if (available < needed)
            {
                portENTER_CRITICAL(&s_mux);
                ++s_prebuffer_waits;
                note_queued_locked(s_count);
                portEXIT_CRITICAL(&s_mux);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
                continue;
            }

            primed = true;
            portENTER_CRITICAL(&s_mux);
            ++s_prebuffer_resumes;
            portEXIT_CRITICAL(&s_mux);
        }

        size_t take = 0;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        const int64_t speaker_work_t0 = esp_timer_get_time();
#endif
        portENTER_CRITICAL(&s_mux);
        if (s_count >= kChunkFrames)
        {
            take = kChunkFrames;
            int16_t *dst = s_play[play_index];
            const size_t first = std::min(take, kRingFrames - s_rd);
            std::memcpy(dst, &s_ring[s_rd * 2], first * 2 * sizeof(int16_t));
            if (first < take)
                std::memcpy(dst + first * 2, s_ring, (take - first) * 2 * sizeof(int16_t));
            s_rd = (s_rd + take) & kRingMask;
            s_count -= take;
            if (s_count < kLowWatermarkFrames)
                ++s_low_water_hits;
            note_queued_locked(s_count);
        }
        else if (s_count)
        {
            /* Build 5.61: preserve the tail instead of draining it early. */
            ++s_partial_holds;
            note_queued_locked(s_count);
        }
        portEXIT_CRITICAL(&s_mux);

        if (!take)
        {
            if (speaker_queue == 0 && playback_started)
            {
                /* A real speaker starvation: rebuild reserve before restart. */
                portENTER_CRITICAL(&s_mux);
                ++s_underflow;
                ++s_queue_empty_events;
                portEXIT_CRITICAL(&s_mux);
                primed = false;
            }

            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }

        /* Build 5.98g9b: GUARD/CRIT may trade bandwidth for quality, but
         * NEVER trade pitch for continuity.  In 22.05-kHz mode consume the
         * same 1024 source frames, low-pass/decimate 2:1 to 512 frames, then
         * play those 512 frames at 22.05 kHz.  Chunk duration remains 23.2 ms,
         * so voices keep their original pitch; only bandwidth is reduced.
         * This intentionally does not pretend to fix a slow guest clock -- it
         * merely halves downstream speaker/DMA sample work while overloaded. */
        uint32_t play_rate = PX68K_TAB5_AUDIO_RATE;
        size_t play_frames = take;
        if (quality_22k_requested)
        {
            int16_t *buf = s_play[play_index];
            const size_t out_frames = take >> 1;
            for (size_t i = 0; i < out_frames; ++i)
            {
                const size_t si = i << 2; /* two stereo source frames */
                const int32_t l = (int32_t)buf[si + 0] + (int32_t)buf[si + 2];
                const int32_t r = (int32_t)buf[si + 1] + (int32_t)buf[si + 3];
                buf[(i << 1) + 0] = (int16_t)(l / 2);
                buf[(i << 1) + 1] = (int16_t)(r / 2);
            }
            play_frames = out_frames;
            play_rate = PX68K_TAB5_AUDIO_RATE / 2u;
        }
        portENTER_CRITICAL(&s_mux);
        if (s_quality_22k_active != quality_22k_requested || s_speaker_rate_hz != play_rate)
            ++s_rate_changes;
        s_quality_22k_active = quality_22k_requested;
        s_speaker_rate_hz = play_rate;
        portEXIT_CRITICAL(&s_mux);
if (M5.Speaker.playRaw(s_play[play_index],
                               play_frames * 2,
                               play_rate,
                               true,
                               1,
                               kSpeakerChannel,
                               false))
        {
            portENTER_CRITICAL(&s_mux);
            s_played += (uint32_t)take;
            portEXIT_CRITICAL(&s_mux);
            playback_started = true;
            ++play_index;
            if (play_index == kPlayBuffers)
                play_index = 0;
        }
        else
        {
            portENTER_CRITICAL(&s_mux);
            ++s_play_fail;
            portEXIT_CRITICAL(&s_mux);
        }
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        s_cpu0_speaker_work_us += (uint32_t)(esp_timer_get_time() - speaker_work_t0);
#endif

        /* Give the core's IDLE task a scheduling window. */
        vTaskDelay(1);
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
    portENTER_CRITICAL(&s_mux);
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_rate_window_start_us = 0;
    s_rate_window_frames = 0;
#endif
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_producer_rate_hz = PX68K_TAB5_AUDIO_RATE;
#endif
    s_speaker_rate_hz = PX68K_TAB5_AUDIO_RATE;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_rate_valid = false;
#endif
    s_quality_22k_requested = false;
    s_quality_22k_active = false;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Build 6.00 CPU0-final-mix + pressure-relief + pitch-safe audio: NORMAL=%uHz GUARD/CRIT=%uHz pair-average 2:1; no clock stretching",
             (unsigned)PX68K_TAB5_AUDIO_RATE,
             (unsigned)(PX68K_TAB5_AUDIO_RATE / 2u));

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
    ESP_LOGI(TAG,
             "PX68K_AUDIO_R57E67: continuity feeder ACTIVE; MDX quantum=256; CPU0 YM2151 prio=3, feeder=4, speaker=4; pressure-relief=ON; rate=%u ring=%u chunk=%u startup=%u resume=%u playbuf_internal=%u/%u",
             (unsigned)PX68K_TAB5_AUDIO_RATE,
             (unsigned)kRingFrames,
             (unsigned)kChunkFrames,
             (unsigned)kStartupWatermarkFrames,
             (unsigned)kResumeWatermarkFrames,
             (unsigned)s_playbuf_internal,
             (unsigned)kPlayBuffers);
    return 1;
}

static void audio_enqueue_mixed(const int16_t *samples, size_t frames)
{
if (!s_started || !samples || !frames)
        return;

#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    /* Diagnostic-only producer wall-rate estimator. R57E68 release quiet
     * compiles this entire wall-clock/critical-section probe out. */
    {
        const int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_mux);
        if (!s_rate_window_start_us) s_rate_window_start_us = now_us;
        s_rate_window_frames += (uint32_t)frames;
        const int64_t dt = now_us - s_rate_window_start_us;
        if (dt >= 250000)
        {
            uint32_t inst = (uint32_t)(((uint64_t)s_rate_window_frames * 1000000ULL) / (uint64_t)dt);
            if (inst > 60000u) inst = 60000u;
            if (inst < 1000u) inst = 1000u;
            if (!s_rate_valid) s_producer_rate_hz = inst;
            else s_producer_rate_hz = (s_producer_rate_hz + inst) >> 1;
            s_rate_valid = true;
            s_rate_window_frames = 0;
            s_rate_window_start_us = now_us;
        }
        portEXIT_CRITICAL(&s_mux);
    }
#endif

#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    if (!s_nonzero_announced)
    {
        const size_t probe = std::min(frames * 2, (size_t)512);
        for (size_t i = 0; i < probe; ++i)
        {
            if (samples[i] > 32 || samples[i] < -32)
            {
                s_nonzero_announced = true;
                ESP_LOGI(TAG, "*** PX68K AUDIO ACTIVE: non-zero mixed PCM reached Tab5 ***");
                break;
            }
        }
    }
#endif

    size_t accepted = 0;
    portENTER_CRITICAL(&s_mux);
    const size_t free_frames = kRingFrames - s_count;
    accepted = std::min(frames, free_frames);
    if (accepted)
    {
        const size_t first = std::min(accepted, kRingFrames - s_wr);
        std::memcpy(&s_ring[s_wr * 2], samples, first * 2 * sizeof(int16_t));
        if (first < accepted)
            std::memcpy(s_ring, samples + first * 2, (accepted - first) * 2 * sizeof(int16_t));
        s_wr = (s_wr + accepted) & kRingMask;
        s_count += accepted;
        note_queued_locked(s_count);
    }
    s_submitted += (uint32_t)accepted;
    s_dropped += (uint32_t)(frames - accepted);
    portEXIT_CRITICAL(&s_mux);

    if (accepted && s_task)
        xTaskNotifyGive(s_task);
}

extern "C" void tab5_audio_kick(void)
{
    if (s_task)
        xTaskNotifyGive(s_task);
}

extern "C" void tab5_audio_set_high_load_22k(int enable)
{
    if (!s_started)
        return;
    portENTER_CRITICAL(&s_mux);
    s_quality_22k_requested = (enable != 0);
    portEXIT_CRITICAL(&s_mux);
    if (s_task)
        xTaskNotifyGive(s_task);
}

extern "C" void tab5_audio_flush(void)
{
    if (!s_started)
        return;

    M5.Speaker.stop(kSpeakerChannel);
    portENTER_CRITICAL(&s_mux);
    s_rd = s_wr = s_count = 0;
    ++s_reset_seq;
    s_min_queued = UINT32_MAX;
    s_max_queued = 0;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_rate_window_frames = 0;
    s_rate_window_start_us = 0;
#endif
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_producer_rate_hz = PX68K_TAB5_AUDIO_RATE;
#endif
    s_speaker_rate_hz = PX68K_TAB5_AUDIO_RATE;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    s_rate_valid = false;
#endif
    s_quality_22k_requested = false;
    s_quality_22k_active = false;
    portEXIT_CRITICAL(&s_mux);
    if (s_task)
        xTaskNotifyGive(s_task);
}

extern "C" void tab5_audio_get_stats(tab5_audio_stats_t *out)
{
    if (!out)
        return;

    portENTER_CRITICAL(&s_mux);
    out->queued_frames = (uint32_t)s_count;
    out->submitted_frames = s_submitted;
    out->dropped_frames = s_dropped;
    out->played_frames = s_played;
    out->underflow_events = s_underflow;
    out->play_failures = s_play_fail;
    out->min_queued_frames = (s_min_queued == UINT32_MAX) ? 0u : s_min_queued;
    out->max_queued_frames = s_max_queued;
    out->prebuffer_waits = s_prebuffer_waits;
    out->prebuffer_resumes = s_prebuffer_resumes;
    out->low_water_hits = s_low_water_hits;
    out->partial_holds = s_partial_holds;
    out->queue_empty_events = s_queue_empty_events;
    out->play_buffers_internal = s_playbuf_internal;
    out->speaker_queued_frames = s_speaker_queued_frames;
    out->speaker_full_waits = s_speaker_full_waits;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT || PX68K_TAB5_RELEASE_DIAGNOSTICS
    out->producer_rate_hz = s_producer_rate_hz;
#else
    out->producer_rate_hz = 0;
#endif
    out->speaker_rate_hz = s_speaker_rate_hz;
    out->rate_servo_active = s_quality_22k_active ? 1u : 0u;
    out->rate_changes = s_rate_changes;
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    out->cpu0_mix_work_us = s_cpu0_mix_work_us;
    out->cpu0_speaker_work_us = s_cpu0_speaker_work_us;
#else
    out->cpu0_mix_work_us = 0;
    out->cpu0_speaker_work_us = 0;
#endif
    portEXIT_CRITICAL(&s_mux);
}


/* Build 6.12h: runtime volume control is intentionally narrow in scope.
 * The 6.12f cold-recovery sequence remains the only place that restarts the
 * Tab5 speaker/ES8388 path.  During playback we change only the master-volume
 * scalar, using coarse levels so each press is obvious from the 33/255
 * startup point. */
extern "C" int tab5_audio_step_volume(int direction)
{
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

extern "C" uint32_t tab5_audio_stack_highwater(void)
{
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0u;
}
