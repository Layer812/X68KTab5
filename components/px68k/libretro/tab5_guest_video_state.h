#ifndef PX68K_TAB5_GUEST_VIDEO_STATE_H
#define PX68K_TAB5_GUEST_VIDEO_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BAT172E0: pure CPU1 guest-video semantic state.
 *
 * This module deliberately has NO FreeRTOS, ESP-IDF, Screen Manager, LP/R57,
 * compositor, LCD, heap, queue, mutex, or wall-clock dependency.  It is owned
 * by the X68000 guest timeline and records only semantic facts.
 *
 * video_epoch changes only for HARD interpretation boundaries where pixels
 * from the old and new regime must not silently become one ScreenVersion:
 *   - guest video reset
 *   - output geometry/pitch change observed at a guest frame boundary
 *   - VCtrl R0 pixel/screen-mode change
 *   - CRTC mode register $29 change
 *
 * visual_seq is diagnostic ordering for softer visual-control changes such as
 * palette, priority/enable, and scroll.  A ScreenVersion may legitimately span
 * more than one visual_seq (raster effects), so visual_seq is NEVER a reject
 * condition in BAT172E0.
 */

enum {
    TAB5_GVIDEO_HARD_RESET       = 1u << 0,
    TAB5_GVIDEO_HARD_GEOMETRY    = 1u << 1,
    TAB5_GVIDEO_HARD_VCTRL_MODE  = 1u << 2,
    TAB5_GVIDEO_HARD_CRTC_MODE   = 1u << 3,
};

enum {
    TAB5_GVIDEO_VIS_PALETTE      = 1u << 0,
    TAB5_GVIDEO_VIS_PRIORITY     = 1u << 1,
    TAB5_GVIDEO_VIS_ENABLE       = 1u << 2,
    TAB5_GVIDEO_VIS_SCROLL       = 1u << 3,
    TAB5_GVIDEO_VIS_CRTC_OTHER   = 1u << 4,
};

typedef struct {
    uint32_t video_epoch;
    uint32_t visual_seq;
} tab5_guest_video_stamp_t;

typedef struct {
    uint32_t video_epoch;
    uint32_t visual_seq;
    uint32_t hard_transitions;
    uint32_t visual_transitions;
    uint32_t last_hard_cause;
    uint32_t hard_cause_or;
    uint32_t visual_cause_or;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t vctrl_mode;
    uint8_t crtc_mode;
    uint8_t geometry_seen;
    uint8_t reserved;
} tab5_guest_video_state_stats_t;

/* CPU1 guest reset fact. */
void tab5_guest_video_state_reset(void);

/* CPU1 guest frame-boundary fact.  This does not present or wait. */
void tab5_guest_video_state_frame_boundary(uint32_t width, uint32_t height,
                                           uint32_t pitch,
                                           uint8_t vctrl_mode,
                                           uint8_t crtc_mode);

/* CPU1 X68000 register-write facts. */
void tab5_guest_video_state_note_vctrl0(uint8_t new_mode);
void tab5_guest_video_state_note_vctrl1(void);
void tab5_guest_video_state_note_vctrl2(void);
void tab5_guest_video_state_note_crtc(uint8_t reg, uint8_t data);
void tab5_guest_video_state_note_palette(int graphics_palette);

/* Hot render-request stamp: CPU1-only plain loads, no atomics/locks. */
tab5_guest_video_stamp_t tab5_guest_video_state_stamp(void);

/* Sparse CPU1 diagnostic snapshot. */
void tab5_guest_video_state_get_stats(tab5_guest_video_state_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PX68K_TAB5_GUEST_VIDEO_STATE_H */
