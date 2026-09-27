#ifndef _WINX68K_MIDI_H
#define _WINX68K_MIDI_H

#include <stdint.h>
#include "common.h"

void MIDI_Init(void);
void MIDI_Cleanup(void);
void MIDI_Reset(void);
uint8_t FASTCALL MIDI_Read(uint32_t adr);
void FASTCALL MIDI_Write(uint32_t adr, uint8_t data);
void FASTCALL MIDI_Timer(uint32_t clk);
int MIDI_SetMimpiMap(char *filename);
int MIDI_EnableMimpiDef(int enable);
void MIDI_DelayOut(unsigned int delay);
#ifdef ESP_PLATFORM
extern uint8_t MIDI_R127DelayPending;
extern uint8_t MIDI_R127TimerActive;
/* X68KTAB_R1A20_EVENT_DEADLINE_CORE: preserve the proven R1A13 1 ms
 * YM3802 service quantum, but remove MIDI_Timer() from every active scanline. */
extern volatile uint32_t g_x68p4_midi_lazy_pending;
extern volatile uint32_t g_x68p4_midi_lazy_deadline;
void FASTCALL MIDI_R1A20MaterializeDeadline(void);

/* X68KTAB_R1A4_HF1_ONEPASS_FINAL_ORACLE_R2
 * Diagnostic-only cross-subsystem event IDs. CPU1 only writes the lock-free
 * ring; a low-priority CPU0 task performs serial output. */
enum {
    MIDI_OP_HFS_REQ = 1, MIDI_OP_HFS_DONE, MIDI_OP_HFS_PATH, MIDI_OP_HFS_NAME,
    MIDI_OP_HFS_FILE, MIDI_OP_HFS_OPEN, MIDI_OP_HFS_READ, MIDI_OP_HFS_CLOSE,
    MIDI_OP_YM_W = 16, MIDI_OP_YM_R, MIDI_OP_GT_FIRE, MIDI_OP_MT_FIRE,
    MIDI_OP_TX_EMPTY, MIDI_OP_IRQ_ASSERT, MIDI_OP_IACK, MIDI_OP_TDR,
    MIDI_OP_HOST_SYSEX, MIDI_OP_MIDI_INIT, MIDI_OP_TIMER_ARM, MIDI_OP_IRQ_CLEAR,
    MIDI_OP_HOST_SUBMIT, MIDI_OP_HFS_ENUM, MIDI_OP_HFS_FAIL
};
void MIDI_OnePassEnsureStarted(void);
void MIDI_OnePassAdvanceGuestCycles(uint32_t cycles, uint32_t pc);
void MIDI_OnePassTrace(uint8_t type, uint8_t a, uint16_t z, uint32_t x, uint32_t y);
#endif
int MIDI_StateAction(StateMem *sm, int load, int data_only);

#endif /* _WINX68K_MIDI_H */
