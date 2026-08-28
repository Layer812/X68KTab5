#include "tab5_guest_video_state.h"

#include <string.h>

/* CPU1-only state.  Keep epoch/seq non-zero so zero remains an explicit
 * unknown/uninitialized tag in Screen Manager diagnostics. */
static uint32_t s_video_epoch = 1u;
static uint32_t s_visual_seq = 1u;
static uint32_t s_hard_transitions;
static uint32_t s_visual_transitions;
static uint32_t s_last_hard_cause;
static uint32_t s_hard_cause_or;
static uint32_t s_visual_cause_or;
static uint32_t s_width;
static uint32_t s_height;
static uint32_t s_pitch;
static uint8_t s_vctrl_mode;
static uint8_t s_crtc_mode;
static uint8_t s_geometry_seen;

static inline uint32_t next_nonzero_u32(uint32_t v)
{
    ++v;
    return v ? v : 1u;
}

static inline void bump_hard(uint32_t cause)
{
    s_video_epoch = next_nonzero_u32(s_video_epoch);
    ++s_hard_transitions;
    s_last_hard_cause = cause;
    s_hard_cause_or |= cause;
}

static inline void bump_visual(uint32_t cause)
{
    s_visual_seq = next_nonzero_u32(s_visual_seq);
    ++s_visual_transitions;
    s_visual_cause_or |= cause;
}

void tab5_guest_video_state_reset(void)
{
    bump_hard(TAB5_GVIDEO_HARD_RESET);
    s_visual_seq = next_nonzero_u32(s_visual_seq);
    ++s_visual_transitions;
    s_geometry_seen = 0u;
    s_width = 0u;
    s_height = 0u;
    s_pitch = 0u;
    s_vctrl_mode = 0u;
    s_crtc_mode = 0u;
}

void tab5_guest_video_state_frame_boundary(uint32_t width, uint32_t height,
                                           uint32_t pitch,
                                           uint8_t vctrl_mode,
                                           uint8_t crtc_mode)
{
    const uint8_t vm = (uint8_t)(vctrl_mode & 0x07u);
    const uint8_t cm = (uint8_t)(crtc_mode & 0x1cu);

    if (!s_geometry_seen) {
        s_geometry_seen = 1u;
        s_width = width;
        s_height = height;
        s_pitch = pitch;
        s_vctrl_mode = vm;
        s_crtc_mode = cm;
        return;
    }

    if (width != s_width || height != s_height || pitch != s_pitch) {
        s_width = width;
        s_height = height;
        s_pitch = pitch;
        bump_hard(TAB5_GVIDEO_HARD_GEOMETRY);
    }

    /* Normally these were already observed directly at the register write.
     * The frame-boundary comparison is an idempotent safety net for reset/load
     * paths that restore register arrays without calling the ordinary writer. */
    if (vm != s_vctrl_mode) {
        s_vctrl_mode = vm;
        bump_hard(TAB5_GVIDEO_HARD_VCTRL_MODE);
    }
    if (cm != s_crtc_mode) {
        s_crtc_mode = cm;
        bump_hard(TAB5_GVIDEO_HARD_CRTC_MODE);
    }
}

void tab5_guest_video_state_note_vctrl0(uint8_t new_mode)
{
    const uint8_t vm = (uint8_t)(new_mode & 0x07u);
    if (!s_geometry_seen) {
        s_vctrl_mode = vm;
        bump_visual(TAB5_GVIDEO_VIS_PRIORITY);
        return;
    }
    if (vm != s_vctrl_mode) {
        s_vctrl_mode = vm;
        bump_hard(TAB5_GVIDEO_HARD_VCTRL_MODE);
    }
}

void tab5_guest_video_state_note_vctrl1(void)
{
    /* Reserved for a later soft-state pass; BAT172E0 does not instrument hot control traffic. */
}

void tab5_guest_video_state_note_vctrl2(void)
{
    /* Reserved for a later soft-state pass. */
}

void tab5_guest_video_state_note_crtc(uint8_t reg, uint8_t data)
{
    if (reg != 0x29u)
        return;
    const uint8_t cm = (uint8_t)(data & 0x1cu);
    if (!s_geometry_seen) {
        s_crtc_mode = cm;
        return;
    }
    if (cm != s_crtc_mode) {
        s_crtc_mode = cm;
        bump_hard(TAB5_GVIDEO_HARD_CRTC_MODE);
    }
}

void tab5_guest_video_state_note_palette(int graphics_palette)
{
    (void)graphics_palette;
    /* Intentionally not instrumented in BAT172E0. */
}

tab5_guest_video_stamp_t tab5_guest_video_state_stamp(void)
{
    tab5_guest_video_stamp_t s;
    s.video_epoch = s_video_epoch;
    s.visual_seq = s_visual_seq;
    return s;
}

void tab5_guest_video_state_get_stats(tab5_guest_video_state_stats_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->video_epoch = s_video_epoch;
    out->visual_seq = s_visual_seq;
    out->hard_transitions = s_hard_transitions;
    out->visual_transitions = s_visual_transitions;
    out->last_hard_cause = s_last_hard_cause;
    out->hard_cause_or = s_hard_cause_or;
    out->visual_cause_or = s_visual_cause_or;
    out->width = s_width;
    out->height = s_height;
    out->pitch = s_pitch;
    out->vctrl_mode = s_vctrl_mode;
    out->crtc_mode = s_crtc_mode;
    out->geometry_seen = s_geometry_seen;
}
