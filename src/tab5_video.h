/*
 * Tab5 port-specific implementation.
 * Intent: CPU1-to-CPU0 video submission, in-game touch chrome, and runtime media UI for Tab5.
 * Layer8 Aug/17/2026
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define TAB5_VIDEO_MEDIA_PATH_MAX 512

typedef struct
{
    uint32_t submitted_frames;
    uint32_t presented_frames;
    uint32_t dropped_frames;
    uint32_t last_copy_us;
    uint32_t last_push_us;
    uint32_t queued_frames;
    uint32_t live_presented_frames;
    uint32_t live_row_retries;
    uint32_t live_unstable_rows;
    /* Build 6.14d: host-side display cadence / latest-frame pacing stats. */
    uint32_t pace_waits;
    uint32_t pace_wait_us;
    uint32_t pace_last_wait_us;
    uint32_t pace_coalesced_frames;
    uint32_t pace_skipped_slots;
    uint32_t pace_last_interval_us;
} tab5_video_async_stats_t;

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

/* WinDraw writer-generation hooks used by host-core live row presentation. */
void tab5_video_fb_line_write_begin(uint32_t y);
void tab5_video_fb_line_write_end(uint32_t y);
void tab5_video_fb_range_write_begin(uint32_t y, uint32_t count);
void tab5_video_fb_range_write_end(uint32_t y, uint32_t count);

int tab5_video_width(void);
int tab5_video_height(void);
void tab5_video_get_async_stats(tab5_video_async_stats_t *out);

#ifdef __cplusplus
}
#endif
