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

/* R140F1R2 compatibility ABI: CPU0 final pixels are handed directly to
 * Screen Manager in this production flow generation; no extra framebuffer or
 * cross-core publication is needed here. */
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
/* CPU0 scheduling control only; not telemetry. */
uint32_t tab5_video_flow_dirty_lines(void);

/* One-way wake only. Implemented by Screen Manager; never admits/waits. */
void tab5_screen_no_wait_kick(void);

#ifdef __cplusplus
}
#endif

#endif
