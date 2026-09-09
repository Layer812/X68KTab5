#ifndef _DSWIN_H
#define _DSWIN_H

#include "common.h"

void DSound_Play(void);
void DSound_Stop(void);
void DSound_Send0(int32_t clock);
void DSound_SetHostSourceRate(uint32_t rate);
uint32_t DSound_GetHostSourceRate(void);
uint64_t DSound_AbsGuestTick64(void);
void DSound_FlushPending(void);
/* Build 6.15b: split audible-state boundaries so dense YM2151 writes do not
 * force CPU1 ADPCM generation. */
void DSound_FlushADPCMPending(void);
void DSound_OPMWrite(uint32_t adr, uint8_t data);
int DSound_ReadFrames(int16_t *dst, int max_frames);
int DSound_HostFramesAvail(void);
int DSound_HostReadFrames(int16_t *dst, int max_frames);
uint32_t DSound_HostProducedFrames(void);
/* R140P4: host source completion wake.  FM/ADPCM producers signal the CPU0
 * feeder when a new common-timeline chunk may have become consumable. */
void DSound_SetHostSourceReadyCallback(void (*cb)(void));
void DSound_HostSourceReady(void);

int audio_samples_avail(void);
void audio_samples_discard(int discard);
void raudio_callback(void *userdata, unsigned char *stream, int len);
int dswin_StateAction(StateMem *sm, int load, int data_only);

#endif /* _DSWIN_H */
