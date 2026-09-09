#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PX68K Tab5 Screen Manager - R56 timeline/ownership contract
 * -----------------------------------------------------------
 * Emulator/renderer/display are message producers/consumers only.
 * They never own a ScreenVersion and never receive a writable Screen surface.
 *
 * Guest ordering and host scheduling are intentionally separate. R57E9 uses guest_seq as the common logical ordering key for CPU1->CPU0 screen notifications; wall-clock time is never an ordering source:
 *   - guest_seq orders VIDEO_LINE_LATCH / VIDEO_FRAME_BOUNDARY facts.
 *   - render_ticket identifies one immutable render request/result.
 *   - present_token identifies one physical display transaction.
 *
 * Only this module commits pixels into the managed working surface and only
 * this module changes ScreenVersion lifecycle state.
 */

enum {
    TAB5_SCREEN_EMU_ACTION_NONE = 0u,
    TAB5_SCREEN_EMU_ACTION_MARK_ALL_DIRTY = 1u << 0,
};

int tab5_screen_manager_init(void);

/*
 * VIDEO_LINE_LATCH fact.
 * The caller supplies y/width only; Screen Manager assigns guest_seq and an
 * opaque render_ticket.  private_seed_line receives a COPY of the currently
 * committed line.  The returned pointer is never Screen Manager memory.
 */
int tab5_screen_video_line_latch(uint32_t y, uint32_t width,
                                 uint16_t *private_seed_line,
                                 uint64_t *render_ticket);

/* CPU1->Screen publication contract. video_epoch is a hard guest semantic
 * regime; visual_seq is diagnostic-only soft visual ordering.  BAT175A0 also
 * permits private_seed_line==NULL for post-render publication: admission still
 * assigns an opaque ticket, but Screen Manager does not copy/seed host pixels
 * back into the CPU1 renderer. */
int tab5_screen_video_line_latch_state(uint32_t y, uint32_t width,
                                       uint32_t video_epoch,
                                       uint32_t visual_seq,
                                       uint16_t *private_seed_line,
                                       uint64_t *render_ticket);

/* Renderer completion/cancel messages.  ticket is opaque outside Screen
 * Manager.  Result pixels are copied into a message slot before returning. */
int  tab5_screen_render_result(uint64_t render_ticket, uint32_t width,
                               const uint16_t *private_result_line);
void tab5_screen_render_cancel(uint64_t render_ticket);

/* BAT177NW1 CPU0-only final-compositor handoff. These APIs may sleep on CPU0
 * bounded result-slot pressure; CPU1 never calls them. The acquired pixels
 * belong to Screen Manager until submit/release. */
uint16_t *tab5_screen_cpu0_flow_acquire(uint32_t *slot_token);
int tab5_screen_cpu0_flow_submit(uint32_t slot_token,
                                 uint32_t y, uint32_t width,
                                 uint64_t render_seq,
                                 uint32_t video_epoch,
                                 uint32_t visual_seq);
void tab5_screen_cpu0_flow_release(uint32_t slot_token);

/* Guest reset fact.  Invalidates the current construction generation without
 * exposing Screen ids to the emulator.  The currently VISIBLE screen remains
 * visible until a post-reset ScreenVersion is physically presented. */
void tab5_screen_video_reset(void);

/* Guest VIDEO_FRAME_BOUNDARY fact.  This never means "present now". */
void tab5_screen_video_frame_boundary(uint32_t width, uint32_t height,
                                      uint32_t pitch);

/* Host presentation policy offers an opportunity without naming a screen.
 * Screen Manager decides which construction (if any) may be sealed. */
/* Audio reserve is a scheduling hint only.  CPU1 publishes facts and never
 * waits for Screen Manager or physical refresh. */
void tab5_screen_manager_production_reserve_hint(uint32_t effective_reserve_frames,
                                                   uint32_t submitted_frames);

void tab5_screen_present_opportunity(void);

/* Display backend completion message. present_token belongs to the display
 * time axis and is deliberately distinct from ScreenVersion id. */
void tab5_screen_manager_present_complete(uint64_t present_token, int success);

/* Screen Manager may request emulator-side work without revealing a Screen id. */
uint32_t tab5_screen_take_emulator_actions(void);

/* Renderer failure is also a message.  Screen Manager decides whether the
 * emulator should retry that source line; CPU0 never writes TextDirtyLine. */
int tab5_screen_take_emulator_retry_line(uint32_t *y);

/* Compatibility/diagnostic view only. Never write through this pointer. */
const uint16_t *tab5_screen_readonly_work_buffer(void);


#ifdef __cplusplus
}
#endif
