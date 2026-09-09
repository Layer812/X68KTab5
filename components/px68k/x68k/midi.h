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
#endif
int MIDI_StateAction(StateMem *sm, int load, int data_only);

#endif /* _WINX68K_MIDI_H */
