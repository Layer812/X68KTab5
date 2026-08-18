#ifndef _DSWIN_H
#define _DSWIN_H

#include "common.h"

void DSound_Play(void);
void DSound_Stop(void);
void DSound_Send0(int32_t clock);
void DSound_FlushPending(void);
int DSound_ReadFrames(int16_t *dst, int max_frames);
int DSound_HostFramesAvail(void);
int DSound_HostReadFrames(int16_t *dst, int max_frames);
uint32_t DSound_HostProducedFrames(void);
void DSound_PerfSetSample(int enabled);
void DSound_PerfGetLast(uint32_t *adpcm_us, uint32_t *opm_us, uint32_t *mix_calls, uint32_t *mix_frames);
void DSound_AsyncGetStats(uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns, uint32_t *fm_avail);

int audio_samples_avail(void);
void audio_samples_discard(int discard);
void raudio_callback(void *userdata, unsigned char *stream, int len);
int dswin_StateAction(StateMem *sm, int load, int data_only);

#endif /* _DSWIN_H */
