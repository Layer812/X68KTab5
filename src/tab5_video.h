/*
 * Tab5 port-specific implementation.
 * Intent: CPU1-to-CPU0 video submission, in-game touch chrome, and runtime media UI for Tab5.
 * Layer8 Aug/17/2026
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define TAB5_VIDEO_MEDIA_PATH_MAX 512

typedef enum
{
    TAB5_VIDEO_ACTION_NONE = 0,
    TAB5_VIDEO_ACTION_PANIC_RANDOM,
    TAB5_VIDEO_ACTION_MOUNT_FDD0,
    TAB5_VIDEO_ACTION_MOUNT_FDD1,
    TAB5_VIDEO_ACTION_EJECT_FDD0,
    TAB5_VIDEO_ACTION_EJECT_FDD1,
    TAB5_VIDEO_ACTION_MOUNT_HDD0,
    TAB5_VIDEO_ACTION_EJECT_HDD0,
    TAB5_VIDEO_ACTION_REBOOT_GUEST,
} tab5_video_action_type_t;

typedef struct
{
    tab5_video_action_type_t type;
    char path[TAB5_VIDEO_MEDIA_PATH_MAX];
} tab5_video_action_t;

#ifdef __cplusplus
extern "C" {
#endif

void tab5_video_init(void);

/* Enable the reference game-screen chrome in the 160px side bars. */
void tab5_video_set_game_controls_enabled(int enabled);

/* Enable PanicPlayer's direct packed-GVRAM compatibility presenter. */
void tab5_video_set_panic_compat_enabled(int enabled);

/* CPU1 consumes high-level touch actions produced by CPU0. */
int tab5_video_poll_action(tab5_video_action_t *out);

/* Keep the runtime FILE/disk-changer labels synchronized with guest mounts. */
void tab5_video_set_runtime_media_paths(const char *fdd0,
                                        const char *fdd1,
                                        const char *hdd0);
void tab5_video_set_runtime_boot_source(int boot_source);

/* Temporarily hand the LCD/touch surface to the native launcher while the
 * guest task is paused. No ESP32-P4 peripheral is reinitialized. */
void tab5_video_begin_host_ui(void);
void tab5_video_end_host_ui(void);

void tab5_video_show_message(
    const char *line1,
    const char *line2
);

void tab5_video_status(
    const char *line1,
    const char *line2
);

/* Packed snapshot (host text view / fallback). */
void tab5_video_present_px68k(
    const uint16_t *frame,
    uint32_t width,
    uint32_t height,
    uint32_t pitch_pixels
);

/* Build 5.45: zero-copy live PX68K framebuffer presentation. */
void tab5_video_present_px68k_live(
    const uint16_t *frame,
    uint32_t width,
    uint32_t height,
    uint32_t pitch_pixels
);

/* R52: transition-only immutable LIVE publish.
 * Copies the completed guest framebuffer into an existing PSRAM presenter
 * slot, queues it as a non-coalescable LIVE frame, and waits until CPU0 has
 * actually rendered that exact snapshot once.  Normal frames continue using
 * the zero-copy API above.  Returns non-zero on confirmed presentation. */
int tab5_video_present_px68k_live_frozen(
    const uint16_t *frame,
    uint32_t width,
    uint32_t height,
    uint32_t pitch_pixels
);

/* R56 Screen Manager: copy an immutable completed ScreenVersion into a
 * presenter-owned slot and queue it without blocking the emulator.  The
 * display backend reports completion back to Screen Manager by token. */
int tab5_video_present_px68k_managed(
    const uint16_t *frame,
    uint32_t width,
    uint32_t height,
    uint32_t pitch_pixels,
    const uint32_t *dirty_tiles32,
    uint64_t screen_token
);
/* R48: invalidate queued/pacing LIVE requests from the previous source/geometry epoch. */
uint32_t tab5_video_live_epoch_invalidate(void);
/* R48 transition barrier: invalidate old LIVE tokens, wait out an in-flight
 * CPU0 present, optionally blank only the 960x720 game viewport, and reset
 * LCD geometry/generation caches before CPU1 rebuilds the new source. */
uint32_t tab5_video_live_transition_begin(int clear_game_viewport);
uint32_t tab5_video_live_epoch_current(void);

/* R57E3: publish a semantic guest-screen transition from CPU1.  CPU0 fences
 * touch actions until one matching physical present completes and the current
 * finger has been released.  This call never waits and never accesses LCD/touch. */
void tab5_video_touch_transition_begin(uint32_t width, uint32_t height, uint32_t pitch_pixels);

/* WinDraw writer-generation hooks used by host-core live row presentation. */
void tab5_video_fb_line_write_begin(uint32_t y);
void tab5_video_fb_line_write_end(uint32_t y);
/* R38: CPU0 compositor can retire a writer without publishing a new generation when the final RGB565 line is bit-identical or a queued job was stale-dropped. */
void tab5_video_fb_line_write_end_changed(uint32_t y, int changed);
/* R40: exact producer paths may additionally publish the changed source-X span [x0,x1).
 * Generic/legacy writers continue to publish a conservative full-width span. */
void tab5_video_fb_line_write_end_changed_span(uint32_t y, int changed, uint32_t x0, uint32_t x1);
/* R42: exact 65K producer may publish a sparse 32-source-pixel tile mask.
 * Bit n covers source X [n*32,(n+1)*32).  width_pixels bounds the last tile.
 * Multiple writes before LCD consumption OR their masks; generation still advances once per changed writer. */
void tab5_video_fb_line_write_end_changed_tiles32(uint32_t y, int changed, uint32_t tile_mask, uint32_t width_pixels);
void tab5_video_fb_range_write_begin(uint32_t y, uint32_t count);
void tab5_video_fb_range_write_end(uint32_t y, uint32_t count);

int tab5_video_width(void);
int tab5_video_height(void);

#ifdef __cplusplus
}
#endif
