/*
 * PX68K source modified for the Tab5 port.
 * Intent: Expose the asynchronous CPU0 YM2151 rendering interface used by the Tab5 audio split.
 * Layer8 Aug/17/2026
 */
#ifndef _WIN68_OPM_FMGEN_H
#define _WIN68_OPM_FMGEN_H

#include <stdint.h>

int OPM_Init(int clock);
void OPM_Cleanup(void);
void OPM_Reset(void);
void OPM_Update(int16_t *buffer, int length, uint8_t *pbsp, uint8_t *pbep);
void FASTCALL OPM_Write(uint32_t r, uint8_t v);
uint8_t FASTCALL OPM_Read(void);
void FASTCALL OPM_Timer(uint32_t step);
void OPM_SetVolume(uint8_t vol);
uint32_t OPM_DebugDataWriteCount(void);
uint32_t OPM_DebugKeyOnCount(void);
int OPM_StateAction(StateMem *sm, int load, int data_only);

/* ESP32-P4: CPU0 asynchronous YM2151 waveform synthesis. */
int OPM_AsyncEnabled(void);
int OPM_AsyncRender(uint32_t frames, int profile);
uint32_t OPM_AsyncFramesAvail(void);
int OPM_AsyncMixRead(int16_t *dst, int frames);
void OPM_AsyncPerfBegin(void);
void OPM_AsyncPerfGet(uint32_t *us, uint32_t *calls, uint32_t *frames,
                      uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns);

int M288_Init(int clock, const char* path);
void M288_Cleanup(void);
void M288_Reset(void);
void M288_Update(int16_t *buffer, size_t length);
void FASTCALL M288_Write(uint32_t r, uint8_t v);
uint8_t FASTCALL M288_Read(uint16_t a);
void FASTCALL M288_Timer(uint32_t step);
void M288_SetVolume(uint8_t vol);
void M288_RomeoOut(unsigned int delay);

#endif /* _WIN68_OPM_FMGEN_H */
