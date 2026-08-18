/*
 * PX68K source modified for the Tab5 port.
 * Intent: Palette-generation hooks used by the Tab5 dirty compositor.
 * Layer8 Aug/17/2026
 */
#ifndef _WINX68K_PAL_H
#define _WINX68K_PAL_H

#include <stdint.h>
#include "common.h"

extern uint8_t	Pal_Regs[1024];
extern uint16_t TextPal[256];
extern uint16_t GrphPal[256];
extern uint16_t Pal16[65536];

void Pal_SetColor(void);
void Pal_Init(void);

uint8_t FASTCALL Pal_Read(uint32_t adr);
void FASTCALL Pal_Write(uint32_t adr, uint8_t data);
void Pal_ChangeContrast(int num);

/* Build 5.8 Tab5 palette activity diagnostics (read-only). */
uint32_t Pal_DebugGrphWriteCount(void);
uint32_t Pal_DebugTextWriteCount(void);
uint32_t Pal_DebugLastAddr(void);
uint8_t Pal_DebugLastData(void);
int Pal_StateAction(StateMem *sm, int load, int data_only);

extern uint16_t Ibit, Pal_HalfMask, Pal_Ix2;

#endif /* _WINX68K_PAL_H */
