#ifndef _WINX68K_WINDRAW_H
#define _WINX68K_WINDRAW_H

#include <stdint.h>

extern uint16_t WinDraw_Pal16B, WinDraw_Pal16R, WinDraw_Pal16G;

void WinDraw_Init(void);
void WinDraw_Cleanup(void);
void FASTCALL WinDraw_Draw(void);
void WinDraw_DrawLine(void);
void WinDraw_PerfSetSample(int enabled);
void WinDraw_PerfGetLast(uint32_t *grp_us, uint32_t *text_us, uint32_t *bg_us,
                         uint32_t *blend_us, uint32_t *clear_us,
                         uint32_t *dirty_lines, uint32_t *grp_calls,
                         uint32_t *text_calls, uint32_t *bg_calls,
                         uint32_t *blend_calls);
void WinDraw_PerfGetR57E40(uint32_t *gbt_us, uint32_t *commit_us,
                           uint32_t *gbt_calls, uint32_t *commit_calls,
                           uint32_t *arm_count, uint32_t *active);
void WinDraw_PerfGetR57E44(uint32_t *fused_lines, uint32_t *fallback_lines,
                           uint32_t *cache_rebuilds, uint32_t *cache_failures);
void WinDraw_PerfGetR57E48(uint32_t *direct_lines, uint32_t *fallback_lines,
                           uint32_t *text_reject, uint32_t *bg_reject,
                           uint32_t *final_reject);

int WinDraw_MenuInit(void);
void WinDraw_DrawMenu(int menu_state, int mkey_pos, int mkey_y, int *mval_y);

extern struct menu_flist mfl;

void WinDraw_DrawMenufile(struct menu_flist *mfl);
void WinDraw_ClearMenuBuffer(void);

#endif /* _WINX68K_WINDRAW_H */
