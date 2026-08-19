/*
 * Tab5 port-specific implementation.
 * Intent: Mailbox and snapshot contract between the CPU1 guest renderer and the CPU0 Tab5 compositor.
 * Layer8 Aug/17/2026
 */
#ifndef TAB5_COMPOSE_H
#define TAB5_COMPOSE_H

#include <stdint.h>
#include "x68k/bg_host_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t submitted_lines;
    uint32_t completed_lines;
    uint32_t fallback_lines;
    uint32_t queue_full;
    uint32_t frame_waits;
    uint32_t max_pending;
    uint32_t last_copy_us;
    uint32_t last_blend_us;
    uint32_t grp8_submitted_lines;
    uint32_t grp8_completed_lines;
    uint32_t last_grp8_copy_us;
    uint32_t last_grp8_render_us;
    /* Build 5.49: lock-free TCM/SPM mailbox + dynamic L2 SRAM packet pool. */
    uint32_t slot_count;
    uint32_t slot_bytes;
    uint32_t pool_bytes;
    uint32_t mailbox_bytes;
    uint32_t mailbox_tcm;
    uint32_t notify_count;
    uint32_t ready_empty;
    uint32_t free_empty;
    /* Build 5.53a: early-reserved Internal SRAM arena + host BG/Sprite source path. */
    uint32_t arena_bytes;
    uint32_t arena_used;
    uint32_t arena_spare;
    uint32_t bgsp_submitted_lines;
    uint32_t bgsp_completed_lines;
    uint32_t last_bgsp_render_us;
    /* Build 6.14a: common 256-colour GRP + already-rasterized BG/TEXT
     * final compositor moved to CPU0.  Guest-side layer generation remains
     * authoritative; these counters only describe the final line handoff. */
    uint32_t gbt_submitted_lines;
    uint32_t gbt_completed_lines;
    uint32_t last_gbt_copy_us;
    uint32_t last_gbt_render_us;
    /* Build 6.14b: shared-scroll raw GRP8 pair + BG/TEXT final compositor. */
    uint32_t gbt_raw_submitted_lines;
    uint32_t gbt_raw_completed_lines;
    uint32_t last_gbt_raw_copy_us;
    uint32_t last_gbt_raw_render_us;
    /* Build 6.14b2: async memcpy/GDMA raw-GVRAM snapshot stats. */
    uint32_t gbt_raw_dma_lines;
    uint32_t gbt_raw_dma_reqs;
    uint32_t gbt_raw_dma_submit_fail;
    uint32_t last_gbt_raw_dma_us;
    /* Build 6.14c: decoded GRP8 scroll-cache statistics. */
    uint32_t scroll_cache_submitted_lines;
    uint32_t scroll_cache_completed_lines;
    uint32_t scroll_cache_hits;
    uint32_t scroll_cache_misses;
    uint32_t scroll_cache_rebuilds;
    uint32_t last_scroll_cache_build_us;
    uint32_t last_scroll_cache_render_us;
    uint32_t gvram_barrier_calls;
    uint32_t gvram_barrier_waits;
    uint32_t gvram_barrier_us;
    uint32_t bg_barrier_calls;
    uint32_t bg_barrier_waits;
    uint32_t bg_barrier_us;
    /* Build 5.98g3: CPU0 direct three-source PIE key-zero compositor stats. */
    uint32_t p4_blend_scalar_calls;
    uint32_t p4_blend_pie_calls;
    uint32_t p4_blend_scalar_pixels;
    uint32_t p4_blend_pie_pixels;
    uint32_t p4_blend_align_fallbacks;
    uint32_t p4_blend_failures;
    uint32_t p4_blend_backend0;
    uint32_t p4_blend_backend1;
    uint32_t p4_blend_backend2;
} tab5_compose_stats_t;

int tab5_compose_reserve_arena(void);
int tab5_compose_init(void);
int tab5_compose_submit_line(uint32_t y, uint32_t width,
                             const uint16_t *bottom, const uint16_t *top,
                             uint16_t *dst);

int tab5_compose_submit_grp8pair_line(uint32_t y, uint32_t width,
                                      const uint8_t *gvram,
                                      uint32_t y_lo_base, uint32_t y_hi_base,
                                      uint32_t x_lo, uint32_t x_hi,
                                      int bottom_page, int top_page,
                                      const uint16_t *palette,
                                      const uint16_t *bg, int bg_on_top,
                                      uint16_t *dst);
int tab5_compose_submit_grp8pair_bgsp_line(uint32_t y, uint32_t width,
                                           const uint8_t *gvram,
                                           uint32_t y_lo_base, uint32_t y_hi_base,
                                           uint32_t x_lo, uint32_t x_hi,
                                           int bottom_page, int top_page,
                                           const uint16_t *grph_palette,
                                           const uint16_t *text_palette,
                                           const BG_HOST_LINE_STATE *bg_state,
                                           int bg_on_top, uint16_t *dst);
int tab5_compose_submit_grp8split_line(uint32_t y, uint32_t width,
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
int tab5_compose_submit_grp8split_bgsp_line(uint32_t y, uint32_t width,
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

/* Build 6.14a: low-risk render offload for the common three-layer case used
 * heavily by SFXVI.  CPU1 still generates the exact PX68K GRP line and the
 * shared BG/TEXT raster/flags.  CPU0 only performs the final priority/key-zero
 * selection in one pass, so no guest-visible rendering semantics move yet. */
int tab5_compose_submit_gbt_line(uint32_t y, uint32_t width,
                                 const uint16_t *grp,
                                 const uint16_t *bg_text,
                                 const uint8_t *text_tr_flags,
                                 uint8_t grp_pri, uint8_t bg_pri,
                                 uint8_t text_pri, uint16_t *dst);

/* Build 6.14b: shared-scroll common 256-colour path. CPU1 snapshots the two
 * packed GVRAM page-pair lanes plus palette/BG/TEXT state; CPU0 reconstructs
 * the exact GRP8 pair and performs the 6.14a priority/key-zero blend directly.
 * Unequal-scroll/special cases remain on the 6.14a path. */
/* Build 6.14c: persistent decoded GRP8 scroll cache. */
int tab5_compose_submit_gbt_scrollcache_line(uint32_t y, uint32_t width,
                                              const uint8_t *gvram,
                                              uint32_t y_lo_base, uint32_t y_hi_base,
                                              uint32_t x_lo, uint32_t x_hi,
                                              int bottom_page, int top_page,
                                              const uint16_t *palette,
                                              const uint16_t *bg_text,
                                              const uint8_t *text_tr_flags,
                                              uint8_t grp_pri, uint8_t bg_pri,
                                              uint8_t text_pri, uint16_t *dst);

int tab5_compose_submit_gbt_rawpair_line(uint32_t y, uint32_t width,
                                         const uint8_t *gvram,
                                         uint32_t y_lo_base, uint32_t y_hi_base,
                                         uint32_t x_lo, uint32_t x_hi,
                                         int bottom_page, int top_page,
                                         const uint16_t *palette,
                                         const uint16_t *bg_text,
                                         const uint8_t *text_tr_flags,
                                         uint8_t grp_pri, uint8_t bg_pri,
                                         uint8_t text_pri, uint16_t *dst);

/* Called by guest-side BG_Write before mutating shared BG/sprite source. */
int tab5_compose_grp8split_needs_selfcheck(void);
void tab5_compose_guest_bg_barrier(void);
/* Called only when the cheap exported pending flag is nonzero. */
void tab5_compose_guest_gvram_barrier(void);
extern volatile uint32_t tab5_compose_gvram_dma_pending;
extern volatile uint32_t tab5_compose_gvram_cache_pending;
void tab5_compose_wait_idle(void);
void tab5_compose_get_stats(tab5_compose_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif
