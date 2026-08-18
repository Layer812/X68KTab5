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
 * Intent: Rework the render handoff for Tab5: CPU1 snapshots guest graphics state and CPU0 performs final composition/LCD-oriented work with validated PIE helpers.
 * Layer8 Aug/17/2026
 */
#include "common.h"

#include "winx68k.h"
#include "winui.h"

#include "bg.h"
#include "crtc.h"
#include "gvram.h"
#include "mouse.h"
#include "palette.h"
#include "prop.h"
#include "status.h"
#include "tvram.h"
#include "joystick.h"
#include "keyboard.h"

#ifndef PX68K_TAB5_PERF_PROFILE
#define PX68K_TAB5_PERF_PROFILE 0
#endif
#define WD_PERF_ACTIVE (PX68K_TAB5_PERF_PROFILE && s_wd_perf_enabled)

#define		SCREEN_WIDTH		768
#define		FULLSCREEN_WIDTH	800

extern uint16_t *videoBuffer;
uint16_t menu_buffer[800*600];

extern uint8_t Debug_Text, Debug_Grp, Debug_Sp;

static uint16_t *ScrBuf = 0;

/* Build 5.31: ESP32-P4 PPA host hooks live in the Tab5 application component. */
extern void *tab5_ppa_alloc_framebuffer(size_t bytes);
/* RC: exact key-zero overlay uses the startup-validated fixed PIE-128 backend
 * with scalar alignment/failure fallback. */
extern int tab5_p4blend_key0_overlay(uint16_t *frame_base, uint32_t frame_w,
                                     uint32_t frame_h, uint32_t y,
                                     const uint16_t *top_base, uint32_t top_pic_w,
                                     uint32_t top_offset_x, uint32_t width);
/* Build 5.98g4c: reusable P4 PIE graphics primitives. */
extern void tab5_pie_graphics_init(void);
extern void tab5_pie_graphics_copy(void *dst, const void *src, uint32_t bytes);
extern void tab5_pie_graphics_fill16(uint16_t *dst, uint16_t value, uint32_t pixels);
extern void tab5_pie_graphics_fill8(uint8_t *dst, uint8_t value, uint32_t bytes);
extern int tab5_pie_graphics_diff(const void *a, const void *b, uint32_t bytes);
/* Build 5.99rc1: retired PPA batch staging removed; host compositor is authoritative. */

/* Build 5.45: line-generation handshake for zero-copy Core1 LCD reads.
 * Writers never wait for the LCD.  The Core1 reader retries a row if it
 * changed while pushImage consumed it. */
extern void tab5_video_fb_line_write_begin(uint32_t y);
extern void tab5_video_fb_line_write_end(uint32_t y);

/* Build 5.46: Core1 normal two-layer compositor.  CPU0 snapshots the already
 * raster-correct GRP/BG line into internal SRAM; Core1 performs the final
 * key-zero blend into ScrBuf.  Return value 1 means the write is asynchronous. */
extern int tab5_compose_submit_line(uint32_t y, uint32_t width,
                                    const uint16_t *bottom, const uint16_t *top,
                                    uint16_t *dst);
extern int tab5_compose_submit_grp8pair_line(uint32_t y, uint32_t width,
                                             const uint8_t *gvram,
                                             uint32_t y_lo_base, uint32_t y_hi_base,
                                             uint32_t x_lo, uint32_t x_hi,
                                             int bottom_page, int top_page,
                                             const uint16_t *palette,
                                             const uint16_t *bg, int bg_on_top,
                                             uint16_t *dst);
extern int tab5_compose_submit_grp8pair_bgsp_line(uint32_t y, uint32_t width,
                                                  const uint8_t *gvram,
                                                  uint32_t y_lo_base, uint32_t y_hi_base,
                                                  uint32_t x_lo, uint32_t x_hi,
                                                  int bottom_page, int top_page,
                                                  const uint16_t *grph_palette,
                                                  const uint16_t *text_palette,
                                                  const BG_HOST_LINE_STATE *bg_state,
                                                  int bg_on_top, uint16_t *dst);
extern int tab5_compose_submit_grp8split_line(uint32_t y, uint32_t width,
                                              const uint8_t *gvram,
                                              uint32_t by_lo_base, uint32_t by_hi_base,
                                              uint32_t ty_lo_base, uint32_t ty_hi_base,
                                              uint32_t bx_lo, uint32_t bx_hi,
                                              uint32_t tx_lo, uint32_t tx_hi,
                                              int bottom_page, int top_page,
                                              const uint16_t *palette,
                                              const uint16_t *bg, int bg_on_top,
                                              uint16_t *dst,
                                              const uint16_t *selfcheck_ref);
extern int tab5_compose_submit_grp8split_bgsp_line(uint32_t y, uint32_t width,
                                                   const uint8_t *gvram,
                                                   uint32_t by_lo_base, uint32_t by_hi_base,
                                                   uint32_t ty_lo_base, uint32_t ty_hi_base,
                                                   uint32_t bx_lo, uint32_t bx_hi,
                                                   uint32_t tx_lo, uint32_t tx_hi,
                                                   int bottom_page, int top_page,
                                                   const uint16_t *grph_palette,
                                                   const uint16_t *text_palette,
                                                   const BG_HOST_LINE_STATE *bg_state,
                                                   int bg_on_top, uint16_t *dst,
                                                   const uint16_t *selfcheck_ref);
extern int tab5_compose_grp8split_needs_selfcheck(void);
extern void tab5_compose_wait_idle(void);

/* Build 5.49: the hardware PPA batch path is retired.  Keep only the legacy
 * perf counter name because existing diagnostics expose it as the count of
 * normal lines handed to the CPU0 host compositor. */
static uint32_t s_wd_perf_host_lines = 0;

/* First real two-page 256-colour line is cross-checked against the legacy
 * two-call renderer.  A mismatch permanently falls back to the old path. */
static int s_grp8host_reported = 0;


/* Build 5.24: sample-only graphics compose profiler.  Enabled for the same
 * one frame / 600 used by the host PERF logger, so normal gameplay is not
 * burdened by esp_timer calls. */
extern int64_t esp_timer_get_time(void);
static int s_wd_perf_enabled = 0;
static uint64_t s_wd_perf_grp_us = 0;
static uint64_t s_wd_perf_text_us = 0;
static uint64_t s_wd_perf_bg_us = 0;
static uint64_t s_wd_perf_blend_us = 0;
static uint64_t s_wd_perf_clear_us = 0;
static uint32_t s_wd_perf_dirty_lines = 0;
static uint32_t s_wd_perf_grp_calls = 0;
static uint32_t s_wd_perf_text_calls = 0;
static uint32_t s_wd_perf_bg_calls = 0;
static uint32_t s_wd_perf_blend_calls = 0;

/* Build 5.53a: sampled-frame reject map for the CPU0 common GRP8/BGSP path.
 * These counters are touched only while the existing one-frame PERF sample is
 * active, so normal gameplay pays no diagnostic cost. */
static uint32_t s_hp_mode16, s_hp_mode256, s_hp_mode65k;
static uint32_t s_hp_256_common, s_hp_async_geom, s_hp_submit, s_hp_accept;
static uint32_t s_hp_split_geom, s_hp_split_accept;
static uint32_t s_hp_rej_pages, s_hp_rej_special, s_hp_rej_valid;
static uint32_t s_hp_rej_scrolly, s_hp_rej_scrollx;
static uint32_t s_hp_rej_nobg, s_hp_rej_text, s_hp_rej_tron, s_hp_rej_pron;
static uint32_t s_hp_rej_queue;

void WinDraw_PerfSetSample(int enabled)
{
    s_wd_perf_enabled = enabled ? 1 : 0;
    if (WD_PERF_ACTIVE) {
        s_wd_perf_grp_us = s_wd_perf_text_us = s_wd_perf_bg_us = 0;
        s_wd_perf_blend_us = s_wd_perf_clear_us = 0;
        s_wd_perf_dirty_lines = 0;
        s_wd_perf_grp_calls = s_wd_perf_text_calls = 0;
        s_wd_perf_bg_calls = s_wd_perf_blend_calls = 0;
        s_wd_perf_host_lines = 0;
        s_hp_mode16 = s_hp_mode256 = s_hp_mode65k = 0;
        s_hp_256_common = s_hp_async_geom = s_hp_submit = s_hp_accept = 0;
        s_hp_split_geom = s_hp_split_accept = 0;
        s_hp_rej_pages = s_hp_rej_special = s_hp_rej_valid = 0;
        s_hp_rej_scrolly = s_hp_rej_scrollx = 0;
        s_hp_rej_nobg = s_hp_rej_text = s_hp_rej_tron = s_hp_rej_pron = 0;
        s_hp_rej_queue = 0;
    }
}

void WinDraw_PerfGetLast(uint32_t *grp_us, uint32_t *text_us, uint32_t *bg_us,
                         uint32_t *blend_us, uint32_t *clear_us,
                         uint32_t *dirty_lines, uint32_t *grp_calls,
                         uint32_t *text_calls, uint32_t *bg_calls,
                         uint32_t *blend_calls)
{
    if (grp_us) *grp_us = (uint32_t)s_wd_perf_grp_us;
    if (text_us) *text_us = (uint32_t)s_wd_perf_text_us;
    if (bg_us) *bg_us = (uint32_t)s_wd_perf_bg_us;
    if (blend_us) *blend_us = (uint32_t)s_wd_perf_blend_us;
    if (clear_us) *clear_us = (uint32_t)s_wd_perf_clear_us;
    if (dirty_lines) *dirty_lines = s_wd_perf_dirty_lines;
    if (grp_calls) *grp_calls = s_wd_perf_grp_calls;
    if (text_calls) *text_calls = s_wd_perf_text_calls;
    if (bg_calls) *bg_calls = s_wd_perf_bg_calls;
    if (blend_calls) *blend_calls = s_wd_perf_blend_calls;
    if (WD_PERF_ACTIVE && s_wd_perf_host_lines) {
        printf("PX68K_HOSTCOMPOSE: sample normal-lines=%u\n",
               (unsigned)s_wd_perf_host_lines);
    }
    if (WD_PERF_ACTIVE) {
        printf("PX68K_HOSTPATH553: mode16=%u mode256=%u mode65k=%u common256=%u geom=%u split=%u submit=%u accept=%u splitacc=%u "
               "reject:pages=%u special=%u valid=%u nobg=%u text=%u tron=%u pron=%u queue=%u diff:sy=%u sx=%u\n",
               (unsigned)s_hp_mode16, (unsigned)s_hp_mode256, (unsigned)s_hp_mode65k,
               (unsigned)s_hp_256_common, (unsigned)s_hp_async_geom,
               (unsigned)s_hp_split_geom, (unsigned)s_hp_submit, (unsigned)s_hp_accept,
               (unsigned)s_hp_split_accept,
               (unsigned)s_hp_rej_pages, (unsigned)s_hp_rej_special,
               (unsigned)s_hp_rej_valid,
               (unsigned)s_hp_rej_nobg, (unsigned)s_hp_rej_text,
               (unsigned)s_hp_rej_tron, (unsigned)s_hp_rej_pron,
               (unsigned)s_hp_rej_queue,
               (unsigned)s_hp_rej_scrolly, (unsigned)s_hp_rej_scrollx);
    }
}

#define WD_PERF_DO(ACC, CALLS, ...) do { \
    if (WD_PERF_ACTIVE) { \
        int64_t _wd_t0 = esp_timer_get_time(); \
        __VA_ARGS__; \
        (ACC) += (uint64_t)(esp_timer_get_time() - _wd_t0); \
        (CALLS)++; \
    } else { \
        __VA_ARGS__; \
    } \
} while (0)
#define WD_PERF_GRP(...)   WD_PERF_DO(s_wd_perf_grp_us,   s_wd_perf_grp_calls,   __VA_ARGS__)
#define WD_PERF_TEXT(...)  WD_PERF_DO(s_wd_perf_text_us,  s_wd_perf_text_calls,  __VA_ARGS__)
#define WD_PERF_BG(...)    WD_PERF_DO(s_wd_perf_bg_us,    s_wd_perf_bg_calls,    __VA_ARGS__)
#define WD_PERF_BLEND(...) WD_PERF_DO(s_wd_perf_blend_us, s_wd_perf_blend_calls, __VA_ARGS__)
#define WD_PERF_CLEAR(...) WD_PERF_DO(s_wd_perf_clear_us, s_wd_perf_blend_calls, __VA_ARGS__)

uint16_t WinDraw_Pal16B, WinDraw_Pal16R, WinDraw_Pal16G;

void WinDraw_Init(void)
{
	WinDraw_Pal16R = 0xf800;
	WinDraw_Pal16G = 0x07e0;
	WinDraw_Pal16B = 0x001f;

	/* RC: allocate the framebuffer in aligned PSRAM; retired PPA staging is gone. */
	ScrBuf = (uint16_t *)tab5_ppa_alloc_framebuffer(800u * 600u * sizeof(uint16_t));
	if (ScrBuf) {
        tab5_pie_graphics_init();
        tab5_pie_graphics_fill16(ScrBuf, 0u, 800u * 600u);
        printf("PX68K_GFX599RC1: aligned PSRAM framebuffer + fixed PIE/CPU0 compose ready\n");
	} else {
		ScrBuf = calloc(800 * 600, sizeof(uint16_t));
		printf("PX68K_GFX599RC1: aligned framebuffer allocation failed; calloc fallback\n");
	}
}

void WinDraw_Cleanup(void)
{
        if (ScrBuf)
           free(ScrBuf);
        ScrBuf = NULL;
}

/* Forward declarations */
extern uint32_t retrow, retroh;
extern int CHANGEAV;

void FASTCALL WinDraw_Draw(void)
{
	static int oldtextx = -1, oldtexty = -1;

	/* Finish any partial PPA batch and all asynchronous Core1 line jobs before
	 * exposing this frame.  Jobs run in parallel with CPU0 during the frame, so
	 * this is normally only a short tail barrier. */
	tab5_compose_wait_idle();

	if (oldtextx != TextDotX)
	{
		oldtextx = TextDotX;
		CHANGEAV=1;
	}
	if (oldtexty != TextDotY)
	{
		oldtexty = TextDotY;
		CHANGEAV=1;
	}

	if (CHANGEAV==1)
	{
		retrow=TextDotX;
		retroh=TextDotY;
	}

	videoBuffer = (uint16_t*)ScrBuf;
}

#define WD_MEMCPY(src) tab5_pie_graphics_copy(&ScrBuf[adr], (src), (uint32_t)TextDotX * 2u)

#define WD_LOOP(start, end, sub)                 \
	{                                            \
		for (i = (start); i < (end); i++, adr++) \
		{                                        \
			sub();                               \
		}                                        \
	}

#define WD_SUB(SUFFIX, src)          \
	{                                \
		w = (src);                   \
		if (w != 0)                  \
			ScrBuf##SUFFIX[adr] = w; \
	}

static INLINE void WinDraw_DrawGrpLine(int opaq)
{
#define _DGL_SUB(SUFFIX) WD_SUB(SUFFIX, Grp_LineBuf[i])

	uint32_t adr = VLINE * FULLSCREEN_WIDTH;
	uint16_t w;
	int i;

	if (opaq) {
		WD_MEMCPY(Grp_LineBuf);
	} else if (!tab5_p4blend_key0_overlay(ScrBuf, FULLSCREEN_WIDTH, 600u, VLINE,
	                                      Grp_LineBuf, 1024u, 0u, (uint32_t)TextDotX)) {
		WD_LOOP(0, TextDotX, _DGL_SUB);
	}
}

static INLINE void WinDraw_DrawGrpLineNonSP(int opaq)
{
#define _DGL_NSP_SUB(SUFFIX) WD_SUB(SUFFIX, Grp_LineBufSP2[i])

	uint32_t adr = VLINE*FULLSCREEN_WIDTH;
	uint16_t w;
	int i;

	if (opaq) {
		WD_MEMCPY(Grp_LineBufSP2);
	} else if (!tab5_p4blend_key0_overlay(ScrBuf, FULLSCREEN_WIDTH, 600u, VLINE,
	                                      Grp_LineBufSP2, 1024u, 0u, (uint32_t)TextDotX)) {
		WD_LOOP(0, TextDotX, _DGL_NSP_SUB);
	}
}

static INLINE void WinDraw_DrawTextLine(int opaq, int td)
{
#define _DTL_SUB2(SUFFIX) WD_SUB(SUFFIX, BG_LineBuf[i])

#define _DTL_SUB(SUFFIX)        \
	{                           \
		if (Text_TrFlag[i] & 1) \
		{                       \
			_DTL_SUB2(SUFFIX);  \
		}                       \
	}

	uint32_t adr = VLINE*FULLSCREEN_WIDTH;
	uint16_t w;
	int i;

	if (opaq) {
		WD_MEMCPY(&BG_LineBuf[16]);
	} else {
		if (td) {
			WD_LOOP(16, TextDotX + 16, _DTL_SUB);
		} else if (!tab5_p4blend_key0_overlay(ScrBuf, FULLSCREEN_WIDTH, 600u, VLINE,
		                                           BG_LineBuf, 1600u, 16u, (uint32_t)TextDotX)) {
			WD_LOOP(16, TextDotX + 16, _DTL_SUB2);
		}
	}
}

static INLINE void WinDraw_DrawTextLineTR(int opaq)
{
#define _DTL_TR_SUB(SUFFIX)                \
	{                                      \
		w = Grp_LineBufSP[i - 16];         \
		if (w != 0)                        \
		{                                  \
			w &= Pal_HalfMask;             \
			v = BG_LineBuf[i];             \
			if (v & Ibit)                  \
				w += Pal_Ix2;              \
			v &= Pal_HalfMask;             \
			v += w;                        \
			v >>= 1;                       \
		}                                  \
		else                               \
		{                                  \
			if (Text_TrFlag[i] & 1)        \
				v = BG_LineBuf[i];         \
			else                           \
				v = 0;                     \
		}                                  \
		ScrBuf##SUFFIX[adr] = (uint16_t)v; \
	}

#define _DTL_TR_SUB2(SUFFIX)                       \
	{                                              \
		if (Text_TrFlag[i] & 1)                    \
		{                                          \
			w = Grp_LineBufSP[i - 16];             \
			v = BG_LineBuf[i];                     \
                                                   \
			if (v != 0)                            \
			{                                      \
				if (w != 0)                        \
				{                                  \
					w &= Pal_HalfMask;             \
					if (v & Ibit)                  \
						w += Pal_Ix2;              \
					v &= Pal_HalfMask;             \
					v += w;                        \
					v >>= 1;                       \
				}                                  \
				ScrBuf##SUFFIX[adr] = (uint16_t)v; \
			}                                      \
		}                                          \
	}

	uint32_t adr = VLINE*FULLSCREEN_WIDTH;
	uint32_t v;
	uint16_t w;
	int i;

	if (opaq) {
		WD_LOOP(16, TextDotX + 16, _DTL_TR_SUB);
	} else {
		WD_LOOP(16, TextDotX + 16, _DTL_TR_SUB2);
	}
}

static INLINE void WinDraw_DrawBGLine(int opaq, int td)
{
#define _DBL_SUB2(SUFFIX) WD_SUB(SUFFIX, BG_LineBuf[i])

#define _DBL_SUB(SUFFIX)        \
	{                           \
		if (Text_TrFlag[i] & 2) \
		{                       \
			_DBL_SUB2(SUFFIX);  \
		}                       \
	}

	uint32_t adr = VLINE*FULLSCREEN_WIDTH;
	uint16_t w;
	int i;

	if (opaq) {
		WD_MEMCPY(&BG_LineBuf[16]);
	} else {
		if (td) {
			WD_LOOP(16, TextDotX + 16, _DBL_SUB);
		} else if (!tab5_p4blend_key0_overlay(ScrBuf, FULLSCREEN_WIDTH, 600u, VLINE,
		                                           BG_LineBuf, 1600u, 16u, (uint32_t)TextDotX)) {
			WD_LOOP(16, TextDotX + 16, _DBL_SUB2);
		}
	}
}

static INLINE void WinDraw_DrawBGLineTR(int opaq)
{
#define _DBL_TR_SUB3()         \
	{                          \
		if (w != 0)            \
		{                      \
			w &= Pal_HalfMask; \
			if (v & Ibit)      \
				w += Pal_Ix2;  \
			v &= Pal_HalfMask; \
			v += w;            \
			v >>= 1;           \
		}                      \
	}

#define _DBL_TR_SUB(SUFFIX)                \
	{                                      \
		w = Grp_LineBufSP[i - 16];         \
		v = BG_LineBuf[i];                 \
                                           \
		_DBL_TR_SUB3()                     \
		ScrBuf##SUFFIX[adr] = (uint16_t)v; \
	}

#define _DBL_TR_SUB2(SUFFIX)                       \
	{                                              \
		if (Text_TrFlag[i] & 2)                    \
		{                                          \
			w = Grp_LineBufSP[i - 16];             \
			v = BG_LineBuf[i];                     \
                                                   \
			if (v != 0)                            \
			{                                      \
				_DBL_TR_SUB3()                     \
				ScrBuf##SUFFIX[adr] = (uint16_t)v; \
			}                                      \
		}                                          \
	}

	uint32_t adr = VLINE*FULLSCREEN_WIDTH;
	uint32_t v;
	uint16_t w;
	int i;

	if (opaq) {
		WD_LOOP(16, TextDotX + 16, _DBL_TR_SUB);
	} else {
		WD_LOOP(16, TextDotX + 16, _DBL_TR_SUB2);
	}

}

static INLINE void WinDraw_DrawPriLine(void)
{
#define _DPL_SUB(SUFFIX) WD_SUB(SUFFIX, Grp_LineBufSP[i])

	uint32_t adr = VLINE*FULLSCREEN_WIDTH;
	uint16_t w;
	int i;

	if (!tab5_p4blend_key0_overlay(ScrBuf, FULLSCREEN_WIDTH, 600u, VLINE,
	                               Grp_LineBufSP, 1024u, 0u, (uint32_t)TextDotX))
		WD_LOOP(0, TextDotX, _DPL_SUB);
}

/* Build 5.31: CPU reference for the common two-layer line: top color 0 is
 * transparent, otherwise top wins. BG/Sprite wins equal priorities in the
 * legacy compositor because it is drawn after GRP at equal priority. */
static INLINE void WinDraw_BlendBuffersCPU(uint16_t *dst, const uint16_t *bottom,
                                             const uint16_t *top, int width)
{
    int i = 0;
    for (; i + 3 < width; i += 4) {
        uint16_t t0 = top[i+0], t1 = top[i+1], t2 = top[i+2], t3 = top[i+3];
        dst[i+0] = t0 ? t0 : bottom[i+0];
        dst[i+1] = t1 ? t1 : bottom[i+1];
        dst[i+2] = t2 ? t2 : bottom[i+2];
        dst[i+3] = t3 ? t3 : bottom[i+3];
    }
    for (; i < width; ++i) {
        uint16_t t = top[i];
        dst[i] = t ? t : bottom[i];
    }
}

static INLINE void WinDraw_DrawGrpBGFusedCPU(uint16_t *dst)
{
    const uint16_t *grp = Grp_LineBuf;
    const uint16_t *bg = &BG_LineBuf[16];
    const int grp_pri = VCReg1[0] & 3;
    const int bg_pri = (VCReg1[0] >> 4) & 3;
    const uint16_t *top = (bg_pri <= grp_pri) ? bg : grp;
    const uint16_t *bottom = (bg_pri <= grp_pri) ? grp : bg;
    WinDraw_BlendBuffersCPU(dst, bottom, top, TextDotX);
}

/* Build 6.00: paired 256-colour renderer is production-proven.
 * The former first-live-line fused-vs-legacy A/B rendered the same scanline
 * twice and kept a 1600-byte internal reference buffer.  RC uses the fused
 * implementation directly; unusual geometry still takes the legacy fallback
 * selected by WinDraw_GetGrp8PairParams(). */
static INLINE void WinDraw_DrawGrp8PairChecked(int bottom_page, int top_page)
{
    if (TextDotX > 800) {
        Grp_DrawLine8(bottom_page, 1);
        Grp_DrawLine8(top_page, 0);
        return;
    }
    Grp_DrawLine8Pair(bottom_page, top_page);
}

/* Build 5.48: determine whether the already-validated common 256-colour
 * two-page line can be reconstructed from two physical page-pair lanes on
 * CPU0.  This is the same shared-scroll condition used by the 5.37 GVRAM
 * fast path, but returns immutable source geometry for the async snapshot. */
typedef struct {
    uint32_t by_lo_base, by_hi_base, ty_lo_base, ty_hi_base;
    uint32_t bx_lo, bx_hi, tx_lo, tx_hi;
} WinDraw_Grp8Geom;

/* Build 5.53a: return 1 for the original shared-scroll packed-lane path and
 * 2 for the new page-split path. Unequal X/Y scroll is no longer a reject:
 * CPU1 snapshots four page-specific byte streams into the same 3200-byte
 * source footprint and CPU0 reconstructs the exact two-page GRP8 result. */
static INLINE int WinDraw_GetGrp8PairParams(int bottom_page, int top_page,
                                            WinDraw_Grp8Geom *g)
{
    uint32_t by_lo, by_hi, ty_lo, ty_hi;
    bottom_page &= 1;
    top_page &= 1;
    if (!g || bottom_page == top_page)
        return 0;

    by_lo = GrphScrollY[bottom_page * 2] + VLINE;
    by_hi = GrphScrollY[bottom_page * 2 + 1] + VLINE;
    ty_lo = GrphScrollY[top_page * 2] + VLINE;
    ty_hi = GrphScrollY[top_page * 2 + 1] + VLINE;
    if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) {
        by_lo += VLINE; by_hi += VLINE;
        ty_lo += VLINE; ty_hi += VLINE;
    }

    g->by_lo_base = (by_lo & 0x1ffu) << 10;
    g->by_hi_base = (by_hi & 0x1ffu) << 10;
    g->ty_lo_base = (ty_lo & 0x1ffu) << 10;
    g->ty_hi_base = (ty_hi & 0x1ffu) << 10;
    g->bx_lo = GrphScrollX[bottom_page * 2] & 0x1ffu;
    g->bx_hi = GrphScrollX[bottom_page * 2 + 1] & 0x1ffu;
    g->tx_lo = GrphScrollX[top_page * 2] & 0x1ffu;
    g->tx_hi = GrphScrollX[top_page * 2 + 1] & 0x1ffu;

    const int same_y = g->by_lo_base == g->ty_lo_base &&
                       g->by_hi_base == g->ty_hi_base;
    const int same_x = g->bx_lo == g->tx_lo && g->bx_hi == g->tx_hi;
    if (WD_PERF_ACTIVE) {
        if (!same_y) ++s_hp_rej_scrolly;
        if (!same_x) ++s_hp_rej_scrollx;
    }
    return (same_y && same_x) ? 1 : 2;
}

static INLINE int WinDraw_QueueHostCommonTwoLayer(void)
{
    uint16_t *dst = &ScrBuf[VLINE * FULLSCREEN_WIDTH];
    const uint16_t *grp = Grp_LineBuf;
    const uint16_t *bg = &BG_LineBuf[16];
    const int grp_pri = VCReg1[0] & 3;
    const int bg_pri = (VCReg1[0] >> 4) & 3;
    const uint16_t *top = (bg_pri <= grp_pri) ? bg : grp;
    const uint16_t *bottom = (bg_pri <= grp_pri) ? grp : bg;

    /* Build 5.49: CPU/P4 measurements have repeatedly selected the host-core
     * CPU compositor over PPA+staging.  Submit the immutable raster line
     * directly to the TCM-mailbox compositor; synchronous CPU1 blend is only
     * the safety fallback when the host pool is unavailable. */
    if (tab5_compose_submit_line(VLINE, (uint32_t)TextDotX, bottom, top, dst)) {
        if (WD_PERF_ACTIVE) s_wd_perf_host_lines++;
        return 2;
    }
    WinDraw_BlendBuffersCPU(dst, bottom, top, TextDotX);
    if (WD_PERF_ACTIVE) s_wd_perf_host_lines++;
    return 1;
}

void WinDraw_DrawLine(void)
{
	int opaq, ton=0, gon=0, bgon=0, tron=0, pron=0, tdrawed=0;
    int grp8_async = 0, grp8_split = 0, grp8_bottom = 0, grp8_top = 0;
    int bgsp_async = 0;
    BG_HOST_LINE_STATE bgsp_state;
    WinDraw_Grp8Geom grp8_geom = {0};

	if(VLINE==(uint32_t)-1) {
			return;
	}
	if (!TextDirtyLine[VLINE]) {
			return;
	}

	TextDirtyLine[VLINE] = 0;
	if (WD_PERF_ACTIVE) s_wd_perf_dirty_lines++;

    /* Build 5.45: publish write activity without ever waiting on Core1. */
    tab5_video_fb_line_write_begin(VLINE);

	if (Debug_Grp)
	{
	switch(VCReg0[1]&3)
	{
	case 0:					/* 16 colors */
        if (WD_PERF_ACTIVE) ++s_hp_mode16;
		if (VCReg0[1]&4)		/* 1024dot */
		{
			if (VCReg2[1]&0x10)
			{
				if ( (VCReg2[0]&0x14)==0x14 )
				{
					WD_PERF_GRP(Grp_DrawLine4hSP());
					pron = tron = 1;
				}
				else
				{
					WD_PERF_GRP(Grp_DrawLine4h());
					gon=1;
				}
			}
		}
		else				/* 512dot */
		{
			if ( (VCReg2[0]&0x10)&&(VCReg2[1]&1) )
			{
				WD_PERF_GRP(Grp_DrawLine4SP((VCReg1[1]   )&3/*, 1*/));			/* ȾƩ���β����� */
				pron = tron = 1;
			}
			opaq = 1;
			if (VCReg2[1]&8)
			{
				WD_PERF_GRP(Grp_DrawLine4((VCReg1[1]>>6)&3, 1));
				opaq = 0;
				gon=1;
			}
			if (VCReg2[1]&4)
			{
				WD_PERF_GRP(Grp_DrawLine4((VCReg1[1]>>4)&3, opaq));
				opaq = 0;
				gon=1;
			}
			if (VCReg2[1]&2)
			{
				if ( ((VCReg2[0]&0x1e)==0x1e)&&(tron) )
					WD_PERF_GRP(Grp_DrawLine4TR((VCReg1[1]>>2)&3, opaq));
				else
					WD_PERF_GRP(Grp_DrawLine4((VCReg1[1]>>2)&3, opaq));
				opaq = 0;
				gon=1;
			}
			if (VCReg2[1]&1)
			{
				if ( (VCReg2[0]&0x14)!=0x14 )
				{
					WD_PERF_GRP(Grp_DrawLine4((VCReg1[1]   )&3, opaq));
					gon=1;
				}
			}
		}
		break;
	case 1:	
	case 2:	
        if (WD_PERF_ACTIVE) ++s_hp_mode256;
		opaq = 1; /* 256 colors */

        /* Build 5.36: common game path.  With both normal 256-colour pages
         * enabled and no special/half-transparent graphics mode, the legacy
         * code below always draws one page opaque and the other transparently.
         * Fuse those two calls into one palette/store pass. */
        if (WD_PERF_ACTIVE) {
            if ((VCReg2[1] & 0x05) != 0x05) ++s_hp_rej_pages;
            if (VCReg2[0] & 0x10) ++s_hp_rej_special;
        }
        if (((VCReg2[1] & 0x05) == 0x05) && !(VCReg2[0] & 0x10))
        {
            if (WD_PERF_ACTIVE) ++s_hp_256_common;
            grp8_bottom = ((VCReg1[1] & 3) <= ((VCReg1[1] >> 4) & 3)) ? 1 : 0;
            grp8_top = grp8_bottom ^ 1;

            /* Build 6.00: fused renderer was validated during development;
             * production enters the async geometry path immediately. Build 5.49 uses
             * the measured CPU-DIRECT/host path and no longer stages a PPA probe.
             * Only shared-scroll lines use this path; every special/raster
             * case falls back to the exact 5.47 renderer. */
            {
                int gkind = WinDraw_GetGrp8PairParams(grp8_bottom, grp8_top, &grp8_geom);
                if (gkind) {
                    grp8_async = 1;
                    grp8_split = (gkind == 2);
                    if (WD_PERF_ACTIVE) {
                        ++s_hp_async_geom;
                        if (grp8_split) ++s_hp_split_geom;
                    }
                } else {
                    WD_PERF_GRP(WinDraw_DrawGrp8PairChecked(grp8_bottom, grp8_top));
                }
            }
            gon = 1;
        }
        else if ( (VCReg1[1]&3) <= ((VCReg1[1]>>4)&3) ) /* same priority: GRP0 wins */
		{
			if ( (VCReg2[0]&0x10)&&(VCReg2[1]&1) )
			{
				WD_PERF_GRP(Grp_DrawLine8SP(0));
				tron = pron = 1;
			}
			if (VCReg2[1]&4)
			{
				if ( ((VCReg2[0]&0x1e)==0x1e)&&(tron) )
					WD_PERF_GRP(Grp_DrawLine8TR(1, 1));
				else if ( ((VCReg2[0]&0x1d)==0x1d)&&(tron) )
					WD_PERF_GRP(Grp_DrawLine8TR_GT(1, 1));
				else
					WD_PERF_GRP(Grp_DrawLine8(1, 1));
				opaq = 0;
				gon=1;
			}
			if (VCReg2[1]&1)
			{
				if ( (VCReg2[0]&0x14)!=0x14 )
				{
					WD_PERF_GRP(Grp_DrawLine8(0, opaq));
					gon=1;
				}
			}
		}
		else
		{
			if ( (VCReg2[0]&0x10)&&(VCReg2[1]&1) )
			{
				WD_PERF_GRP(Grp_DrawLine8SP(1));
				tron = pron = 1;
			}
			if (VCReg2[1]&4)
			{
				if ( ((VCReg2[0]&0x1e)==0x1e)&&(tron) )
					WD_PERF_GRP(Grp_DrawLine8TR(0, 1));
				else if ( ((VCReg2[0]&0x1d)==0x1d)&&(tron) )
					WD_PERF_GRP(Grp_DrawLine8TR_GT(0, 1));
				else
					WD_PERF_GRP(Grp_DrawLine8(0, 1));
				opaq = 0;
				gon=1;
			}
			if (VCReg2[1]&1)
			{
				if ( (VCReg2[0]&0x14)!=0x14 )
				{
					WD_PERF_GRP(Grp_DrawLine8(1, opaq));
					gon=1;
				}
			}
		}
		break;
	case 3:					/* 65536 colors */
        if (WD_PERF_ACTIVE) ++s_hp_mode65k;
		if (VCReg2[1]&15)
		{
			if ( (VCReg2[0]&0x14)==0x14 )
			{
				WD_PERF_GRP(Grp_DrawLine16SP());
				tron = pron = 1;
			}
			else
			{
				WD_PERF_GRP(Grp_DrawLine16());
				gon=1;
			}
		}
		break;
	}
	}


	if ( ((VCReg1[0]&0x30)>>2) < (VCReg1[0]&0x0c) )
	{						/* BG�������� */
		if ((VCReg2[1]&0x20)&&(Debug_Text))
		{
			WD_PERF_TEXT(Text_DrawLine(1));
			ton = 1;
		}
		else
			tab5_pie_graphics_fill8(Text_TrFlag, 0u, (uint32_t)TextDotX + 16u);

		if ((VCReg2[1]&0x40)&&(BG_Regs[8]&2)&&(!(BG_Regs[0x11]&2))&&(Debug_Sp))
		{
			int s1, s2;
			s1 = (((BG_Regs[0x11]  &4)?2:1)-((BG_Regs[0x11]  &16)?1:0));
			s2 = (((CRTC_Regs[0x29]&4)?2:1)-((CRTC_Regs[0x29]&16)?1:0));
			VLINEBG = VLINE;
			VLINEBG <<= s1;
			VLINEBG >>= s2;
			if ( !(BG_Regs[0x11]&16) ) VLINEBG -= ((BG_Regs[0x0f]>>s1)-(CRTC_Regs[0x0d]>>s2));
            if (grp8_async && !ton) {
                if (WD_PERF_ACTIVE) {
                    int64_t _t = esp_timer_get_time();
                    bgsp_async = BG_CaptureHostLineState(&bgsp_state, VLINEBG, 0);
                    s_wd_perf_bg_us += (uint64_t)(esp_timer_get_time() - _t);
                    s_wd_perf_bg_calls++;
                } else {
                    bgsp_async = BG_CaptureHostLineState(&bgsp_state, VLINEBG, 0);
                }
            }
            if (!bgsp_async)
                WD_PERF_BG(BG_DrawLine(!ton, 0));
			bgon = 1;
		}
	}
	else
	{						/* Text�������� */
		if ((VCReg2[1]&0x40)&&(BG_Regs[8]&2)&&(!(BG_Regs[0x11]&2))&&(Debug_Sp))
		{
			int s1, s2;
			s1 = (((BG_Regs[0x11]  &4)?2:1)-((BG_Regs[0x11]  &16)?1:0));
			s2 = (((CRTC_Regs[0x29]&4)?2:1)-((CRTC_Regs[0x29]&16)?1:0));
			VLINEBG = VLINE;
			VLINEBG <<= s1;
			VLINEBG >>= s2;
			if ( !(BG_Regs[0x11]&16) ) VLINEBG -= ((BG_Regs[0x0f]>>s1)-(CRTC_Regs[0x0d]>>s2));
			tab5_pie_graphics_fill8(Text_TrFlag, 0u, (uint32_t)TextDotX + 16u);
            if (grp8_async && !((VCReg2[1]&0x20)&&(Debug_Text))) {
                if (WD_PERF_ACTIVE) {
                    int64_t _t = esp_timer_get_time();
                    bgsp_async = BG_CaptureHostLineState(&bgsp_state, VLINEBG, 1);
                    s_wd_perf_bg_us += (uint64_t)(esp_timer_get_time() - _t);
                    s_wd_perf_bg_calls++;
                } else {
                    bgsp_async = BG_CaptureHostLineState(&bgsp_state, VLINEBG, 1);
                }
            }
            if (!bgsp_async)
                WD_PERF_BG(BG_DrawLine(1, 1));
			bgon = 1;
		}
		else
		{
			if ((VCReg2[1]&0x20)&&(Debug_Text))
			{
				int i;
				for (i = 16; i < TextDotX + 16; ++i)
					BG_LineBuf[i] = TextPal[0];
			} else {		/* 20010120 �����ῧ�� */
				tab5_pie_graphics_fill16(&BG_LineBuf[16], 0u, (uint32_t)TextDotX);
			}
			tab5_pie_graphics_fill8(Text_TrFlag, 0u, (uint32_t)TextDotX + 16u);
			bgon = 1;
		}

		if ((VCReg2[1]&0x20)&&(Debug_Text))
		{
			WD_PERF_TEXT(Text_DrawLine(!bgon));
			ton = 1;
		}
	}

	/* Build 5.31 PPA fast path: stage consecutive normal GRP+BG/Sprite
	 * lines in internal SRAM and blend up to eight rows per transaction. */
    if (WD_PERF_ACTIVE && grp8_async && !(gon && bgon && !ton && !tron && !pron)) {
        if (!bgon) ++s_hp_rej_nobg;
        if (ton) ++s_hp_rej_text;
        if (tron) ++s_hp_rej_tron;
        if (pron) ++s_hp_rej_pron;
    }
	if (gon && bgon && !ton && !tron && !pron)
	{
        int compose_result;

/* Intent: Avoid duplicate rendering across cores: CPU1 captures authoritative guest sources and CPU0 owns the final host-visible blend.  Layer8 Aug/17/2026 */
        /* Build 5.48 common-game pipeline.  CPU1 snapshots two raw GVRAM
         * pair lanes + palette + raster-correct BG line into internal SRAM.
         * CPU0 reconstructs GRP and performs final priority blend in one pass.
         * If the queue is temporarily full, generate GRP synchronously and use
         * the proven 5.46 final-compose path for this line only. */
        if (grp8_async) {
            if (WD_PERF_ACTIVE) ++s_hp_submit;
            const int grp_pri = VCReg1[0] & 3;
            const int bg_pri = (VCReg1[0] >> 4) & 3;
            const int bg_on_top = (bg_pri <= grp_pri);
            uint16_t *dst = &ScrBuf[VLINE * FULLSCREEN_WIDTH];
            int accepted;
            const uint16_t *split_ref = NULL;
            if (grp8_split && tab5_compose_grp8split_needs_selfcheck()) {
                /* One line only: preserve the exact stock result so CPU0's
                 * unequal-scroll reconstruction validates itself on target. */
                WD_PERF_GRP(Grp_DrawLine8Pair(grp8_bottom, grp8_top));
                split_ref = Grp_LineBuf;
            }
            if (WD_PERF_ACTIVE) {
                int64_t _t = esp_timer_get_time();
                if (grp8_split) {
                    accepted = bgsp_async
                        ? tab5_compose_submit_grp8split_bgsp_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.ty_lo_base, grp8_geom.ty_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_geom.tx_lo, grp8_geom.tx_hi,
                            grp8_bottom, grp8_top, GrphPal, TextPal, &bgsp_state,
                            bg_on_top, dst, split_ref)
                        : tab5_compose_submit_grp8split_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.ty_lo_base, grp8_geom.ty_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_geom.tx_lo, grp8_geom.tx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst, split_ref);
                } else {
                    accepted = bgsp_async
                        ? tab5_compose_submit_grp8pair_bgsp_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, TextPal, &bgsp_state,
                            bg_on_top, dst)
                        : tab5_compose_submit_grp8pair_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst);
                }
                s_wd_perf_grp_us += (uint64_t)(esp_timer_get_time() - _t);
                s_wd_perf_grp_calls++;
                s_wd_perf_blend_calls++;
            } else {
                if (grp8_split) {
                    accepted = bgsp_async
                        ? tab5_compose_submit_grp8split_bgsp_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.ty_lo_base, grp8_geom.ty_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_geom.tx_lo, grp8_geom.tx_hi,
                            grp8_bottom, grp8_top, GrphPal, TextPal, &bgsp_state,
                            bg_on_top, dst, split_ref)
                        : tab5_compose_submit_grp8split_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.ty_lo_base, grp8_geom.ty_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_geom.tx_lo, grp8_geom.tx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst, split_ref);
                } else {
                    accepted = bgsp_async
                        ? tab5_compose_submit_grp8pair_bgsp_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, TextPal, &bgsp_state,
                            bg_on_top, dst)
                        : tab5_compose_submit_grp8pair_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst);
                }
            }
            if (accepted) {
                if (WD_PERF_ACTIVE) {
                    ++s_hp_accept;
                    if (grp8_split) ++s_hp_split_accept;
                }
                if (!s_grp8host_reported) {
                    s_grp8host_reported = 1;
                    printf("PX68K_GRP8HOST: Build 5.53a raw GVRAM + Internal-SRAM BG/SP -> CPU0 common-line render ACTIVE (shared + unequal-scroll raw16)\n");
                }
                return; /* CPU0 owns write_end after final RGB565 commit. */
            }

            if (WD_PERF_ACTIVE) ++s_hp_rej_queue;
            /* If the host-BG submission itself failed, reconstruct the exact
             * guest-side BG line before falling through to the 5.49 fallback. */
            if (bgsp_async) {
                tab5_pie_graphics_fill8(Text_TrFlag, 0u, (uint32_t)TextDotX + 16u);
                WD_PERF_BG(BG_DrawLine(1, bgsp_state.gd));
                bgsp_async = 0;
            }
            WD_PERF_GRP(WinDraw_DrawGrp8PairChecked(grp8_bottom, grp8_top));
            grp8_async = 0;
        }

		if (WD_PERF_ACTIVE) {
			int64_t _t = esp_timer_get_time();
			compose_result = WinDraw_QueueHostCommonTwoLayer();
			s_wd_perf_blend_us += (uint64_t)(esp_timer_get_time() - _t);
			s_wd_perf_blend_calls++;
		} else {
			compose_result = WinDraw_QueueHostCommonTwoLayer();
		}
        if (compose_result != 2)
            tab5_video_fb_line_write_end(VLINE);
		return;
	}

	/* A legacy/special compositor line cannot join the pending normal block. */
	opaq = 1;

	/* Pri = 2 or 3�ʺǲ��̡ˤ����ꤵ��Ƥ�����̤�ɽ��
	 * �ץ饤����ƥ���Ʊ�����ϡ�GRP<SP<TEXT���ʥɥ饹�ԡ�������YsIII����

	 * Grp���Text����ˤ������Text�Ȥ�ȾƩ����Ԥ��ȡ�SP�Υץ饤����ƥ���
	 * Text�˰��������롩�ʤĤޤꡢGrp��겼�ˤ��äƤ�SP��ɽ������롩��
	 * KnightArms�Ȥ��򸫤�ȡ�ȾƩ���Υ١����ץ졼��ϰ��־�ˤʤ�ߤ����ġ�
	 */

	if ( (VCReg1[0]&0x02) )
	{
		if (gon)
		{
			WD_PERF_BLEND(WinDraw_DrawGrpLine(opaq));
			opaq = 0;
		}
		if (tron)
		{
			WD_PERF_BLEND(WinDraw_DrawGrpLineNonSP(opaq));
			opaq = 0;
		}
	}
	if ( (VCReg1[0]&0x20)&&(bgon) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&((VCReg1[0]&0x03)!=0x02)&&(tron) )
		{
			if ( (VCReg1[0]&3)<((VCReg1[0]>>2)&3) )
			{
				WD_PERF_BLEND(WinDraw_DrawBGLineTR(opaq));
				tdrawed = 1;
				opaq = 0;
			}
		}
		else
		{
			WD_PERF_BLEND(WinDraw_DrawBGLine(opaq, /*0*/tdrawed));
			tdrawed = 1;
			opaq = 0;
		}
	}
	if ( (VCReg1[0]&0x08)&&(ton) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&((VCReg1[0]&0x03)!=0x02)&&(tron) )
			WD_PERF_BLEND(WinDraw_DrawTextLineTR(opaq));
		else
			WD_PERF_BLEND(WinDraw_DrawTextLine(opaq, tdrawed/*((VCReg1[0]&0x30)>=0x20)*/));
		opaq = 0;
		tdrawed = 1;
	}

	/* Pri = 1��2���ܡˤ����ꤵ��Ƥ�����̤�ɽ�� */
	if ( ((VCReg1[0]&0x03)==0x01)&&(gon) )
	{
		WD_PERF_BLEND(WinDraw_DrawGrpLine(opaq));
		opaq = 0;
	}
	if ( ((VCReg1[0]&0x30)==0x10)&&(bgon) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&(!(VCReg1[0]&0x03))&&(tron) )
		{
			if ( (VCReg1[0]&3)<((VCReg1[0]>>2)&3) )
			{
				WD_PERF_BLEND(WinDraw_DrawBGLineTR(opaq));
				tdrawed = 1;
				opaq = 0;
			}
		}
		else
		{
			WD_PERF_BLEND(WinDraw_DrawBGLine(opaq, ((VCReg1[0]&0xc)==0x8)));
			tdrawed = 1;
			opaq = 0;
		}
	}
	if ( ((VCReg1[0]&0x0c)==0x04) && ((VCReg2[0]&0x5d)==0x1d) && (VCReg1[0]&0x03) && (((VCReg1[0]>>4)&3)>(VCReg1[0]&3)) && (bgon) && (tron) )
	{
		WD_PERF_BLEND(WinDraw_DrawBGLineTR(opaq));
		tdrawed = 1;
		opaq = 0;
		if (tron)
		{
			WD_PERF_BLEND(WinDraw_DrawGrpLineNonSP(opaq));
		}
	}
	else if ( ((VCReg1[0]&0x03)==0x01)&&(tron)&&(gon)&&(VCReg2[0]&0x10) )
	{
		WD_PERF_BLEND(WinDraw_DrawGrpLineNonSP(opaq));
		opaq = 0;
	}
	if ( ((VCReg1[0]&0x0c)==0x04)&&(ton) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&(!(VCReg1[0]&0x03))&&(tron) )
			WD_PERF_BLEND(WinDraw_DrawTextLineTR(opaq));
		else
			/* FIXME: Verify corrent td param here. Games like Overdriver
			 * expect a td value of 1 here, yet the condition
			 * ((VCReg1[0]&0x30)>=0x10) returns 0 for this game causing
			 * grp > text. See stage 2 of the game. */
			WD_PERF_BLEND(WinDraw_DrawTextLine(opaq, ((VCReg1[0]&0x30)>=0x10)));
		opaq = 0;
		tdrawed = 1;
	}

	/* Pri = 0�ʺ�ͥ��ˤ����ꤵ��Ƥ�����̤�ɽ�� */
	if ( (!(VCReg1[0]&0x03))&&(gon) )
	{
		WD_PERF_BLEND(WinDraw_DrawGrpLine(opaq));
		opaq = 0;
	}
	if ( (!(VCReg1[0]&0x30))&&(bgon) )
	{
		WD_PERF_BLEND(WinDraw_DrawBGLine(opaq, /*tdrawed*/((VCReg1[0]&0xc)>=0x4)));
		tdrawed = 1;
		opaq = 0;
	}
	if ( (!(VCReg1[0]&0x0c)) && ((VCReg2[0]&0x5d)==0x1d) && (((VCReg1[0]>>4)&3)>(VCReg1[0]&3)) && (bgon) && (tron) )
	{
		WD_PERF_BLEND(WinDraw_DrawBGLineTR(opaq));
		tdrawed = 1;
		opaq = 0;
		if (tron)
		{
			WD_PERF_BLEND(WinDraw_DrawGrpLineNonSP(opaq));
		}
	}
	else if ( (!(VCReg1[0]&0x03))&&(tron)&&(VCReg2[0]&0x10) )
	{
		WD_PERF_BLEND(WinDraw_DrawGrpLineNonSP(opaq));
		opaq = 0;
	}
	if ( (!(VCReg1[0]&0x0c))&&(ton) )
	{
		WD_PERF_BLEND(WinDraw_DrawTextLine(opaq, 1));
		tdrawed = 1;
		opaq = 0;
	}

	/* �ü�ץ饤����ƥ����Υ���ե��å� */
	if ( ((VCReg2[0]&0x5c)==0x14)&&(pron) )	/* �ü�Pri���ϡ��оݥץ졼��ӥåȤϰ�̣��̵���餷���ʤĤ���ӡ��� */
	{
		WD_PERF_BLEND(WinDraw_DrawPriLine());
	}
	else if ( ((VCReg2[0]&0x5d)==0x1c)&&(tron) )	/* ȾƩ���������Ƥ�Ʃ���ʥɥåȤ�ϡ��ե��顼������ */
	{						/* ��AQUALES�� */
#define _DL_SUB(SUFFIX)                                    \
	{                                                      \
		w = Grp_LineBufSP[i];                              \
		if (w != 0 && (ScrBuf##SUFFIX[adr] & 0xffff) == 0) \
			ScrBuf##SUFFIX[adr] = (w & Pal_HalfMask) >> 1; \
	}

		uint32_t adr = VLINE*FULLSCREEN_WIDTH;
		uint16_t w;
		int i;

		WD_LOOP(0, TextDotX, _DL_SUB);
	}

	if (opaq)
	{
		uint32_t adr = VLINE*FULLSCREEN_WIDTH;
		WD_PERF_CLEAR(tab5_pie_graphics_fill16(&ScrBuf[adr], 0u, (uint32_t)TextDotX));
	}

    tab5_video_fb_line_write_end(VLINE);
}

/********** menu ��Ϣ�롼���� **********/

struct _px68k_menu
{
	uint16_t *sbp;    /* surface buffer ptr */
	uint16_t *mlp;    /* menu locate ptr */
	uint16_t mcolor;  /* color of chars to write */
	uint16_t mbcolor; /* back ground color of chars to write */
	int ml_x;
	int ml_y;
	int mfs;      /* menu font size; */
} p6m;

/* sjis��jis�������Ѵ� */
static uint16_t sjis2jis(uint16_t w)
{
	uint8_t wh = w / 256;
	uint8_t wl = w % 256;

	wh <<= 1;
	if (wl < 0x9f)
	{
		wh += (wh < 0x3f)? 0x1f : -0x61;
		wl -= (wl > 0x7e)? 0x20 : 0x1f;
	}
	else
	{
		wh += (wh < 0x3f)? 0x20 : -0x60;
		wl -= 0x7e;
	}

	return (wh * 256 + wl);
}

/* JIS�����ɤ���0 origin��index���Ѵ����� */
/* ������0x2921-0x2f7e��X68K��ROM��ˤʤ��Τ����Ф� */
static uint16_t jis2idx(uint16_t jc)
{
	if (jc >= 0x3000)
		jc -= 0x3021;
	else
		jc -= 0x2121;
	jc = jc % 256 + (jc / 256) * 0x5e;

	return jc;
}

#define isHankaku(s) ((s) >= 0x20 && (s) <= 0x7e || (s) >= 0xa0 && (s) <= 0xdf)
#define MENU_WIDTH 800

/* fs : font size : 16 or 24
 * Ⱦ��ʸ���ξ���16bit�ξ��8bit�˥ǡ���������Ƥ�������
 * (Ⱦ��or���Ѥ�Ƚ�Ǥ��Ǥ���褦��)
 */
static uint32_t get_font_addr(uint16_t sjis, int fs)
{
	uint16_t jis, j_idx;
	uint8_t jhi;
	int fsb; /* file size in bytes */

	/* Ⱦ��ʸ�� */
	if (isHankaku(sjis >> 8)) {
		switch (fs) {
		case 8:
			return (0x3a000 + (sjis >> 8) * (1 * 8));
		case 16:
			return (0x3a800 + (sjis >> 8) * (1 * 16));
		case 24:
			return (0x3d000 + (sjis >> 8) * (2 * 24));
		default:
			return -1;
		}
	}

	/* ����ʸ�� */
	if (fs == 16)
		fsb = 2 * 16;
	else if (fs == 24)
		fsb = 3 * 24;
	else
		return -1;

	jis   = sjis2jis(sjis);
	j_idx = (uint32_t)jis2idx(jis);
	jhi   = (uint8_t)(jis >> 8);

	/* ����� */
	if (jhi >= 0x21 && jhi <= 0x28)
		return  ((fs == 16)? 0x0 : 0x40000) + j_idx * fsb;
	/* �����/������ */
	else if (jhi >= 0x30 && jhi <= 0x74)
		return  ((fs == 16)? 0x5e00 : 0x4d380) + j_idx * fsb;
	/* �����ˤ��뤳�ȤϤʤ��Ϥ� */
	return -1;
}

/* RGB565 */

/* ����饯��ʸ���κ�ɸ (������1��ɸ��Ⱦ��ʸ�����ˤʤ�) */
static void set_mlocateC(int x, int y)
{
	p6m.ml_x = x * p6m.mfs / 2, p6m.ml_y = y * p6m.mfs;
}

static uint16_t *get_ml_ptr(void)
{
	p6m.mlp = p6m.sbp + MENU_WIDTH * p6m.ml_y + p6m.ml_x;
	return p6m.mlp;
}

/* ��Ⱦ��ʸ���ξ���16bit�ξ��8bit�˥ǡ���������Ƥ�������
 *   (Ⱦ��or���Ѥ�Ƚ�Ǥ��Ǥ���褦��)
 * ��ɽ������ʬcursor����˰�ư����
 */
static void draw_char(uint16_t sjis)
{
	int i, j, k, wc, w;
	uint8_t c;
	uint16_t bc;
	int h    = p6m.mfs;
	uint16_t *p  = get_ml_ptr();
	uint32_t f  = get_font_addr(sjis, h);

	if (f < 0)
		return;

	/* h=8��Ⱦ�ѤΤ� */
	w = (h == 8)? 8 : (isHankaku(sjis >> 8)? h / 2 : h);

	for (i = 0; i < h; i++) {
		wc = w;
		for (j = 0; j < ((w % 8 == 0)? w / 8 : w / 8 + 1); j++) {
			c = FONT[f++];
			for (k = 0; k < 8 ; k++) {
				bc = p6m.mbcolor? p6m.mbcolor : *p;
				*p = (c & 0x80)? p6m.mcolor : bc;
				p++;
				c = c << 1;
				wc--;
				if (wc == 0)
					break;
			}
		}
		p = p + MENU_WIDTH - w;
	}

	p6m.ml_x += w;
}

static void draw_str(char *cp)
{
	int i;
	uint16_t wc;
	int len    = strlen(cp);
	uint8_t *s = (uint8_t *)cp;

	for (i = 0; i < len; i++) {
		if (isHankaku(*s)) {
			/* �ǽ��8bit��Ⱦ���Ѥ�Ƚ�Ǥ���Τ�Ⱦ�Ѥξ���
			 * ���餫����8bit�����եȤ��Ƥ��� */
			draw_char((uint16_t)*s << 8);
			s++;
		} else {
			wc = (uint16_t)(*s << 8) + *(s + 1);
			draw_char(wc);
			s += 2;
			i++;
		}
		/* 8x8����(���եȥ����ܡ��ɤ�FUNC������ʸ������̤��) */
		if (p6m.mfs == 8) {
			p6m.ml_x -= 3;
		}
	}
}

int WinDraw_MenuInit(void)
{
	p6m.sbp     = menu_buffer;
	p6m.mfs     = 16;
	p6m.mcolor  = 0xffff;
	p6m.mbcolor = 0;
	return 1;
}

#include "menu_str_sjis.txt"
char menu_item_desc[][60] = {
	"Reset / NMI reset / Quit",
	"Change / Eject floppy 0",
	"Change / Eject floppy 1",
	"Change / Eject HDD 0",
	"Change / Eject HDD 1"
};

void WinDraw_DrawMenu(int menu_state, int mkey_pos, int mkey_y, int *mval_y)
{
	int i, drv;
	char tmp[256];

	p6m.sbp     = menu_buffer;
	p6m.mfs     = Config.MenuFontSize ? 24 : 16;

	/* �����ȥ� */
	p6m.mcolor  = 0x07ff; /* cyan */
	set_mlocateC(0, 0);
	draw_str(twaku_str);
	set_mlocateC(0, 1);
	draw_str(twaku2_str);
	set_mlocateC(0, 2);
	draw_str(twaku3_str);

	p6m.mcolor  = 0xffff;
	set_mlocateC(2, 1);
        strcpy(tmp, title_str);
        strcat(tmp, PX68KVERSTR);
	draw_str(tmp);

	
	p6m.mcolor  = 0xffff; /* ������ */

	/* �������� */
	p6m.mcolor  = 0xffe0; /* yellow */
	set_mlocateC(1, 4);
	draw_str(waku_str);
	for (i = 5; i < 10; i++)
	{
		set_mlocateC(1, i);
		draw_str(waku2_str);
	}
	set_mlocateC(1, 10);
	draw_str(waku3_str);

	/* �����ƥ�/������� */
	p6m.mcolor = 0xffff;
	for (i = 0; i < 5; i++)
	{
		set_mlocateC(3, 5 + i);
		if (menu_state == MS_KEY && i == (mkey_y - mkey_pos))
		{
			p6m.mcolor  = 0x0;
			p6m.mbcolor = 0xffe0;
		}
		else
		{
			p6m.mcolor  = 0xffff;
			p6m.mbcolor = 0x0;
		}
		draw_str(menu_item_key[i + mkey_pos]);
	}

	/* �����ƥ�/������ */
	p6m.mcolor  = 0xffff;
	p6m.mbcolor = 0x0;
	for (i = 0; i < 5; i++)
	{
		if (       (menu_state == MS_VALUE 
               || menu_state == MS_HWJOY_SET)
               && i == (mkey_y - mkey_pos))
		{
			p6m.mcolor  = 0x0;
			p6m.mbcolor = 0xffe0;
		}
		else
		{
			p6m.mcolor  = 0xffff;
			p6m.mbcolor = 0x0;
		}
		set_mlocateC(17, 5 + i);

		drv = WinUI_get_drv_num(i + mkey_pos);
		if (drv >= 0  && mval_y[i + mkey_pos] == 0)
		{
			char *p;
			if (drv < 2)
				p = Config.FDDImage[drv];
			else
				p = Config.HDImage[drv - 2];

			if (p[0] == '\0')
				draw_str(" -- no disk --");
			else
			{
				/* ��Ƭ�Υ����ȥǥ��쥯�ȥ�̾��ɽ�����ʤ� */
				char ptr[PATH_MAX];
				if (!strncmp(cur_dir_str, p, cur_dir_slen))
					strncpy(ptr, p + cur_dir_slen, sizeof(ptr));
				else
					strncpy(ptr, p, sizeof(ptr));
				ptr[40] = '\0';
				draw_str(ptr);
			}
		} else {
			draw_str(menu_items[i + mkey_pos][mval_y[i + mkey_pos]]);
		}
	}

	/* ���� */
	p6m.mcolor  = 0x07ff; /* cyan */
	p6m.mbcolor = 0x0;
	set_mlocateC(0, 11);
	draw_str(swaku_str);
	set_mlocateC(0, 12);
	draw_str(swaku2_str);
	set_mlocateC(0, 13);
	draw_str(swaku3_str);

	/* ����ץ���� */
	p6m.mcolor  = 0xffff;
	p6m.mbcolor = 0x0;
	set_mlocateC(2, 12);
	draw_str(menu_item_desc[mkey_y]);

	videoBuffer=(uint16_t*)menu_buffer;

}

void WinDraw_DrawMenufile(struct menu_flist *mfl)
{
	int i;
	char ptr[PATH_MAX];

   /* 0xf800 - red */
	/* 0xf81f - magenta */

	/* bottom frame */

	p6m.mcolor  = 0xffff;
	p6m.mbcolor = 0x1; /* 0 means transparent */
	set_mlocateC(1, 1);
	draw_str(swaku_str);
	for (i = 2; i < 16; i++)
   {
		set_mlocateC(1, i);
		draw_str(swaku2_str);
	}
	set_mlocateC(1, 16);
	draw_str(swaku3_str);

	for (i = 0; i < 14; i++)
	{
		if (i + 1 > mfl->num)
			break;
		if (i == mfl->y)
		{
			p6m.mcolor  = 0x0;
			p6m.mbcolor = 0xffff;
		}
		else
		{
			p6m.mcolor  = 0xffff;
			p6m.mbcolor = 0x1;
		}
		/* enclose directory in '[ ]' */
		set_mlocateC(3, i + 2);
		if (mfl->type[i + mfl->ptr])
         draw_str("[");
		strncpy(ptr, mfl->name[i + mfl->ptr], sizeof(ptr));
		ptr[56] = '\0';
		draw_str(ptr);
		if (mfl->type[i + mfl->ptr])
         draw_str("]");
	}

	p6m.mbcolor = 0x0; /* switch back to transparent mode */

	videoBuffer=(uint16_t*)menu_buffer;
}

void WinDraw_ClearMenuBuffer(void)
{
	tab5_pie_graphics_fill16(menu_buffer, 0u, 800u * 600u);
}
