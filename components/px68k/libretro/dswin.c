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
extern int64_t esp_timer_get_time(void);
#endif

static int s_perf_sample = 0;
static uint32_t s_perf_adpcm_us = 0;
static uint32_t s_perf_opm_us = 0;
static uint32_t s_perf_mix_calls = 0;
static uint32_t s_perf_mix_frames = 0;
static int s_pending_frames = 0;
#ifdef ESP_PLATFORM
/* Intent: CPU1 remains authoritative for guest-timed ADPCM state; CPU0 consumes only samples that CPU1 has already published.  Layer8 Aug/17/2026 */
/* Build 5.98g12: CPU1 is the single ADPCM producer, CPU0 is the single
 * final-mix consumer.  Publish the producer/consumer ring pointers under a
 * tiny cross-core spinlock; sample data itself is written before pbwp is
 * published and read before pbrp is advanced, so the 4 KiB chunk copies do
 * not hold a critical section. */
static portMUX_TYPE s_pcm_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_host_produced_frames = 0;
#endif

#define PCMBUF_SIZE 2*2*48000

static uint8_t pcmbuffer[PCMBUF_SIZE];
static uint8_t rsndbuf  [PCMBUF_SIZE];
static int32_t snd_precounter = 0;

uint8_t *pbsp = pcmbuffer;
uint8_t *pbrp = pcmbuffer, *pbwp = pcmbuffer;
uint8_t *pbep = &pcmbuffer[PCMBUF_SIZE];

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

static void sound_send(int length)
{
   if (length <= 0) return;

   if (s_perf_sample)
   {
      s_perf_mix_calls++;
      s_perf_mix_frames += (uint32_t)length;
   }

#ifdef ESP_PLATFORM
   if (s_perf_sample)
   {
      int64_t t0 = esp_timer_get_time();
      ADPCM_Update((int16_t *)pbwp, length, pbsp, pbep);
      s_perf_adpcm_us += (uint32_t)(esp_timer_get_time() - t0);

      if (OPM_AsyncEnabled())
      {
         /* CPU0 host worker renders the FM contribution into its aligned PCM ring. */
         OPM_AsyncRender((uint32_t)length, 1);
      }
      else
      {
         t0 = esp_timer_get_time();
         OPM_Update((int16_t *)pbwp, length, pbsp, pbep);
         s_perf_opm_us += (uint32_t)(esp_timer_get_time() - t0);
      }
   }
   else
#endif
   {
      ADPCM_Update((int16_t *)pbwp, length, pbsp, pbep);
      if (OPM_AsyncEnabled())
         OPM_AsyncRender((uint32_t)length, 0);
      else
         OPM_Update((int16_t *)pbwp, length, pbsp, pbep);
   }

   uint8_t *next_wp = pbwp + length * sizeof(uint16_t) * 2;
   if (next_wp >= pbep)
      next_wp = pbsp + (next_wp - pbep);
#ifdef ESP_PLATFORM
   /* Publish only after ADPCM samples are complete.  CPU0 never executes
    * ADPCM_Update() or DSound_FlushPending(); it only consumes published PCM. */
   portENTER_CRITICAL(&s_pcm_mux);
   pbwp = next_wp;
   s_host_produced_frames += (uint32_t)length;
   portEXIT_CRITICAL(&s_pcm_mux);
#else
   pbwp = next_wp;
#endif
}

void DSound_Send0(int32_t clock)
{
	int length = 0;

	snd_precounter += (44100 * clock);

	while (snd_precounter >= 10000000L)
	{
		length++;
		snd_precounter -= 10000000L;
	}

	/*
	 * Tab5/P4: do not invoke FMGEN for 1-2 samples on every scanline.
	 * Accumulate samples and flush at observable sound-state boundaries
	 * (OPM/ADPCM writes) and once at the end of every emulated frame.
	 * This keeps register-write timing while removing thousands of tiny Mix() calls.
	 */
	s_pending_frames += length;

	/*
	 * Build 5.23: feed Core1 throughout the emulated frame instead of handing
	 * it one large job at frame end.  64 samples is ~1.45 ms at 44.1 kHz and
	 * gives useful overlap while register writes still force exact boundaries.
	 */
	if (OPM_AsyncEnabled())
	{
		while (s_pending_frames >= 64)
		{
			s_pending_frames -= 64;
			sound_send(64);
		}
	}
}

void DSound_FlushPending(void)
{
	if (s_pending_frames > 0)
	{
		int length = s_pending_frames;
		s_pending_frames = 0;
		sound_send(length);
	}
}

int audio_samples_avail(void)
{
   if (pbrp <= pbwp)
      return (pbwp - pbrp) / 4;
   return (pbep - pbrp) / 4 + (pbwp - pbsp) / 4;
}

void audio_samples_discard(int discard)
{
   int avail = audio_samples_avail();
   if (discard > avail)
      discard = avail;

   if (discard <= 0)
      return;

   if (pbrp > pbwp)
   {
      int availa = (pbep - pbrp) / 4;
      if (discard >= availa)
      {
         pbrp = pbsp;
         discard -= availa;
      }
   }
   
   pbrp += 4 * discard;
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
   uint8_t *rd, *wr;
   portENTER_CRITICAL(&s_pcm_mux);
   rd = pbrp;
   wr = pbwp;
   portEXIT_CRITICAL(&s_pcm_mux);

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
   uint32_t n;
   portENTER_CRITICAL(&s_pcm_mux);
   n = s_host_produced_frames;
   portEXIT_CRITICAL(&s_pcm_mux);
   return n;
#else
   return 0;
#endif
}

int DSound_HostReadFrames(int16_t *dst, int max_frames)
{
#ifdef ESP_PLATFORM
   if (!dst || max_frames <= 0)
      return 0;

   uint8_t *rd, *wr;
   portENTER_CRITICAL(&s_pcm_mux);
   rd = pbrp;
   wr = pbwp;
   portEXIT_CRITICAL(&s_pcm_mux);

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
   {
      portENTER_CRITICAL(&s_pcm_mux);
      pbrp = cursor;
      portEXIT_CRITICAL(&s_pcm_mux);
   }
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

         memcpy(rsndbuf, pbrp, lena);
         memcpy(&rsndbuf[lena], pbsp, lenb);
         buf  = rsndbuf;
         pbrp = pbsp + lenb;
      }
   }
   memcpy(userdata, buf, len);
}
