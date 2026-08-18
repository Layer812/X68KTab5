/*
 * Tab5 port-specific implementation.
 * Intent: Compact BG/SP snapshot format shared with the Tab5 CPU0 compositor.
 * Layer8 Aug/17/2026
 */
#ifndef _WINX68K_BG_HOST_STATE_H
#define _WINX68K_BG_HOST_STATE_H

#include <stdint.h>

/* Build 5.50a: dependency-light raster snapshot shared with the Tab5 host
 * compose worker.  Keep this header free of libretro/common.h so application
 * code can consume the state without pulling FASTCALL/StateMem dependencies. */
typedef struct BG_HOST_LINE_STATE {
    uint16_t bg0_top;
    uint16_t bg1_top;
    uint32_t bg0_scroll_x;
    uint32_t bg0_scroll_y;
    uint32_t bg1_scroll_x;
    uint32_t bg1_scroll_y;
    int32_t h_adjust;
    int32_t bg_vline;
    uint32_t vline_bg;
    uint8_t chr_size;
    uint8_t reg9;
    uint8_t gd;
    uint8_t sprite_count[3];
    uint8_t sprite_idx[3][128];
} BG_HOST_LINE_STATE;

#endif /* _WINX68K_BG_HOST_STATE_H */
