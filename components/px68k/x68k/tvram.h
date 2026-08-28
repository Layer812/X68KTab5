#ifndef _WINX68K_TVRAM_H
#define _WINX68K_TVRAM_H

#include <stdint.h>
#include "common.h"

extern	uint8_t	TVRAM[0x80000];
extern	uint8_t	TextDirtyLine[1024];
extern	uint8_t	Text_TrFlag[1024];

void TVRAM_SetAllDirty(void);
enum {
    TAB5_DIRTY_ALL_OTHER = 0,
    TAB5_DIRTY_ALL_CRTC,
    TAB5_DIRTY_ALL_VCTRL,
    TAB5_DIRTY_ALL_PALETTE,
    TAB5_DIRTY_ALL_BG_REG,
    TAB5_DIRTY_ALL_BG_MEM,
    TAB5_DIRTY_ALL_FASTCLR,
    TAB5_DIRTY_ALL_N
};
void TVRAM_SetAllDirtyReason(uint32_t reason);
void TVRAM_DebugDirtyAllTake(uint32_t *out, uint32_t count);

void TVRAM_Init(void);
void TVRAM_Cleanup(void);

uint8_t FASTCALL TVRAM_Read(uint32_t adr);
void FASTCALL TVRAM_Write(uint32_t adr, uint8_t data);
void FASTCALL TVRAM_RCUpdate(void);
void FASTCALL Text_DrawLine(int opaq);
#ifdef ESP_PLATFORM
const uint8_t *TVRAM_GetExpandedLine(uint32_t y, uint32_t x, uint32_t width);
const uint8_t *TVRAM_GetExpandedPixels(void);
uint8_t *TVRAM_GetR57ShadowStorage(void);
void TVRAM_Tab5TextFastStatsTake(uint32_t *fast_lines, uint32_t *fallback_lines,
                                 uint32_t *fast_blocks);
uint32_t TVRAM_Tab5TextWriteEpoch(void);
/* BAT177NW18/R57E48: decode the common visible TEXT span to raw 4-bit
 * palette indices without materializing RGB565 or Text_TrFlag. */
int TVRAM_Tab5DecodeVisibleIndexLine(uint8_t *out, uint32_t width);
#endif
int TVRAM_StateAction(StateMem *sm, int load, int data_only);

#endif /* _WINX68K_TVRAM_H */
