/*
 * PX68K source modified for the Tab5 port.
 * Intent: Expose the asynchronous CPU0 YM2151 rendering interface used by the Tab5 audio split.
 * Layer8 Aug/17/2026
 */
#ifndef _WIN68_OPM_FMGEN_H
#define _WIN68_OPM_FMGEN_H

#include <stdint.h>

typedef struct {
    uint32_t sampled_frames;
    uint32_t total_cycles;
    uint32_t envelope_cycles;
    uint32_t lfo_noise_cycles;
    uint32_t channel_prep_cycles;
    uint32_t operator_cycles;
    uint32_t routing_pan_cycles;
    uint32_t post_cycles;
} OPMAsyncDetailProfile;

int OPM_Init(int clock);
void OPM_Cleanup(void);
void OPM_Reset(void);
void OPM_Update(int16_t *buffer, int length, uint8_t *pbsp, uint8_t *pbep);
void FASTCALL OPM_Write(uint32_t r, uint8_t v);
/* Build 6.15b: render `frames` of the old YM2151 state on CPU0 immediately
 * before applying this write, in the same queue event. */
void FASTCALL OPM_WriteTimed(uint32_t r, uint8_t v, uint32_t frames);
uint8_t FASTCALL OPM_Read(void);
void FASTCALL OPM_Timer(uint32_t step);
void OPM_SetVolume(uint8_t vol);
uint32_t OPM_DebugDataWriteCount(void);
uint32_t OPM_DebugKeyOnCount(void);
void OPM_R57E63TimerAuditGet(
    uint64_t *guest_clocks, uint32_t *timer_calls,
    uint32_t *tb_writes, uint32_t *tc_writes,
    uint32_t *tb_starts, uint32_t *tb_stops,
    uint32_t *tb_expires, uint32_t *tb_irq_sets,
    uint32_t *tb_status_clears,
    uint32_t *last_tb, uint32_t *last_tc,
    uint32_t *tb_period_us, uint32_t *tb_count_us);
void OPM_R57E64MixClipAuditGet(uint64_t *mixed_samples,
                               uint64_t *clipped_samples,
                               uint32_t *max_raw_abs);
int OPM_StateAction(StateMem *sm, int load, int data_only);

/* ESP32-P4: CPU0 asynchronous YM2151 waveform synthesis. */
/* R23: bind the 16 KiB generated-YM2151 PCM FIFO to static Internal SRAM.
 * PSRAM/default-heap fallback is forbidden. Returns 2=Internal, 0=failed. */
int OPM_AsyncReserveRing(void);
int OPM_AsyncRingPlacement(void);
int OPM_AsyncEnabled(void);
int OPM_AsyncRender(uint32_t frames, int profile);
uint32_t OPM_AsyncFramesAvail(void);
int OPM_AsyncMixRead(int16_t *dst, int frames);
void OPM_AsyncPerfBegin(void);
void OPM_AsyncPerfGet(uint32_t *us, uint32_t *calls, uint32_t *frames,
                      uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns);
void WinX68k_AudioAsyncGetQueueTaxonomy(
    uint32_t *write_attempt, uint32_t *write_drop, uint32_t *write_drop_frames,
    uint32_t *render_attempt, uint32_t *render_drop, uint32_t *render_drop_frames,
    uint32_t *reset_drop, uint32_t *volume_drop, uint32_t *csm_drop,
    uint32_t *stop_drop, uint32_t *other_drop);
void WinX68k_AudioAsyncGetBackpressureStats(
    uint32_t *bp_events, uint32_t *wait_calls, uint32_t *timeouts,
    uint32_t *max_timeouts, uint32_t *discard_events, uint32_t *discard_frames);
void OPM_AsyncWorkGet(uint32_t *us, uint32_t *calls, uint32_t *frames);
void OPM_AsyncDetailProfileGet(OPMAsyncDetailProfile *out);
void OPM_AsyncSemanticAlgoGet(uint32_t out[8]);
/* R23 Internal-only FM memory/stack diagnostics. */
uint32_t OPM_AsyncStackHighWater(void);
uint32_t OPM_AsyncRingBytes(void);
int OPM_AsyncInternalOnly(void);
int OPM_AsyncSpmControlOk(void);

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
