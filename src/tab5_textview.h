/*
 * Tab5 port-specific implementation.
 * Intent: Interface for the Tab5 host-side X68000 text view.
 * Layer8 Aug/17/2026
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint16_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
    uint32_t nonzero_pixels;
    uint32_t hash;
    uint32_t expanded_mismatch_pixels;
    int palette_active;

    /* Exact PX68K text viewport state used for this rendered frame. */
    uint32_t scroll_x;
    uint32_t scroll_y;
    uint32_t text_dot_x;
    uint32_t text_dot_y;
    int double_scan;
} tab5_textview_frame_t;

int tab5_textview_init(void);
int tab5_textview_render(tab5_textview_frame_t *out);

#ifdef __cplusplus
}
#endif
