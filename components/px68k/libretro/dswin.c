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
#include	"mercury.h"
#include	"fmg_wrap.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
extern int64_t esp_timer_get_time(void);
#endif

#ifndef PX68K_TAB5_R57E63_AUDIO_AUDIT
#define PX68K_TAB5_R57E63_AUDIO_AUDIT 0
#endif

static int s_perf_sample = 0;
static uint32_t s_perf_adpcm_us = 0;
static uint32_t s_perf_opm_us = 0;
static uint32_t s_perf_mix_calls = 0;
static uint32_t s_perf_mix_frames = 0;
/* Build 6.15b: FM and ADPCM pending timelines are tracked separately.
 * OPM writes can now advance only the FM timeline on CPU0 without forcing
 * CPU1 ADPCM generation. */
static int s_pending_adpcm_frames = 0;
static int s_pending_opm_frames = 0;

/* Build 6.15b: 256 frames (~5.8 ms) remains the steady-state batching
 * quantum, but OPM and ADPCM boundaries are now independent. */
#define TAB5_AUDIO615B_ASYNC_QUANTUM 256
#ifdef ESP_PLATFORM
/* Intent: CPU1 remains authoritative for guest-timed ADPCM state; CPU0 consumes only samples that CPU1 has already published.  Layer8 Aug/17/2026 */
/* BAT177NW0: CPU1 is the single ADPCM producer, CPU0 the single final-mix
 * consumer. This is a true SPSC ring: sample bytes are written before CPU1
 * release-publishes pbwp, and CPU0 finishes copy/mix before release-publishing
 * pbrp. No cross-core portMUX or retry is permitted on CPU1. */
/* R57E76: always retain this single monotonic counter.  It is incremented
 * once per already-batched guest audio publication and lets the silent
 * flight recorder measure real producer rate without wall-clock probes or
 * periodic UART. */
static volatile uint32_t s_host_produced_frames = 0;
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

static void sound_send_adpcm(int length)
{
   if (length <= 0) return;

   if (s_perf_sample)
   {
      ++s_perf_mix_calls;
      s_perf_mix_frames += (uint32_t)length;
   }

#ifdef ESP_PLATFORM
   uint8_t *write_wp = pcm_load_wp();
   if (s_perf_sample)
   {
      int64_t t0 = esp_timer_get_time();
      ADPCM_Update((int16_t *)write_wp, length, pbsp, pbep);
      s_perf_adpcm_us += (uint32_t)(esp_timer_get_time() - t0);
   }
   else
#endif
   {
#ifdef ESP_PLATFORM
      ADPCM_Update((int16_t *)write_wp, length, pbsp, pbep);
#else
      ADPCM_Update((int16_t *)pbwp, length, pbsp, pbep);
#endif
   }

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
   __atomic_add_fetch(&s_host_produced_frames, (uint32_t)length, __ATOMIC_RELAXED);
#else
   pbwp = next_wp;
#endif
}

static void sound_send_opm(int length)
{
   if (length <= 0) return;
#ifdef ESP_PLATFORM
   if (OPM_AsyncEnabled())
   {
      OPM_AsyncRender((uint32_t)length, s_perf_sample ? 1 : 0);
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

   if (s_perf_sample)
   {
      ++s_perf_mix_calls;
      s_perf_mix_frames += (uint32_t)length;
   }

#ifdef ESP_PLATFORM
   int64_t t0 = s_perf_sample ? esp_timer_get_time() : 0;
#endif
   ADPCM_Update((int16_t *)pbwp, length, pbsp, pbep);
#ifdef ESP_PLATFORM
   if (s_perf_sample)
   {
      s_perf_adpcm_us += (uint32_t)(esp_timer_get_time() - t0);
      t0 = esp_timer_get_time();
   }
#endif
   OPM_Update((int16_t *)pbwp, length, pbsp, pbep);
#ifdef ESP_PLATFORM
   if (s_perf_sample)
      s_perf_opm_us += (uint32_t)(esp_timer_get_time() - t0);
#endif

   uint8_t *next_wp = pbwp + length * sizeof(uint16_t) * 2;
   if (next_wp >= pbep)
      next_wp = pbsp + (next_wp - pbep);
#ifdef ESP_PLATFORM
   pcm_store_wp(next_wp);
   __atomic_add_fetch(&s_host_produced_frames, (uint32_t)length, __ATOMIC_RELAXED);
#else
   pbwp = next_wp;
#endif
}

/* Legacy pull callback helper.  The Tab5 production path does not use
 * raudio_callback(), but keep its historical on-demand behavior buildable. */
static void sound_send(int length)
{
#ifdef ESP_PLATFORM
   if (OPM_AsyncEnabled())
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

   snd_precounter += (44100 * clock);

   while (snd_precounter >= 10000000L)
   {
      ++length;
      snd_precounter -= 10000000L;
   }

   s_pending_adpcm_frames += length;
   s_pending_opm_frames += length;

#ifdef ESP_PLATFORM
   if (OPM_AsyncEnabled())
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
#ifndef ESP_PLATFORM
   DSound_FlushPending();
   return;
#else
   if (!OPM_AsyncEnabled())
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
   if (OPM_AsyncEnabled())
   {
      if ((adr & 1u) == 0u)
      {
         /* Address-latch changes are not audible and need no timeline flush. */
         OPM_WriteTimed(adr, data, 0);
      }
      else
      {
         /* One CPU0 queue event renders the old state up to this exact sample
          * boundary and then applies the new YM2151 register value. */
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
   if (OPM_AsyncEnabled())
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

void DSound_PerfSetSample(int enabled)
{
   s_perf_sample = enabled ? 1 : 0;
   if (s_perf_sample)
   {
      s_perf_adpcm_us = 0;
      s_perf_opm_us = 0;
      s_perf_mix_calls = 0;
      s_perf_mix_frames = 0;
      if (OPM_AsyncEnabled())
         OPM_AsyncPerfBegin();
   }
}

void DSound_PerfGetLast(uint32_t *adpcm_us, uint32_t *opm_us, uint32_t *mix_calls, uint32_t *mix_frames)
{
   if (adpcm_us) *adpcm_us = s_perf_adpcm_us;
   if (OPM_AsyncEnabled())
   {
      uint32_t async_us = 0;
      OPM_AsyncPerfGet(&async_us, NULL, NULL, NULL, NULL, NULL);
      if (opm_us) *opm_us = async_us;
   }
   else if (opm_us)
      *opm_us = s_perf_opm_us;
   if (mix_calls) *mix_calls = s_perf_mix_calls;
   if (mix_frames) *mix_frames = s_perf_mix_frames;
}


void DSound_R57E63PCMAuditGet(uint64_t *generated_frames,
                              uint64_t *probe_samples,
                              uint64_t *nonzero_samples,
                              uint32_t *peak)
{
#if defined(ESP_PLATFORM) && PX68K_TAB5_R57E63_AUDIO_AUDIT
   if (generated_frames) *generated_frames = s_r57e63_adpcm_generated_frames;
   if (probe_samples) *probe_samples = s_r57e63_adpcm_probe_samples;
   if (nonzero_samples) *nonzero_samples = s_r57e63_adpcm_nonzero_samples;
   if (peak) *peak = s_r57e63_adpcm_peak;
#else
   if (generated_frames) *generated_frames = 0;
   if (probe_samples) *probe_samples = 0;
   if (nonzero_samples) *nonzero_samples = 0;
   if (peak) *peak = 0;
#endif
}

void DSound_AsyncGetStats(uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns, uint32_t *fm_avail)
{
   uint32_t dummy_us = 0, dummy_calls = 0, dummy_frames = 0;
   OPM_AsyncPerfGet(&dummy_us, &dummy_calls, &dummy_frames, qdepth, event_drops, ring_overruns);
   if (fm_avail) *fm_avail = OPM_AsyncFramesAvail();
}

/* Build 5.98g12: CPU0-only final PCM consumer.  Unlike DSound_ReadFrames(),
 * this function NEVER flushes guest pending audio: flushing advances ADPCM/DMA
 * state and therefore belongs exclusively to CPU1's emulated time axis. */
int DSound_HostFramesAvail(void)
{
#ifdef ESP_PLATFORM
   uint8_t *rd = pcm_load_rp();
   uint8_t *wr = pcm_load_wp();

   int avail;
   if (rd <= wr)
      avail = (int)((wr - rd) / 4);
   else
      avail = (int)(((pbep - rd) + (wr - pbsp)) / 4);

   if (OPM_AsyncEnabled())
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
#ifdef ESP_PLATFORM
   return __atomic_load_n(&s_host_produced_frames, __ATOMIC_ACQUIRE);
#else
   return 0;
#endif
}

int DSound_HostReadFrames(int16_t *dst, int max_frames)
{
#ifdef ESP_PLATFORM
   if (!dst || max_frames <= 0)
      return 0;

   uint8_t *rd = pcm_load_rp();
   uint8_t *wr = pcm_load_wp();

   int avail;
   if (rd <= wr)
      avail = (int)((wr - rd) / 4);
   else
      avail = (int)(((pbep - rd) + (wr - pbsp)) / 4);
   if (OPM_AsyncEnabled())
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
   if (copied > 0 && OPM_AsyncEnabled())
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
   DSound_FlushPending();
   int avail = audio_samples_avail();
   if (OPM_AsyncEnabled())
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
   if (copied > 0 && OPM_AsyncEnabled())
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
