#ifndef _WINX68K_ADPCM_H
#define _WINX68K_ADPCM_H

#include <stdint.h>

void FASTCALL ADPCM_PreUpdate(uint32_t clock);
void ADPCM_Update(int16_t *buffer, size_t length, uint8_t *pbsp, uint8_t *pbep);

void FASTCALL ADPCM_Write(uint32_t adr, uint8_t data);
uint8_t FASTCALL ADPCM_Read(uint32_t adr);
uint32_t ADPCM_DebugControlWriteCount(void);
uint32_t ADPCM_DebugDataWriteCount(void);
int ADPCM_DebugPlaying(void);
/* ESP32-P4 R23 decode-staging SRAM diagnostics. */
uint32_t ADPCM_Tab5BufferCapacity(void);
uint32_t ADPCM_Tab5BufferHighWater(void);
uint32_t ADPCM_Tab5BufferOverflows(void);
int ADPCM_Tab5SpmStateOk(void);

void ADPCM_SetVolume(uint8_t vol);
void ADPCM_SetPan(int n);
void ADPCM_SetClock(int n);

void ADPCM_Init(void);
int ADPCM_StateAction(StateMem *sm, int load, int data_only);

#endif /* _WINX68K_ADPCM_H */
