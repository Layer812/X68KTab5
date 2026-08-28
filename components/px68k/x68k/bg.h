#ifndef _WINX68K_BG_H
#define _WINX68K_BG_H

#include <stdint.h>
#include "common.h"
#include "bg_host_state.h"

extern	uint8_t	BG_DrawWork0[1024*1024];
extern	uint8_t	BG_DrawWork1[1024*1024];
extern	uint8_t	BG_Regs[0x12];
extern	int32_t	BG_HAdjust;
extern	int32_t	BG_VLINE;
extern	uint32_t VLINEBG;

extern	uint8_t	Sprite_DrawWork[1024*1024];
extern	uint16_t BG_LineBuf[1600];

int BG_CaptureHostLineState(BG_HOST_LINE_STATE *out, uint32_t vline_bg, int gd);

void BG_Init(void);

uint8_t FASTCALL BG_Read(uint32_t adr);
void FASTCALL BG_Write(uint32_t adr, uint8_t data);

void FASTCALL BG_DrawLine(int opaq, int gd);

/* BAT177NW6/R57E36: BG-map local-dirty telemetry. */
void BG_Tab5DirtyPruneStatsTake(uint32_t *pattern_full,
                                uint32_t *map0_local,
                                uint32_t *map1_local,
                                uint32_t *local_lines,
                                uint32_t *inactive_skip,
                                uint32_t *inv_rebuilds,
                                uint32_t *inv_visits,
                                uint32_t *sprite_mask_rebuilds,
                                uint32_t *sprite_mask_queries);
/* BAT177NW7/R57E37: conservative current TextPal bank reachability. */
uint32_t BG_Tab5TextPalBankMayAffect(uint32_t bank);
/* BAT177NW12/R57E42: BG1/BG0 first-writer and sprite-Y telemetry. */
void BG_Tab5RenderFastStatsTake(uint32_t *bg1_first_hits,
                                uint32_t *bg1_first_fallback,
                                uint32_t *bg0_first_hits,
                                uint32_t *bg0_first_fallback,
                                uint32_t *sprite_y_rebuilds,
                                uint32_t *sprite_y_probes,
                                uint32_t *sprite_y_items);
/* BAT177NW13/R57E43: exact fused BG1+BG0 8px renderer telemetry. */
void BG_Tab5FuseStatsTake(uint32_t *lines, uint32_t *fallback, uint32_t *blocks);
/* BAT177NW18/R57E48: common 8px BG1+BG0 path to palette indices.
 * out_full uses the historical BG line coordinate space (visible starts at 16). */
int BG_Tab5DecodeIndexLine(uint8_t *out_full, uint32_t capacity, int gd);

int BG_StateAction(StateMem *sm, int load, int data_only);

#endif /* _WINX68K_BG_H */
