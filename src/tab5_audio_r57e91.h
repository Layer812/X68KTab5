#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAB5_AUDIO_R57E91_TOP 4u

typedef struct {
    uint32_t frame_pos;
    uint32_t jump;
    uint32_t local_ref; /* R92: 1=chunk boundary, 0=intra-chunk */
} tab5_audio_r57e91_pcm_event_t;

typedef struct {
    uint32_t frame_pos;
    uint32_t gap_us;
    uint32_t ring_after_take;
    uint32_t speaker_queue_before;
    uint32_t boundary_jump;
} tab5_audio_r57e91_submit_event_t;

typedef struct {
    /* R92 observes the exact final stereo buffer immediately before playRaw(). */
    uint32_t out_frames;
    uint32_t out_chunks;
    uint32_t out_boundaries;
    uint32_t out_boundary_gt4k;
    uint32_t out_boundary_gt8k;
    uint32_t out_boundary_gt16k;
    uint32_t out_boundary_max;
    uint32_t out_boundary_max_frame;
    uint32_t out_adj_gt4k;
    uint32_t out_adj_gt8k;
    uint32_t out_adj_gt16k;
    uint32_t out_adj_max;
    uint32_t out_adj_max_frame;
    uint32_t out_zero_run_max;
    uint32_t out_zero_runs_ge16;
    uint32_t out_zero_runs_ge64;
    uint32_t out_nonzero_frames;
    uint32_t out_peak_abs;

    uint32_t submit_count;
    uint32_t submit_gap_min_us;
    uint32_t submit_gap_max_us;
    uint32_t submit_gap_gt15ms;
    uint32_t submit_gap_gt20ms;
    uint32_t submit_gap_gt30ms;
    uint32_t submit_zeroq_refill;
    uint32_t submit_boundary_max;
    uint32_t submit_boundary_max_frame;
    uint32_t submit_ring_min;
    uint32_t submit_ring_max;

    tab5_audio_r57e91_pcm_event_t pcm_top[TAB5_AUDIO_R57E91_TOP];
    tab5_audio_r57e91_submit_event_t submit_top[TAB5_AUDIO_R57E91_TOP];
} tab5_audio_r57e91_stats_t;

void tab5_audio_r57e91_reset_stats(void);
void tab5_audio_r57e91_get_stats(tab5_audio_r57e91_stats_t *out);

#ifdef __cplusplus
}
#endif
