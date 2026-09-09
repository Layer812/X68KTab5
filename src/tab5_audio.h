/*
 * Tab5 port-specific implementation.
 * Intent: Public interface for the Tab5 CPU0 44.1-kHz product audio worker.
 * Layer8 Aug/17/2026
 */
#ifndef TAB5_AUDIO_H
#define TAB5_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t queued_frames;
    uint32_t submitted_frames;
    uint32_t dropped_frames;
    uint32_t played_frames;
    uint32_t underflow_events;
    uint32_t play_failures;
    /* Build 5.61/5.62 continuity diagnostics (cumulative since boot). */
    uint32_t min_queued_frames;
    uint32_t max_queued_frames;
    uint32_t prebuffer_waits;
    uint32_t prebuffer_resumes;
    uint32_t low_water_hits;
    uint32_t partial_holds;
    uint32_t queue_empty_events;
    uint32_t play_buffers_internal;
    /* Approximate PCM already queued inside M5Unified speaker (0..2048). */
    uint32_t speaker_queued_frames;
    /* Build 5.66: cumulative real one-tick waits while both M5 speaker slots are full. */
    uint32_t speaker_full_waits;
    /* Legacy ABI fields retained as zero for source compatibility. */
    uint32_t producer_rate_hz;
    uint32_t speaker_rate_hz;
    uint32_t rate_servo_active; /* P12R1 always 0 */
    uint32_t rate_changes;
    uint32_t cpu0_mix_work_us;      /* Build 6.15g cumulative host final-mix/pull work */
    uint32_t cpu0_speaker_work_us;  /* Build 6.15g cumulative speaker staging work */
} tab5_audio_stats_t;

int tab5_audio_init(void);
/* P12R6A4 three-stage host A/V profile.
 * 0=NORMAL 44.1k, 1=GREEN TURBO true22.05k, 2=RED TURBO true11.025k.
 * Guest CPU/device timing is unchanged in every profile. */
void tab5_audio_set_turbo_profile(uint32_t profile);
/* Legacy two-state ABI retained for any older caller. */
void tab5_audio_set_high_load_22k(int enable);
void tab5_audio_flush(void);
void tab5_audio_get_stats(tab5_audio_stats_t *out);
/* Runtime master-volume stepper. direction >0 = louder, <0 = quieter. */
int tab5_audio_step_volume(int direction);
int tab5_audio_get_volume(void);

#ifdef __cplusplus
}
#endif

#endif
