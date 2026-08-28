#ifndef PX68K_TAB5_VIDEO_CPU1_H
#define PX68K_TAB5_VIDEO_CPU1_H

#include <stdint.h>

#include "tab5_guest_video_state.h"
#include "tab5_video_flow.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BAT177NW0 CPU1 NO-WAIT contract.
 *
 * CPU1 owns guest semantics and never asks a host resource whether rendering
 * may proceed. A dirty scanline is resolved at the guest raster. The final
 * result is published through tab5_video_flow using release-store metadata;
 * Screen/compose/LCD progress is downstream-only telemetry.
 */

typedef enum {
    TAB5_CPU1_VIDEO_MODE_16 = 0,
    TAB5_CPU1_VIDEO_MODE_256_A = 1,
    TAB5_CPU1_VIDEO_MODE_256_B = 2,
    TAB5_CPU1_VIDEO_MODE_65K = 3
} tab5_cpu1_video_mode_t;

typedef enum {
    TAB5_CPU1_VIDEO_LINE_CLEAN = 0,
    TAB5_CPU1_VIDEO_LINE_READY = 1,
    TAB5_CPU1_VIDEO_LINE_DEFER = 2 /* retained for ABI; never returned */
} tab5_cpu1_video_line_begin_result_t;

typedef struct {
    uint64_t render_seq;
    uint32_t y;
    uint32_t width;
    tab5_cpu1_video_mode_t mode;
    uint32_t video_epoch;
    uint32_t visual_seq;
    uint8_t active;
} tab5_cpu1_video_line_t;

extern uint8_t TextDirtyLine[1024];

static inline __attribute__((always_inline))
tab5_cpu1_video_mode_t tab5_cpu1_video_classify_mode(uint8_t vc_mode)
{
    return (tab5_cpu1_video_mode_t)(vc_mode & 3u);
}

static inline __attribute__((always_inline))
tab5_cpu1_video_line_begin_result_t
tab5_cpu1_video_line_begin(uint32_t y, uint32_t width, uint8_t vc_mode,
                           uint16_t *private_line,
                           tab5_cpu1_video_line_t *out_line)
{
    const tab5_guest_video_stamp_t stamp = tab5_guest_video_state_stamp();
    (void)private_line;

    out_line->render_seq = 0u;
    out_line->y = y;
    out_line->width = width;
    out_line->mode = tab5_cpu1_video_classify_mode(vc_mode);
    out_line->video_epoch = stamp.video_epoch;
    out_line->visual_seq = stamp.visual_seq;
    out_line->active = 0u;

    if (!TextDirtyLine[y])
        return TAB5_CPU1_VIDEO_LINE_CLEAN;

    out_line->render_seq = tab5_video_flow_cpu1_begin(y);
    out_line->active = 1u;
    return TAB5_CPU1_VIDEO_LINE_READY;
}

int tab5_cpu1_video_publish_commit(tab5_cpu1_video_line_t *line,
                                   const uint16_t *private_result_line);
void tab5_cpu1_video_offload_submitted(tab5_cpu1_video_line_t *line);
/* R57E50 queue pressure drops visual work, not guest semantics. */
void tab5_cpu1_video_offload_dropped(tab5_cpu1_video_line_t *line);
/* R57E54: CPU0 may fail to obtain a downstream Screen result slot after an
 * exact CPU0 compose.  It never touches guest dirty state directly; instead
 * it posts a one-bit retry request that CPU1 folds into TextDirtyLine at the
 * next guest frame boundary. */
void tab5_cpu1_video_retry_from_cpu0(uint32_t y);

static inline __attribute__((always_inline))
int tab5_cpu1_video_line_commit(tab5_cpu1_video_line_t *line,
                                const uint16_t *private_result_line)
{
    return tab5_cpu1_video_publish_commit(line, private_result_line);
}

static inline __attribute__((always_inline))
void tab5_cpu1_video_line_cancel(tab5_cpu1_video_line_t *line)
{
    if (line && line->active) {
        tab5_video_flow_cpu1_abort(line->y);
        line->active = 0u;
    }
}

typedef struct {
    uint64_t exact_renders;
    uint64_t accelerated_renders;
    uint64_t no_wait_commits;
    uint64_t no_wait_offloads;
    uint64_t visual_drops;
} tab5_cpu1_video_publish_stats_t;

void tab5_cpu1_video_get_publish_stats(tab5_cpu1_video_publish_stats_t *out);
void WinX68k_MarkVideoLineDirty(uint32_t y);
void tab5_cpu1_video_frame_boundary_sync(void);

#ifdef __cplusplus
}
#endif

#endif
