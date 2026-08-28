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
#include "tab5_video_cpu1.h"
#include "tab5_video_flow.h"
#include "joystick.h"
#include "keyboard.h"

/* Research layer profiler is recoverable, but release builds compile the
 * hot-path timer branches and telemetry increments out completely. */
#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
# ifdef PX68K_TAB5_PERF_PROFILE
#  undef PX68K_TAB5_PERF_PROFILE
# endif
# define PX68K_TAB5_PERF_PROFILE 1
# define WD_PERF_ACTIVE (s_wd_perf_enabled)
# define WD_DIAG_INC(v) (++(v))
# define WD_DIAG_ADD(v,n) ((v) += (n))
#else
# ifdef PX68K_TAB5_PERF_PROFILE
#  undef PX68K_TAB5_PERF_PROFILE
# endif
# define PX68K_TAB5_PERF_PROFILE 0
# define WD_PERF_ACTIVE 0
# define WD_DIAG_INC(v) ((void)0)
# define WD_DIAG_ADD(v,n) ((void)0)
#endif

#define		SCREEN_WIDTH		768
#define		FULLSCREEN_WIDTH	800

extern uint16_t *videoBuffer;
uint16_t menu_buffer[800*600];

extern uint8_t Debug_Text, Debug_Grp, Debug_Sp;

/* BAT177NW3 ownership split:
 * - CPU1 remains the sole guest-semantic renderer and never asks CPU0/Screen for admission;
 * - source-skipping CPU0 raster paths remain quarantined;
 * - measured BAT177NW2 showed final-stage offload snapshots were more expensive
 *   than CPU1's exact final selector (all dirty lines copied 2.5-4 KiB into
 *   immutable compose packets). Keep final RGB565 on CPU1 and publish the
 *   completed RenderBuf line through the zero-wait latest-wins flow. */
#define TAB5_CPU1_AUTHORITATIVE_REFERENCE 1
#define TAB5_CPU0_FINAL_STAGE 0
/* R57E49 production async: only the proven normal-65K path leaves CPU1.
 * 256-color and legacy/special paths remain NW18 CPU1-authoritative. */
#define TAB5_R57E49_ASYNC65K 1
#define TAB5_R57E52_SUBMITFIX 1
#define TAB5_R57E53_PREFLIGHT 1
#define TAB5_CPU1_INLINE_GBT 1

static uint16_t *RenderBuf = 0;

/* BAT177NW4/R57E34: CPU1 exact-final fast path for the common normal
 * G+BG+TEXT case.  BAT177NW2 proved that cross-core immutable packet copies
 * cost far more than the final selector.  Keep ownership on CPU1, but replace
 * the stock multi-pass legacy compositor with the already-validated one-pass
 * GBT priority/key-zero rule.  A PIE block path is used only when every pixel
 * has a BG/TEXT candidate and both candidate priorities are on the same side
 * of GRP; all other cases use the exact flag-aware scalar one-pass rule. */
static int s_r57e34_pie_ok;

#if defined(__riscv)
__asm__(
    ".section .iram1,\"ax\"\n"
    ".align 2\n"
    ".global tab5_wd_r57e34_key0_8\n"
    ".type tab5_wd_r57e34_key0_8, @function\n"
    ".balign 4\n"
    "tab5_wd_r57e34_key0_8:\n"
    "beqz a3, 2f\n"
    "esp.xorq q7, q7, q7\n"
    "1:\n"
    "esp.vld.128.ip q0, a2, 16\n"
    "esp.vld.128.ip q1, a1, 16\n"
    "esp.vcmp.eq.u16 q2, q0, q7\n"
    "esp.andq q1, q1, q2\n"
    "esp.orq q0, q0, q1\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a3, a3, -1\n"
    "bnez a3, 1b\n"
    "2:\n"
    "ret\n"
    ".size tab5_wd_r57e34_key0_8, .-tab5_wd_r57e34_key0_8\n"
    ".previous\n"
);
extern void tab5_wd_r57e34_key0_8(uint16_t *dst, const uint16_t *bottom,
                                  const uint16_t *top, uint32_t blocks8);
#else
static void tab5_wd_r57e34_key0_8(uint16_t *dst, const uint16_t *bottom,
                                  const uint16_t *top, uint32_t blocks8)
{
    const uint32_t n = blocks8 * 8u;
    for (uint32_t i = 0; i < n; ++i) dst[i] = top[i] ? top[i] : bottom[i];
}
#endif

static inline int r57e34_flags_have_zero(const uint8_t *flags, uint32_t width)
{
    uint32_t i = 0;
    for (; i + 4u <= width; i += 4u) {
        uint32_t v;
        __builtin_memcpy(&v, flags + i, sizeof(v));
        if (((v - 0x01010101u) & ~v & 0x80808080u) != 0u) return 1;
    }
    for (; i < width; ++i) if ((flags[i] & 3u) == 0u) return 1;
    return 0;
}

static inline uint16_t r57e34_gbt_ref_pixel(uint16_t g, uint16_t bt, uint8_t f,
                                             uint8_t gp, uint8_t bp, uint8_t tp)
{
    f &= 3u; gp &= 3u; bp &= 3u; tp &= 3u;
    if (!f) return g;
    uint8_t p = 4u;
    if (f & 2u) p = bp;
    if ((f & 1u) && tp < p) p = tp;
    return (p <= gp) ? (bt ? bt : g) : (g ? g : bt);
}

static int r57e34_cpu1_gbt(uint16_t *dst, const uint16_t *grp,
                            const uint16_t *bt, const uint8_t *flags,
                            uint32_t width, uint8_t gp, uint8_t bp, uint8_t tp)
{
    gp &= 3u; bp &= 3u; tp &= 3u;

    /* No flags==0 means every pixel owns a BG/TEXT candidate.  When BG and
     * TEXT are both on the same side of GRP, their exact shared candidate has
     * one uniform key-zero relation to GRP, so a single PIE pass is exact. */
    const int btop = bp <= gp;
    const int ttop = tp <= gp;
    if (__builtin_expect(s_r57e34_pie_ok && btop == ttop &&
                         width >= 8u && !r57e34_flags_have_zero(flags, width), 1)) {
        const uint16_t *bottom = btop ? grp : bt;
        const uint16_t *top    = btop ? bt  : grp;
        const uintptr_t a = ((uintptr_t)dst | (uintptr_t)bottom | (uintptr_t)top) & 15u;
        if (a == 0u) {
            const uint32_t blocks = width >> 3;
            const uint32_t vec = blocks << 3;
            tab5_wd_r57e34_key0_8(dst, bottom, top, blocks);
            for (uint32_t i = vec; i < width; ++i)
                dst[i] = top[i] ? top[i] : bottom[i];
            return 1;
        }
    }

    for (uint32_t i = 0; i < width; ++i)
        dst[i] = r57e34_gbt_ref_pixel(grp[i], bt[i], flags[i], gp, bp, tp);
    return 0;
}

static void r57e34_cpu1_gbt_selfcheck(void)
{
    static uint16_t b[16] __attribute__((aligned(16)));
    static uint16_t t[16] __attribute__((aligned(16)));
    static uint16_t d[16] __attribute__((aligned(16)));

    for (uint32_t i = 0; i < 16u; ++i) {
        b[i] = (uint16_t)(0x1101u + i * 37u);
        t[i] = (i & 2u) ? 0u : (uint16_t)(0x8201u + i * 53u);
        d[i] = 0u;
    }
    tab5_wd_r57e34_key0_8(d, b, t, 2u);
    for (uint32_t i = 0; i < 16u; ++i) {
        if (d[i] != (t[i] ? t[i] : b[i])) {
            s_r57e34_pie_ok = 0;
            printf("PX68K_CPU1GBT_R57E34: PIE key0 self-check FAIL x=%lu got=%04X exp=%04X; scalar-only\n",
                   (unsigned long)i, d[i], t[i] ? t[i] : b[i]);
            return;
        }
    }

    /* Exhaustively validate the priority/flag decision table itself. */
    for (uint32_t gp = 0; gp < 4u; ++gp)
      for (uint32_t bp = 0; bp < 4u; ++bp)
        for (uint32_t tp = 0; tp < 4u; ++tp)
          for (uint32_t f = 0; f < 4u; ++f) {
              const uint16_t g = (f & 1u) ? 0x1234u : 0u;
              const uint16_t q = (f & 2u) ? 0x5678u : 0x9abcu;
              const uint16_t r = r57e34_gbt_ref_pixel(g, q, (uint8_t)f,
                                                       (uint8_t)gp, (uint8_t)bp, (uint8_t)tp);
              /* Same formula written structurally as the historical 6.14a
               * reference; keep the check independent of the fast loops. */
              uint8_t p = 4u;
              if (f & 2u) p = (uint8_t)bp;
              if ((f & 1u) && tp < p) p = (uint8_t)tp;
              const uint16_t e = !f ? g :
                  ((p <= gp) ? (q ? q : g) : (g ? g : q));
              if (r != e) {
                  s_r57e34_pie_ok = 0;
                  printf("PX68K_CPU1GBT_R57E34: priority self-check FAIL\n");
                  return;
              }
          }

    s_r57e34_pie_ok = 1;
    printf("PX68K_CPU1GBT_R57E34: one-pass exact GBT self-check PASS; PIE-key0 eligible rows armed; no snapshot/queue\n");
}


/* PX68K_R56R_COUNTERS
 * Counters-only taxonomy of the already-existing WinDraw return paths.
 * CPU1 is the sole writer/taker. No timers, queue changes, allocations, or
 * rendering decisions are introduced. 16 uint32_t = 64 bytes total. */
enum {
    R56R_LAT=0, R56R_LATCH_REJ, R56R_MODE16, R56R_MODE256, R56R_MODE65,
    R56R_CPU0_65, R56R_CPU0_GBT, R56R_CPU0_GRP8, R56R_CPU0_2L,
    R56R_CPU1_2L, R56R_CPU1_LEGACY, R56R_LEG16, R56R_LEG256, R56R_LEG65,
    R56R_65_QFULL, R56R_65_REJECT, R56R_CPU0_LEGACY,
    R56R_CPU1_GBT, R56R_CPU1_GBT_PIE, R56R_N
};
static uint32_t s_r56r_path[R56R_N];

void WinDraw_R56RPathTake(uint32_t *out, uint32_t count)
{
    if (!out || count < R56R_N) return;
    for (uint32_t i = 0; i < R56R_N; ++i) {
        out[i] = s_r56r_path[i];
        s_r56r_path[i] = 0u;
    }
}

/* Build 5.31: ESP32-P4 PPA host hooks live in the Tab5 application component. */
extern void *tab5_ppa_alloc_framebuffer(size_t bytes);
extern void tab5_ppa_free_framebuffer(void *ptr);
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

/* R56 Screen Manager display surface remains read-only here.  The CPU1
 * dirty/admission/ticket/commit lifecycle is owned by tab5_video_cpu1. */
extern const uint16_t *tab5_screen_readonly_work_buffer(void);

/* Build 5.46: Core1 normal two-layer compositor.  CPU0 snapshots the already
 * raster-correct GRP/BG line into internal SRAM; Core1 performs the final
 * key-zero blend into RenderBuf.  Return value 1 means the write is asynchronous. */
extern int tab5_compose_submit_line(uint32_t y, uint32_t width,
                                    const uint16_t *bottom, const uint16_t *top,
                                    uint16_t *dst, uint64_t render_seq,
                                    uint32_t video_epoch, uint32_t visual_seq);
extern int tab5_compose_submit_legacy_final(uint32_t y, uint32_t width,
                                             const uint16_t *grp,
                                             const uint16_t *grp_sp,
                                             const uint16_t *grp_sp2,
                                             const uint16_t *bg_text,
                                             const uint8_t *flags,
                                             uint8_t vc1_0, uint8_t vc2_0,
                                             int gon, int bgon, int ton, int tron, int pron,
                                             uint16_t half_mask, uint16_t ix2, uint16_t ibit,
                                             uint16_t *dst, uint64_t render_seq,
                                             uint32_t video_epoch, uint32_t visual_seq);
extern int tab5_compose_submit_grp8pair_line(uint32_t y, uint32_t width,
                                             const uint8_t *gvram,
                                             uint32_t y_lo_base, uint32_t y_hi_base,
                                             uint32_t x_lo, uint32_t x_hi,
                                             int bottom_page, int top_page,
                                             const uint16_t *palette,
                                             const uint16_t *bg, int bg_on_top,
                                             uint16_t *dst, uint64_t render_ticket);
extern int tab5_compose_submit_grp8pair_bgsp_line(uint32_t y, uint32_t width,
                                                  const uint8_t *gvram,
                                                  uint32_t y_lo_base, uint32_t y_hi_base,
                                                  uint32_t x_lo, uint32_t x_hi,
                                                  int bottom_page, int top_page,
                                                  const uint16_t *grph_palette,
                                                  const uint16_t *text_palette,
                                                  const BG_HOST_LINE_STATE *bg_state,
                                                  int bg_on_top, uint16_t *dst,
                                                  uint64_t render_ticket);
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
                                              const uint16_t *selfcheck_ref,
                                              uint64_t render_ticket);
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
                                                   const uint16_t *selfcheck_ref,
                                                   uint64_t render_ticket);
extern int tab5_compose_submit_gbt_line(uint32_t y, uint32_t width,
                                        const uint16_t *grp,
                                        const uint16_t *bg_text,
                                        const uint8_t *text_tr_flags,
                                        uint8_t grp_pri, uint8_t bg_pri,
                                        uint8_t text_pri, uint16_t *dst,
                                        uint64_t render_seq, uint32_t video_epoch,
                                        uint32_t visual_seq);
extern void tab5_guest_bus_post_raster(uint32_t vline); /* R57c CPU1->CPU0 ordered boundary */
extern uint32_t tab5_guest_bus_post_raster_hold(uint32_t vline);
extern void tab5_guest_bus_shadow_hold_cancel(uint32_t seq);
extern int tab5_compose_r57d_class_state(uint16_t key);
extern int tab5_compose_gbt65k_line_admit(uint32_t y, uint32_t height, uint64_t render_seq);
extern int tab5_compose_gbt65k_hostbt_state(void);
extern int tab5_compose_submit_gbt65k_exact_bt_line(uint32_t y, uint32_t width,
                                                    const uint8_t *gvram,
                                                    uint32_t gvram_row, uint32_t gvram_x,
                                                    const uint8_t *pal_regs,
                                                    uint32_t pal_generation, uint8_t contrast,
                                                    const uint16_t *bg_text,
                                                    const uint8_t *text_tr_flags,
                                                    const uint8_t *text_src, uint32_t text_valid,
                                                    uint32_t text_x, uint32_t text_y,
                                                    const uint16_t *text_palette,
                                                    const BG_HOST_LINE_STATE *bg_state,
                                                    int bg_on, int text_on,
                                                    uint8_t grp_pri, uint8_t bg_pri,
                                                    uint8_t text_pri, uint16_t *dst,
                                                    uint16_t r57d_class, uint32_t r57d_shadow_seq, uint64_t render_ticket);
extern int tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,
                                           const uint8_t *gvram,
                                           uint32_t gvram_row, uint32_t gvram_x,
                                           const uint8_t *pal_regs,
                                           uint32_t pal_generation, uint8_t contrast,
                                           const uint8_t *text_src, uint32_t text_valid,
                                           uint32_t text_x, uint32_t text_y, int shadow_text,
                                           const uint16_t *text_palette,
                                           const BG_HOST_LINE_STATE *bg_state,
                                           int bg_on, int text_on, int exact_host_bt,
                                           uint8_t grp_pri, uint8_t bg_pri,
                                           uint8_t text_pri, uint16_t *dst,
                                           uint32_t r57d_shadow_seq,
                                           uint64_t render_seq, uint32_t video_epoch,
                                           uint32_t visual_seq);
#ifdef ESP_PLATFORM
extern const uint8_t *TVRAM_GetExpandedPixels(void);
extern const uint8_t *TVRAM_GetExpandedLine(uint32_t y,uint32_t x,uint32_t width);
#endif
extern int tab5_compose_submit_gbt_scrollcache_line(uint32_t y, uint32_t width,
                                                     const uint8_t *gvram,
                                                     uint32_t y_lo_base, uint32_t y_hi_base,
                                                     uint32_t x_lo, uint32_t x_hi,
                                                     int bottom_page, int top_page,
                                                     const uint16_t *palette,
                                                     const uint16_t *bg_text,
                                                     const uint8_t *text_tr_flags,
                                                     uint8_t grp_pri, uint8_t bg_pri,
                                                     uint8_t text_pri, uint16_t *dst,
                                                     uint64_t render_ticket);
extern int tab5_compose_submit_gbt_rawpair_line(uint32_t y, uint32_t width,
                                                const uint8_t *gvram,
                                                uint32_t y_lo_base, uint32_t y_hi_base,
                                                uint32_t x_lo, uint32_t x_hi,
                                                int bottom_page, int top_page,
                                                const uint16_t *palette,
                                                const uint16_t *bg_text,
                                                const uint8_t *text_tr_flags,
                                                uint8_t grp_pri, uint8_t bg_pri,
                                                uint8_t text_pri, uint16_t *dst,
                                                uint64_t render_ticket);
extern int tab5_compose_grp8split_needs_selfcheck(void);

/* Build 5.49: the hardware PPA batch path is retired.  Keep only the legacy
 * perf counter name because existing diagnostics expose it as the count of
 * normal lines handed to the CPU0 host compositor. */
static uint32_t s_wd_perf_host_lines = 0;

/* First real two-page 256-colour line is cross-checked against the legacy
 * two-call renderer.  A mismatch permanently falls back to the old path. */
static int s_grp8host_reported = 0;
/* Build 6.15e: one-shot confirmation that the normal 65K raster has moved
 * to CPU0.  Internal development build numbers are intentionally not exposed
 * by public README, but the UART tag remains useful during this validation. */
static int s_gbt65k615e_reported = 0;
static uint32_t s_gbt65k615e_dropped_lines = 0;
static int s_gbt65k615e_reject_reported = 0;


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
/* BAT177NW9/R57E40: common one-pass GBT and latest-wins commit were outside
 * the legacy blend macros, so account them explicitly. */
static uint64_t s_wd_r57e39_gbt_us = 0;
static uint64_t s_wd_r57e39_commit_us = 0;
static uint32_t s_wd_r57e39_gbt_calls = 0;
static uint32_t s_wd_r57e39_commit_calls = 0;
static uint32_t s_wd_r57e39_arm_count = 0;
/* BAT177NW14/R57E44: cumulative exact 65K GRP+GBT fusion telemetry. */
static uint32_t s_wd_r57e44_fused_lines = 0;
static uint32_t s_wd_r57e44_fallback_lines = 0;
/* BAT177NW18/R57E48 FINAL: compact TEXT/BG-index -> raw65K final fusion.
 * The scratch is CPU1-private internal BSS and replaces two full RGB/flag
 * materialization passes on the proven common steady path. */
static uint32_t s_wd_r57e48_direct_lines = 0;
static uint32_t s_wd_r57e48_fallback_lines = 0;
static uint32_t s_wd_r57e48_text_reject = 0;
static uint32_t s_wd_r57e48_bg_reject = 0;
static uint32_t s_wd_r57e48_final_reject = 0;
static int s_wd_r57e48_selfcheck_ok = 0;
#ifdef ESP_PLATFORM
static uint8_t s_wd_r57e48_text_idx[800] __attribute__((aligned(16)));
static uint8_t s_wd_r57e48_bg_idx[1600] __attribute__((aligned(16)));
#endif
static uint32_t s_wd_perf_dirty_lines = 0;
static uint32_t s_wd_perf_grp_calls = 0;
static uint32_t s_wd_perf_text_calls = 0;
static uint32_t s_wd_perf_bg_calls = 0;
static uint32_t s_wd_perf_blend_calls = 0;
/* Build 6.13b: split the large guest-render bucket into actionable targets. */
static uint64_t s_wd613_grp_mode_us[3] = {0}; /* 16/256/65k */
static uint32_t s_wd613_grp_mode_calls[3] = {0};
static uint64_t s_wd613_bg_capture_us = 0, s_wd613_bg_draw_us = 0;
static uint32_t s_wd613_bg_capture_calls = 0, s_wd613_bg_draw_calls = 0;
static uint64_t s_wd613_blend_grp_us = 0, s_wd613_blend_bg_us = 0;
static uint64_t s_wd613_blend_text_us = 0, s_wd613_blend_pri_us = 0;
static uint64_t s_wd613_hostprep_us = 0, s_wd613_hostblend_us = 0;
static uint32_t s_wd613_blend_grp_calls = 0, s_wd613_blend_bg_calls = 0;
static uint32_t s_wd613_blend_text_calls = 0, s_wd613_blend_pri_calls = 0;
static uint32_t s_wd613_hostprep_calls = 0, s_wd613_hostblend_calls = 0;
static uint32_t s_wd613_layer_mask[8] = {0}; /* bit0=GRP bit1=BG/SP bit2=TEXT */
static uint32_t s_wd613_special_lines = 0;
static int s_gbt614a_reported = 0;

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
        ++s_wd_r57e39_arm_count;
        s_wd_perf_grp_us = s_wd_perf_text_us = s_wd_perf_bg_us = 0;
        s_wd_perf_blend_us = s_wd_perf_clear_us = 0;
        s_wd_r57e39_gbt_us = s_wd_r57e39_commit_us = 0;
        s_wd_r57e39_gbt_calls = s_wd_r57e39_commit_calls = 0;
        s_wd_perf_dirty_lines = 0;
        s_wd_perf_grp_calls = s_wd_perf_text_calls = 0;
        s_wd_perf_bg_calls = s_wd_perf_blend_calls = 0;
        memset(s_wd613_grp_mode_us, 0, sizeof(s_wd613_grp_mode_us));
        memset(s_wd613_grp_mode_calls, 0, sizeof(s_wd613_grp_mode_calls));
        s_wd613_bg_capture_us = s_wd613_bg_draw_us = 0;
        s_wd613_bg_capture_calls = s_wd613_bg_draw_calls = 0;
        s_wd613_blend_grp_us = s_wd613_blend_bg_us = 0;
        s_wd613_blend_text_us = s_wd613_blend_pri_us = 0;
        s_wd613_hostprep_us = s_wd613_hostblend_us = 0;
        s_wd613_blend_grp_calls = s_wd613_blend_bg_calls = 0;
        s_wd613_blend_text_calls = s_wd613_blend_pri_calls = 0;
        s_wd613_hostprep_calls = s_wd613_hostblend_calls = 0;
        memset(s_wd613_layer_mask, 0, sizeof(s_wd613_layer_mask));
        s_wd613_special_lines = 0;
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
        printf("PX68K_RENDER613B: grp16=%luus/%u grp256=%luus/%u grp65k=%luus/%u text=%luus/%u "
               "bgdraw=%luus/%u bgcap=%luus/%u blend:G/B/T/P=%lu/%lu/%lu/%luus calls=%u/%u/%u/%u "
               "hostprep=%luus/%u hostblend=%luus/%u layers[none,g,b,gb,t,gt,bt,gbt]=%u/%u/%u/%u/%u/%u/%u/%u special=%u\n",
               (unsigned long)s_wd613_grp_mode_us[0], (unsigned)s_wd613_grp_mode_calls[0],
               (unsigned long)s_wd613_grp_mode_us[1], (unsigned)s_wd613_grp_mode_calls[1],
               (unsigned long)s_wd613_grp_mode_us[2], (unsigned)s_wd613_grp_mode_calls[2],
               (unsigned long)s_wd_perf_text_us, (unsigned)s_wd_perf_text_calls,
               (unsigned long)s_wd613_bg_draw_us, (unsigned)s_wd613_bg_draw_calls,
               (unsigned long)s_wd613_bg_capture_us, (unsigned)s_wd613_bg_capture_calls,
               (unsigned long)s_wd613_blend_grp_us, (unsigned long)s_wd613_blend_bg_us,
               (unsigned long)s_wd613_blend_text_us, (unsigned long)s_wd613_blend_pri_us,
               (unsigned)s_wd613_blend_grp_calls, (unsigned)s_wd613_blend_bg_calls,
               (unsigned)s_wd613_blend_text_calls, (unsigned)s_wd613_blend_pri_calls,
               (unsigned long)s_wd613_hostprep_us, (unsigned)s_wd613_hostprep_calls,
               (unsigned long)s_wd613_hostblend_us, (unsigned)s_wd613_hostblend_calls,
               (unsigned)s_wd613_layer_mask[0], (unsigned)s_wd613_layer_mask[1],
               (unsigned)s_wd613_layer_mask[2], (unsigned)s_wd613_layer_mask[3],
               (unsigned)s_wd613_layer_mask[4], (unsigned)s_wd613_layer_mask[5],
               (unsigned)s_wd613_layer_mask[6], (unsigned)s_wd613_layer_mask[7],
               (unsigned)s_wd613_special_lines);
    }
}

void WinDraw_PerfGetR57E40(uint32_t *gbt_us, uint32_t *commit_us,
                            uint32_t *gbt_calls, uint32_t *commit_calls,
                            uint32_t *arm_count, uint32_t *active)
{
    if (gbt_us) *gbt_us = (uint32_t)s_wd_r57e39_gbt_us;
    if (commit_us) *commit_us = (uint32_t)s_wd_r57e39_commit_us;
    if (gbt_calls) *gbt_calls = s_wd_r57e39_gbt_calls;
    if (commit_calls) *commit_calls = s_wd_r57e39_commit_calls;
    if (arm_count) *arm_count = s_wd_r57e39_arm_count;
    if (active) *active = WD_PERF_ACTIVE ? 1u : 0u;
}

void WinDraw_PerfGetR57E44(uint32_t *fused_lines, uint32_t *fallback_lines,
                            uint32_t *cache_rebuilds, uint32_t *cache_failures)
{
    if (fused_lines) *fused_lines = s_wd_r57e44_fused_lines;
    if (fallback_lines) *fallback_lines = s_wd_r57e44_fallback_lines;
    Grp_DrawLine16GBT_DebugGet(cache_rebuilds, cache_failures);
}

void WinDraw_PerfGetR57E48(uint32_t *direct_lines, uint32_t *fallback_lines,
                            uint32_t *text_reject, uint32_t *bg_reject,
                            uint32_t *final_reject)
{
    if (direct_lines) *direct_lines = s_wd_r57e48_direct_lines;
    if (fallback_lines) *fallback_lines = s_wd_r57e48_fallback_lines;
    if (text_reject) *text_reject = s_wd_r57e48_text_reject;
    if (bg_reject) *bg_reject = s_wd_r57e48_bg_reject;
    if (final_reject) *final_reject = s_wd_r57e48_final_reject;
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
#define WD_PERF_GRP(...) do { \
    if (WD_PERF_ACTIVE) { \
        int64_t _wd_t0 = esp_timer_get_time(); \
        __VA_ARGS__; \
        uint64_t _wd_dt = (uint64_t)(esp_timer_get_time() - _wd_t0); \
        unsigned _wd_m = (unsigned)(VCReg0[1] & 3); \
        unsigned _wd_i = (_wd_m == 0u) ? 0u : ((_wd_m == 3u) ? 2u : 1u); \
        s_wd_perf_grp_us += _wd_dt; ++s_wd_perf_grp_calls; \
        s_wd613_grp_mode_us[_wd_i] += _wd_dt; ++s_wd613_grp_mode_calls[_wd_i]; \
    } else { __VA_ARGS__; } \
} while (0)
#define WD_PERF_TEXT(...)  WD_PERF_DO(s_wd_perf_text_us,  s_wd_perf_text_calls,  __VA_ARGS__)
#define WD_PERF_BG(...) do { \
    if (WD_PERF_ACTIVE) { \
        int64_t _wd_t0 = esp_timer_get_time(); __VA_ARGS__; \
        uint64_t _wd_dt = (uint64_t)(esp_timer_get_time() - _wd_t0); \
        s_wd_perf_bg_us += _wd_dt; ++s_wd_perf_bg_calls; \
        s_wd613_bg_draw_us += _wd_dt; ++s_wd613_bg_draw_calls; \
    } else { __VA_ARGS__; } \
} while (0)
#define WD_PERF_BLEND_KIND(CACC, CCALLS, ...) do { \
    if (WD_PERF_ACTIVE) { \
        int64_t _wd_t0 = esp_timer_get_time(); __VA_ARGS__; \
        uint64_t _wd_dt = (uint64_t)(esp_timer_get_time() - _wd_t0); \
        s_wd_perf_blend_us += _wd_dt; ++s_wd_perf_blend_calls; \
        (CACC) += _wd_dt; ++(CCALLS); \
    } else { __VA_ARGS__; } \
} while (0)
#define WD_PERF_BLEND_GRP(...)  WD_PERF_BLEND_KIND(s_wd613_blend_grp_us,  s_wd613_blend_grp_calls,  __VA_ARGS__)
#define WD_PERF_BLEND_BG(...)   WD_PERF_BLEND_KIND(s_wd613_blend_bg_us,   s_wd613_blend_bg_calls,   __VA_ARGS__)
#define WD_PERF_BLEND_TEXT(...) WD_PERF_BLEND_KIND(s_wd613_blend_text_us, s_wd613_blend_text_calls, __VA_ARGS__)
#define WD_PERF_BLEND_PRI(...)  WD_PERF_BLEND_KIND(s_wd613_blend_pri_us,  s_wd613_blend_pri_calls,  __VA_ARGS__)
#define WD_PERF_CLEAR(...) WD_PERF_DO(s_wd_perf_clear_us, s_wd_perf_blend_calls, __VA_ARGS__)

uint16_t WinDraw_Pal16B, WinDraw_Pal16R, WinDraw_Pal16G;

void WinDraw_Init(void)
{
	WinDraw_Pal16R = 0xf800;
	WinDraw_Pal16G = 0x07e0;
	WinDraw_Pal16B = 0x001f;

	/* R56: this is renderer-private scratch only.  It is never the displayable
	 * ScreenVersion and is never passed to the LCD backend. */
	RenderBuf = (uint16_t *)tab5_ppa_alloc_framebuffer(800u * 600u * sizeof(uint16_t));
	if (RenderBuf) {
        tab5_pie_graphics_init();
        r57e34_cpu1_gbt_selfcheck();
        if (Grp_DrawLine16GBT_SelfCheck())
            printf("PX68K_GRPGBT_R57E44: exact priority-table self-check PASS; direct 65K decode + GBT fusion armed\n");
        else
            printf("PX68K_GRPGBT_R57E44: self-check FAIL; retained materialized GRP + R57E34 fallback only\n");
        s_wd_r57e48_selfcheck_ok = Grp_DrawLine16TBGI_SelfCheck();
        if (s_wd_r57e48_selfcheck_ok)
            printf("PX68K_FINALFUSE_R57E48: compact TEXT/BG index + raw65K final priority self-check PASS; direct final pipeline armed\n");
        else
            printf("PX68K_FINALFUSE_R57E48: self-check FAIL; NW17 materialized pipeline retained\n");
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
        printf("PX68K_LAYERPROF_R57E48: low-duty profiler retained; direct compact TEXT/BG/final time maps into text/bg/gbt buckets\n");
#endif
        tab5_pie_graphics_fill16(RenderBuf, 0u, 800u * 600u);
        (void)tab5_video_flow_init();
        tab5_video_flow_register_cpu1_fb(RenderBuf, FULLSCREEN_WIDTH, 600u);
        printf("PX68K_SCREEN_R56: WinDraw renderer-private 800x600 scratch ready; no direct screen ownership | R56a windraw compile-fix ACTIVE\n");
	} else {
        /* Alignment is a correctness invariant for the P4 vector/async paths.
         * Never continue with an unaligned libc calloc framebuffer. */
        printf("PX68K_GFX615H17R4: aligned PSRAM framebuffer allocation FAILED; rendering cannot safely continue\n");
	}
}

void WinDraw_Cleanup(void)
{
        tab5_video_flow_register_cpu1_fb(NULL, 0u, 0u);
        if (RenderBuf)
           tab5_ppa_free_framebuffer(RenderBuf);
        RenderBuf = NULL;
}

/* Forward declarations */
extern uint32_t retrow, retroh;
extern int CHANGEAV;

void FASTCALL WinDraw_Draw(void)
{
	static int oldtextx = -1, oldtexty = -1;
#ifdef ESP_PLATFORM
    /* R57e1: a framebuffer allocation failure must never turn into a NULL+offset
     * memcpy destination inside Screen Manager. Keep the guest alive and leave
     * a single diagnostic instead. */
    if (__builtin_expect(RenderBuf==NULL,0)) {
        static uint8_t once;
        if (!once) { once=1; printf("PX68K_R57E1: WinDraw RenderBuf unavailable; render suppressed instead of crashing\n"); }
        return;
    }
#endif

	/* R56: no renderer tail wait here.  Completion belongs to Screen Manager,
	 * which seals only after its own pending-result count reaches zero. */

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

	/* Compatibility getter remains read-only: expose only the Screen Manager
	 * working surface, never WinDraw's mutable renderer scratch. */
	videoBuffer = (uint16_t*)tab5_screen_readonly_work_buffer();
}

#define WD_MEMCPY(src) tab5_pie_graphics_copy(&RenderBuf[adr], (src), (uint32_t)TextDotX * 2u)

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
			RenderBuf##SUFFIX[adr] = w; \
	}

static INLINE void WinDraw_DrawGrpLine(int opaq)
{
#define _DGL_SUB(SUFFIX) WD_SUB(SUFFIX, Grp_LineBuf[i])

	uint32_t adr = VLINE * FULLSCREEN_WIDTH;
	uint16_t w;
	int i;

	if (opaq) {
		WD_MEMCPY(Grp_LineBuf);
	} else if (!tab5_p4blend_key0_overlay(RenderBuf, FULLSCREEN_WIDTH, 600u, VLINE,
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
	} else if (!tab5_p4blend_key0_overlay(RenderBuf, FULLSCREEN_WIDTH, 600u, VLINE,
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
		} else if (!tab5_p4blend_key0_overlay(RenderBuf, FULLSCREEN_WIDTH, 600u, VLINE,
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
		RenderBuf##SUFFIX[adr] = (uint16_t)v; \
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
				RenderBuf##SUFFIX[adr] = (uint16_t)v; \
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
		} else if (!tab5_p4blend_key0_overlay(RenderBuf, FULLSCREEN_WIDTH, 600u, VLINE,
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
		RenderBuf##SUFFIX[adr] = (uint16_t)v; \
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
				RenderBuf##SUFFIX[adr] = (uint16_t)v; \
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

	if (!tab5_p4blend_key0_overlay(RenderBuf, FULLSCREEN_WIDTH, 600u, VLINE,
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

static INLINE int WinDraw_QueueHostCommonTwoLayer(tab5_cpu1_video_line_t *cpu1_line)
{
    uint16_t *dst = &RenderBuf[VLINE * FULLSCREEN_WIDTH];
    const uint16_t *grp = Grp_LineBuf;
    const uint16_t *bg = &BG_LineBuf[16];
    const int grp_pri = VCReg1[0] & 3;
    const int bg_pri = (VCReg1[0] >> 4) & 3;
    const uint16_t *top = (bg_pri <= grp_pri) ? bg : grp;
    const uint16_t *bottom = (bg_pri <= grp_pri) ? grp : bg;

    /* BAT177NW0: immutable source snapshots go straight to the CPU0 compose
     * queue. Screen Manager is not consulted by CPU1 at all. */
    if (TAB5_CPU0_FINAL_STAGE &&
        tab5_compose_submit_line(VLINE, (uint32_t)TextDotX, bottom, top,
                                 dst, cpu1_line->render_seq,
                                 cpu1_line->video_epoch, cpu1_line->visual_seq)) {
        tab5_cpu1_video_offload_submitted(cpu1_line);
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
#ifdef ESP_PLATFORM
    int grp65_deferred = 0;
#endif
    int bgsp_async = 0;
    BG_HOST_LINE_STATE bgsp_state;
    WinDraw_Grp8Geom grp8_geom = {0};
    uint64_t render_ticket = 0u;
    tab5_cpu1_video_line_t cpu1_line = {0};
    int r56s4_65k_exact_bt = 0;
    /* PX68K_R56S5_HOSTBT_EXACT
     * Keep source snapshots for the one-shot live A/B against R56s4 stock
     * BG_LineBuf/Text_TrFlag. Once CPU0 proves exact, visible-TEXT rows return
     * to the early asynchronous path without running stock BG/TEXT on CPU1. */
    const uint8_t *r56s5_text_src = NULL;
    uint32_t r56s5_text_valid = 0u;
    BG_HOST_LINE_STATE r56s5_bg_state;
    int r56s5_have_bg_state = 0;
    int r56s5_text_on = 0, r56s5_bg_on = 0;
    uint16_t r57d_class = 0xffffu;
    int r57d_class_state = 3;
    uint32_t r57d_shadow_seq = 0u;

	if(VLINE==(uint32_t)-1) {
			return;
	}

    /* BAT177NW0: CPU1 guest rendering is Screen-admission blind.
     * Clean means the CPU1 logical framebuffer already contains the latest
     * guest-semantic result; there is no host retry work on this path. */
    {
        const tab5_cpu1_video_line_begin_result_t begin_rc =
            tab5_cpu1_video_line_begin(VLINE, (uint32_t)TextDotX, VCReg0[1],
                                       &RenderBuf[VLINE * FULLSCREEN_WIDTH],
                                       &cpu1_line);
        if (begin_rc == TAB5_CPU1_VIDEO_LINE_CLEAN)
            return;
        if (begin_rc != TAB5_CPU1_VIDEO_LINE_READY) {
            WD_DIAG_INC(s_r56r_path[R56R_LATCH_REJ]);
            return;
        }
    }
    render_ticket = cpu1_line.render_seq; /* quarantined legacy source APIs only */
    WD_DIAG_INC(s_r56r_path[R56R_LAT]);
    switch (cpu1_line.mode) {
        case TAB5_CPU1_VIDEO_MODE_16: WD_DIAG_INC(s_r56r_path[R56R_MODE16]); break;
        case TAB5_CPU1_VIDEO_MODE_256_A:
        case TAB5_CPU1_VIDEO_MODE_256_B: WD_DIAG_INC(s_r56r_path[R56R_MODE256]); break;
        default: WD_DIAG_INC(s_r56r_path[R56R_MODE65]); break;
    }
    if (WD_PERF_ACTIVE) s_wd_perf_dirty_lines++;

#ifdef ESP_PLATFORM
    /* PX68K_R57E_CPU1_FINAL_VIDEO_DETACH
     * Certified normal-65K classes no longer read/expand TextDrawWork on CPU1.
     * CPU1 posts guest TVRAM writes; CPU0 freezes the ordered TVRAM/BG shadow,
     * expands only this visible text line, and performs the proven exact host-BT
     * order. Uncertified/failed classes lazily materialize one CPU1 text row and
     * retain the R57d exact validator/fallback contract. */
    if (TAB5_R57E49_ASYNC65K &&
        Debug_Grp && ((VCReg0[1] & 3u) == 3u) && (VCReg2[1] & 15u) &&
        ((VCReg2[0] & 0x14u) != 0x14u) && TextDotX > 0u && TextDotX <= 800u &&
        VLINE < 600u)
    {
        const int text_on = ((VCReg2[1] & 0x20u) != 0u) && Debug_Text;
        const int bg_on = ((VCReg2[1] & 0x40u) != 0u) &&
                          (BG_Regs[8] & 2u) && !(BG_Regs[0x11] & 2u) && Debug_Sp;
        const uint32_t tx = TextScrollX & 0x3ffu;

        if ((!text_on || (TextPal[0] == 0u && tx + TextDotX <= 1024u)) &&
            (!bg_on || TextPal[0] == 0u))
        {
            /* R57E53: saturation is checked before BG capture/classification.
             * line_begin already gave us the newest render sequence, allowing
             * the preflight to stale older queued jobs while this newest line
             * remains dirty for a later visual retry. */
            if (!tab5_compose_gbt65k_line_admit(VLINE, (uint32_t)TextDotY,
                                                cpu1_line.render_seq)) {
                tab5_cpu1_video_offload_dropped(&cpu1_line);
                return;
            }
            uint32_t gy = GrphScrollY[0] + VLINE;
            uint32_t ty = TextScrollY + VLINE;
            if ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu) { gy += VLINE; ty += VLINE; }
            gy &= 0x1ffu; ty &= 0x3ffu;

            const uint8_t grp_pri = (uint8_t)(VCReg1[0] & 3u);
            const uint8_t text_pri = (uint8_t)((VCReg1[0] >> 2) & 3u);
            const uint8_t bg_pri = (uint8_t)((VCReg1[0] >> 4) & 3u);
            BG_HOST_LINE_STATE st;
            const BG_HOST_LINE_STATE *stp = NULL;
            if (bg_on) {
                int s1 = (((BG_Regs[0x11] & 4u) ? 2 : 1) -
                          ((BG_Regs[0x11] & 16u) ? 1 : 0));
                int s2 = (((CRTC_Regs[0x29] & 4u) ? 2 : 1) -
                          ((CRTC_Regs[0x29] & 16u) ? 1 : 0));
                uint32_t vbg = VLINE;
                vbg <<= s1; vbg >>= s2;
                if (!(BG_Regs[0x11] & 16u))
                    vbg -= ((BG_Regs[0x0f] >> s1) - (CRTC_Regs[0x0d] >> s2));
                VLINEBG = vbg;
                const int gd = (bg_pri < text_pri) ? 0 : 1;
                if (BG_CaptureHostLineState(&st, vbg, gd)) stp = &st;
            }

            r57d_class=(uint16_t)(((stp && stp->gd)?1u:0u) |
                         ((stp && stp->chr_size==16u)?2u:0u) |
                         ((stp && (stp->reg9&1u))?4u:0u) |
                         ((stp && (stp->reg9&8u))?8u:0u) |
                         ((uint16_t)(grp_pri&3u)<<4) |
                         ((uint16_t)(text_pri&3u)<<6) |
                         ((uint16_t)(bg_pri&3u)<<8));
            r57d_class_state=tab5_compose_r57d_class_state(r57d_class);

            /* R57E49: R57 ordered packed-TVRAM shadow is already the production
             * source of truth. Do not spend CPU1 time expanding TEXT merely to
             * certify a renderer class again. */
            const int r57e_shadow_text = text_on && TAB5_R57E49_ASYNC65K;
            const uint8_t *text_src = NULL;
            uint32_t text_valid = 0u;
            int r56s3_text_visible = 0;
            if (text_on && !r57e_shadow_text) {
                text_src=TVRAM_GetExpandedLine(ty,tx,(uint32_t)TextDotX);
                if (text_src) {
                    text_valid=(uint32_t)TextDotX;
                    for (uint32_t x=0;x<text_valid;++x) {
                        if (text_src[x]&0x0fu) { r56s3_text_visible=1; break; }
                    }
                }
            }

            if (r56s3_text_visible && r57d_class_state!=2) {
                r56s4_65k_exact_bt=1;
                r56s5_text_src=text_src; r56s5_text_valid=text_valid;
                r56s5_text_on=text_on; r56s5_bg_on=bg_on;
                if (stp) { r56s5_bg_state=*stp; r56s5_have_bg_state=1; }
            }

            static uint8_t s_r56s5k_seen_order;
            if (r56s3_text_visible) {
                const uint8_t bit=(!bg_on||!stp)?4u:(stp->gd?2u:1u);
                if (!(s_r56s5k_seen_order&bit)) {
                    s_r56s5k_seen_order|=bit;
                    printf("PX68K_R56S5K_ORDER: visible TEXT stock-fenced gd=%d bg=%d pri G/T/B=%u/%u/%u chr=%u reg9=%02X\n",
                           stp?(int)stp->gd:-1,bg_on,(unsigned)grp_pri,(unsigned)text_pri,(unsigned)bg_pri,
                           stp?(unsigned)stp->chr_size:0u,stp?(unsigned)stp->reg9:0u);
                }
            }

            /* R57E49 production correctness: CPU0 reconstructs the stock
             * BG/TEXT ordering from frozen R57 state, then applies the proven
             * exact G-vs-BT selector. Never revive the old independent
             * G/B/T priority compositor that failed visible TEXT semantics. */
            int exact_host_bt=1;
            /* R57E51: do not publish a shadow hold here. The CPU0 submitter
             * first reserves a 16-slot visual packet, then publishes exactly
             * one ordered hold for that accepted job. Queue-full rows carry
             * dirty debt only and create no R57 freeze pressure. */
            r57d_shadow_seq=0u;

            if ((!text_on || text_src || r57e_shadow_text) &&
                (!bg_on || stp)) {
                int accepted=tab5_compose_submit_gbt65k_line(
                    VLINE,(uint32_t)TextDotX,GVRAM,gy,GrphScrollX[0],
                    Pal_Regs,Pal_DebugVisualGeneration(),Pal_DebugEffectiveContrast(),
                    text_src,text_valid,tx,ty,r57e_shadow_text,TextPal,stp,bg_on,text_on,
                    exact_host_bt,grp_pri,bg_pri,text_pri,
                    &RenderBuf[VLINE*FULLSCREEN_WIDTH],r57d_shadow_seq,
                    cpu1_line.render_seq,cpu1_line.video_epoch,cpu1_line.visual_seq);
                if (accepted<=0 && r57d_shadow_seq) {
                    tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq); r57d_shadow_seq=0u;
                }
                if (accepted==1) {
                    tab5_cpu1_video_offload_submitted(&cpu1_line);
                    WD_DIAG_INC(s_r56r_path[R56R_CPU0_65]);
                    if (!s_gbt65k615e_reported) {
                        s_gbt65k615e_reported=1;
                        printf("PX68K_R57E53: CPU0 frozen TVRAM/BG shadow 65K latest-wins PREFLIGHT compositor ACTIVE text=%u bg=%u class=%u\n",
                               (unsigned)text_on,(unsigned)bg_on,(unsigned)r57d_class);
                    }
                    return;
                }
                if (accepted==2) {
                    /* R57E50: host pressure is visual-only. Keep this line
                     * dirty and retry later; never execute NW18 compose here. */
                    tab5_cpu1_video_offload_dropped(&cpu1_line);
                    WD_DIAG_INC(s_r56r_path[R56R_65_QFULL]);
                    WD_DIAG_INC(s_gbt65k615e_dropped_lines);
                    return;
                }
                if (accepted==0) {
                    WD_DIAG_INC(s_r56r_path[R56R_65_QFULL]);
                } else {
                    WD_DIAG_INC(s_r56r_path[R56R_65_REJECT]);
                }
            }
        } else if (!s_gbt65k615e_reject_reported) {
            s_gbt65k615e_reject_reported=1;
            printf("PX68K_GBT65K615E: candidate kept on legacy path VC=%02X/%02X special=%u width=%lu\n",
                   VCReg2[0],VCReg2[1],(unsigned)(((VCReg2[0]&0x14u)==0x14u)?1u:0u),(unsigned long)TextDotX);
        }
    }
#endif

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
                {
                    int gkind = WinDraw_GetGrp8PairParams(grp8_bottom, grp8_top, &grp8_geom);
                    if (gkind && !TAB5_CPU1_AUTHORITATIVE_REFERENCE) {
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
                /* PX68K_R56S4_EXACT_BT_65K_HANDOFF
                 * Visible-TEXT common 65K rows keep GRP unmaterialized on CPU1.
                 * Stock TEXT/BG generation below remains authoritative; CPU0
                 * reconstructs cached 65K GRP and performs the proven 6.14a
                 * final selector. */
                if (!r56s4_65k_exact_bt) {
                    /* BAT177NW14/R57E44: defer normal 65K materialization.
                     * BG/TEXT generation does not consume Grp_LineBuf. If the
                     * common exact GBT path survives, raw GVRAM decode and the
                     * final selector run once in Grp_DrawLine16GBT(). */
#ifdef ESP_PLATFORM
                    if (TAB5_CPU1_INLINE_GBT)
                        grp65_deferred = 1;
                    else
                        WD_PERF_GRP(Grp_DrawLine16());
#else
                    WD_PERF_GRP(Grp_DrawLine16());
#endif
                }
				gon=1;
			}
		}
		break;
	}
	}


#ifdef ESP_PLATFORM
    /* BAT177NW18/R57E48 FINAL:
     * On the proven common normal-65K G+BG+TEXT row, avoid materializing
     * BG_LineBuf/Text_TrFlag entirely.  R57E40 emits compact TEXT indices,
     * R57E43 emits the exact fused BG/sprite candidate indices, and the
     * R57E44 raw-65K decoder resolves G/B/T directly into RenderBuf.
     *
     * The BG compact helper deliberately uses the already-proven GD=1
     * R57E43 eligibility.  Current steady workload has 100% bgFuse coverage;
     * any different priority/mode simply falls through to untouched NW17. */
    if (s_wd_r57e48_selfcheck_ok && TAB5_CPU1_INLINE_GBT &&
        grp65_deferred && gon && !tron && !pron && !grp8_async &&
        (VCReg2[1] & 0x20) && Debug_Text &&
        (VCReg2[1] & 0x40) && (BG_Regs[8] & 2) &&
        !(BG_Regs[0x11] & 2) && Debug_Sp &&
        TextDotX > 0 && TextDotX <= 800)
    {
        const uint8_t grp_pri = (uint8_t)(VCReg1[0] & 3u);
        const uint8_t text_pri = (uint8_t)((VCReg1[0] >> 2) & 3u);
        const uint8_t bg_pri = (uint8_t)((VCReg1[0] >> 4) & 3u);

        /* gd=1 is the historical "TEXT priority >= BG" branch, including
         * equal priority where TEXT wins.  The current workload is entirely
         * in this proven branch; preserve NW17 for every other case. */
        if (bg_pri >= text_pri) {
            int text_ok, bg_ok = 0, final_ok = 0;

            if (WD_PERF_ACTIVE) {
                int64_t _t = esp_timer_get_time();
                text_ok = TVRAM_Tab5DecodeVisibleIndexLine(
                    s_wd_r57e48_text_idx, (uint32_t)TextDotX);
                s_wd_perf_text_us += (uint64_t)(esp_timer_get_time() - _t);
                ++s_wd_perf_text_calls;
            } else {
                text_ok = TVRAM_Tab5DecodeVisibleIndexLine(
                    s_wd_r57e48_text_idx, (uint32_t)TextDotX);
            }

            if (text_ok) {
                int s1, s2;
                s1 = (((BG_Regs[0x11] & 4) ? 2 : 1) -
                      ((BG_Regs[0x11] & 16) ? 1 : 0));
                s2 = (((CRTC_Regs[0x29] & 4) ? 2 : 1) -
                      ((CRTC_Regs[0x29] & 16) ? 1 : 0));
                VLINEBG = VLINE;
                VLINEBG <<= s1;
                VLINEBG >>= s2;
                if (!(BG_Regs[0x11] & 16))
                    VLINEBG -= ((BG_Regs[0x0f] >> s1) -
                                (CRTC_Regs[0x0d] >> s2));

                if (WD_PERF_ACTIVE) {
                    int64_t _t = esp_timer_get_time();
                    bg_ok = BG_Tab5DecodeIndexLine(
                        s_wd_r57e48_bg_idx,
                        (uint32_t)sizeof(s_wd_r57e48_bg_idx), 1);
                    {
                        const uint64_t _dt =
                            (uint64_t)(esp_timer_get_time() - _t);
                        s_wd_perf_bg_us += _dt;
                        s_wd613_bg_draw_us += _dt;
                    }
                    ++s_wd_perf_bg_calls;
                    ++s_wd613_bg_draw_calls;
                } else {
                    bg_ok = BG_Tab5DecodeIndexLine(
                        s_wd_r57e48_bg_idx,
                        (uint32_t)sizeof(s_wd_r57e48_bg_idx), 1);
                }
            }

            if (text_ok && bg_ok) {
                if (WD_PERF_ACTIVE) {
                    int64_t _t = esp_timer_get_time();
                    final_ok = Grp_DrawLine16TBGI(
                        &RenderBuf[VLINE * FULLSCREEN_WIDTH],
                        s_wd_r57e48_text_idx, &s_wd_r57e48_bg_idx[16],
                        (uint32_t)TextDotX, grp_pri, bg_pri, text_pri);
                    s_wd_r57e39_gbt_us +=
                        (uint64_t)(esp_timer_get_time() - _t);
                    ++s_wd_r57e39_gbt_calls;
                } else {
                    final_ok = Grp_DrawLine16TBGI(
                        &RenderBuf[VLINE * FULLSCREEN_WIDTH],
                        s_wd_r57e48_text_idx, &s_wd_r57e48_bg_idx[16],
                        (uint32_t)TextDotX, grp_pri, bg_pri, text_pri);
                }
            }

            if (final_ok) {
                WD_DIAG_INC(s_wd_r57e48_direct_lines);
                WD_DIAG_INC(s_wd_r57e44_fused_lines);
                grp65_deferred = 0;
                WD_DIAG_INC(s_r56r_path[R56R_CPU1_GBT]);
                if (WD_PERF_ACTIVE) {
                    ++s_wd613_layer_mask[7];
                    int64_t _t = esp_timer_get_time();
                    (void)tab5_cpu1_video_line_commit(
                        &cpu1_line, &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
                    s_wd_r57e39_commit_us +=
                        (uint64_t)(esp_timer_get_time() - _t);
                    ++s_wd_r57e39_commit_calls;
                } else {
                    (void)tab5_cpu1_video_line_commit(
                        &cpu1_line, &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
                }
                return;
            }

            WD_DIAG_INC(s_wd_r57e48_fallback_lines);
            if (!text_ok) WD_DIAG_INC(s_wd_r57e48_text_reject);
            else if (!bg_ok) WD_DIAG_INC(s_wd_r57e48_bg_reject);
            else WD_DIAG_INC(s_wd_r57e48_final_reject);
        } else {
            WD_DIAG_INC(s_wd_r57e48_fallback_lines);
        }
    }
#endif /* ESP_PLATFORM */

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
            if (0 && grp8_async && !ton) {
                if (WD_PERF_ACTIVE) {
                    int64_t _t = esp_timer_get_time();
                    bgsp_async = BG_CaptureHostLineState(&bgsp_state, VLINEBG, 0);
                    {
                        uint64_t _dt = (uint64_t)(esp_timer_get_time() - _t);
                        s_wd_perf_bg_us += _dt;
                        s_wd613_bg_capture_us += _dt;
                    }
                    s_wd_perf_bg_calls++;
                    s_wd613_bg_capture_calls++;
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
            if (0 && grp8_async && !((VCReg2[1]&0x20)&&(Debug_Text))) {
                if (WD_PERF_ACTIVE) {
                    int64_t _t = esp_timer_get_time();
                    bgsp_async = BG_CaptureHostLineState(&bgsp_state, VLINEBG, 1);
                    {
                        uint64_t _dt = (uint64_t)(esp_timer_get_time() - _t);
                        s_wd_perf_bg_us += _dt;
                        s_wd613_bg_capture_us += _dt;
                    }
                    s_wd_perf_bg_calls++;
                    s_wd613_bg_capture_calls++;
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
    if (WD_PERF_ACTIVE) {
        unsigned _mask = (gon ? 1u : 0u) | (bgon ? 2u : 0u) | (ton ? 4u : 0u);
        ++s_wd613_layer_mask[_mask & 7u];
        if (tron || pron) ++s_wd613_special_lines;
    }

#ifdef ESP_PLATFORM
    /* BAT177NW4/R57E34: keep final ownership on CPU1, but do the common
     * normal G+BG+TEXT selector in one exact pass.  No Screen query, no queue,
     * no immutable packet and no second full-frame surface are involved. */
    if (TAB5_CPU1_INLINE_GBT && gon && bgon && ton && !tron && !pron && !grp8_async)
    {
        const uint8_t grp_pri = (uint8_t)(VCReg1[0] & 3u);
        const uint8_t text_pri = (uint8_t)((VCReg1[0] >> 2) & 3u);
        const uint8_t bg_pri = (uint8_t)((VCReg1[0] >> 4) & 3u);
        int pie = 0;
        int fused65 = 0;
        if (grp65_deferred) {
            if (WD_PERF_ACTIVE) {
                int64_t _t = esp_timer_get_time();
                fused65 = Grp_DrawLine16GBT(
                    &RenderBuf[VLINE * FULLSCREEN_WIDTH],
                    &BG_LineBuf[16], &Text_TrFlag[16], (uint32_t)TextDotX,
                    grp_pri, bg_pri, text_pri);
                s_wd_r57e39_gbt_us += (uint64_t)(esp_timer_get_time() - _t);
                ++s_wd_r57e39_gbt_calls;
            } else {
                fused65 = Grp_DrawLine16GBT(
                    &RenderBuf[VLINE * FULLSCREEN_WIDTH],
                    &BG_LineBuf[16], &Text_TrFlag[16], (uint32_t)TextDotX,
                    grp_pri, bg_pri, text_pri);
            }
        }
        if (!fused65) {
            if (grp65_deferred) {
                WD_PERF_GRP(Grp_DrawLine16());
                grp65_deferred = 0;
                WD_DIAG_INC(s_wd_r57e44_fallback_lines);
            }
            if (WD_PERF_ACTIVE) {
                int64_t _t = esp_timer_get_time();
                pie = r57e34_cpu1_gbt(
                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], Grp_LineBuf,
                    &BG_LineBuf[16], &Text_TrFlag[16], (uint32_t)TextDotX,
                    grp_pri, bg_pri, text_pri);
                s_wd_r57e39_gbt_us += (uint64_t)(esp_timer_get_time() - _t);
                ++s_wd_r57e39_gbt_calls;
            } else {
                pie = r57e34_cpu1_gbt(
                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], Grp_LineBuf,
                    &BG_LineBuf[16], &Text_TrFlag[16], (uint32_t)TextDotX,
                    grp_pri, bg_pri, text_pri);
            }
        }
        if (fused65) {
            WD_DIAG_INC(s_wd_r57e44_fused_lines);
            grp65_deferred = 0;
        }
        WD_DIAG_INC(s_r56r_path[R56R_CPU1_GBT]);
        if (pie) WD_DIAG_INC(s_r56r_path[R56R_CPU1_GBT_PIE]);
        if (WD_PERF_ACTIVE) {
            int64_t _t = esp_timer_get_time();
            (void)tab5_cpu1_video_line_commit(&cpu1_line,
                                               &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
            s_wd_r57e39_commit_us += (uint64_t)(esp_timer_get_time() - _t);
            ++s_wd_r57e39_commit_calls;
        } else {
            (void)tab5_cpu1_video_line_commit(&cpu1_line,
                                               &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
        }
        return;
    }

    /* Any normal 65K row that did not qualify for the common GBT return must
     * materialize the historical Grp_LineBuf before later legacy/special
     * paths can observe it. */
    if (grp65_deferred) {
        WD_PERF_GRP(Grp_DrawLine16());
        grp65_deferred = 0;
        WD_DIAG_INC(s_wd_r57e44_fallback_lines);
    }

    /* BAT176A1: safe CPU0 restore #1.  GRP/BG/TEXT are already fully
     * materialized on CPU1 at the correct guest raster.  The CPU0 job is only
     * the pure final priority/key-zero selector, and Screen admission is late. */
    if (TAB5_CPU0_FINAL_STAGE && gon && bgon && ton && !tron && !pron && !grp8_async)
    {
        const uint8_t grp_pri = (uint8_t)(VCReg1[0] & 3u);
        const uint8_t text_pri = (uint8_t)((VCReg1[0] >> 2) & 3u);
        const uint8_t bg_pri = (uint8_t)((VCReg1[0] >> 4) & 3u);
        if (tab5_compose_submit_gbt_line(
                VLINE, (uint32_t)TextDotX, Grp_LineBuf,
                &BG_LineBuf[16], &Text_TrFlag[16],
                grp_pri, bg_pri, text_pri,
                &RenderBuf[VLINE * FULLSCREEN_WIDTH], cpu1_line.render_seq,
                cpu1_line.video_epoch, cpu1_line.visual_seq)) {
            WD_DIAG_INC(s_r56r_path[R56R_CPU0_GBT]);
            tab5_cpu1_video_offload_submitted(&cpu1_line);
            return;
        }
    }
#endif

    /* R56s4: for visible-TEXT normal 65K, stock WinDraw has now generated
     * authoritative BG_LineBuf/Text_TrFlag.  Hand those exact semantics to
     * CPU0 together with an immutable 65K GVRAM row snapshot. */
    if (!TAB5_CPU1_AUTHORITATIVE_REFERENCE &&
        r56s4_65k_exact_bt && gon && bgon && ton && !tron && !pron)
    {
        uint32_t gy = GrphScrollY[0] + VLINE;
        uint32_t ty = TextScrollY + VLINE;
        if ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu) { gy += VLINE; ty += VLINE; }
        gy &= 0x1ffu; ty &= 0x3ffu;
        const uint8_t grp_pri = (uint8_t)(VCReg1[0] & 3u);
        const uint8_t text_pri = (uint8_t)((VCReg1[0] >> 2) & 3u);
        const uint8_t bg_pri = (uint8_t)((VCReg1[0] >> 4) & 3u);
        const BG_HOST_LINE_STATE *r56s5_stp =
            (r56s5_bg_on && r56s5_have_bg_state) ? &r56s5_bg_state : NULL;
        if (r57d_class_state==0 && (r56s5_bg_on || r56s5_text_on) && !r57d_shadow_seq)
            r57d_shadow_seq=tab5_guest_bus_post_raster_hold(VLINE);
        int accepted = tab5_compose_submit_gbt65k_exact_bt_line(
            VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],
            Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),
            &BG_LineBuf[16], &Text_TrFlag[16],
            r56s5_text_src, r56s5_text_valid, TextScrollX & 0x3ffu, ty, TextPal, r56s5_stp,
            r56s5_bg_on, r56s5_text_on, grp_pri, bg_pri, text_pri,
            &RenderBuf[VLINE * FULLSCREEN_WIDTH], r57d_class, r57d_shadow_seq, render_ticket);
        if (accepted <= 0 && r57d_shadow_seq) { tab5_guest_bus_shadow_hold_cancel(r57d_shadow_seq); r57d_shadow_seq=0u; }
        if (accepted > 0) {
            WD_DIAG_INC(s_r56r_path[R56R_CPU0_65]);
            return;
        }
        if (accepted == 0) {
            WD_DIAG_INC(s_r56r_path[R56R_65_QFULL]);
            tab5_cpu1_video_line_cancel(&cpu1_line);
            return;
        }
        /* Host exact path unavailable: materialize the GRP line now and enter
         * the untouched stock final compositor. Correctness always wins. */
        WD_DIAG_INC(s_r56r_path[R56R_65_REJECT]);
        WD_PERF_GRP(Grp_DrawLine16());
    }

    /* Build 6.14b2 Render Phase B2.
     *
     * 6.14a proved the G+BG+TEXT priority/key-zero path on real SFXVI and
     * restored the missing background.  For the shared-scroll 256-colour
     * pair, stop materializing Grp_LineBuf on CPU1: snapshot the two packed
     * GVRAM lanes and let CPU0 reconstruct GRP + perform the validated G/B/T
     * selection in one pass.  Unequal-scroll keeps the exact 6.14a path. */
    if (!TAB5_CPU1_AUTHORITATIVE_REFERENCE &&
        gon && bgon && ton && !tron && !pron && grp8_async)
    {
        int accepted = 0;
        uint16_t *dst = &RenderBuf[VLINE * FULLSCREEN_WIDTH];
        const uint8_t grp_pri = (uint8_t)(VCReg1[0] & 3);
        const uint8_t text_pri = (uint8_t)((VCReg1[0] >> 2) & 3);
        const uint8_t bg_pri = (uint8_t)((VCReg1[0] >> 4) & 3);

        if (!grp8_split) {
            /* Build 6.14c: scroll is an address change, not new image data.
             * Try the persistent decoded GRP8 row cache first. On allocation/
             * queue failure retain the exact 6.14b2a raw-snapshot path. */
            accepted = tab5_compose_submit_gbt_scrollcache_line(
                VLINE, (uint32_t)TextDotX, GVRAM,
                grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                grp8_geom.bx_lo, grp8_geom.bx_hi,
                grp8_bottom, grp8_top, GrphPal,
                &BG_LineBuf[16], &Text_TrFlag[16],
                grp_pri, bg_pri, text_pri, dst, 0u);
            if (accepted) {
                WD_DIAG_INC(s_r56r_path[R56R_CPU0_GBT]);
                if (!s_gbt614a_reported) {
                    s_gbt614a_reported = 1;
                    printf("PX68K_SCROLL614C: CPU0 persistent GRP8 scroll-cache + BG+TEXT compositor ACTIVE pri G/T/B=%u/%u/%u\n",
                           (unsigned)grp_pri, (unsigned)text_pri, (unsigned)bg_pri);
                }
                return;
            }
            accepted = tab5_compose_submit_gbt_rawpair_line(
                VLINE, (uint32_t)TextDotX, GVRAM,
                grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                grp8_geom.bx_lo, grp8_geom.bx_hi,
                grp8_bottom, grp8_top, GrphPal,
                &BG_LineBuf[16], &Text_TrFlag[16],
                grp_pri, bg_pri, text_pri, dst, 0u);
            if (accepted) {
                WD_DIAG_INC(s_r56r_path[R56R_CPU0_GBT]);
                return;
            }
            WD_PERF_GRP(WinDraw_DrawGrp8PairChecked(grp8_bottom, grp8_top));
            grp8_async = 0;
        } else {
            /* Phase B intentionally does not enlarge the packet for four raw
             * split lanes. Preserve 6.14a exactly for unequal-scroll lines. */
            WD_PERF_GRP(WinDraw_DrawGrp8PairChecked(grp8_bottom, grp8_top));
            grp8_async = 0;
            accepted = tab5_compose_submit_gbt_line(
                VLINE, (uint32_t)TextDotX,
                Grp_LineBuf, &BG_LineBuf[16], &Text_TrFlag[16],
                grp_pri, bg_pri, text_pri, dst, cpu1_line.render_seq,
                cpu1_line.video_epoch, cpu1_line.visual_seq);
            if (accepted) {
                WD_DIAG_INC(s_r56r_path[R56R_CPU0_GBT]);
                if (!s_gbt614a_reported) {
                    s_gbt614a_reported = 1;
                    printf("PX68K_GBT614B2: CPU0 G+BG+TEXT compositor ACTIVE; unequal-scroll uses 6.14a materialized-GRP fallback\n");
                }
                return;
            }
        }
    }

	if (TAB5_CPU0_FINAL_STAGE &&
        gon && bgon && !ton && !tron && !pron)
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
            uint16_t *dst = &RenderBuf[VLINE * FULLSCREEN_WIDTH];
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
                            bg_on_top, dst, split_ref, render_ticket)
                        : tab5_compose_submit_grp8split_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.ty_lo_base, grp8_geom.ty_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_geom.tx_lo, grp8_geom.tx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst, split_ref, render_ticket);
                } else {
                    accepted = bgsp_async
                        ? tab5_compose_submit_grp8pair_bgsp_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, TextPal, &bgsp_state,
                            bg_on_top, dst, render_ticket)
                        : tab5_compose_submit_grp8pair_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst, render_ticket);
                }
                {
                    uint64_t _dt = (uint64_t)(esp_timer_get_time() - _t);
                    s_wd_perf_grp_us += _dt;
                    s_wd613_hostprep_us += _dt;
                }
                s_wd_perf_grp_calls++;
                s_wd613_hostprep_calls++;
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
                            bg_on_top, dst, split_ref, render_ticket)
                        : tab5_compose_submit_grp8split_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.ty_lo_base, grp8_geom.ty_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_geom.tx_lo, grp8_geom.tx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst, split_ref, render_ticket);
                } else {
                    accepted = bgsp_async
                        ? tab5_compose_submit_grp8pair_bgsp_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, TextPal, &bgsp_state,
                            bg_on_top, dst, render_ticket)
                        : tab5_compose_submit_grp8pair_line(
                            VLINE, (uint32_t)TextDotX, GVRAM,
                            grp8_geom.by_lo_base, grp8_geom.by_hi_base,
                            grp8_geom.bx_lo, grp8_geom.bx_hi,
                            grp8_bottom, grp8_top, GrphPal, &BG_LineBuf[16],
                            bg_on_top, dst, render_ticket);
                }
            }
            if (accepted) {
                WD_DIAG_INC(s_r56r_path[R56R_CPU0_GRP8]);
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
			compose_result = WinDraw_QueueHostCommonTwoLayer(&cpu1_line);
			{
				uint64_t _dt = (uint64_t)(esp_timer_get_time() - _t);
				s_wd_perf_blend_us += _dt;
				s_wd613_hostblend_us += _dt;
			}
			s_wd_perf_blend_calls++;
			s_wd613_hostblend_calls++;
		} else {
			compose_result = WinDraw_QueueHostCommonTwoLayer(&cpu1_line);
		}
        if (compose_result != 2) {
            WD_DIAG_INC(s_r56r_path[R56R_CPU1_2L]);
            (void)tab5_cpu1_video_line_commit(&cpu1_line,
                                                &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
        } else {
            WD_DIAG_INC(s_r56r_path[R56R_CPU0_2L]);
        }
		return;
	}

    /* BAT167M1: move the legacy/special *final* compositor to CPU0.
     * CPU1's guest-semantic GRP/TEXT/BG layer generation above is untouched.
     * Every mutable scratch input is snapshotted into the existing compose
     * slot before CPU1 advances; queue pressure falls back to the exact old
     * CPU1 compositor below. */
    if (TAB5_CPU0_FINAL_STAGE) {
        if (tab5_compose_submit_legacy_final(
                VLINE, (uint32_t)TextDotX,
                Grp_LineBuf, Grp_LineBufSP, Grp_LineBufSP2,
                &BG_LineBuf[16], &Text_TrFlag[16],
                VCReg1[0], VCReg2[0], gon, bgon, ton, tron, pron,
                Pal_HalfMask, Pal_Ix2, Ibit,
                &RenderBuf[VLINE * FULLSCREEN_WIDTH], cpu1_line.render_seq,
                cpu1_line.video_epoch, cpu1_line.visual_seq)) {
            WD_DIAG_INC(s_r56r_path[R56R_CPU0_LEGACY]);
            tab5_cpu1_video_offload_submitted(&cpu1_line);
            return;
        }
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
			WD_PERF_BLEND_GRP(WinDraw_DrawGrpLine(opaq));
			opaq = 0;
		}
		if (tron)
		{
			WD_PERF_BLEND_GRP(WinDraw_DrawGrpLineNonSP(opaq));
			opaq = 0;
		}
	}
	if ( (VCReg1[0]&0x20)&&(bgon) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&((VCReg1[0]&0x03)!=0x02)&&(tron) )
		{
			if ( (VCReg1[0]&3)<((VCReg1[0]>>2)&3) )
			{
				WD_PERF_BLEND_BG(WinDraw_DrawBGLineTR(opaq));
				tdrawed = 1;
				opaq = 0;
			}
		}
		else
		{
			WD_PERF_BLEND_BG(WinDraw_DrawBGLine(opaq, /*0*/tdrawed));
			tdrawed = 1;
			opaq = 0;
		}
	}
	if ( (VCReg1[0]&0x08)&&(ton) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&((VCReg1[0]&0x03)!=0x02)&&(tron) )
			WD_PERF_BLEND_TEXT(WinDraw_DrawTextLineTR(opaq));
		else
			WD_PERF_BLEND_TEXT(WinDraw_DrawTextLine(opaq, tdrawed/*((VCReg1[0]&0x30)>=0x20)*/));
		opaq = 0;
		tdrawed = 1;
	}

	/* Pri = 1��2���ܡˤ����ꤵ��Ƥ�����̤�ɽ�� */
	if ( ((VCReg1[0]&0x03)==0x01)&&(gon) )
	{
		WD_PERF_BLEND_GRP(WinDraw_DrawGrpLine(opaq));
		opaq = 0;
	}
	if ( ((VCReg1[0]&0x30)==0x10)&&(bgon) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&(!(VCReg1[0]&0x03))&&(tron) )
		{
			if ( (VCReg1[0]&3)<((VCReg1[0]>>2)&3) )
			{
				WD_PERF_BLEND_BG(WinDraw_DrawBGLineTR(opaq));
				tdrawed = 1;
				opaq = 0;
			}
		}
		else
		{
			WD_PERF_BLEND_BG(WinDraw_DrawBGLine(opaq, ((VCReg1[0]&0xc)==0x8)));
			tdrawed = 1;
			opaq = 0;
		}
	}
	if ( ((VCReg1[0]&0x0c)==0x04) && ((VCReg2[0]&0x5d)==0x1d) && (VCReg1[0]&0x03) && (((VCReg1[0]>>4)&3)>(VCReg1[0]&3)) && (bgon) && (tron) )
	{
		WD_PERF_BLEND_BG(WinDraw_DrawBGLineTR(opaq));
		tdrawed = 1;
		opaq = 0;
		if (tron)
		{
			WD_PERF_BLEND_GRP(WinDraw_DrawGrpLineNonSP(opaq));
		}
	}
	else if ( ((VCReg1[0]&0x03)==0x01)&&(tron)&&(gon)&&(VCReg2[0]&0x10) )
	{
		WD_PERF_BLEND_GRP(WinDraw_DrawGrpLineNonSP(opaq));
		opaq = 0;
	}
	if ( ((VCReg1[0]&0x0c)==0x04)&&(ton) )
	{
		if ( ((VCReg2[0]&0x5d)==0x1d)&&(!(VCReg1[0]&0x03))&&(tron) )
			WD_PERF_BLEND_TEXT(WinDraw_DrawTextLineTR(opaq));
		else
			/* FIXME: Verify corrent td param here. Games like Overdriver
			 * expect a td value of 1 here, yet the condition
			 * ((VCReg1[0]&0x30)>=0x10) returns 0 for this game causing
			 * grp > text. See stage 2 of the game. */
			WD_PERF_BLEND_TEXT(WinDraw_DrawTextLine(opaq, ((VCReg1[0]&0x30)>=0x10)));
		opaq = 0;
		tdrawed = 1;
	}

	/* Pri = 0�ʺ�ͥ��ˤ����ꤵ��Ƥ�����̤�ɽ�� */
	if ( (!(VCReg1[0]&0x03))&&(gon) )
	{
		WD_PERF_BLEND_GRP(WinDraw_DrawGrpLine(opaq));
		opaq = 0;
	}
	if ( (!(VCReg1[0]&0x30))&&(bgon) )
	{
		WD_PERF_BLEND_BG(WinDraw_DrawBGLine(opaq, /*tdrawed*/((VCReg1[0]&0xc)>=0x4)));
		tdrawed = 1;
		opaq = 0;
	}
	if ( (!(VCReg1[0]&0x0c)) && ((VCReg2[0]&0x5d)==0x1d) && (((VCReg1[0]>>4)&3)>(VCReg1[0]&3)) && (bgon) && (tron) )
	{
		WD_PERF_BLEND_BG(WinDraw_DrawBGLineTR(opaq));
		tdrawed = 1;
		opaq = 0;
		if (tron)
		{
			WD_PERF_BLEND_GRP(WinDraw_DrawGrpLineNonSP(opaq));
		}
	}
	else if ( (!(VCReg1[0]&0x03))&&(tron)&&(VCReg2[0]&0x10) )
	{
		WD_PERF_BLEND_GRP(WinDraw_DrawGrpLineNonSP(opaq));
		opaq = 0;
	}
	if ( (!(VCReg1[0]&0x0c))&&(ton) )
	{
		WD_PERF_BLEND_TEXT(WinDraw_DrawTextLine(opaq, 1));
		tdrawed = 1;
		opaq = 0;
	}

	/* �ü�ץ饤����ƥ����Υ���ե��å� */
	if ( ((VCReg2[0]&0x5c)==0x14)&&(pron) )	/* �ü�Pri���ϡ��оݥץ졼��ӥåȤϰ�̣��̵���餷���ʤĤ���ӡ��� */
	{
		WD_PERF_BLEND_PRI(WinDraw_DrawPriLine());
	}
	else if ( ((VCReg2[0]&0x5d)==0x1c)&&(tron) )	/* ȾƩ���������Ƥ�Ʃ���ʥɥåȤ�ϡ��ե��顼������ */
	{						/* ��AQUALES�� */
#define _DL_SUB(SUFFIX)                                    \
	{                                                      \
		w = Grp_LineBufSP[i];                              \
		if (w != 0 && (RenderBuf##SUFFIX[adr] & 0xffff) == 0) \
			RenderBuf##SUFFIX[adr] = (w & Pal_HalfMask) >> 1; \
	}

		uint32_t adr = VLINE*FULLSCREEN_WIDTH;
		uint16_t w;
		int i;

		WD_LOOP(0, TextDotX, _DL_SUB);
	}

	if (opaq)
	{
		uint32_t adr = VLINE*FULLSCREEN_WIDTH;
		WD_PERF_CLEAR(tab5_pie_graphics_fill16(&RenderBuf[adr], 0u, (uint32_t)TextDotX));
	}

    WD_DIAG_INC(s_r56r_path[R56R_CPU1_LEGACY]);
    switch (VCReg0[1] & 3u) {
        case 0u: WD_DIAG_INC(s_r56r_path[R56R_LEG16]); break;
        case 1u:
        case 2u: WD_DIAG_INC(s_r56r_path[R56R_LEG256]); break;
        default: WD_DIAG_INC(s_r56r_path[R56R_LEG65]); break;
    }
    (void)tab5_cpu1_video_line_commit(&cpu1_line,
                                        &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
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

#define isHankaku(s) (((s) >= 0x20 && (s) <= 0x7e) || ((s) >= 0xa0 && (s) <= 0xdf))
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

	if (f == (uint32_t)-1)
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
