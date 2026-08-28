/*
 * PX68K source modified for the Tab5 port.
 * Intent: Generation and snapshot helpers added for the Tab5 dirty-video pipeline.
 * Layer8 Aug/17/2026
 */
#ifndef _WINX68K_GVRAM_H
#define _WINX68K_GVRAM_H

#include <stdint.h>
#include "common.h"

extern	uint8_t	GVRAM[0x80000];
/* Build 6.14c: physical 512-row generation counters for the host scroll cache. */
extern volatile uint32_t GVRAM_RowGeneration[512];
uint32_t GVRAM_RowGenerationGet(uint32_t row);
extern	uint16_t	Grp_LineBuf[1024];
extern	uint16_t	Grp_LineBufSP[1024];
extern	uint16_t	Grp_LineBufSP2[1024];

void GVRAM_Init(void);

void FASTCALL GVRAM_FastClear(void);

uint8_t FASTCALL GVRAM_Read(uint32_t adr);
void FASTCALL GVRAM_Write(uint32_t adr, uint8_t data);

/* Build 5.76 stream backends for contiguous 68000 word stores in the
 * 256-color/compatible GVRAM mode.  Both preserve GVRAM_Write-visible
 * dirty/debug state and return count on success, 0 when the current mode/
 * range is not eligible and the caller must use the ordinary write path.
 * src_native_words points at PX68K ordinary RAM in its native word-swapped
 * representation; last_word receives the final guest word for MOVE flags. */
uint32_t GVRAM_WriteWordRepeat256(uint32_t adr, uint16_t data, uint32_t count);
uint32_t GVRAM_WriteWordCopy256(uint32_t adr, const uint8_t *src_native_words,
                                uint32_t count, uint16_t *last_word);
/* Validated P4 PIE/XespV stream backend. Eligibility is separate so Musashi
 * can use the fixed production repeat path with scalar fallback. */
int GVRAM_WriteWordRepeat256P4Eligible(uint32_t adr, uint32_t count);
int GVRAM_WriteWordCopy256P4Eligible(uint32_t adr, const uint8_t *src_native_words,
                                     uint32_t count);
uint32_t GVRAM_WriteWordRepeat256P4(uint32_t adr, uint16_t data, uint32_t count,
                                    uint32_t *pie_words);
uint32_t GVRAM_WriteWordCopy256P4(uint32_t adr, const uint8_t *src_native_words,
                                  uint32_t count, uint16_t *last_word,
                                  uint32_t *pie_words);
int GVRAM_P4StreamSelfcheck(void);
uint32_t GVRAM_CopyWordStream256(uint32_t src_adr, uint32_t dst_adr,
                                 uint32_t count, uint16_t *last_word);

/* Build 5.8 Tab5 graphics activity diagnostics (read-only). */
uint32_t GVRAM_DebugWriteCount(void);
uint32_t GVRAM_DebugFastClearCount(void);
uint32_t GVRAM_DebugLastAddr(void);
uint8_t GVRAM_DebugLastData(void);
uint8_t GVRAM_DebugLastMode(void);

void Grp_DrawLine16(void);
/* BAT177NW14/R57E44: exact common 65K GRP decode + GBT selector fusion.
 * Returns 1 when fused rendering completed, 0 when caller must materialize
 * Grp_LineBuf and use the retained R57E34 selector. */
int Grp_DrawLine16GBT(uint16_t *dst, const uint16_t *bt, const uint8_t *flags,
                      uint32_t width, uint8_t grp_pri, uint8_t bg_pri,
                      uint8_t text_pri);
int Grp_DrawLine16GBT_SelfCheck(void);
void Grp_DrawLine16GBT_DebugGet(uint32_t *cache_rebuilds, uint32_t *cache_failures);
/* BAT177NW18/R57E48: direct final compositor. TEXT/BG are compact palette
 * indices; raw 65K GVRAM decode and G/B/T priority resolution happen once. */
int Grp_DrawLine16TBGI(uint16_t *dst, const uint8_t *text_idx,
                       const uint8_t *bg_idx, uint32_t width,
                       uint8_t grp_pri, uint8_t bg_pri, uint8_t text_pri);
int Grp_DrawLine16TBGI_SelfCheck(void);
void FASTCALL Grp_DrawLine8(int page, int opaq);
void FASTCALL Grp_DrawLine8Pair(int bottom_page, int top_page);
void FASTCALL Grp_DrawLine4(uint32_t page, int opaq);
void FASTCALL Grp_DrawLine4h(void);
void FASTCALL Grp_DrawLine16SP(void);
void FASTCALL Grp_DrawLine8SP(int page/*, int opaq*/);
void FASTCALL Grp_DrawLine4SP(uint32_t page/*, int opaq*/);
void FASTCALL Grp_DrawLine4hSP(void);
void FASTCALL Grp_DrawLine8TR(int page, int opaq);
void FASTCALL Grp_DrawLine8TR_GT(int page, int opaq);
void FASTCALL Grp_DrawLine4TR(uint32_t page, int opaq);
int GVRAM_StateAction(StateMem *sm, int load, int data_only);

#endif /* _WINX68K_GVRAM_H */
