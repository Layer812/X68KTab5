#ifndef PX68K_TAB5_VIDEO_FLOW_H
#define PX68K_TAB5_VIDEO_FLOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAB5_VIDEO_FLOW_MAX_LINES 1024u
#define TAB5_VIDEO_FLOW_MAX_WIDTH 800u
#define TAB5_VIDEO_FLOW_FB_LINES  600u

typedef enum {
    TAB5_VIDEO_FLOW_SOURCE_NONE = 0,
    TAB5_VIDEO_FLOW_SOURCE_CPU1 = 1,
    TAB5_VIDEO_FLOW_SOURCE_CPU0 = 2
} tab5_video_flow_source_t;

typedef struct {
    uint64_t render_seq;
    uint32_t video_epoch;
    uint32_t visual_seq;
    uint16_t y;
    uint16_t width;
    uint8_t source;
    uint8_t reserved[3];
} tab5_video_flow_line_t;

typedef struct {
    uint64_t guest_frontier;
    uint64_t compose_frontier;
    uint64_t screen_commit_frontier;
    uint64_t visible_frontier;
    uint64_t cpu1_exact_commits;
    uint64_t cpu0_final_commits;
    uint64_t dirty_posts;
    uint64_t snapshot_races;
    uint64_t stale_skips;
    uint64_t requeues;
    uint32_t dirty_lines;
    uint32_t dirty_max;
} tab5_video_flow_stats_t;

/* One-time buffer registration. CPU1 remains sole writer of cpu1_fb. */
int tab5_video_flow_init(void);
void tab5_video_flow_register_cpu1_fb(uint16_t *cpu1_fb,
                                      uint32_t pitch_pixels,
                                      uint32_t height_lines);

/* CPU1 guest line lifecycle. No Screen/CPU0 state is consulted. */
uint64_t tab5_video_flow_cpu1_begin(uint32_t y);
void tab5_video_flow_cpu1_abort(uint32_t y);
void tab5_video_flow_cpu1_commit(uint32_t y, uint32_t width,
                                 uint64_t render_seq,
                                 uint32_t video_epoch,
                                 uint32_t visual_seq);

/* CPU0 final compositor progress only. Pixels stay on CPU0 and are handed to
 * Screen Manager through its bounded result slots; video_flow owns only the
 * cross-core CPU1 authoritative publication. */
void tab5_video_flow_cpu0_complete(uint64_t render_seq);
uint64_t tab5_video_flow_cpu1_line_seq(uint32_t y);

/* Screen Manager CPU0-side latest-wins ingest. */
uint32_t tab5_video_flow_take_dirty_word(uint32_t word_index);
void tab5_video_flow_requeue_line(uint32_t y);
int tab5_video_flow_snapshot_latest(uint32_t y,
                                    uint16_t *dst,
                                    uint32_t dst_capacity_pixels,
                                    tab5_video_flow_line_t *meta);

uint64_t tab5_video_flow_guest_frontier(void);
void tab5_video_flow_set_screen_commit_frontier(uint64_t seq);
void tab5_video_flow_set_visible_frontier(uint64_t seq);
void tab5_video_flow_get_stats(tab5_video_flow_stats_t *out);

/* One-way wake only. Implemented by Screen Manager; never admits/waits. */
void tab5_screen_no_wait_kick(void);

#ifdef __cplusplus
}
#endif

#endif
