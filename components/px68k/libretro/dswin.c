/* 
 * Copyright (c) 2003 NONAKA Kimihiro
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Separate guest-timed ADPCM production from host PCM consumption so CPU1 no longer performs final FM+ADPCM mixing or speaker-bound copies.
 * Layer8 Aug/17/2026
 */
#include        <stdint.h>
#include	"common.h"
#include	"dswin.h"
#include	"prop.h"
#include	"adpcm.h"
#include    "adpcm_cpu0.h"
#include	"mercury.h"
#include	"fmg_wrap.h"

#ifdef ESP_PLATFORM
/* R140P1: async YM2151 backend is mandatory on the Tab5 production path.
 * Use the init/cleanup latch directly rather than crossing C -> C++ for
 * TAB5_OPM_ASYNC_ENABLED() at every audio boundary/read. */
extern volatile int g_x68p4_opm_async_ready;
#define TAB5_OPM_ASYNC_ENABLED() (__builtin_expect(g_x68p4_opm_async_ready != 0, 1))
#else
#define TAB5_OPM_ASYNC_ENABLED() OPM_AsyncEnabled()
#endif

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_attr.h"
extern int OPM_AbsTimelineRenderCPU0(uint32_t frames);
extern int OPM_AbsTimelineWriteCPU0(uint8_t reg,uint8_t data);
extern int OPM_AbsTimelineResetCPU0(void);
extern int OPM_AbsTimelineVolumeCPU0(uint8_t vol);
extern int OPM_AbsTimelineCSMCPU0(void);
#endif

#ifndef PX68K_TAB5_R57E63_AUDIO_AUDIT
#define PX68K_TAB5_R57E63_AUDIO_AUDIT 0
#endif

/* Build 6.15b: FM and ADPCM pending timelines are tracked separately.
 * OPM writes can now advance only the FM timeline on CPU0 without forcing
 * CPU1 ADPCM generation. */
static int s_pending_adpcm_frames = 0;
static int s_pending_opm_frames = 0;

/* Build 6.15b: 256 frames (~5.8 ms) remains the steady-state batching
 * quantum, but OPM and ADPCM boundaries are now independent. */
#define TAB5_AUDIO615B_ASYNC_QUANTUM 256

/* R140P4: completion-driven host wake.  The callback is registered once by
 * the Tab5 host before guest execution starts.  Keeping the callback here
 * lets both the ADPCM command producer and the YM2151 CPU0 renderer publish
 * the same "source may now be consumable" event without coupling either
 * source to the host task implementation. */
static void (*volatile s_host_source_ready_cb)(void) = NULL;
void DSound_SetHostSourceReadyCallback(void (*cb)(void))
{
    s_host_source_ready_cb = cb;
}
void DSound_HostSourceReady(void)
{
    void (*cb)(void) = s_host_source_ready_cb;
    if (cb) cb();
}
#ifdef ESP_PLATFORM
/* Intent: CPU1 remains authoritative for guest-timed ADPCM state; CPU0 consumes only samples that CPU1 has already published.  Layer8 Aug/17/2026 */
/* BAT177NW0: CPU1 is the single ADPCM producer, CPU0 the single final-mix
 * consumer. This is a true SPSC ring: sample bytes are written before CPU1
 * release-publishes pbwp, and CPU0 finishes copy/mix before release-publishing
 * pbrp. No cross-core portMUX or retry is permitted on CPU1. */
/* Production clean: historical host-produced flight-recorder counter removed.
 * Host flow control uses actual ring availability, not a monotonic statistic. */
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
static volatile uint64_t s_r57e63_adpcm_generated_frames = 0;
static volatile uint64_t s_r57e63_adpcm_probe_samples = 0;
static volatile uint64_t s_r57e63_adpcm_nonzero_samples = 0;
static volatile uint32_t s_r57e63_adpcm_peak = 0;
#endif
#endif

/* R12 AUDIO-LOCAL: 32768 stereo frames = 128 KiB.
 * This is still ~743 ms at 44.1 kHz, while returning 64 KiB of static SRAM. */
#define PCMBUF_FRAMES 32768u
#define PCMBUF_SIZE (2u * 2u * PCMBUF_FRAMES)

static uint8_t pcmbuffer[PCMBUF_SIZE];
/* R12: legacy 192 KiB rsndbuf removed.  The wrap case copies its two spans
 * directly into the caller's destination, so no contiguous bounce buffer is
 * required on Tab5 or legacy frontends. */
static int32_t snd_precounter = 0;
#ifdef ESP_PLATFORM
static DRAM_ATTR volatile uint32_t s_r1a17_send0_calls = 0;
static DRAM_ATTR volatile uint32_t s_r1a17_send0_zero = 0;
static DRAM_ATTR volatile uint32_t s_r1a17_send0_frames = 0;
static DRAM_ATTR volatile uint32_t s_r1a17_flush_calls = 0;
#endif

/* P12R1 product source clock: fixed 44.1 kHz.  No runtime sample-rate
 * selector remains in the standalone product hot path. */
#define P12R1_SOURCE_RATE 44100u
#define P12R6A4HF4_EXACT_PRODUCTION44_RATE_FIX 1
/* P12R6A4HF4: exact extension of the captured P12R1 production-fixed 44.1-kHz
 * source domain to explicit 44.1/22.05/11.025-kHz native rates. */
static volatile uint32_t s_p12r6a4_source_rate = P12R1_SOURCE_RATE;

static inline uint32_t p12r6a4_normalize_source_rate(uint32_t rate)
{
    if (rate <= 11025u) return 11025u;
    if (rate <= 22050u) return 22050u;
    return 44100u;
}


#ifdef ESP_PLATFORM
/* P12R1: one absolute 44.1-kHz source-domain guest sample axis. */
static uint64_t s_abs_guest_audio_tick = 0u;
int DSound_AbsTimelineActive(void);
int DSound_AbsTimelineADPCMControl(uint8_t data);
int DSound_AbsTimelineADPCMData(uint8_t data);
int DSound_AbsTimelineADPCMPan(uint8_t data);
int DSound_AbsTimelineADPCMClock(uint8_t data);
int DSound_AbsTimelineADPCMVolume(uint8_t data);
int DSound_AbsTimelineOPMWrite(uint8_t reg,uint8_t data);
int DSound_AbsTimelineOPMCSM(void);
int DSound_AbsTimelineOPMReset(void);
int DSound_AbsTimelineOPMVolume(uint8_t vol);
static int abs_timeline_epoch_reset(void);
#endif

void DSound_SetHostSourceRate(uint32_t rate)
{
    const uint32_t r = p12r6a4_normalize_source_rate(rate);
    __atomic_store_n(&s_p12r6a4_source_rate, r, __ATOMIC_RELEASE);
#ifdef ESP_PLATFORM
    ADPCM_CPU0_SetSourceRate(r);
    OPM_AsyncSetSourceRate(r);
#endif
}

uint32_t DSound_GetHostSourceRate(void)
{
    return __atomic_load_n(&s_p12r6a4_source_rate, __ATOMIC_ACQUIRE);
}

void DSound_SetHostSourceRateRed11025(void)
{
    DSound_SetHostSourceRate(11025u);
}

uint64_t DSound_AbsGuestTick64(void)
{
#ifdef ESP_PLATFORM
   return s_abs_guest_audio_tick;
#else
   return 0u;
#endif
}


uint8_t *pbsp = pcmbuffer;
uint8_t *pbrp = pcmbuffer, *pbwp = pcmbuffer;
uint8_t *pbep = &pcmbuffer[PCMBUF_SIZE];

#ifdef ESP_PLATFORM
static inline uint8_t *pcm_load_wp(void) { return __atomic_load_n(&pbwp, __ATOMIC_ACQUIRE); }
static inline uint8_t *pcm_load_rp(void) { return __atomic_load_n(&pbrp, __ATOMIC_ACQUIRE); }
static inline void pcm_store_wp(uint8_t *p) { __atomic_store_n(&pbwp, p, __ATOMIC_RELEASE); }
static inline void pcm_store_rp(uint8_t *p) { __atomic_store_n(&pbrp, p, __ATOMIC_RELEASE); }
#else
static inline uint8_t *pcm_load_wp(void) { return pbwp; }
static inline uint8_t *pcm_load_rp(void) { return pbrp; }
static inline void pcm_store_wp(uint8_t *p) { pbwp = p; }
static inline void pcm_store_rp(uint8_t *p) { pbrp = p; }
#endif

int dswin_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFVAR(snd_precounter),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_DSND", false);

	return ret;
}

void DSound_Play(void)
{
	ADPCM_SetVolume((uint8_t)Config.PCM_VOL);
	OPM_SetVolume((uint8_t)Config.OPM_VOL);	
}

void DSound_Stop(void)
{
	ADPCM_SetVolume(0);
	OPM_SetVolume(0);	
}


#if defined(ESP_PLATFORM) && PX68K_TAB5_R57E63_AUDIO_AUDIT
static inline void r57e63_probe_adpcm(uint8_t *start, int length)
{
    if (!start || length <= 0) return;
    s_r57e63_adpcm_generated_frames += (uint64_t)length;

    /* Probe one stereo frame out of every 16. This is enough to prove that
     * decoded MSM6258/PCM8 content is non-zero without making the audit itself
     * a meaningful audio workload. */
    uint8_t *p = start;
    for (int i = 0; i < length; i += 16)
    {
        const int16_t *sp = (const int16_t *)p;
        int32_t l = sp[0];
        int32_t r = sp[1];
        uint32_t al = (uint32_t)(l < 0 ? -l : l);
        uint32_t ar = (uint32_t)(r < 0 ? -r : r);
        uint32_t peak = al > ar ? al : ar;
        ++s_r57e63_adpcm_probe_samples;
        if (peak > 16u)
            ++s_r57e63_adpcm_nonzero_samples;
        if (peak > s_r57e63_adpcm_peak)
            s_r57e63_adpcm_peak = peak;

        p += 16 * 4;
        while (p >= pbep) p = pbsp + (p - pbep);
    }
}
#endif


#ifdef ESP_PLATFORM
/* R57E107X_CPU0_ADPCM_COMMAND_ENGINE
 * Lossless ordered SPSC command stream: CPU1 produces only state changes,
 * packed ADPCM bytes and RENDER(frame-count) boundaries.  CPU0 decodes the
 * MSM6258 and produces 44.1-kHz stereo PCM immediately before the existing
 * CPU0 YM2151 final mix.  CPU0 never reads X68000 guest memory. */
#define ADCPU_CMD_LEN 4096u
#define ADCPU_CMD_MASK (ADCPU_CMD_LEN - 1u)
#define ADCPU_OUT_LEN 8192u
#define ADCPU_OUT_MASK (ADCPU_OUT_LEN - 1u)
#define ADCPU_RAW_LEN 2048u
#define ADCPU_RAW_MASK (ADCPU_RAW_LEN - 1u)
#define ADCPU_SAMPLE_RATE_X12_DEFAULT (44100u * 12u)
static volatile uint32_t s_adcpu_rate_req_x12 = ADCPU_SAMPLE_RATE_X12_DEFAULT;
static uint32_t s_adcpu_rate_x12 = ADCPU_SAMPLE_RATE_X12_DEFAULT;
#define ADCPU_IPSCALE 256L
#define ADCPU_MAX 2047
#define ADCPU_MIN (-2048)

typedef struct { uint32_t w0, w1; } adcpu_cmd_t;
enum {
    ADCPU_RESET=1, ADCPU_CONTROL, ADCPU_DATA_PACK,
    ADCPU_PAN, ADCPU_CLOCK, ADCPU_VOLUME, ADCPU_RENDER
};

static adcpu_cmd_t *s_adcpu_cmd;
static uint32_t *s_adcpu_out_ring;
static uint32_t *s_adcpu_raw_ring;
static volatile uint32_t s_adcpu_cmd_head, s_adcpu_cmd_tail;
static uint32_t s_adcpu_out_head, s_adcpu_out_tail;
static uint32_t s_adcpu_raw_head, s_adcpu_raw_tail;
static volatile uint32_t s_adcpu_active;
static volatile uint32_t s_adcpu_drops;
/* Producer-only 4-byte pack: cuts CPU1 PSRAM/atomic command traffic ~4x. */
static uint32_t s_adcpu_pack;
static uint8_t s_adcpu_pack_n;
static int abs_tl_init(void);

/* CPU0 decoder state. */
static int32_t s_adcpu_out;
static int32_t s_adcpu_step;
static uint32_t s_adcpu_count;
static uint32_t s_adcpu_clock_rate;
static uint8_t s_adcpu_clock, s_adcpu_pan, s_adcpu_playing, s_adcpu_volume;
static int32_t s_adcpu_old_r, s_adcpu_old_l;
static int32_t s_adcpu_outs[8], s_adcpu_outsip[4], s_adcpu_outsip_r[4], s_adcpu_outsip_l[4];

static const int8_t s_adcpu_index_shift[8] = {-1,-1,-1,-1,2,4,6,8};
static const uint16_t s_adcpu_base_step[49] = {
    16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
    494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552
};
static const uint8_t s_adcpu_volume_table[17] = {0,1,1,1,2,2,2,3,4,4,5,6,8,9,11,13,16};
static const uint32_t s_adcpu_clocks[8] = {93750u,125000u,187500u,125000u,46875u,62500u,93750u,62500u};

static inline int adcpu_clip12(int v) { return v>ADCPU_MAX?ADCPU_MAX:(v<ADCPU_MIN?ADCPU_MIN:v); }
static inline int adcpu_clip16(int v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}
static inline int adcpu_interp(int A,int B,int C,int y1,int x)
{
    int t=(A*x+ADCPU_IPSCALE/2)/ADCPU_IPSCALE+B;
    t=(t*x+ADCPU_IPSCALE/2)/ADCPU_IPSCALE+C;
    return (t*x+3*ADCPU_IPSCALE)/(6*ADCPU_IPSCALE)+y1;
}

static int adcpu_send_raw(uint8_t type, uint8_t aux, uint32_t payload, uint8_t flags)
{
    if (!__atomic_load_n(&s_adcpu_active, __ATOMIC_ACQUIRE)) return 0;
    const uint32_t head=__atomic_load_n(&s_adcpu_cmd_head,__ATOMIC_RELAXED);
    const uint32_t tail=__atomic_load_n(&s_adcpu_cmd_tail,__ATOMIC_ACQUIRE);
    if ((uint32_t)(head-tail) >= ADCPU_CMD_LEN) {
        __atomic_add_fetch(&s_adcpu_drops,1u,__ATOMIC_RELAXED); return 0;
    }
    adcpu_cmd_t *c=&s_adcpu_cmd[head & ADCPU_CMD_MASK];
    c->w0=(uint32_t)type | ((uint32_t)aux<<8) | ((uint32_t)flags<<24);
    c->w1=payload;
    __atomic_store_n(&s_adcpu_cmd_head,head+1u,__ATOMIC_RELEASE);
    return 1;
}

static int adcpu_flush_pack(void)
{
    if (!s_adcpu_pack_n) return 1;
    const uint8_t n=s_adcpu_pack_n; const uint32_t p=s_adcpu_pack;
    if (!adcpu_send_raw(ADCPU_DATA_PACK,n,p,0)) return 0;
    s_adcpu_pack=0; s_adcpu_pack_n=0; return 1;
}

int ADPCM_CPU0_Init(void)
{
    if (__atomic_load_n(&s_adcpu_active,__ATOMIC_ACQUIRE)) {
        if (DSound_AbsTimelineActive()) (void)abs_timeline_epoch_reset();
        else ADPCM_CPU0_Reset();
        return 1;
    }
    if (!s_adcpu_cmd) s_adcpu_cmd=(adcpu_cmd_t*)heap_caps_malloc(sizeof(adcpu_cmd_t)*ADCPU_CMD_LEN,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_adcpu_out_ring) s_adcpu_out_ring=(uint32_t*)heap_caps_malloc(sizeof(uint32_t)*ADCPU_OUT_LEN,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_adcpu_raw_ring) s_adcpu_raw_ring=(uint32_t*)heap_caps_malloc(sizeof(uint32_t)*ADCPU_RAW_LEN,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_adcpu_cmd || !s_adcpu_out_ring || !s_adcpu_raw_ring) {
        printf("PX68K_ADCPU107X: allocation failed; scalar CPU1 ADPCM fallback\n"); return 0;
    }
    s_adcpu_cmd_head=s_adcpu_cmd_tail=0; s_adcpu_out_head=s_adcpu_out_tail=0;
    s_adcpu_raw_head=s_adcpu_raw_tail=0; s_adcpu_pack=s_adcpu_pack_n=0; s_adcpu_drops=0;
    s_adcpu_out=0; s_adcpu_step=0; s_adcpu_count=0; s_adcpu_clock=2u;
    s_adcpu_clock_rate=s_adcpu_clocks[s_adcpu_clock]; s_adcpu_pan=0x0bu;
    s_adcpu_playing=0; s_adcpu_volume=16u; s_adcpu_old_r=s_adcpu_old_l=0;
    for(int i=0;i<8;++i)s_adcpu_outs[i]=0;
    for(int i=0;i<4;++i){s_adcpu_outsip[i]=-1;s_adcpu_outsip_r[i]=0;s_adcpu_outsip_l[i]=0;}
    __atomic_store_n(&s_adcpu_active,1u,__ATOMIC_RELEASE);
    printf("PX68K_ADCPU107X: ACTIVE CPU1 command producer -> CPU0 MSM6258 renderer cmd=%u out=%u packed4\n",
           (unsigned)ADCPU_CMD_LEN,(unsigned)ADCPU_OUT_LEN);
    if (!abs_tl_init()) { __atomic_store_n(&s_adcpu_active,0u,__ATOMIC_RELEASE); printf("PX68K_AUDTL_R140P4S3: FATAL absolute timeline init failed\n"); return 0; }
    return 1;
}
int ADPCM_CPU0_Active(void){return __atomic_load_n(&s_adcpu_active,__ATOMIC_ACQUIRE)?1:0;}
void ADPCM_CPU0_SetSourceRate(uint32_t rate)
{
    const uint32_t r = p12r6a4_normalize_source_rate(rate);
    __atomic_store_n(&s_adcpu_rate_req_x12, r * 12u, __ATOMIC_RELEASE);
}
uint32_t ADPCM_CPU0_CommandDrops(void){return __atomic_load_n(&s_adcpu_drops,__ATOMIC_RELAXED);}
uint32_t ADPCM_CPU0_BufferedFrames(void){return (uint32_t)(s_adcpu_out_head-s_adcpu_out_tail);}
void ADPCM_CPU0_Reset(void){if(ADPCM_CPU0_Active()){(void)adcpu_flush_pack();(void)adcpu_send_raw(ADCPU_RESET,0,0,0);}}
void ADPCM_CPU0_Control(uint8_t d){if(ADPCM_CPU0_Active()){(void)adcpu_flush_pack();(void)adcpu_send_raw(ADCPU_CONTROL,d,0,0);}}
void ADPCM_CPU0_SetPan(uint8_t d){if(ADPCM_CPU0_Active()){(void)adcpu_flush_pack();(void)adcpu_send_raw(ADCPU_PAN,d,0,0);}}
void ADPCM_CPU0_SetClock(uint8_t d){if(ADPCM_CPU0_Active()){(void)adcpu_flush_pack();(void)adcpu_send_raw(ADCPU_CLOCK,d,0,0);}}
void ADPCM_CPU0_SetVolume(uint8_t d){if(ADPCM_CPU0_Active()){(void)adcpu_flush_pack();(void)adcpu_send_raw(ADCPU_VOLUME,d,0,0);}}
void ADPCM_CPU0_Data(uint8_t d)
{
    if(!ADPCM_CPU0_Active())return;
    s_adcpu_pack |= ((uint32_t)d) << (8u*s_adcpu_pack_n);
    if(++s_adcpu_pack_n==4u)(void)adcpu_flush_pack();
}
int ADPCM_CPU0_Render(uint32_t f,int lpf)
{
    if(!f) return 1;
    if(!ADPCM_CPU0_Active()) return 0;
    if(!adcpu_flush_pack())return 0;
    const int ok=adcpu_send_raw(ADCPU_RENDER,0,f,lpf?1u:0u);
    if(ok) DSound_HostSourceReady();
    return ok;
}

static void adcpu_state_reset(void)
{
    s_adcpu_raw_head=s_adcpu_raw_tail=0; s_adcpu_out=0; s_adcpu_step=0; s_adcpu_count=0;
    s_adcpu_clock=2u; s_adcpu_clock_rate=s_adcpu_clocks[s_adcpu_clock]; s_adcpu_pan=0x0bu;
    s_adcpu_playing=0; s_adcpu_volume=16u; s_adcpu_old_r=s_adcpu_old_l=0;
    for(int i=0;i<8;++i)s_adcpu_outs[i]=0;
    for(int i=0;i<4;++i){s_adcpu_outsip[i]=-1;s_adcpu_outsip_r[i]=0;s_adcpu_outsip_l[i]=0;}
    /* Reset is an audible timeline boundary; discard stale pre-reset PCM. */
    s_adcpu_out_tail=s_adcpu_out_head;
}
static void adcpu_raw_push(int16_t r,int16_t l)
{
    if((uint32_t)(s_adcpu_raw_head-s_adcpu_raw_tail)>=ADCPU_RAW_LEN){__atomic_add_fetch(&s_adcpu_drops,1u,__ATOMIC_RELAXED);return;}
    s_adcpu_raw_ring[s_adcpu_raw_head & ADCPU_RAW_MASK]=(uint16_t)r|((uint32_t)(uint16_t)l<<16); ++s_adcpu_raw_head;
}
static int adcpu_raw_pop(int16_t *r,int16_t *l)
{
    if(s_adcpu_raw_tail==s_adcpu_raw_head)return 0;
    const uint32_t p=s_adcpu_raw_ring[s_adcpu_raw_tail & ADCPU_RAW_MASK]; ++s_adcpu_raw_tail;
    *r=(int16_t)(p&0xffffu); *l=(int16_t)(p>>16); return 1;
}
static void adcpu_decode_nibble(uint8_t v)
{
    const int base=(int)s_adcpu_base_step[s_adcpu_step]; int mag=base/8;
    if(v&1u) mag+=base/4;
    if(v&2u) mag+=base/2;
    if(v&4u) mag+=base;
    s_adcpu_out += (v&8u)?-mag:mag; s_adcpu_out=adcpu_clip12(s_adcpu_out);
    s_adcpu_step += s_adcpu_index_shift[v&7u]; if(s_adcpu_step<0)s_adcpu_step=0;else if(s_adcpu_step>48)s_adcpu_step=48;
    if(s_adcpu_outsip[0]==-1){s_adcpu_outsip[0]=s_adcpu_outsip[1]=s_adcpu_outsip[2]=s_adcpu_outsip[3]=s_adcpu_out;}
    else{s_adcpu_outsip[0]=s_adcpu_outsip[1];s_adcpu_outsip[1]=s_adcpu_outsip[2];s_adcpu_outsip[2]=s_adcpu_outsip[3];s_adcpu_outsip[3]=s_adcpu_out;}
    int16_t ratios[16]; int nr=0;
    while(s_adcpu_rate_x12>s_adcpu_count){if(s_adcpu_playing&&nr<16)ratios[nr++]=(int16_t)(((s_adcpu_count/100u)*ADCPU_IPSCALE)/(s_adcpu_rate_x12/100u));s_adcpu_count+=s_adcpu_clock_rate;}
    s_adcpu_count-=s_adcpu_rate_x12; if(!s_adcpu_playing||nr==0)return;
    const int y0=s_adcpu_outsip[0],y1=s_adcpu_outsip[1],y2=s_adcpu_outsip[2],y3=s_adcpu_outsip[3];
    const int A=-y0+3*y1-3*y2+y3,B=3*(y0-2*y1+y2),C=-2*y0-3*y1+6*y2-y3;
    for(int i=0;i<nr;++i){const int t=adcpu_clip12(adcpu_interp(A,B,C,y1,ratios[i]));const int16_t br=(s_adcpu_pan&1u)?0:(int16_t)t;const int16_t bl=(s_adcpu_pan&2u)?0:(int16_t)t;adcpu_raw_push(bl,br);}
}
static void adcpu_control(uint8_t d)
{
    if(d&1u)s_adcpu_playing=0;
    else if(d&2u){if(!s_adcpu_playing){s_adcpu_step=0;s_adcpu_out=0;s_adcpu_old_l=s_adcpu_old_r=-2;s_adcpu_playing=1;}s_adcpu_outsip[0]=s_adcpu_outsip[1]=s_adcpu_outsip[2]=s_adcpu_outsip[3]=-1;}
}
static void adcpu_set_pan(uint8_t n){if((s_adcpu_pan&0x0cu)!=(n&0x0cu)){s_adcpu_count=0;s_adcpu_clock=(uint8_t)((s_adcpu_clock&4u)|((n>>2)&3u));s_adcpu_clock_rate=s_adcpu_clocks[s_adcpu_clock];}s_adcpu_pan=n;}
static void adcpu_set_clock(uint8_t n){n&=4u;if((s_adcpu_clock&4u)!=n){s_adcpu_count=0;s_adcpu_clock=(uint8_t)(n|((s_adcpu_pan>>2)&3u));s_adcpu_clock_rate=s_adcpu_clocks[s_adcpu_clock];}}
static uint32_t adcpu_out_free(void){return ADCPU_OUT_LEN-(uint32_t)(s_adcpu_out_head-s_adcpu_out_tail);}
static void adcpu_out_push(int16_t r,int16_t l){s_adcpu_out_ring[s_adcpu_out_head&ADCPU_OUT_MASK]=(uint16_t)r|((uint32_t)(uint16_t)l<<16);++s_adcpu_out_head;}
static void adcpu_render_frames(uint32_t frames,uint8_t lpf)
{
    const int vol=(int)s_adcpu_volume_table[s_adcpu_volume<=16u?s_adcpu_volume:16u];
    for(uint32_t i=0;i<frames;++i){int16_t rr,ll;if(adcpu_raw_pop(&rr,&ll)){s_adcpu_old_r=rr;s_adcpu_old_l=ll;}else{rr=(int16_t)s_adcpu_old_r;ll=(int16_t)s_adcpu_old_l;}int orr,oll;
        if(lpf){int x=(int)rr*40*vol;int y=(x+s_adcpu_outs[3]*2+s_adcpu_outs[2]+s_adcpu_outs[1]*157-s_adcpu_outs[0]*61)>>8;s_adcpu_outs[2]=s_adcpu_outs[3];s_adcpu_outs[3]=x;s_adcpu_outs[0]=s_adcpu_outs[1];s_adcpu_outs[1]=y;s_adcpu_outsip_r[0]=s_adcpu_outsip_r[1];s_adcpu_outsip_r[1]=s_adcpu_outsip_r[2];s_adcpu_outsip_r[2]=s_adcpu_outsip_r[3];s_adcpu_outsip_r[3]=y;orr=adcpu_clip16(s_adcpu_outsip_r[1]);x=(int)ll*40*vol;y=(x+s_adcpu_outs[7]*2+s_adcpu_outs[6]+s_adcpu_outs[5]*157-s_adcpu_outs[4]*61)>>8;s_adcpu_outs[6]=s_adcpu_outs[7];s_adcpu_outs[7]=x;s_adcpu_outs[4]=s_adcpu_outs[5];s_adcpu_outs[5]=y;s_adcpu_outsip_l[0]=s_adcpu_outsip_l[1];s_adcpu_outsip_l[1]=s_adcpu_outsip_l[2];s_adcpu_outsip_l[2]=s_adcpu_outsip_l[3];s_adcpu_outsip_l[3]=y;oll=adcpu_clip16(s_adcpu_outsip_l[1]);}
        else{const int x=(int)rr*vol;s_adcpu_outsip_r[0]=s_adcpu_outsip_r[1];s_adcpu_outsip_r[1]=s_adcpu_outsip_r[2];s_adcpu_outsip_r[2]=s_adcpu_outsip_r[3];s_adcpu_outsip_r[3]=x;orr=adcpu_clip16(s_adcpu_outsip_r[1]);const int y=(int)ll*vol;s_adcpu_outsip_l[0]=s_adcpu_outsip_l[1];s_adcpu_outsip_l[1]=s_adcpu_outsip_l[2];s_adcpu_outsip_l[2]=s_adcpu_outsip_l[3];s_adcpu_outsip_l[3]=y;oll=adcpu_clip16(s_adcpu_outsip_l[1]);}
        adcpu_out_push((int16_t)orr,(int16_t)oll);
    }

}
static int adcpu_process_one(void)
{
    const uint32_t tail=__atomic_load_n(&s_adcpu_cmd_tail,__ATOMIC_RELAXED),head=__atomic_load_n(&s_adcpu_cmd_head,__ATOMIC_ACQUIRE);
    if(tail==head) return 0;
    const adcpu_cmd_t c=s_adcpu_cmd[tail&ADCPU_CMD_MASK];
    const uint8_t type=(uint8_t)c.w0,aux=(uint8_t)(c.w0>>8),flags=(uint8_t)(c.w0>>24);
    if(type==ADCPU_RENDER && adcpu_out_free()<c.w1)return 0;
    switch(type){case ADCPU_RESET:adcpu_state_reset();break;case ADCPU_CONTROL:adcpu_control(aux);break;case ADCPU_DATA_PACK:for(uint8_t i=0;i<aux&&i<4u;++i)if(s_adcpu_playing){const uint8_t b=(uint8_t)(c.w1>>(8u*i));adcpu_decode_nibble(b&15u);adcpu_decode_nibble(b>>4);}break;case ADCPU_PAN:adcpu_set_pan(aux);break;case ADCPU_CLOCK:adcpu_set_clock(aux);break;case ADCPU_VOLUME:s_adcpu_volume=aux<=16u?aux:16u;break;case ADCPU_RENDER:adcpu_render_frames(c.w1,flags&1u);break;default:break;}
    __atomic_store_n(&s_adcpu_cmd_tail,tail+1u,__ATOMIC_RELEASE);return 1;
}
static void adcpu_pump(uint32_t target_frames)
{
    const uint32_t r=__atomic_load_n(&s_adcpu_rate_req_x12,__ATOMIC_ACQUIRE);
    if(r!=s_adcpu_rate_x12){s_adcpu_rate_x12=r;s_adcpu_count=0u;}

    if(!ADPCM_CPU0_Active()) return;
    unsigned guard=0;
    while((uint32_t)(s_adcpu_out_head-s_adcpu_out_tail)<target_frames && guard++<ADCPU_CMD_LEN){if(!adcpu_process_one())break;}
}
/* R140P4S3 ABSOLUTE GUEST AUDIO TIMELINE
 * One CPU1 producer -> one CPU0 semantic owner. Every FM/ADPCM semantic event
 * carries {absolute guest sample tick, sequence}. The tick rate follows the
 * fixed 44.1-kHz product source domain. Frame COMMIT moves
 * both sources to the same absolute tick, preserving relative sample position
 * independently of CPU0 scheduling latency. */
/* R140P4S5F1: S5 measured qmax=69 across the long MDX capture.  A 1024-event
 * ring still provides >14x measured burst headroom while fitting in internal
 * SRAM on the normal Tab5 layout.  Internal-first removes hot CPU1 semantic
 * writes from PSRAM; allocation retains a PSRAM fallback for fragmentation. */
#define ABS_TL_LEN 1024u
#define ABS_TL_MASK (ABS_TL_LEN-1u)
#if (ABS_TL_LEN & (ABS_TL_LEN-1u)) != 0
#error ABS_TL_LEN_must_be_power_of_two
#endif
enum {
    ABS_TL_COMMIT=1, ABS_TL_FM_WRITE, ABS_TL_FM_CSM, ABS_TL_FM_RESET,
    ABS_TL_FM_VOLUME, ABS_TL_AD_CONTROL, ABS_TL_AD_DATA, ABS_TL_AD_PAN,
    ABS_TL_AD_CLOCK, ABS_TL_AD_VOLUME, ABS_TL_EPOCH_RESET
};
typedef struct {
    uint64_t tick;
    uint32_t seq;
    uint32_t payload;
    uint8_t type,a,b,flags;
} abs_tl_event_t;
static abs_tl_event_t *s_abs_tl_ring;
static uint8_t s_abs_tl_ring_internal;
static volatile uint32_t s_abs_tl_head,s_abs_tl_tail,s_abs_tl_active,s_abs_tl_fault,s_abs_tl_drops;
static TaskHandle_t s_abs_tl_task;
static uint32_t s_abs_tl_seq;
static uint64_t s_abs_tl_fm_cursor,s_abs_tl_ad_cursor;
static uint32_t s_abs_tl_last_seq;
/* Production clean: absolute-timeline audit publications/counters removed.
 * Functional head/tail/fault/sequence/cursors remain authoritative. */

static inline void adcpu_apply_rate_req(void)
{
    const uint32_t r=__atomic_load_n(&s_adcpu_rate_req_x12,__ATOMIC_ACQUIRE);
    if(r!=s_adcpu_rate_x12){s_adcpu_rate_x12=r;s_adcpu_count=0u;}
}
static void adcpu_abs_render_cpu0(uint32_t frames,uint8_t lpf)
{
    adcpu_apply_rate_req();
    while(frames)
    {
        uint32_t n=frames>256u?256u:frames;
        while(adcpu_out_free()<n)
            vTaskDelay(1);
        adcpu_render_frames(n,lpf);
        DSound_HostSourceReady();
        frames-=n;
    }
}
static int abs_tl_advance_fm_to(uint64_t tick)
{
    if(tick<s_abs_tl_fm_cursor){__atomic_store_n(&s_abs_tl_fault,1u,__ATOMIC_RELEASE);return 0;}
    uint64_t delta=tick-s_abs_tl_fm_cursor;
    while(delta)
    {
        uint32_t n=delta>256u?256u:(uint32_t)delta;
        if(!OPM_AbsTimelineRenderCPU0(n)){__atomic_store_n(&s_abs_tl_fault,1u,__ATOMIC_RELEASE);return 0;}
        s_abs_tl_fm_cursor+=n;
        delta-=n;
    }
    return 1;
}
static int abs_tl_advance_ad_to(uint64_t tick,uint8_t lpf)
{
    if(tick<s_abs_tl_ad_cursor){__atomic_store_n(&s_abs_tl_fault,1u,__ATOMIC_RELEASE);return 0;}
    uint64_t delta=tick-s_abs_tl_ad_cursor;
    while(delta)
    {
        uint32_t n=delta>256u?256u:(uint32_t)delta;
        adcpu_abs_render_cpu0(n,lpf);
        s_abs_tl_ad_cursor+=n;
        delta-=n;
    }
    return 1;
}
static int abs_tl_pop(abs_tl_event_t *ev)
{
    const uint32_t t=__atomic_load_n(&s_abs_tl_tail,__ATOMIC_RELAXED);
    const uint32_t h=__atomic_load_n(&s_abs_tl_head,__ATOMIC_ACQUIRE);
    if(t==h)return 0;
    *ev=s_abs_tl_ring[t&ABS_TL_MASK];
    __atomic_store_n(&s_abs_tl_tail,t+1u,__ATOMIC_RELEASE);
    return 1;
}
static void abs_tl_reset_cpu0(void)
{
    (void)OPM_AbsTimelineResetCPU0();
    adcpu_state_reset();
    s_adcpu_out_head=s_adcpu_out_tail=0;
    s_adcpu_cmd_head=s_adcpu_cmd_tail=0;
    s_adcpu_pack=s_adcpu_pack_n=0;
    s_abs_tl_fm_cursor=s_abs_tl_ad_cursor=0u;
    s_abs_tl_last_seq=0u;
}
static void abs_tl_task(void *)
{
    abs_tl_event_t ev;
    for(;;)
    {
        while(!abs_tl_pop(&ev))(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if(__atomic_load_n(&s_abs_tl_fault,__ATOMIC_ACQUIRE)){abs_tl_reset_cpu0();continue;}
        if(ev.type==ABS_TL_EPOCH_RESET)
        {
            abs_tl_reset_cpu0();
            s_abs_tl_last_seq=ev.seq;
            taskYIELD();
            continue;
        }
        if(s_abs_tl_last_seq && ev.seq!=s_abs_tl_last_seq+1u){__atomic_store_n(&s_abs_tl_fault,1u,__ATOMIC_RELEASE);continue;}
        switch(ev.type)
        {
            case ABS_TL_COMMIT:
                if(!abs_tl_advance_fm_to(ev.tick) || !abs_tl_advance_ad_to(ev.tick,ev.flags&1u)) continue;
                break;
            case ABS_TL_FM_WRITE:
                if(!abs_tl_advance_fm_to(ev.tick)) continue;
                (void)OPM_AbsTimelineWriteCPU0(ev.a,ev.b); break;
            case ABS_TL_FM_CSM:
                if(!abs_tl_advance_fm_to(ev.tick)) continue;
                (void)OPM_AbsTimelineCSMCPU0(); break;
            case ABS_TL_FM_RESET:
                if(!abs_tl_advance_fm_to(ev.tick)) continue;
                (void)OPM_AbsTimelineResetCPU0(); break;
            case ABS_TL_FM_VOLUME:
                if(!abs_tl_advance_fm_to(ev.tick)) continue;
                (void)OPM_AbsTimelineVolumeCPU0(ev.a); break;
            case ABS_TL_AD_CONTROL:
                if(!abs_tl_advance_ad_to(ev.tick,ev.flags&1u)) continue;
                adcpu_control(ev.a); break;
            case ABS_TL_AD_DATA:
                if(!abs_tl_advance_ad_to(ev.tick,ev.flags&1u)) continue;
                if(s_adcpu_playing){adcpu_decode_nibble(ev.a&15u);adcpu_decode_nibble(ev.a>>4);} break;
            case ABS_TL_AD_PAN:
                if(!abs_tl_advance_ad_to(ev.tick,ev.flags&1u)) continue;
                adcpu_set_pan(ev.a); break;
            case ABS_TL_AD_CLOCK:
                if(!abs_tl_advance_ad_to(ev.tick,ev.flags&1u)) continue;
                adcpu_set_clock(ev.a); break;
            case ABS_TL_AD_VOLUME:
                if(!abs_tl_advance_ad_to(ev.tick,ev.flags&1u)) continue;
                s_adcpu_volume=ev.a<=16u?ev.a:16u; break;
            default: __atomic_store_n(&s_abs_tl_fault,1u,__ATOMIC_RELEASE); break;
        }
        s_abs_tl_last_seq=ev.seq;
        taskYIELD();
    }
}
static int abs_tl_init(void)
{
    if(__atomic_load_n(&s_abs_tl_active,__ATOMIC_ACQUIRE))return 1;
    if(!s_abs_tl_ring)
    {
        s_abs_tl_ring=(abs_tl_event_t*)heap_caps_malloc(sizeof(abs_tl_event_t)*ABS_TL_LEN,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(s_abs_tl_ring)
            s_abs_tl_ring_internal=1u;
        else
        {
            s_abs_tl_ring=(abs_tl_event_t*)heap_caps_malloc(sizeof(abs_tl_event_t)*ABS_TL_LEN,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            s_abs_tl_ring_internal=0u;
        }
    }
    if(!s_abs_tl_ring)return 0;
    s_abs_tl_head=s_abs_tl_tail=0u;s_abs_tl_fault=s_abs_tl_drops=0u;s_abs_tl_seq=0u;s_abs_tl_fm_cursor=s_abs_tl_ad_cursor=0u;s_abs_tl_last_seq=0u;
    s_abs_guest_audio_tick=0u;snd_precounter=0;s_pending_adpcm_frames=s_pending_opm_frames=0;
#if portNUM_PROCESSORS > 1
    BaseType_t ok=xTaskCreatePinnedToCoreWithCaps(abs_tl_task,"px68k_audtl",6144,NULL,4,&s_abs_tl_task,0,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
    BaseType_t ok=xTaskCreateWithCaps(abs_tl_task,"px68k_audtl",6144,NULL,4,&s_abs_tl_task,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#endif
    if(ok!=pdPASS){s_abs_tl_task=NULL;return 0;}
    __atomic_store_n(&s_abs_tl_active,1u,__ATOMIC_RELEASE);
    printf("PX68K_AUDTL_R140P4S5F2: ABSOLUTE timeline ACTIVE events=%u bytes=%u queue=%s task=CPU0/prio4 stack=INTERNAL rate=%uHz NO-WAIT SINGLE-CPU1-PRODUCER\n",
           (unsigned)ABS_TL_LEN,(unsigned)(sizeof(abs_tl_event_t)*ABS_TL_LEN),
           s_abs_tl_ring_internal?"INTERNAL":"PSRAM-FALLBACK",(unsigned)DSound_GetHostSourceRate());
    return 1;
}
int DSound_AbsTimelineActive(void){return __atomic_load_n(&s_abs_tl_active,__ATOMIC_ACQUIRE)?1:0;}
static int abs_tl_push(uint8_t type,uint8_t a,uint8_t b,uint32_t payload)
{
    if(!DSound_AbsTimelineActive())return 0;
    if(__atomic_load_n(&s_abs_tl_fault,__ATOMIC_ACQUIRE))return 0;
    const uint32_t h=__atomic_load_n(&s_abs_tl_head,__ATOMIC_RELAXED),t=__atomic_load_n(&s_abs_tl_tail,__ATOMIC_ACQUIRE);
    if((uint32_t)(h-t)>=ABS_TL_LEN){__atomic_add_fetch(&s_abs_tl_drops,1u,__ATOMIC_RELAXED);__atomic_store_n(&s_abs_tl_fault,1u,__ATOMIC_RELEASE);if(s_abs_tl_task)xTaskNotifyGive(s_abs_tl_task);return 0;}
    abs_tl_event_t *ev=&s_abs_tl_ring[h&ABS_TL_MASK];
    ev->tick=s_abs_guest_audio_tick;ev->seq=++s_abs_tl_seq;ev->payload=payload;ev->type=type;ev->a=a;ev->b=b;ev->flags=Config.Sound_LPF?1u:0u;
    __atomic_store_n(&s_abs_tl_head,h+1u,__ATOMIC_RELEASE);
    if(s_abs_tl_task)xTaskNotifyGive(s_abs_tl_task);
    return 1;
}
int DSound_AbsTimelineADPCMControl(uint8_t d){return abs_tl_push(ABS_TL_AD_CONTROL,d,0,0);}
int DSound_AbsTimelineADPCMData(uint8_t d){return abs_tl_push(ABS_TL_AD_DATA,d,0,0);}
int DSound_AbsTimelineADPCMPan(uint8_t d){return abs_tl_push(ABS_TL_AD_PAN,d,0,0);}
int DSound_AbsTimelineADPCMClock(uint8_t d){return abs_tl_push(ABS_TL_AD_CLOCK,d,0,0);}
int DSound_AbsTimelineADPCMVolume(uint8_t d){return abs_tl_push(ABS_TL_AD_VOLUME,d,0,0);}
int DSound_AbsTimelineOPMCSM(void){return abs_tl_push(ABS_TL_FM_CSM,0,0,0);}
int DSound_AbsTimelineOPMReset(void){return abs_tl_push(ABS_TL_FM_RESET,0,0,0);}
int DSound_AbsTimelineOPMVolume(uint8_t d){return abs_tl_push(ABS_TL_FM_VOLUME,d,0,0);}
int DSound_AbsTimelineOPMWrite(uint8_t reg,uint8_t data){return abs_tl_push(ABS_TL_FM_WRITE,reg,data,0);}
static int abs_timeline_epoch_reset(void)
{
    if(!DSound_AbsTimelineActive())return 0;
    const int ok=abs_tl_push(ABS_TL_EPOCH_RESET,0,0,0);
    s_abs_guest_audio_tick=0u;snd_precounter=0;s_pending_adpcm_frames=s_pending_opm_frames=0;
    return ok;
}
static int abs_tl_commit(void){return abs_tl_push(ABS_TL_COMMIT,0,0,0);}

/* Production clean: retain historical audit ABI without steady-state counters.
 * Functional queue/fault/sequence state is still observable when queried. */
void DSound_AbsTimelineAuditGet(uint32_t *guest_tick,uint32_t *fm_cursor,uint32_t *ad_cursor,
                                uint32_t *seq,uint32_t *opm_pushes,uint32_t *qdepth,uint32_t *qmax,
                                uint32_t *fault,uint32_t *drops,uint32_t *fm_render_frames,
                                uint32_t *fm_render_work_us,uint32_t *ad_render_frames,uint32_t *ad_render_work_us)
{
    if(guest_tick)*guest_tick=(uint32_t)s_abs_guest_audio_tick;
    if(fm_cursor)*fm_cursor=0u;
    if(ad_cursor)*ad_cursor=0u;
    if(seq)*seq=s_abs_tl_seq;
    if(opm_pushes)*opm_pushes=0u;
    if(qdepth)*qdepth=(uint32_t)(__atomic_load_n(&s_abs_tl_head,__ATOMIC_ACQUIRE)-__atomic_load_n(&s_abs_tl_tail,__ATOMIC_ACQUIRE));
    if(qmax)*qmax=0u;
    if(fault)*fault=__atomic_load_n(&s_abs_tl_fault,__ATOMIC_ACQUIRE);
    if(drops)*drops=__atomic_load_n(&s_abs_tl_drops,__ATOMIC_RELAXED);
    if(fm_render_frames)*fm_render_frames=0u;
    if(fm_render_work_us)*fm_render_work_us=0u;
    if(ad_render_frames)*ad_render_frames=0u;
    if(ad_render_work_us)*ad_render_work_us=0u;
}
void DSound_AbsTimelineAuditGetEx(uint32_t *fm_wait_us,uint32_t *fm_wait_events,
                                  uint32_t *ad_wait_us,uint32_t *ad_wait_events,
                                  uint32_t *events_consumed,uint32_t *commit_pushes,
                                  uint32_t *fm_ctl_pushes,uint32_t *ad_data_pushes,uint32_t *ad_ctl_pushes)
{
    if(fm_wait_us)*fm_wait_us=0u;
    if(fm_wait_events)*fm_wait_events=0u;
    if(ad_wait_us)*ad_wait_us=0u;
    if(ad_wait_events)*ad_wait_events=0u;
    if(events_consumed)*events_consumed=__atomic_load_n(&s_abs_tl_tail,__ATOMIC_ACQUIRE);
    if(commit_pushes)*commit_pushes=0u;
    if(fm_ctl_pushes)*fm_ctl_pushes=0u;
    if(ad_data_pushes)*ad_data_pushes=0u;
    if(ad_ctl_pushes)*ad_ctl_pushes=0u;
}
void DSound_AbsTimelineAuditWindowBegin(void) {}

uint32_t ADPCM_CPU0_FramesAvail(void){adcpu_pump(1024u);return (uint32_t)(s_adcpu_out_head-s_adcpu_out_tail);}
int ADPCM_CPU0_ReadFrames(int16_t *dst,int frames)
{
    if(!dst||frames<=0||!ADPCM_CPU0_Active()) return 0;
    adcpu_pump((uint32_t)frames);
    uint32_t avail=(uint32_t)(s_adcpu_out_head-s_adcpu_out_tail);
    if((uint32_t)frames>avail) frames=(int)avail;
    for(int i=0;i<frames;++i){const uint32_t p=s_adcpu_out_ring[s_adcpu_out_tail&ADCPU_OUT_MASK];++s_adcpu_out_tail;dst[i*2]=(int16_t)(p&0xffffu);dst[i*2+1]=(int16_t)(p>>16);}return frames;
}
#endif /* ESP_PLATFORM */

static void sound_send_adpcm(int length)
{
   if (length <= 0) return;
#ifdef ESP_PLATFORM
   if (ADPCM_CPU0_Active())
   {
      ADPCM_CPU0_TimingConsume((size_t)length);
      (void)ADPCM_CPU0_Render((uint32_t)length, Config.Sound_LPF ? 1 : 0);
      return;
   }
#endif

#ifdef ESP_PLATFORM
   uint8_t *write_wp = pcm_load_wp();
   ADPCM_Update((int16_t *)write_wp, length, pbsp, pbep);
#else
   ADPCM_Update((int16_t *)pbwp, length, pbsp, pbep);
#endif

#if defined(ESP_PLATFORM) && PX68K_TAB5_R57E63_AUDIO_AUDIT
   r57e63_probe_adpcm(write_wp, length);
#endif
#ifdef ESP_PLATFORM
   uint8_t *next_wp = write_wp + length * sizeof(uint16_t) * 2;
#else
   uint8_t *next_wp = pbwp + length * sizeof(uint16_t) * 2;
#endif
   if (next_wp >= pbep)
      next_wp = pbsp + (next_wp - pbep);
#ifdef ESP_PLATFORM
   pcm_store_wp(next_wp);
#else
   pbwp = next_wp;
#endif
}

static void sound_send_opm(int length)
{
   if (length <= 0) return;
#ifdef ESP_PLATFORM
   if (TAB5_OPM_ASYNC_ENABLED())
   {
      OPM_AsyncRender((uint32_t)length, 0);
      return;
   }
#endif
   /* Non-ESP / rare synchronous fallback: OPM mixes additively into the
    * ADPCM buffer and therefore stays coupled to the legacy path. */
   OPM_Update((int16_t *)pbwp, length, pbsp, pbep);
}


static void sound_send_coupled(int length)
{
   if (length <= 0) return;
   ADPCM_Update((int16_t *)pbwp, length, pbsp, pbep);
   OPM_Update((int16_t *)pbwp, length, pbsp, pbep);

   uint8_t *next_wp = pbwp + length * sizeof(uint16_t) * 2;
   if (next_wp >= pbep)
      next_wp = pbsp + (next_wp - pbep);
#ifdef ESP_PLATFORM
   pcm_store_wp(next_wp);
#else
   pbwp = next_wp;
#endif
}

/* Legacy pull callback helper.  The Tab5 production path does not use
 * raudio_callback(), but keep its historical on-demand behavior buildable. */
static void sound_send(int length)
{
#ifdef ESP_PLATFORM
   if (TAB5_OPM_ASYNC_ENABLED())
   {
      sound_send_adpcm(length);
      sound_send_opm(length);
      return;
   }
#endif
   sound_send_coupled(length);
}

void DSound_Send0(int32_t clock)
{
   int length = 0;
#ifdef ESP_PLATFORM
   (void)0;
#endif

   snd_precounter += ((int32_t)DSound_GetHostSourceRate() * clock);

   while (snd_precounter >= 10000000L)
   {
      ++length;
      snd_precounter -= 10000000L;
   }
#ifdef ESP_PLATFORM
   if (length == 0) (void)0;
   else (void)0;

   if (DSound_AbsTimelineActive())
   {
      s_abs_guest_audio_tick += (uint32_t)length;
      return;
   }
#endif
   s_pending_adpcm_frames += length;
   s_pending_opm_frames += length;

#ifdef ESP_PLATFORM
   if (TAB5_OPM_ASYNC_ENABLED())
   {
      /* Keep both producers moving in coarse steady-state batches.  They may
       * temporarily lead/lag each other at their own register boundaries;
       * CPU0 consumes only min(ADPCM, FM), so final PCM remains aligned. */
      while (s_pending_adpcm_frames >= TAB5_AUDIO615B_ASYNC_QUANTUM)
      {
         s_pending_adpcm_frames -= TAB5_AUDIO615B_ASYNC_QUANTUM;
         sound_send_adpcm(TAB5_AUDIO615B_ASYNC_QUANTUM);
      }
      while (s_pending_opm_frames >= TAB5_AUDIO615B_ASYNC_QUANTUM)
      {
         s_pending_opm_frames -= TAB5_AUDIO615B_ASYNC_QUANTUM;
         sound_send_opm(TAB5_AUDIO615B_ASYNC_QUANTUM);
      }
      return;
   }
#endif

   /* Synchronous fallback keeps the historical coupled behavior. */
   while (s_pending_adpcm_frames >= TAB5_AUDIO615B_ASYNC_QUANTUM &&
          s_pending_opm_frames >= TAB5_AUDIO615B_ASYNC_QUANTUM)
   {
      s_pending_adpcm_frames -= TAB5_AUDIO615B_ASYNC_QUANTUM;
      s_pending_opm_frames -= TAB5_AUDIO615B_ASYNC_QUANTUM;
      sound_send_coupled(TAB5_AUDIO615B_ASYNC_QUANTUM);
   }
}

#ifdef ESP_PLATFORM
void IRAM_ATTR DSound_FlushADPCMPending(void)
#else
void DSound_FlushADPCMPending(void)
#endif
{
#ifdef ESP_PLATFORM
   /* R140P4S5F2: old split-FIFO ADPCM boundary flush is redundant under the
    * Absolute Timeline.  ADPCM data/control/pan/clock/volume already carry
    * the authoritative absolute tick+seq, so a preceding COMMIT only duplicates
    * the same boundary and doubles dense MDX transport traffic.  Frame-end
    * DSound_FlushPending() remains the semantic COMMIT owner. */
   if (DSound_AbsTimelineActive()) return;
#endif
#ifndef ESP_PLATFORM
   DSound_FlushPending();
   return;
#else
   if (!TAB5_OPM_ASYNC_ENABLED())
   {
      DSound_FlushPending();
      return;
   }
#endif
   if (s_pending_adpcm_frames > 0)
   {
      const int length = s_pending_adpcm_frames;
      s_pending_adpcm_frames = 0;
      sound_send_adpcm(length);
   }
}

static void DSound_FlushOPMPending(void)
{
   if (s_pending_opm_frames > 0)
   {
      const int length = s_pending_opm_frames;
      s_pending_opm_frames = 0;
      sound_send_opm(length);
   }
}

void DSound_OPMWrite(uint32_t adr, uint8_t data)
{
#ifdef ESP_PLATFORM
   if (DSound_AbsTimelineActive())
   {
      /* OPM_WriteTimed owns the selected-register latch and stamps every
       * data-port write into the absolute timeline.  Keeping the hook inside
       * fmg_wrap also catches any direct OPM_Write() caller. */
      OPM_WriteTimed(adr,data,0);
      return;
   }
   if (TAB5_OPM_ASYNC_ENABLED())
   {
      if ((adr & 1u) == 0u)
      {
         /* Address-latch changes are not audible and need no timeline flush. */
         OPM_WriteTimed(adr, data, 0);
      }
      else
      {
         uint32_t frames = (uint32_t)s_pending_opm_frames;
         s_pending_opm_frames = 0;
         OPM_WriteTimed(adr, data, frames);
      }
      return;
   }
#endif

   /* Rare synchronous fallback: preserve the old coupled ordering. */
   DSound_FlushPending();
   OPM_Write(adr, data);
}

void DSound_FlushPending(void)
{
#ifdef ESP_PLATFORM
   (void)0;
#endif
#ifdef ESP_PLATFORM
   if (DSound_AbsTimelineActive()) { (void)abs_tl_commit(); return; }
   if (TAB5_OPM_ASYNC_ENABLED())
   {
      DSound_FlushADPCMPending();
      DSound_FlushOPMPending();
      return;
   }
#endif

   /* Synchronous fallback: both timelines remain coupled. */
   while (s_pending_adpcm_frames > 0 && s_pending_opm_frames > 0)
   {
      int length = s_pending_adpcm_frames;
      if (length > s_pending_opm_frames) length = s_pending_opm_frames;
      s_pending_adpcm_frames -= length;
      s_pending_opm_frames -= length;
      sound_send_coupled(length);
   }
   /* Defensive only; normal synchronous operation keeps them equal. */
   s_pending_adpcm_frames = 0;
   s_pending_opm_frames = 0;
}

int audio_samples_avail(void)
{
   uint8_t *rd = pcm_load_rp();
   uint8_t *wr = pcm_load_wp();
   if (rd <= wr)
      return (wr - rd) / 4;
   return (pbep - rd) / 4 + (wr - pbsp) / 4;
}

void audio_samples_discard(int discard)
{
   int avail = audio_samples_avail();
   if (discard > avail)
      discard = avail;

   if (discard <= 0)
      return;

   uint8_t *rd = pcm_load_rp();
   uint8_t *wr = pcm_load_wp();
   if (rd > wr)
   {
      int availa = (pbep - rd) / 4;
      if (discard >= availa)
      {
         rd = pbsp;
         discard -= availa;
      }
   }
   rd += 4 * discard;
   pcm_store_rp(rd);
}



/* Build 5.98g12: CPU0-only final PCM consumer.  Unlike DSound_ReadFrames(),
 * this function NEVER flushes guest pending audio: flushing advances ADPCM/DMA
 * state and therefore belongs exclusively to CPU1's emulated time axis. */
#ifdef ESP_PLATFORM
void DSound_R1A17AuditGet(uint32_t out[4])
{
   if (out) out[0]=out[1]=out[2]=out[3]=0u;
}
#else
void DSound_R1A17AuditGet(uint32_t out[4])
{ if (out) out[0]=out[1]=out[2]=out[3]=0u; }
#endif

int DSound_HostFramesAvail(void)
{
#ifdef ESP_PLATFORM
   if (ADPCM_CPU0_Active())
   {
      int avail=(int)ADPCM_CPU0_FramesAvail();
      if (TAB5_OPM_ASYNC_ENABLED()) { const int fm=(int)OPM_AsyncFramesAvail(); if (avail>fm) avail=fm; }
      return avail;
   }

   uint8_t *rd = pcm_load_rp();
   uint8_t *wr = pcm_load_wp();

   int avail;
   if (rd <= wr)
      avail = (int)((wr - rd) / 4);
   else
      avail = (int)(((pbep - rd) + (wr - pbsp)) / 4);

   if (TAB5_OPM_ASYNC_ENABLED())
   {
      const int fm_avail = (int)OPM_AsyncFramesAvail();
      if (avail > fm_avail)
         avail = fm_avail;
   }
   return avail;
#else
   return audio_samples_avail();
#endif
}

uint32_t DSound_HostProducedFrames(void)
{
   return 0u;
}

int DSound_HostReadFrames(int16_t *dst, int max_frames)
{
#ifdef ESP_PLATFORM
   if (!dst || max_frames <= 0)
      return 0;

#ifdef ESP_PLATFORM
   if (ADPCM_CPU0_Active())
   {
      int frames=(int)ADPCM_CPU0_FramesAvail(); /* also drains command queue on CPU0 */
      if (TAB5_OPM_ASYNC_ENABLED()) { const int fm=(int)OPM_AsyncFramesAvail(); if (frames>fm) frames=fm; }
      if (frames>max_frames) frames=max_frames;
      if (frames<=0) return 0;
      frames=ADPCM_CPU0_ReadFrames(dst,frames);
      if (frames>0 && TAB5_OPM_ASYNC_ENABLED()) OPM_AsyncMixRead(dst,frames);
      return frames;
   }
#endif


   uint8_t *rd = pcm_load_rp();
   uint8_t *wr = pcm_load_wp();

   int avail;
   if (rd <= wr)
      avail = (int)((wr - rd) / 4);
   else
      avail = (int)(((pbep - rd) + (wr - pbsp)) / 4);
   if (TAB5_OPM_ASYNC_ENABLED())
   {
      const int fm_avail = (int)OPM_AsyncFramesAvail();
      if (avail > fm_avail)
         avail = fm_avail;
   }

   int frames = (avail < max_frames) ? avail : max_frames;
   if (frames <= 0)
      return 0;

   int remain = frames;
   int16_t *out = dst;
   uint8_t *cursor = rd;
   while (remain > 0)
   {
      int contiguous = (int)((pbep - cursor) / 4);
      if (cursor < wr || rd <= wr)
      {
         int to_wr = (int)((wr - cursor) / 4);
         if (to_wr >= 0 && to_wr < contiguous)
            contiguous = to_wr;
      }
      if (contiguous > remain)
         contiguous = remain;
      if (contiguous <= 0)
      {
         cursor = pbsp;
         continue;
      }
      memcpy(out, cursor, (size_t)contiguous * 4u);
      out += contiguous * 2;
      cursor += contiguous * 4;
      if (cursor >= pbep)
         cursor = pbsp;
      remain -= contiguous;
   }

   const int copied = frames - remain;
   if (copied > 0 && TAB5_OPM_ASYNC_ENABLED())
      OPM_AsyncMixRead(dst, copied);

   /* Publish consumption only after the ADPCM copy + FM saturation mix has
    * completed on CPU0.  CPU1 never advances pbrp. */
   if (copied > 0)
      pcm_store_rp(cursor);
   return copied;
#else
   (void)dst; (void)max_frames;
   return 0;
#endif
}

int DSound_ReadFrames(int16_t *dst, int max_frames)
{
   /* R140P4S5F1: DSound_ReadFrames() is a CPU0 HOST PULL.  Under the absolute
    * timeline it must never create a semantic COMMIT: doing so made CPU0 a
    * second producer in the documented CPU1->CPU0 SPSC ring and generated
    * ~3.1k bogus COMMIT events/s during MDX.  CPU1 frame-end remains the sole
    * COMMIT owner.  Legacy/non-absolute paths retain their historical flush. */
#ifdef ESP_PLATFORM
   if (!DSound_AbsTimelineActive())
      DSound_FlushPending();
#else
   DSound_FlushPending();
#endif
   int avail = audio_samples_avail();
   if (TAB5_OPM_ASYNC_ENABLED())
   {
      const int fm_avail = (int)OPM_AsyncFramesAvail();
      if (avail > fm_avail)
         avail = fm_avail;
   }
   int frames = (avail < max_frames) ? avail : max_frames;
   int remain = frames;
   int16_t *mix_base = dst;

   if (!dst || frames <= 0)
      return 0;

   while (remain > 0)
   {
      int contiguous;
      if (pbrp <= pbwp)
         contiguous = (pbwp - pbrp) / 4;
      else
         contiguous = (pbep - pbrp) / 4;

      if (contiguous > remain)
         contiguous = remain;
      if (contiguous <= 0)
         break;

      memcpy(dst, pbrp, contiguous * 4);
      dst += contiguous * 2;
      pbrp += contiguous * 4;
      if (pbrp >= pbep)
         pbrp = pbsp;
      remain -= contiguous;
   }

   const int copied = frames - remain;
   if (copied > 0 && TAB5_OPM_ASYNC_ENABLED())
      OPM_AsyncMixRead(mix_base, copied);

   return copied;
}

void raudio_callback(void *userdata, unsigned char *stream, int len)
{
   int lena, lenb, datalen;
   uint8_t *buf;

cb_start:
   if (pbrp <= pbwp)
   {
      /* pcmbuffer
       * +---------+-------------+----------+
       * |         |/////////////|          |
       * +---------+-------------+----------+
       * A         A<--datalen-->A          A
       * |         |             |          |
       * pbsp     pbrp          pbwp       pbep
       */

      datalen = pbwp - pbrp;

      /* needs more data */
      if (datalen < len)
      {
	      int length = (len - datalen) / 4;
	      sound_send(length);
      }

      /* change to TYPEC or TYPED */
      if (pbrp > pbwp)
         goto cb_start;

      buf = pbrp;
      pbrp += len;
   }
   else
   {
      /* pcmbuffer
       * +---------+-------------+----------+
       * |/////////|             |//////////|
       * +------+--+-------------+----------+
       * <-lenb->  A             <---lena--->
       * A         |             A          A
       * |         |             |          |
       * pbsp     pbwp          pbrp       pbep
       */

      lena = pbep - pbrp;
      if (lena >= len)
      {
         buf = pbrp;
         pbrp += len;
      }
      else
      {
         lenb = len - lena;

         if (pbwp - pbsp < lenb)
	 {
		 int length = (lenb - (pbwp - pbsp)) / 4;
		 sound_send(length);
	 }

         /* R12: copy the wrapped spans directly into the destination.
          * This removes the old 192 KiB rsndbuf bounce buffer entirely. */
         memcpy(userdata, pbrp, lena);
         memcpy((uint8_t *)userdata + lena, pbsp, lenb);
         pbrp = pbsp + lenb;
         return;
      }
   }
   memcpy(userdata, buf, len);
}
