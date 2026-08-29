/*
 * PX68K source modified for the Tab5 port.
 * Intent: Tab5 MFP timing/cache declarations used by the standalone scheduler.
 * Layer8 Aug/17/2026
 */
#ifndef _WINX68K_MFP_H
#define _WINX68K_MFP_H

#include <stdint.h>
#include "common.h"

extern	uint8_t MFP[24];

#define MFP_GPIP	0
#define MFP_AER		1
#define MFP_DDR		2
#define MFP_IERA	3
#define MFP_IERB	4
#define MFP_IPRA	5
#define MFP_IPRB	6
#define MFP_ISRA	7
#define MFP_ISRB	8
#define MFP_IMRA	9
#define MFP_IMRB	10
#define MFP_VR		11
#define MFP_TACR	12
#define MFP_TBCR	13
#define MFP_TCDCR	14
#define MFP_TADR	15
#define MFP_TBDR	16
#define MFP_TCDR	17
#define MFP_TDDR	18
#define MFP_SCR		19
#define MFP_UCR		20
#define MFP_RSR		21
#define MFP_TSR		22
#define MFP_UDR		23

extern uint8_t LastKey;

void MFP_Init(void);
uint8_t FASTCALL MFP_Read(uint32_t adr);
void FASTCALL MFP_Write(uint32_t adr, uint8_t data);
/* Build 5.91: caller-side no-op gates.  The cached mask is refreshed on every
 * timer-control write, reset and state load.  When no ordinary timer is active
 * there is no state change at all, so skip the cross-TU call. */
extern uint8_t MFP_TimerActiveMask;
/* R57E71: cache the measured steady-state MDX B/C tuple at register-write
 * time instead of re-testing six control/reload bytes every 200-cycle slice. */
extern uint8_t MFP_TimerFastMode;
void FASTCALL MFP_TimerR71ExactBC(int32_t clock);
void FASTCALL MFP_TimerSlow(int32_t clock);
void MFP_Tab5TimerFastStats(uint32_t *exact_bc, uint32_t *fallback);
static inline __attribute__((always_inline)) void MFP_Timer(int32_t clock)
{
#ifdef ESP_PLATFORM
    if (__builtin_expect(MFP_TimerFastMode == 1u, 0)) {
        MFP_TimerR71ExactBC(clock);
        return;
    }
#endif
    if (__builtin_expect(MFP_TimerActiveMask != 0, 0))
        MFP_TimerSlow(clock);
}

/* Timer-A event-count mode exists only for TACR low nibble == 8.  Preserve
 * the old exact per-scanline event timing while avoiding a function call when
 * the mode is inactive. */
void FASTCALL MFP_TimerASlow(void);
static inline __attribute__((always_inline)) void MFP_TimerA(void)
{
    if (__builtin_expect((MFP[MFP_TACR] & 15u) == 8u, 0))
        MFP_TimerASlow();
}
void MFP_Int(int irq);
int MFP_StateAction(StateMem *sm, int load, int data_only);

/* Tab5 standalone keyboard/MFP pipeline diagnostics. */
uint32_t MFP_DebugUDRReads(void);
uint32_t MFP_DebugRSRReads(void);
uint32_t MFP_DebugKeyboardIRQCalls(void);
uint32_t MFP_DebugKeyboardIRQEnabled(void);
uint32_t MFP_DebugKeyboardIRQDisabled(void);
uint8_t  MFP_DebugLastUDR(void);

#endif /* _WINX68K_MFP_H */
