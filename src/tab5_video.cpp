/*
 * Tab5 port-specific implementation.
 * Intent: Tab5 LCD frontend: scale generation-dirty guest rows, suppress pixel-identical LCD bands, and keep LCD work on CPU0.
 * Layer8 Aug/17/2026
 */
#include "tab5_video.h"
#include "tab5_branding.h"

#include "tab5_guest_input.h"
#include "tab5_panic.h"
#include "tab5_audio.h"
#include "tab5_launcher.h"
#include "tab5_media_ui.h"
#include "libretro/joystick.h"
#include "libretro/keyboard.h"

#include <M5Unified.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "sdkconfig.h"

/* Build 5.98g8: reuse the proven PIE graphics primitives from tab5_ppa.c
 * in the CPU0 LCD frontend.  C linkage is required because this file is C++. */
extern "C" int tab5_pie_graphics_diff(const void *a, const void *b, uint32_t bytes);
extern "C" void tab5_pie_graphics_copy(void *dst, const void *src, uint32_t bytes);

/*
 * Intent: PANIC compatibility presentation reads the authoritative X68000
 * video state on CPU0.  These are C globals owned by the frozen PX68K core;
 * normal PX68K rendering remains unchanged.
 * Layer8 Aug/17/2026
 */
extern "C" {
extern uint8_t GVRAM[0x80000];
extern uint16_t GrphPal[256];
extern uint16_t TextPal[256];
extern uint8_t Pal_Regs[1024];
extern uint16_t Pal16[65536];
extern uint16_t Pal16Adr[256];
extern uint32_t GrphScrollX[4];
extern uint32_t GrphScrollY[4];
extern uint8_t VCReg0[2];
extern uint8_t VCReg1[2];
extern uint8_t VCReg2[2];
extern uint8_t CRTC_Regs[48];
extern uint8_t BG[0x8000];
extern uint8_t Sprite_Regs[0x800];
extern uint8_t BG_Regs[0x12];
}

/* BUILD2_TAB5_PSRAM_GUARD */
#ifndef CONFIG_SPIRAM_SPEED_200M
#error "M5Stack Tab5 requires CONFIG_SPIRAM_SPEED_200M=y"
#endif

/* Intent: Submit only changed guest generations and suppress pixel-identical LCD rows; LCD bandwidth is host work and must stay off CPU1.  Layer8 Aug/17/2026 */
static const char *TAG = "TAB5_VIDEO";
static bool s_present_started = false;
static bool s_game_controls_enabled = false;
static bool s_host_ui_exclusive = false;
static bool s_panic_compat_enabled = false;

/*
 * Build 5.45 video split
 * ----------------------
 * Composite/PX68K framebuffer:
 *   CPU1 guest only posts pointer + geometry.  CPU0 host reads the completed ScrBuf
 *   directly and pushes rows to the LCD.  There is no PSRAM->PSRAM snapshot.
 *   A per-scanline generation handshake lets the reader retry a row if CPU1 guest
 *   happened to update that row while the LCD consumed it.  CPU1 never waits.
 *
 * Host text view:
 *   Keep the proven Build 5.44 packed snapshot slots because that buffer is
 *   produced by the host text renderer rather than WinDraw and therefore has
 *   no scanline-generation markers.
 */
static constexpr uint32_t kPresentSlots = 2;
static constexpr uint32_t kTrackedLines = 1024;
static constexpr uint32_t kLiveRowRetryLimit = 8;

enum present_mode_t : uint8_t
{
    PRESENT_SNAPSHOT = 0,
    PRESENT_LIVE_FB = 1,
};

typedef struct
{
    uint16_t *pixels;
    uint32_t width;
    uint32_t height;
} present_slot_t;

typedef struct
{
    present_mode_t mode;
    uint8_t slot_index;
    const uint16_t *frame;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
} present_request_t;

static present_slot_t s_slots[kPresentSlots] = {};
static size_t s_slot_capacity_pixels = 0;
static QueueHandle_t s_free_slots = nullptr;
static QueueHandle_t s_ready_requests = nullptr;
static QueueHandle_t s_ui_actions = nullptr;
static SemaphoreHandle_t s_display_mutex = nullptr;
static TaskHandle_t s_present_task = nullptr;
static bool s_async_ready = false;

/* Writer count permits nested marking (normal line + PPA batch flush).  The
 * generation increments once for every completed writer section. */
static uint32_t s_line_writers[kTrackedLines] = {};
static uint32_t s_line_generation[kTrackedLines] = {};

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_submitted_frames = 0;
static uint32_t s_presented_frames = 0;
static uint32_t s_dropped_frames = 0;
static uint32_t s_last_copy_us = 0;
static uint32_t s_last_push_us = 0;
static uint32_t s_live_presented_frames = 0;
static uint32_t s_live_row_retries = 0;
static uint32_t s_live_unstable_rows = 0;

/* Build 6.14d: presentation pacing is host-only.  Quantize normal live-FB
 * presentation onto the X68000 CRTC cadence while keeping the newest queued
 * live frame.  This never stalls CPU1; it only schedules CPU0 LCD work. */
static esp_timer_handle_t s_pace_timer = nullptr;
static int64_t s_pace_next_us = 0;
static int64_t s_pace_last_present_us = 0;
static uint32_t s_pace_waits = 0;
static uint32_t s_pace_wait_us = 0;
static uint32_t s_pace_last_wait_us = 0;
static uint32_t s_pace_coalesced_frames = 0;
static uint32_t s_pace_skipped_slots = 0;
static uint32_t s_pace_last_interval_us = 0;
static bool s_pace_active_logged = false;

/* Build 5.98: libretro advertises PX68K output at a fixed 4:3 display
 * aspect even when the raw CRTC raster is e.g. 512x240.  The standalone
 * Tab5 frontend used to push those raw pixels 1:1, which made 240-line game
 * modes appear as a very short strip.  Scale only on CPU0 at presentation
 * time; the guest/WinDraw framebuffer and all CRTC semantics stay untouched. */
static constexpr uint32_t kAspectMapMaxWidth = 1280;
static constexpr uint32_t kAspectViewportWidth = 960;
static constexpr uint32_t kAspectViewportHeight = 720;
static uint16_t *s_aspect_frame = nullptr;
static constexpr uint32_t kPanicCompatPitch = 512;
static constexpr uint32_t kPanicCompatHeight = 512;
static uint16_t *s_panic_compat_frame = nullptr;
static uint32_t s_panic_compat_last_w = 0;
static uint32_t s_panic_compat_last_h = 0;
static uint8_t s_panic_compat_last_mode = 0xff;
static uint8_t s_panic_compat_last_enable = 0xff;
/* Build 5.98g8: one destination-width scratch row lets CPU0 render a
 * generation-dirty line without destroying the last displayed copy.  A PIE
 * DIFF can then suppress the LCD transaction when the pixels are unchanged.
 * Keep the raw pointer because the usable row is modulo-16 aligned to
 * s_aspect_frame for the PIE wrapper. */
static uint8_t *s_aspect_line_scratch_raw = nullptr;
static uint16_t *s_aspect_line_scratch = nullptr;
static uint64_t s_vdiff_tested_src_rows = 0;
static uint64_t s_vdiff_same_src_rows = 0;
static uint64_t s_vdiff_changed_src_rows = 0;
static uint64_t s_vdiff_identical_dst_rows = 0;
static uint16_t s_aspect_xmap[kAspectMapMaxWidth] = {};
static uint16_t s_aspect_ymap[kAspectViewportHeight] = {};
static uint32_t s_aspect_map_src_w = 0;
static uint32_t s_aspect_map_dst_w = 0;
static uint32_t s_aspect_map_src_h = 0;
static uint32_t s_aspect_map_dst_h = 0;
static constexpr uint32_t kDirtyMergeGapRows = 2;
static uint32_t s_aspect_log_src_w = 0;
static uint32_t s_aspect_log_src_h = 0;

/* Build 5.98a: the WinDraw generation markers already describe the exact
 * source scanlines that were redrawn (WinDraw only marks TextDirtyLine rows).
 * Cache the last generation presented for each source line and only rebuild /
 * push LCD bands whose source generation changed.  Geometry or framebuffer
 * changes force one full viewport refresh. */
static uint32_t s_aspect_seen_generation[kTrackedLines] = {};
static const uint16_t *s_aspect_seen_frame = nullptr;
static uint32_t s_aspect_seen_src_w = 0;
static uint32_t s_aspect_seen_src_h = 0;
static uint32_t s_aspect_seen_pitch = 0;
static uint32_t s_aspect_dirty_frames = 0;
static uint32_t s_aspect_full_frames = 0;
static uint32_t s_aspect_dirty_bands = 0;
static uint32_t s_aspect_dirty_rows = 0;

/*
 * In-game touch chrome + runtime media changer
 * ---------------------------------------------
 * Intent: Match the agreed game-screen layout without covering the 960x720
 * X68000 viewport.  CPU0 owns drawing/touch.  High-level PANIC/media actions
 * are queued to CPU1 so disk mutation still happens on the emulation task.
 * Layer8 Aug/17/2026
 */
static inline bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static inline bool point_in_circle(int x, int y, int cx, int cy, int r)
{
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void draw_panel_button(int x, int y, int w, int h, const char *label,
                              uint16_t fill, uint16_t border, int text_size)
{
    M5.Display.fillRoundRect(x, y, w, h, 8, fill);
    M5.Display.drawRoundRect(x, y, w, h, 8, border);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(text_size);
    M5.Display.setTextColor(TFT_WHITE, fill);
    M5.Display.drawString(label, x + w / 2, y + h / 2);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

static void draw_folder_icon(int x, int y, uint16_t color)
{
    M5.Display.drawRect(x, y + 7, 38, 25, color);
    M5.Display.drawRect(x + 4, y, 15, 9, color);
    M5.Display.drawFastHLine(x + 1, y + 8, 36, color);
}

static void draw_speaker_icon(int x, int y, bool plus)
{
    const uint16_t c = 0xDEFB;
    M5.Display.fillRect(x, y + 10, 9, 18, c);
    M5.Display.fillTriangle(x + 9, y + 10, x + 25, y + 2, x + 25, y + 36, c);
    M5.Display.drawCircle(x + 29, y + 19, 11, c);
    M5.Display.fillRect(x + 25, y + 2, 10, 34, TFT_BLACK); /* leave a sound-wave arc */
    M5.Display.drawFastHLine(x + 43, y + 19, 16, c);
    if (plus) M5.Display.drawFastVLine(x + 51, y + 11, 16, c);
}

static void draw_round_ab(int cx, int cy, const char *label)
{
    M5.Display.fillCircle(cx, cy, 43, 0x18E3);
    M5.Display.drawCircle(cx, cy, 43, 0x8410);
    M5.Display.drawCircle(cx, cy, 42, 0x4208);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(0xDEFB, 0x18E3);
    M5.Display.drawString(label, cx, cy);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

static void draw_dpad_button(int x, int y, int w, int h, char arrow)
{
    M5.Display.fillRoundRect(x, y, w, h, 5, 0x18E3);
    M5.Display.drawRoundRect(x, y, w, h, 5, 0x5AEB);
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const uint16_t c = 0xDEFB;
    if (arrow == 'U') M5.Display.fillTriangle(cx, cy - 9, cx - 9, cy + 6, cx + 9, cy + 6, c);
    if (arrow == 'D') M5.Display.fillTriangle(cx, cy + 9, cx - 9, cy - 6, cx + 9, cy - 6, c);
    if (arrow == 'L') M5.Display.fillTriangle(cx - 9, cy, cx + 6, cy - 9, cx + 6, cy + 9, c);
    if (arrow == 'R') M5.Display.fillTriangle(cx + 9, cy, cx - 6, cy - 9, cx - 6, cy + 9, c);
}

static void draw_game_controls_unlocked(void)
{
    if (!s_game_controls_enabled) return;
    const int w = M5.Display.width();
    const int h = M5.Display.height();
    if (w < 1200 || h < 700) return;

    const int rx = w - 160;
    M5.Display.fillRect(0, 0, 160, h, TFT_BLACK);
    M5.Display.fillRect(rx, 0, 160, h, TFT_BLACK);
    M5.Display.drawFastVLine(159, 0, h, 0x3186);
    M5.Display.drawFastVLine(rx, 0, h, 0x3186);

    /* Reference layout: PANIC / FILE above a low D-pad. */
    draw_panel_button(31, 29, 98, 68, "PANIC", 0x7800, 0xF800, 2);
    M5.Display.fillRoundRect(31, 118, 98, 68, 8, 0x1082);
    M5.Display.drawRoundRect(31, 118, 98, 68, 8, 0x8410);
    draw_folder_icon(61, 127, 0xDEFB);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xDEFB, 0x1082);
    M5.Display.drawString("FILE", 80, 170);
    M5.Display.setTextDatum(textdatum_t::top_left);

    /* Build 6.12o: software keyboard launcher in the unused left-side slot. */
    draw_panel_button(31, 207, 98, 68, "KEY", 0x1082, 0x8410, 2);

    draw_dpad_button(53, 520, 54, 54, 'U');
    draw_dpad_button(17, 577, 56, 60, 'L');
    draw_dpad_button(87, 577, 56, 60, 'R');
    draw_dpad_button(53, 640, 54, 54, 'D');

    /* Reference layout: volume controls high, B/A circular buttons low. */
    /* Build 6.12d: conventional vertical volume ordering: + above, - below. */
    draw_panel_button(rx + 31, 29, 98, 62, "", 0x1082, 0x8410, 1);
    draw_speaker_icon(rx + 49, 41, true);
    draw_panel_button(rx + 31, 110, 98, 62, "", 0x1082, 0x8410, 1);
    draw_speaker_icon(rx + 49, 122, false);
    /* CPSF/MD bank-1 Start; this is not synthesized as opposing D-pad bits. */
    draw_panel_button(rx + 31, 207, 98, 68, "START", 0x1082, 0x8410, 2);
    draw_round_ab(rx + 80, 520, "B");
    draw_round_ab(rx + 80, 635, "A");
}

enum : uint32_t {
    UTIL_PANIC = 1u << 0,
    UTIL_FILE  = 1u << 1,
    UTIL_VOLM     = 1u << 2,
    UTIL_VOLP     = 1u << 3,
    UTIL_KEYBOARD = 1u << 4,
};

typedef struct
{
    uint16_t joy;
    uint32_t util;
} game_touch_map_t;

/* Build 6.12m: single source of truth for the game-side touch map. */
static game_touch_map_t map_game_touch_point(int x, int y, int display_w)
{
    game_touch_map_t m = {};
    const int rx = display_w - 160;

    if (point_in_rect(x,y,31,29,98,68)) m.util |= UTIL_PANIC;
    if (point_in_rect(x,y,31,118,98,68)) m.util |= UTIL_FILE;
    if (point_in_rect(x,y,31,207,98,68)) m.util |= UTIL_KEYBOARD;
    if (point_in_rect(x,y,53,520,54,54)) m.joy |= JOY_UP;
    if (point_in_rect(x,y,17,577,56,60)) m.joy |= JOY_LEFT;
    if (point_in_rect(x,y,87,577,56,60)) m.joy |= JOY_RIGHT;
    if (point_in_rect(x,y,53,640,54,54)) m.joy |= JOY_DOWN;

    if (point_in_rect(x,y,rx+31,29,98,62)) m.util |= UTIL_VOLP;
    if (point_in_rect(x,y,rx+31,110,98,62)) m.util |= UTIL_VOLM;
    if (point_in_rect(x,y,rx+31,207,98,68)) m.joy |= JOY_HOST_START;

    /* PX68K/libretro normal 2-button convention: B=TRG1, A=TRG2. */
    if (point_in_circle(x,y,rx+80,520,48)) m.joy |= JOY_TRG1; /* B */
    if (point_in_circle(x,y,rx+80,635,48)) m.joy |= JOY_TRG2; /* A */
    return m;
}



static uint32_t s_utility_prev_mask = 0;
/* Build 6.12d touch D-pad repeat shaping.  USB JOY remains a true continuous
 * pad. Touch gets an immediate pulse, a deliberate initial pause, then a
 * moderate repeat cadence so menu cursors do not run away. */
static uint16_t s_touch_dpad_prev = 0;
static int64_t s_touch_dpad_press_us = 0;
static bool s_media_overlay_active = false;
static bool s_media_overlay_redraw = false;
static bool s_media_overlay_touch_down = false;
static bool s_media_overlay_browser = false;
static bool s_game_touch_suppress_until_release = false;
static bool s_force_game_redraw = false;
static int s_media_overlay_drive = 0; /* 0=FDD0, 1=FDD1, 2=HDD0 */
static size_t s_media_overlay_page = 0;
static int s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
static int s_runtime_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;

/* Build 6.12o buffered software keyboard -------------------------------------------------
 * CPU0 owns the overlay.  Key taps enter the same thread-safe queue used by USB
 * HID, so CPU1 still mutates the X68000 keyboard matrix only from guest_input_tick().
 * While the keyboard is visible the last game frame is intentionally frozen on
 * the LCD; the emulated machine and audio continue to run.  This avoids a large
 * keyboard redraw after every video frame and keeps CPU0 headroom predictable. */
static bool s_softkbd_active = false;
static bool s_softkbd_redraw = false;
static bool s_softkbd_touch_down = false;
static bool s_softkbd_shift = false;
static bool s_softkbd_ctrl = false;

/* Build 6.12o: compose locally, preview, then SEND.  One buffered item is one
 * X68000 key tap plus the SHIFT/CTRL state captured when it was entered. */
typedef struct {
    uint8_t scancode;
    uint8_t modifiers;
    const char *label;
} soft_input_event_t;
static constexpr size_t kSoftInputMax = 80;
static soft_input_event_t s_soft_input[kSoftInputMax] = {};
static size_t s_soft_input_count = 0;

enum : uint8_t {
    SOFTKEY_NORMAL = 0,
    SOFTKEY_SHIFT  = 1,
    SOFTKEY_CTRL   = 2,
};

typedef struct {
    const char *label;
    uint8_t scancode;
    uint8_t units;
    uint8_t kind;
} soft_key_t;

/* X68000 keyboard scan codes.  The software keyboard deliberately uses the
 * guest scan codes directly rather than translating through RetroKey/USB HID.
 * This preserves the X68000/JIS layout (including @, :, ^, OPT.1/OPT.2) and
 * guarantees that Human68k and games see the exact make/break bytes. */
static const soft_key_t kSoftRow0[] = {
    {"ESC",0x01,2,0},{"F1",0x63,1,0},{"F2",0x64,1,0},{"F3",0x65,1,0},
    {"F4",0x66,1,0},{"F5",0x67,1,0},{"F6",0x68,1,0},{"F7",0x69,1,0},
    {"F8",0x6A,1,0},{"F9",0x6B,1,0},{"F10",0x6C,1,0},{"BS",0x0F,2,0},
};
static const soft_key_t kSoftRow1[] = {
    {"1",0x02,1,0},{"2",0x03,1,0},{"3",0x04,1,0},{"4",0x05,1,0},{"5",0x06,1,0},
    {"6",0x07,1,0},{"7",0x08,1,0},{"8",0x09,1,0},{"9",0x0A,1,0},{"0",0x0B,1,0},
    {"-",0x0C,1,0},{"^",0x0D,1,0},{"\\",0x0E,1,0},{"DEL",0x37,2,0},
};
static const soft_key_t kSoftRow2[] = {
    {"TAB",0x10,2,0},{"Q",0x11,1,0},{"W",0x12,1,0},{"E",0x13,1,0},{"R",0x14,1,0},
    {"T",0x15,1,0},{"Y",0x16,1,0},{"U",0x17,1,0},{"I",0x18,1,0},{"O",0x19,1,0},
    {"P",0x1A,1,0},{"@",0x1B,1,0},{"[",0x1C,1,0},{"RET",0x1D,2,0},
};
static const soft_key_t kSoftRow3[] = {
    {"CTRL",0x71,2,SOFTKEY_CTRL},{"A",0x1E,1,0},{"S",0x1F,1,0},{"D",0x20,1,0},
    {"F",0x21,1,0},{"G",0x22,1,0},{"H",0x23,1,0},{"J",0x24,1,0},{"K",0x25,1,0},
    {"L",0x26,1,0},{";",0x27,1,0},{":",0x28,1,0},{"]",0x29,1,0},{"RET",0x1D,2,0},
};
static const soft_key_t kSoftRow4[] = {
    {"SHIFT",0x70,2,SOFTKEY_SHIFT},{"Z",0x2A,1,0},{"X",0x2B,1,0},{"C",0x2C,1,0},
    {"V",0x2D,1,0},{"B",0x2E,1,0},{"N",0x2F,1,0},{"M",0x30,1,0},{",",0x31,1,0},
    {".",0x32,1,0},{"/",0x33,1,0},{"_",0x34,1,0},{"SHIFT",0x70,2,SOFTKEY_SHIFT},
};
static const soft_key_t kSoftRow5[] = {
    {"OPT1",0x72,2,0},{"LEFT",0x3B,1,0},{"DOWN",0x3E,1,0},{"UP",0x3C,1,0},
    {"RIGHT",0x3D,1,0},{"SPACE",0x35,7,0},{"HOME",0x36,2,0},{"OPT2",0x73,2,0},
};

typedef struct { const soft_key_t *keys; size_t count; } soft_row_t;
static const soft_row_t kSoftRows[] = {
    {kSoftRow0,sizeof(kSoftRow0)/sizeof(kSoftRow0[0])},
    {kSoftRow1,sizeof(kSoftRow1)/sizeof(kSoftRow1[0])},
    {kSoftRow2,sizeof(kSoftRow2)/sizeof(kSoftRow2[0])},
    {kSoftRow3,sizeof(kSoftRow3)/sizeof(kSoftRow3[0])},
    {kSoftRow4,sizeof(kSoftRow4)/sizeof(kSoftRow4[0])},
    {kSoftRow5,sizeof(kSoftRow5)/sizeof(kSoftRow5[0])},
};

static constexpr int kSoftKbdX = 168;
static constexpr int kSoftKbdY = 266;
static constexpr int kSoftKbdW = 944;
static constexpr int kSoftKbdRowH = 68;
static constexpr int kSoftKbdGap = 4;

static int softkbd_units(const soft_row_t &row)
{
    int n=0; for (size_t i=0;i<row.count;++i) n += row.keys[i].units; return n;
}

static void softkbd_key_rect(const soft_row_t &row, size_t index, int row_index,
                             int *x, int *y, int *w, int *h)
{
    const int units=softkbd_units(row);
    const int usable=kSoftKbdW - (int)(row.count-1)*kSoftKbdGap;
    int xpos=kSoftKbdX;
    int consumed_units=0;
    for (size_t i=0;i<index;++i) {
        const int next_units=consumed_units + row.keys[i].units;
        const int x0=(usable*consumed_units)/units;
        const int x1=(usable*next_units)/units;
        xpos += (x1-x0) + kSoftKbdGap;
        consumed_units=next_units;
    }
    const int next_units=consumed_units + row.keys[index].units;
    const int x0=(usable*consumed_units)/units;
    const int x1=(usable*next_units)/units;
    *x=xpos; *y=kSoftKbdY + row_index*kSoftKbdRowH;
    *w=x1-x0; *h=kSoftKbdRowH-kSoftKbdGap;
}

static void softkbd_append_text(char *dst, size_t cap, const char *src)
{
    if (!dst || !cap || !src) return;
    const size_t used=std::strlen(dst);
    if (used + 1 >= cap) return;
    std::strncat(dst,src,cap-used-1);
}

static void softkbd_build_preview(char *out,size_t cap)
{
    if (!out || !cap) return;
    out[0]='\0';
    for (size_t i=0;i<s_soft_input_count;++i) {
        const soft_input_event_t &e=s_soft_input[i];
        const char *lab=e.label ? e.label : "?";
        char tok[20] = {};
        if (!std::strcmp(lab,"SPACE")) {
            std::strcpy(tok," ");
        } else if (!std::strcmp(lab,"RET")) {
            std::strcpy(tok,"<RET>");
        } else if (std::strlen(lab)==1 && std::isalpha((unsigned char)lab[0])) {
            char c=lab[0];
            if (!(e.modifiers & 0x01u)) c=(char)std::tolower((unsigned char)c);
            if (e.modifiers & 0x02u) std::snprintf(tok,sizeof(tok),"<C-%c>",(char)std::toupper((unsigned char)c));
            else { tok[0]=c; tok[1]='\0'; }
        } else if (std::strlen(lab)==1 && e.modifiers) {
            std::snprintf(tok,sizeof(tok),"<%s%s%s>",(e.modifiers&1u)?"S-":"",(e.modifiers&2u)?"C-":"",lab);
        } else if (std::strlen(lab)==1) {
            tok[0]=lab[0]; tok[1]='\0';
        } else {
            std::snprintf(tok,sizeof(tok),"<%s>",lab);
        }
        softkbd_append_text(out,cap,tok);
    }
}

static void draw_soft_keyboard_unlocked(void)
{
    /* Keep side controls visible; repaint only the central viewport. */
    M5.Display.fillRect(160, 0, 960, 720, 0x0841);
    M5.Display.fillRect(160, 0, 960, 54, 0x1082);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE,0x1082);
    M5.Display.drawString("SOFTWARE KEYBOARD",640,20);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Compose in INPUT BUFFER, then tap SEND",640,43);
    M5.Display.setTextDatum(textdatum_t::top_left);

    /* Input preview: deliberately separate from guest output.  Nothing is
     * injected into Human68k/game until SEND is tapped. */
    const int px=178, py=70, pw=752, ph=140;
    M5.Display.fillRoundRect(px,py,pw,ph,7,0x0000);
    M5.Display.drawRoundRect(px,py,pw,ph,7,0x5AEB);
    M5.Display.setTextColor(0xBDF7,0x0000);
    M5.Display.setTextSize(1);
    M5.Display.drawString("INPUT BUFFER",px+12,py+8);
    char preview[320]; softkbd_build_preview(preview,sizeof(preview));
    const size_t plen=std::strlen(preview);
    const char *shown=preview;
    if (plen > 84) shown=preview + (plen-84);
    char line1[44]={}, line2[44]={};
    std::strncpy(line1,shown,42); line1[42]='\0';
    if (std::strlen(shown)>42) { std::strncpy(line2,shown+42,42); line2[42]='\0'; }
    M5.Display.setTextColor(TFT_WHITE,0x0000);
    M5.Display.setTextSize(2);
    M5.Display.drawString(line1,px+14,py+44);
    M5.Display.drawString(line2,px+14,py+78);
    char cnt[32]; std::snprintf(cnt,sizeof(cnt),"%u / %u keys",(unsigned)s_soft_input_count,(unsigned)kSoftInputMax);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xBDF7,0x0000);
    M5.Display.drawString(cnt,px+14,py+118);

    const int sx=948, sy=70, sw=154, sh=140;
    const uint16_t send_fill=s_soft_input_count ? 0x03EF : 0x18E3;
    M5.Display.fillRoundRect(sx,sy,sw,sh,8,send_fill);
    M5.Display.drawRoundRect(sx,sy,sw,sh,8,s_soft_input_count?0x07FF:0x5AEB);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextColor(TFT_WHITE,send_fill);
    M5.Display.setTextSize(2);
    M5.Display.drawString("SEND",sx+sw/2,sy+sh/2-8);
    M5.Display.setTextSize(1);
    M5.Display.drawString("to X68000",sx+sw/2,sy+sh/2+22);
    M5.Display.setTextDatum(textdatum_t::top_left);

    for (size_t r=0;r<sizeof(kSoftRows)/sizeof(kSoftRows[0]);++r) {
        const soft_row_t &row=kSoftRows[r];
        for (size_t i=0;i<row.count;++i) {
            int x,y,w,h; softkbd_key_rect(row,i,(int)r,&x,&y,&w,&h);
            const soft_key_t &k=row.keys[i];
            bool active=(k.kind==SOFTKEY_SHIFT && s_softkbd_shift) ||
                        (k.kind==SOFTKEY_CTRL && s_softkbd_ctrl);
            const uint16_t fill=active ? 0x03EF : 0x18E3;
            const uint16_t border=active ? 0x07FF : 0x5AEB;
            M5.Display.fillRoundRect(x,y,w,h,5,fill);
            M5.Display.drawRoundRect(x,y,w,h,5,border);
            M5.Display.setTextDatum(textdatum_t::middle_center);
            M5.Display.setTextSize((std::strlen(k.label)>=5)?1:2);
            M5.Display.setTextColor(TFT_WHITE,fill);
            M5.Display.drawString(k.label,x+w/2,y+h/2);
        }
    }
    M5.Display.setTextDatum(textdatum_t::top_left);
    s_softkbd_redraw=false;
}

static void softkbd_buffer_key(const soft_key_t &k)
{
    /* BS edits the local composition buffer instead of being sent to the guest. */
    if (k.scancode==0x0Fu) {
        if (s_soft_input_count) --s_soft_input_count;
        s_softkbd_redraw=true;
        return;
    }
    if (s_soft_input_count >= kSoftInputMax) return;
    soft_input_event_t &e=s_soft_input[s_soft_input_count++];
    e.scancode=k.scancode;
    e.modifiers=(uint8_t)((s_softkbd_shift?1u:0u) | (s_softkbd_ctrl?2u:0u));
    e.label=k.label;
    s_softkbd_redraw=true;
}

static bool softkbd_send_buffer(void)
{
    const size_t original=s_soft_input_count;
    size_t sent=0;
    while (sent < s_soft_input_count) {
        const soft_input_event_t &e=s_soft_input[sent];
        if (!tab5_guest_input_queue_x68k_tap(e.scancode,e.modifiers)) break;
        ++sent;
    }
    if (sent) {
        const size_t remain=s_soft_input_count-sent;
        if (remain) std::memmove(s_soft_input,s_soft_input+sent,remain*sizeof(s_soft_input[0]));
        s_soft_input_count=remain;
    }
    s_softkbd_redraw=true;
    return original != 0 && s_soft_input_count == 0;
}

static void softkbd_close_to_guest(void)
{
    s_softkbd_active=false;
    s_softkbd_redraw=false;
    s_softkbd_touch_down=true;
    s_game_touch_suppress_until_release=true;
    s_force_game_redraw=true;
    s_aspect_seen_frame=nullptr;
    s_softkbd_shift=false;
    s_softkbd_ctrl=false;
}

static void softkbd_click(int x,int y)
{
    /* The physical side KEY button is also the close button while overlay owns LCD. */
    if (point_in_rect(x,y,31,207,98,68)) {
        softkbd_close_to_guest();
        return;
    }
    if (point_in_rect(x,y,948,70,154,140)) {
        /* Build 6.12q: SEND is also the natural "return to source" action.
         * Only close once the whole local buffer has been accepted by the
         * dedicated guest-send queue; a full queue leaves the remainder on
         * screen instead of silently losing keys. */
        if (softkbd_send_buffer()) softkbd_close_to_guest();
        return;
    }
    for (size_t r=0;r<sizeof(kSoftRows)/sizeof(kSoftRows[0]);++r) {
        const soft_row_t &row=kSoftRows[r];
        for (size_t i=0;i<row.count;++i) {
            int kx,ky,kw,kh; softkbd_key_rect(row,i,(int)r,&kx,&ky,&kw,&kh);
            if (!point_in_rect(x,y,kx,ky,kw,kh)) continue;
            const soft_key_t &k=row.keys[i];
            if (k.kind==SOFTKEY_SHIFT) { s_softkbd_shift=!s_softkbd_shift; s_softkbd_redraw=true; return; }
            if (k.kind==SOFTKEY_CTRL)  { s_softkbd_ctrl=!s_softkbd_ctrl; s_softkbd_redraw=true; return; }
            softkbd_buffer_key(k);
            return;
        }
    }
}

static uint16_t s_game_touch_key_prev = 0;

/* Touch controller dual role requested for X68K Tab:
 * D-pad remains JOY1 while also acting as X68000 cursor keys;
 * B remains TRG1 while also SPACE; A remains TRG2 while also RETURN.
 * Use the final shaped touch-joy state, so cursor repeat follows the same
 * deliberate touch repeat cadence instead of flooding the keyboard buffer. */
static void update_game_touch_keyboard(uint16_t joy)
{
    const uint16_t mask=(uint16_t)(JOY_UP|JOY_DOWN|JOY_LEFT|JOY_RIGHT|JOY_TRG1|JOY_TRG2);
    const uint16_t now=(uint16_t)(joy & mask);
    const uint16_t changed=(uint16_t)(now ^ s_game_touch_key_prev);
    struct Map { uint16_t bit; uint8_t scan; };
    static const Map kMap[] = {
        {JOY_UP,0x3C},{JOY_DOWN,0x3E},{JOY_LEFT,0x3B},{JOY_RIGHT,0x3D},
        {JOY_TRG1,0x35}, /* B = SPACE */
        {JOY_TRG2,0x1D}, /* A = RETURN */
    };
    for (const auto &m:kMap) {
        if (changed & m.bit)
            (void)tab5_guest_input_queue_x68k_scancode(m.scan,(now & m.bit)?1:0);
    }
    s_game_touch_key_prev=now;
}

using runtime_media_entry_t = tab5_media_ui_entry_t;
static constexpr size_t kRuntimeMediaMax = 128;
static runtime_media_entry_t *s_runtime_media = nullptr;
static size_t s_runtime_media_count = 0;
static char s_runtime_fdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static char s_runtime_fdd1[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static char s_runtime_hdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static char s_media_pending_fdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static char s_media_pending_fdd1[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static char s_media_pending_hdd0[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
static portMUX_TYPE s_runtime_media_mux = portMUX_INITIALIZER_UNLOCKED;

static void runtime_media_scan(void)
{
    if (!s_runtime_media) {
        s_runtime_media = static_cast<runtime_media_entry_t *>(heap_caps_calloc(
            kRuntimeMediaMax, sizeof(runtime_media_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_runtime_media)
            s_runtime_media = static_cast<runtime_media_entry_t *>(std::calloc(kRuntimeMediaMax, sizeof(runtime_media_entry_t)));
    }
    s_runtime_media_count = s_runtime_media ?
        tab5_media_ui_scan_sd_root(s_runtime_media, kRuntimeMediaMax) : 0;
    ESP_LOGI(TAG, "Runtime FILE scan (shared catalog): %u media file(s)", (unsigned)s_runtime_media_count);
}

static tab5_media_ui_slot_t runtime_ui_slot(void)
{
    return s_media_overlay_drive == 0 ? TAB5_MEDIA_UI_FDD0 :
           (s_media_overlay_drive == 1 ? TAB5_MEDIA_UI_FDD1 : TAB5_MEDIA_UI_HDD0);
}

static bool queue_ui_action(tab5_video_action_type_t type, const char *path)
{
    if (!s_ui_actions) return false;
    tab5_video_action_t a = {};
    a.type = type;
    if (path) std::snprintf(a.path, sizeof(a.path), "%s", path);
    return xQueueSend(s_ui_actions, &a, 0) == pdTRUE;
}

static void snapshot_runtime_media_to_pending(void)
{
    portENTER_CRITICAL(&s_runtime_media_mux);
    std::snprintf(s_media_pending_fdd0, sizeof(s_media_pending_fdd0), "%s", s_runtime_fdd0);
    std::snprintf(s_media_pending_fdd1, sizeof(s_media_pending_fdd1), "%s", s_runtime_fdd1);
    std::snprintf(s_media_pending_hdd0, sizeof(s_media_pending_hdd0), "%s", s_runtime_hdd0);
    s_media_overlay_boot_source = s_runtime_boot_source;
    portEXIT_CRITICAL(&s_runtime_media_mux);
}

static void close_media_overlay(void)
{
    s_media_overlay_active = false;
    s_media_overlay_redraw = false;
    s_media_overlay_touch_down = false;
    s_media_overlay_browser = false;
    s_utility_prev_mask = 0;
    /* Do not drop s_present_started here. Doing so reopened the runtime debug
     * status-banner path before the next game frame. Force a clean game redraw
     * separately while keeping status text suppressed. */
    s_force_game_redraw = true;
    s_aspect_seen_frame = nullptr;
    /* Never reinterpret the BACK press as a game-side control after close. */
    s_game_touch_suppress_until_release = true;
}

static void draw_runtime_media_setup_unlocked(void)
{
    const bool can_boot = s_media_overlay_boot_source == TAB5_LAUNCH_BOOT_HDD0 ?
                          s_media_pending_hdd0[0] != '\0' : s_media_pending_fdd0[0] != '\0';
    tab5_media_ui_setup_view_t view = {};
    view.back_label = nullptr;
    view.header_note = "FDD0 / FDD1 / HDD0 media changes apply immediately";
    view.floppy0 = s_media_pending_fdd0;
    view.floppy1 = s_media_pending_fdd1;
    view.hdd0 = s_media_pending_hdd0;
    view.boot_source = s_media_overlay_boot_source;
    view.bottom_left_label = "BACK";
    view.bottom_left_note = nullptr;
    view.bottom_left_enabled = 1;
    view.bottom_right_label = "(re)BOOT";
    view.bottom_right_note = nullptr;
    view.bottom_right_enabled = can_boot ? 1 : 0;
    view.footer_note = nullptr;
    tab5_media_ui_draw_setup(&view);
}

static void draw_runtime_media_browser_unlocked(void)
{
    const tab5_media_ui_slot_t slot = runtime_ui_slot();
    const size_t pages = tab5_media_ui_browser_pages(s_runtime_media, s_runtime_media_count, slot);
    if (s_media_overlay_page >= pages) s_media_overlay_page = pages - 1;
    tab5_media_ui_draw_browser(s_runtime_media, s_runtime_media_count, slot, s_media_overlay_page);
}

static void draw_media_overlay_unlocked(void)
{
    if (!s_media_overlay_active) return;
    if (s_media_overlay_browser) draw_runtime_media_browser_unlocked();
    else draw_runtime_media_setup_unlocked();
    s_media_overlay_redraw = false;
}

static char *pending_path_for_drive(int drive)
{
    return drive == 0 ? s_media_pending_fdd0 : (drive == 1 ? s_media_pending_fdd1 : s_media_pending_hdd0);
}

static bool queue_runtime_slot_change(int drive, const char *path)
{
    tab5_video_action_type_t type;
    if (path && path[0]) {
        type = drive == 0 ? TAB5_VIDEO_ACTION_MOUNT_FDD0 :
               (drive == 1 ? TAB5_VIDEO_ACTION_MOUNT_FDD1 : TAB5_VIDEO_ACTION_MOUNT_HDD0);
    } else {
        type = drive == 0 ? TAB5_VIDEO_ACTION_EJECT_FDD0 :
               (drive == 1 ? TAB5_VIDEO_ACTION_EJECT_FDD1 : TAB5_VIDEO_ACTION_EJECT_HDD0);
    }
    return queue_ui_action(type, path && path[0] ? path : nullptr);
}

static bool apply_runtime_media_selection(int drive, const char *path)
{
    if (drive < 0 || drive > 2) return false;
    const char *requested = path ? path : "";

    char current[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
    portENTER_CRITICAL(&s_runtime_media_mux);
    const char *runtime_ro = drive == 0 ? s_runtime_fdd0 :
                             (drive == 1 ? s_runtime_fdd1 : s_runtime_hdd0);
    std::snprintf(current, sizeof(current), "%s", runtime_ro);
    portEXIT_CRITICAL(&s_runtime_media_mux);
    if (!std::strcmp(current, requested)) {
        ESP_LOGI(TAG, "Runtime Media Setup immediate change: drive=%d unchanged (%s)",
                 drive, requested[0] ? requested : "<empty>");
        return true;
    }

    if (!queue_runtime_slot_change(drive, requested)) {
        ESP_LOGW(TAG, "Runtime Media Setup: immediate drive=%d change queue full", drive);
        return false;
    }

    /* The action is consumed by CPU1 at the next guest frame boundary. Mirror
     * the requested path into the UI state now; main.c publishes the actual
     * mounted path back after the action, including any mount failure. */
    portENTER_CRITICAL(&s_runtime_media_mux);
    char *runtime = drive == 0 ? s_runtime_fdd0 : (drive == 1 ? s_runtime_fdd1 : s_runtime_hdd0);
    std::snprintf(runtime, TAB5_VIDEO_MEDIA_PATH_MAX, "%s", requested);
    portEXIT_CRITICAL(&s_runtime_media_mux);

    ESP_LOGI(TAG, "Runtime Media Setup immediate change: drive=%d media=%s",
             drive, requested[0] ? requested : "<empty>");
    return true;
}

static bool queue_runtime_media_guest_reboot(void)
{
    const bool hdd_boot = s_media_overlay_boot_source == TAB5_LAUNCH_BOOT_HDD0;
    if (hdd_boot) {
        if (!s_media_pending_hdd0[0]) return false;
    } else {
        if (!s_media_pending_fdd0[0]) return false;
    }

    /* Build 6.12u: the running CPU1 task already owns the authoritative media
     * paths because CHANGE applies immediately. Queue only the desired boot
     * source; CPU1 performs a guest-only reset at the next frame boundary. */
    return queue_ui_action(TAB5_VIDEO_ACTION_REBOOT_GUEST, hdd_boot ? "HDD0" : "FDD0");
}

static void media_overlay_click(int x, int y)
{
    if (!s_media_overlay_browser) {
        switch (tab5_media_ui_setup_hit(x, y)) {
            case TAB5_MEDIA_UI_SETUP_BACK:
                /* Setup-level top-left BACK was removed in 6.12j. */
                return;
            case TAB5_MEDIA_UI_SETUP_BOOT_FDD0:
                s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                s_media_overlay_redraw = true;
                return;
            case TAB5_MEDIA_UI_SETUP_BOOT_HDD0:
                if (s_media_pending_hdd0[0]) s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_HDD0;
                s_media_overlay_redraw = true;
                return;
            case TAB5_MEDIA_UI_SETUP_CHANGE_FDD0:
                s_media_overlay_drive = 0; s_media_overlay_page = 0; s_media_overlay_browser = true; s_media_overlay_redraw = true; return;
            case TAB5_MEDIA_UI_SETUP_CHANGE_FDD1:
                s_media_overlay_drive = 1; s_media_overlay_page = 0; s_media_overlay_browser = true; s_media_overlay_redraw = true; return;
            case TAB5_MEDIA_UI_SETUP_CHANGE_HDD0:
                s_media_overlay_drive = 2; s_media_overlay_page = 0; s_media_overlay_browser = true; s_media_overlay_redraw = true; return;
            case TAB5_MEDIA_UI_SETUP_BOTTOM_LEFT:
                ESP_LOGI(TAG, "Runtime Media Setup: BACK to game");
                close_media_overlay();
                return;
            case TAB5_MEDIA_UI_SETUP_BOTTOM_RIGHT:
                if (queue_runtime_media_guest_reboot()) {
                    ESP_LOGI(TAG, "Runtime Media Setup: (re)BOOT guest reset queued (host stays alive)");
                    close_media_overlay();
                } else {
                    ESP_LOGW(TAG, "Runtime Media Setup: (re)BOOT rejected; select valid boot media");
                    s_media_overlay_redraw = true;
                }
                return;
            default:
                return;
        }
    }

    const tab5_media_ui_slot_t slot = runtime_ui_slot();
    size_t selected = 0;
    int empty = 0;
    switch (tab5_media_ui_browser_hit(s_runtime_media, s_runtime_media_count, slot,
                                      s_media_overlay_page, x, y, &selected, &empty)) {
        case TAB5_MEDIA_UI_BROWSER_BACK:
            s_media_overlay_browser = false;
            s_media_overlay_redraw = true;
            return;
        case TAB5_MEDIA_UI_BROWSER_SELECT: {
            char selected_path[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
            if (!empty) {
                const runtime_media_entry_t *e = tab5_media_ui_filtered_at(
                    s_runtime_media, s_runtime_media_count, slot, selected);
                if (!e) {
                    ESP_LOGW(TAG, "Runtime Media Setup: selected entry disappeared; rescanning");
                    runtime_media_scan();
                    s_media_overlay_redraw = true;
                    return;
                }
                std::snprintf(selected_path, sizeof(selected_path), "%s", e->path);
            }
            if (!apply_runtime_media_selection(s_media_overlay_drive, selected_path)) {
                s_media_overlay_redraw = true;
                return;
            }

            char *dst = pending_path_for_drive(s_media_overlay_drive);
            std::snprintf(dst, TAB5_VIDEO_MEDIA_PATH_MAX, "%s", selected_path);
            if (s_media_overlay_drive == 0 && dst[0]) s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
            if (s_media_overlay_drive == 2 && dst[0]) s_media_overlay_boot_source = TAB5_LAUNCH_BOOT_HDD0;
            ESP_LOGI(TAG, "Runtime Media Setup selected/applied: drive=%d media=%s",
                     s_media_overlay_drive, dst[0] ? dst : "<empty>");
            s_media_overlay_browser = false;
            s_media_overlay_redraw = true;
            return;
        }
        case TAB5_MEDIA_UI_BROWSER_PREV:
            if (s_media_overlay_page > 0) --s_media_overlay_page;
            s_media_overlay_redraw = true;
            return;
        case TAB5_MEDIA_UI_BROWSER_NEXT:
            ++s_media_overlay_page;
            s_media_overlay_redraw = true;
            return;
        default:
            return;
    }
}

static uint16_t poll_game_controls(void)
{
    if (!s_game_controls_enabled) return 0;
    const int w=M5.Display.width(), h=M5.Display.height();
    if (w<1200 || h<700) return 0;

    M5.update();
    const size_t count=M5.Touch.getCount();

    if (s_game_touch_suppress_until_release) {
        bool live=false;
        for (size_t i=0; i<count; ++i) {
            const auto t=M5.Touch.getDetail(i);
            if (t.isPressed()) { live=true; break; }
        }
        if (live) return 0;
        s_game_touch_suppress_until_release=false;
        s_utility_prev_mask=0;
        s_touch_dpad_prev=0;
        s_touch_dpad_press_us=0;
    }

    if (s_media_overlay_active) {
        bool have_pressed_contact=false;
        for (size_t i=0;i<count;++i) {
            const auto t=M5.Touch.getDetail(i);
            /* Only the touch-state bit means a finger is currently down.
             * wasReleased() is edge-only and leaves other non-pressed detail
             * states possible, so using !wasReleased() here can re-fire one
             * physical tap as a second click on the following sample. */
            if (!t.isPressed()) continue;
            have_pressed_contact=true;
            if (!s_media_overlay_touch_down) {
                s_media_overlay_touch_down=true;
                media_overlay_click((int)t.x,(int)t.y);
            }
            break;
        }
        if (!have_pressed_contact) s_media_overlay_touch_down=false;
        return 0;
    }

    if (s_softkbd_active) {
        bool have_pressed_contact=false;
        for (size_t i=0;i<count;++i) {
            const auto t=M5.Touch.getDetail(i);
            if (!t.isPressed()) continue;
            have_pressed_contact=true;
            if (!s_softkbd_touch_down) {
                s_softkbd_touch_down=true;
                softkbd_click((int)t.x,(int)t.y);
            }
            break;
        }
        if (!have_pressed_contact) s_softkbd_touch_down=false;
        return 0;
    }

    uint16_t joy=0;
    uint32_t util=0;
    bool have_pressed_contact=false;
    for (size_t i=0;i<count;++i) {
        const auto t=M5.Touch.getDetail(i);
        /* Build 6.12p: only isPressed() identifies a live finger.  The older
         * !wasReleased() test accepted non-pressed/no-change detail records
         * and could turn one physical tap into two UI actions. */
        if (!t.isPressed()) continue;
        have_pressed_contact=true;
        const int x=(int)t.x, y=(int)t.y;
        const game_touch_map_t mapped = map_game_touch_point(x, y, w);
        joy |= mapped.joy;
        util |= mapped.util;
    }

    /* No live contact means an unconditional release.  In particular, do not
     * carry a D-pad state across a M5Unified release detail frame. */
    if (!have_pressed_contact) { joy=0; util=0; }

    /* Touch D-pad repeat limiter.  A/B stay continuously held like a real
     * joystick, and USB JOY is untouched.  For directions: send 90 ms
     * immediately, pause until 330 ms, then repeat at 6.25 Hz with a 70 ms
     * ON pulse. This preserves taps while taming runaway menu navigation. */
    const uint16_t dpad_mask=(uint16_t)(JOY_UP|JOY_DOWN|JOY_LEFT|JOY_RIGHT);
    const uint16_t raw_dpad=(uint16_t)(joy & dpad_mask);
    joy=(uint16_t)(joy & ~dpad_mask);
    if (!raw_dpad) {
        s_touch_dpad_prev=0;
        s_touch_dpad_press_us=0;
    } else {
        const int64_t now_us=esp_timer_get_time();
        if (raw_dpad != s_touch_dpad_prev || !s_touch_dpad_press_us) {
            s_touch_dpad_prev=raw_dpad;
            s_touch_dpad_press_us=now_us;
        }
        const int64_t held_us=now_us-s_touch_dpad_press_us;
        bool gate=false;
        if (held_us < 90000) gate=true;
        else if (held_us >= 330000) {
            const int64_t phase=(held_us-330000)%160000;
            gate=phase < 70000;
        }
        if (gate) joy|=raw_dpad;
    }

    /* Apply the keyboard aliases after D-pad repeat shaping. */
    update_game_touch_keyboard(joy);

    const uint32_t rising=util & ~s_utility_prev_mask;
    s_utility_prev_mask=util;
    /* Build 6.12j: retain the proven 6.12h coarse volume ladder and touch
     * only M5Unified's master-volume value; never restart/reconfigure the
     * ES8388/I2S stream while a game is running. */
    if (rising & UTIL_VOLP) (void)tab5_audio_step_volume(+1);
    if (rising & UTIL_VOLM) (void)tab5_audio_step_volume(-1);
    if (rising & UTIL_KEYBOARD) {
        s_softkbd_active=true;
        s_softkbd_redraw=true;
        s_softkbd_touch_down=true; /* opening finger must be released first */
        s_softkbd_shift=false;
        s_softkbd_ctrl=false;
        s_soft_input_count=0;
        update_game_touch_keyboard(0);
        joy=0;
        (void)tab5_guest_input_queue_touch_joypad(0);
    }
    if (rising & UTIL_PANIC) {
        /* Build 6.12s: never reboot the ESP32-P4 for the in-game PANIC button.
         * Select and stage the random PAN entirely on the host side, then pass
         * the concrete path to CPU1. CPU1 will reset only the emulated X68000
         * into Flash Human68k, preserving the already-running Tab5 audio host,
         * ES8388 and all ESP-IDF peripheral state. */
        char pan_path[TAB5_VIDEO_MEDIA_PATH_MAX] = {};
        tab5_video_status("PANIC", "Preparing random PAN...");
        if (tab5_panic_prepare_random_runtime(pan_path, sizeof(pan_path))) {
            if (!queue_ui_action(TAB5_VIDEO_ACTION_PANIC_RANDOM, pan_path))
                ESP_LOGW(TAG, "In-game PANIC: action queue full after staging %s", pan_path);
        } else {
            ESP_LOGW(TAG, "In-game PANIC: no usable user .PAN file found");
        }
    }
    if (rising & UTIL_FILE) {
        runtime_media_scan();
        snapshot_runtime_media_to_pending();
        s_media_overlay_drive=0;
        s_media_overlay_page=0;
        s_media_overlay_browser=false;
        s_media_overlay_active=true;
        s_media_overlay_redraw=true;
        s_media_overlay_touch_down=true; /* require release before first menu click */
        joy=0;
        (void)tab5_guest_input_queue_touch_joypad(0);
        ESP_LOGI(TAG,"Runtime FILE opened shared Media Setup: per-slot CHANGE applies immediately; BACK / (re)BOOT");
    }
    return joy;
}

/*
 * PANIC compatibility renderer
 * ----------------------------
 * The original standalone PanicPlayer used a direct reconstruction of the
 * X68000 packed 512-KiB GVRAM.  PANIC data such as PELSIA01 relies on the
 * 512-dot/256-colour two-page layout, while N_OHA also uses the sprite PCG
 * engine.  The normal PX68K ScrBuf renderer is retained for every non-PANIC
 * path; only PANIC mode takes this CPU0 compatibility presenter.
 * Layer8 Aug/17/2026
 */
static inline uint8_t panic_gvram_pixel_16(int page, int x, int y)
{
    page &= 3;
    const uint32_t sx = ((uint32_t)x + GrphScrollX[page]) & 0x1ffu;
    const uint32_t sy = ((uint32_t)y + GrphScrollY[page]) & 0x1ffu;
    const uint32_t off = (sy << 10) + (sx << 1) + (uint32_t)(page >> 1);
    const uint8_t raw = GVRAM[off];
    return (page & 1) ? (uint8_t)(raw >> 4) : (uint8_t)(raw & 0x0f);
}

static inline uint16_t panic_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint8_t panic_sprite_pixel(uint8_t pattern, int x, int y)
{
    const uint32_t raw = (uint32_t)pattern * 0x80u +
                         (uint32_t)(y & 15) * 4u +
                         (uint32_t)((x >> 1) & 3) +
                         ((x & 8) ? 0x40u : 0u);
    const uint8_t b = BG[raw & 0x7fffu];
    return (x & 1) ? (uint8_t)(b & 0x0f) : (uint8_t)(b >> 4);
}

static void panic_render_sprites(uint32_t draw_w, uint32_t draw_h)
{
    if (!s_panic_compat_frame) return;
    const int hstart = ((int)CRTC_Regs[0x04] << 8) | CRTC_Regs[0x05];
    const int vstart = ((int)CRTC_Regs[0x0c] << 8) | CRTC_Regs[0x0d];
    const int h_adjust = ((int)BG_Regs[0x0d] - (hstart + 4)) * 8;
    const int v_div = (BG_Regs[0x11] & 4u) ? 1 : 2;
    const int v_adjust = ((int)BG_Regs[0x0f] - vstart) / v_div;

    for (int pri = 1; pri <= 3; ++pri) {
        for (int n = 127; n >= 0; --n) {
            const uint8_t *sp = Sprite_Regs + (size_t)n * 8u;
            if ((sp[6] & 3u) != (uint8_t)pri) continue;
            const uint16_t posx = panic_le16(sp + 0) & 0x03ffu;
            const uint16_t posy = panic_le16(sp + 2) & 0x03ffu;
            const uint16_t ctrl = panic_le16(sp + 4);
            const int sx = (int)((posx + h_adjust) & 0x03ffu) - 16;
            const int sy = (int)posy + v_adjust - 16;
            if (sx >= (int)draw_w || sy >= (int)draw_h || sx + 16 <= 0 || sy + 16 <= 0) continue;

            const uint8_t pattern = (uint8_t)ctrl;
            const uint8_t palbase = (uint8_t)((ctrl >> 4) & 0xf0u);
            const bool flip_x = (ctrl & 0x4000u) != 0;
            const bool flip_y = (ctrl & 0x8000u) != 0;
            for (int py = 0; py < 16; ++py) {
                const int dy = sy + py;
                if ((unsigned)dy >= draw_h) continue;
                const int src_y = flip_y ? (15 - py) : py;
                uint16_t *dst = s_panic_compat_frame + (size_t)dy * kPanicCompatPitch;
                for (int px = 0; px < 16; ++px) {
                    const int dx = sx + px;
                    if ((unsigned)dx >= draw_w) continue;
                    const int src_x = flip_x ? (15 - px) : px;
                    const uint8_t pix = panic_sprite_pixel(pattern, src_x, src_y);
                    if (pix) dst[dx] = TextPal[(uint8_t)(palbase | pix)];
                }
            }
        }
    }
}

static bool panic_render_compat_source(const present_request_t &req, present_request_t *out)
{
    if (!s_panic_compat_enabled || !s_panic_compat_frame || !out) return false;
    uint32_t draw_w = req.width;
    uint32_t draw_h = req.height;
    if (!draw_w || !draw_h) return false;
    if (draw_w > 512u) draw_w = 512u;
    if (draw_h > 512u) draw_h = 512u;

    for (uint32_t y = 0; y < draw_h; ++y)
        std::memset(s_panic_compat_frame + (size_t)y * kPanicCompatPitch, 0, draw_w * sizeof(uint16_t));

    const uint8_t mode = (uint8_t)(VCReg0[1] & 3u);
    const uint8_t enable = VCReg2[1];
    bool graphics_supported = false;

    if ((mode == 1u || mode == 2u) && (enable & 0x0fu)) {
        uint32_t gx[4], gy[4];
        for (int i=0; i<4; ++i) { gx[i] = GrphScrollX[i] & 0x1ffu; gy[i] = GrphScrollY[i] & 0x1ffu; }
        const uint8_t pri = VCReg1[1];
        const bool p0_on = (enable & 0x01u) != 0;
        const bool p1_on = (enable & 0x04u) != 0;
        const bool p0_top = (pri & 3u) <= ((pri >> 4) & 3u);
        const bool p0_same_scroll = gx[0] == gx[1] && gy[0] == gy[1];
        const bool p1_same_scroll = gx[2] == gx[3] && gy[2] == gy[3];
        for (uint32_t y=0; y<draw_h; ++y) {
            uint16_t *dst = s_panic_compat_frame + (size_t)y * kPanicCompatPitch;
            const uint32_t row0 = ((y + gy[0]) & 0x1ffu) << 10;
            const uint32_t row1 = ((y + gy[1]) & 0x1ffu) << 10;
            const uint32_t row2 = ((y + gy[2]) & 0x1ffu) << 10;
            const uint32_t row3 = ((y + gy[3]) & 0x1ffu) << 10;
            for (uint32_t x=0; x<draw_w; ++x) {
                uint8_t p0=0, p1=0;
                if (p0_on) {
                    const uint32_t x0 = ((x + gx[0]) & 0x1ffu) << 1;
                    if (p0_same_scroll) p0 = GVRAM[row0 + x0];
                    else {
                        const uint32_t x1 = ((x + gx[1]) & 0x1ffu) << 1;
                        p0 = (uint8_t)((GVRAM[row0+x0] & 0x0fu) | (GVRAM[row1+x1] & 0xf0u));
                    }
                }
                uint8_t idx=0;
                if (p0_top && p0_on && p0) idx=p0;
                else {
                    if (p1_on) {
                        const uint32_t x2 = ((x + gx[2]) & 0x1ffu) << 1;
                        if (p1_same_scroll) p1 = GVRAM[row2 + x2 + 1u];
                        else {
                            const uint32_t x3 = ((x + gx[3]) & 0x1ffu) << 1;
                            p1 = (uint8_t)((GVRAM[row2+x2+1u] & 0x0fu) | (GVRAM[row3+x3+1u] & 0xf0u));
                        }
                    }
                    if (p0_top) idx = p0_on ? (p0 ? p0 : p1) : p1;
                    else idx = p1_on ? (p1 ? p1 : p0) : p0;
                }
                dst[x] = GrphPal[idx];
            }
        }
        graphics_supported = true;
    } else if (mode == 3u && (enable & 0x0fu)) {
        /* Build 6.12c: 65536-colour PANIC data must not fall through to the
         * sprite-only path.  Reproduce Grp_DrawLine16() byte-for-byte: one
         * 16-bit GVRAM pixel is converted through Pal16Adr/Pal_Regs/Pal16.
         * Keeping this on CPU0 avoids touching the proven normal WinDraw path. */
        const uint32_t gx = GrphScrollX[0] & 0x1ffu;
        for (uint32_t y=0; y<draw_h; ++y) {
            uint32_t sy = GrphScrollY[0] + y;
            if ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu) sy += y;
            sy &= 0x1ffu;
            uint16_t *dst = s_panic_compat_frame + (size_t)y * kPanicCompatPitch;
            for (uint32_t x=0; x<draw_w; ++x) {
                const uint32_t sx = (gx + x) & 0x1ffu;
                const uint32_t off = (sy << 10) + (sx << 1);
                const uint8_t lo = GVRAM[off];
                const uint8_t hi = GVRAM[off + 1u];
                if ((lo | hi) == 0u) {
                    dst[x] = 0;
                } else {
                    uint16_t pal = Pal_Regs[Pal16Adr[lo]];
                    pal |= (uint16_t)((uint16_t)Pal_Regs[Pal16Adr[hi] + 2u] << 8);
                    dst[x] = Pal16[pal];
                }
            }
        }
        graphics_supported = true;
    } else if (mode == 0u && !(VCReg0[1] & 0x04u) && (enable & 0x0fu)) {
        const uint8_t pri = VCReg1[1];
        for (uint32_t y=0; y<draw_h; ++y) {
            uint16_t *dst = s_panic_compat_frame + (size_t)y * kPanicCompatPitch;
            for (uint32_t x=0; x<draw_w; ++x) {
                uint8_t idx=0; bool have=false;
                for (int slot=3; slot>=0; --slot) {
                    if ((enable & (1u << slot)) == 0) continue;
                    const int page=(pri >> (slot*2)) & 3;
                    const uint8_t v=panic_gvram_pixel_16(page,(int)x,(int)y);
                    if (!have) { idx=v; have=true; } else if (v) idx=v;
                }
                dst[x]=GrphPal[idx];
            }
        }
        graphics_supported = true;
    }

    /* Sprite PCG is independent of the graphics-page mode and is required by
     * PANIC data such as N_OHA. Transparent sprite pixels leave graphics intact. */
    panic_render_sprites(draw_w, draw_h);

    if (s_panic_compat_last_w != draw_w || s_panic_compat_last_h != draw_h ||
        s_panic_compat_last_mode != mode || s_panic_compat_last_enable != enable) {
        ESP_LOGI(TAG,
                 "PANIC compat renderer: raw=%lux%lu VC0=%02X mode=%u VC1=%02X VC2=%02X graphics=%s",
                 (unsigned long)draw_w, (unsigned long)draw_h, (unsigned)VCReg0[1],
                 (unsigned)mode, (unsigned)VCReg1[1], (unsigned)enable,
                 graphics_supported ? "DIRECT-GVRAM" : "SPRITE/FALLBACK");
        s_panic_compat_last_w=draw_w; s_panic_compat_last_h=draw_h;
        s_panic_compat_last_mode=mode; s_panic_compat_last_enable=enable;
    }

    *out = req;
    out->frame = s_panic_compat_frame;
    out->width = draw_w;
    out->height = draw_h;
    out->pitch_pixels = kPanicCompatPitch;
    return graphics_supported || (enable & 0x10u) != 0u;
}

static inline void display_lock(void)
{
    if (s_display_mutex)
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
}

static inline void display_unlock(void)
{
    if (s_display_mutex)
        xSemaphoreGive(s_display_mutex);
}

static inline uint32_t line_writer_count(uint32_t y)
{
    if (y >= kTrackedLines) return 0;
    return __atomic_load_n(&s_line_writers[y], __ATOMIC_ACQUIRE);
}

static inline uint32_t line_generation(uint32_t y)
{
    if (y >= kTrackedLines) return 0;
    return __atomic_load_n(&s_line_generation[y], __ATOMIC_ACQUIRE);
}

static void push_snapshot(const present_request_t &req)
{
    if (req.slot_index >= kPresentSlots)
        return;

    present_slot_t &slot = s_slots[req.slot_index];
    const int lcd_w = M5.Display.width();
    const int lcd_h = M5.Display.height();
    const int x = (lcd_w - (int)slot.width) / 2;
    const int y = (lcd_h - (int)slot.height) / 2;

    M5.Display.pushImage(x, y, (int)slot.width, (int)slot.height, slot.pixels);
}

static void push_live_frame(const present_request_t &req,
                            uint32_t *retry_count,
                            uint32_t *unstable_count,
                            bool force_source_scan = false)
{
    const int lcd_w = M5.Display.width();
    const int lcd_h = M5.Display.height();

    /* Normal games keep the fixed 4:3 960x720 viewport so the agreed side
     * controls always have 160px on both sides.  PANIC's old standalone
     * renderer, however, intentionally preserved the source raster aspect
     * (notably 256x256).  Keep that behaviour in PANIC mode so square data is
     * not stretched into 4:3.  Layer8 Aug/17/2026 */
    uint32_t out_w = kAspectViewportWidth;
    uint32_t out_h = kAspectViewportHeight;
    if (s_panic_compat_enabled && req.width && req.height)
    {
        out_w = kAspectViewportWidth;
        out_h = (uint32_t)(((uint64_t)out_w * req.height) / req.width);
        if (out_h > kAspectViewportHeight)
        {
            out_h = kAspectViewportHeight;
            out_w = (uint32_t)(((uint64_t)out_h * req.width) / req.height);
        }
        if (!out_w) out_w = 1;
        if (!out_h) out_h = 1;
    }
    if (!out_w || !out_h || !s_aspect_frame)
        return;

    const int x = (lcd_w - (int)out_w) / 2;
    const int y0 = (lcd_h - (int)out_h) / 2;
    uint32_t retries_total = 0;
    uint32_t unstable_total = 0;

    /* Horizontal nearest-neighbour map is rebuilt only when CRTC geometry
     * changes.  This avoids integer division in the per-pixel hot loop. */
    if (s_aspect_map_src_w != req.width || s_aspect_map_dst_w != out_w)
    {
        for (uint32_t dx = 0; dx < out_w; ++dx)
        {
            uint32_t sx = (uint32_t)(((uint64_t)dx * (uint64_t)req.width) / (uint64_t)out_w);
            if (sx >= req.width) sx = req.width - 1u;
            s_aspect_xmap[dx] = (uint16_t)sx;
        }
        s_aspect_map_src_w = req.width;
        s_aspect_map_dst_w = out_w;
    }

    if (s_aspect_map_src_h != req.height || s_aspect_map_dst_h != out_h)
    {
        for (uint32_t dy = 0; dy < out_h; ++dy)
        {
            uint32_t sy = (uint32_t)(((uint64_t)dy * (uint64_t)req.height) / (uint64_t)out_h);
            if (sy >= req.height) sy = req.height - 1u;
            s_aspect_ymap[dy] = (uint16_t)sy;
        }
        s_aspect_map_src_h = req.height;
        s_aspect_map_dst_h = out_h;
    }

    const bool geometry_changed =
        s_aspect_seen_frame != req.frame ||
        s_aspect_seen_src_w != req.width ||
        s_aspect_seen_src_h != req.height ||
        s_aspect_seen_pitch != req.pitch_pixels;
    const bool force_full = geometry_changed || !s_aspect_dirty_frames;

    if (s_aspect_log_src_w != req.width || s_aspect_log_src_h != req.height)
    {
        ESP_LOGI(TAG,
                 "Build 5.98a 4:3 DIRTY frontend: raw=%lux%lu pitch=%lu -> LCD=%lux%lu viewport=%lux%lu; geometry change=full, steady-state=dirty bands",
                 (unsigned long)req.width, (unsigned long)req.height,
                 (unsigned long)req.pitch_pixels,
                 (unsigned long)lcd_w, (unsigned long)lcd_h,
                 (unsigned long)out_w, (unsigned long)out_h);
        s_aspect_log_src_w = req.width;
        s_aspect_log_src_h = req.height;
    }

    if (geometry_changed)
    {
        s_aspect_seen_frame = req.frame;
        s_aspect_seen_src_w = req.width;
        s_aspect_seen_src_h = req.height;
        s_aspect_seen_pitch = req.pitch_pixels;
        std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
    }

    uint32_t band_start = 0xffffffffu;
    uint32_t band_end = 0;
    uint32_t clean_gap_rows = 0;
    uint32_t frame_dirty_rows = 0;
    uint32_t frame_dirty_bands = 0;

    auto flush_band = [&](uint32_t start_row, uint32_t end_row) {
        if (start_row == 0xffffffffu || end_row <= start_row)
            return;
        M5.Display.pushImage(x, y0 + (int)start_row,
                             (int)out_w, (int)(end_row - start_row),
                             s_aspect_frame + (size_t)start_row * out_w);
        ++frame_dirty_bands;
        frame_dirty_rows += end_row - start_row;
    };

    /* A source row may be generation-dirty yet pixel-identical.  Treat that
     * case exactly like a clean generation row for LCD band coalescing. */
    auto note_clean_group = [&](uint32_t clean_start, uint32_t clean_end) {
        if (band_start != 0xffffffffu)
        {
            clean_gap_rows += clean_end - clean_start;
            if (clean_gap_rows > kDirtyMergeGapRows)
            {
                flush_band(band_start, band_end);
                band_start = 0xffffffffu;
                band_end = 0;
                clean_gap_rows = 0;
            }
        }
    };

    /* Walk destination rows in groups that reference the same source row.
     * For a 256-line source on a 720-line viewport this scales each source
     * row only once, then memcpy-replicates it into its 2-3 destination rows. */
    uint32_t dy = 0;
    while (dy < out_h)
    {
        const uint32_t sy = s_aspect_ymap[dy];

        uint32_t dy_end = dy + 1u;
        while (dy_end < out_h && s_aspect_ymap[dy_end] == sy)
            ++dy_end;

        const uint32_t observed_gen = line_generation(sy);
        /* PANIC compat pixels are rebuilt on CPU0 and do not carry WinDraw's
         * source-line generations.  Scan those rows every compat frame, but
         * keep the previous scaled LCD image so the exact PIE pixel diff can
         * suppress unchanged rows.  Do NOT turn the whole viewport into a
         * full refresh merely because WinDraw metadata is unavailable. */
        const bool source_dirty = force_full || force_source_scan ||
                                  line_writer_count(sy) != 0u ||
                                  s_aspect_seen_generation[sy] != observed_gen;

        if (!source_dirty)
        {
            note_clean_group(dy, dy_end);
            dy = dy_end;
            continue;
        }

        uint16_t *dst0 = s_aspect_frame + (size_t)dy * out_w;
        uint16_t *render0 = s_aspect_line_scratch ? s_aspect_line_scratch : dst0;
        const uint16_t *src = req.frame + (size_t)sy * req.pitch_pixels;
        uint32_t attempt = 0;
        uint32_t stable_gen = observed_gen;
        bool stable = false;

        if (force_source_scan)
        {
            /* CPU0 just finished building the PANIC compat framebuffer, so
             * unlike WinDraw there is no concurrent writer to race here. */
            for (uint32_t dx = 0; dx < out_w; ++dx)
                render0[dx] = src[s_aspect_xmap[dx]];
            stable = true;
            stable_gen = observed_gen;
        }
        else
        {
            while (attempt < kLiveRowRetryLimit)
            {
                if (line_writer_count(sy) != 0)
                {
                    ++attempt;
                    ++retries_total;
                    taskYIELD();
                    continue;
                }

                const uint32_t gen0 = line_generation(sy);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                for (uint32_t dx = 0; dx < out_w; ++dx)
                    render0[dx] = src[s_aspect_xmap[dx]];
                __atomic_thread_fence(__ATOMIC_ACQUIRE);

                const uint32_t gen1 = line_generation(sy);
                const uint32_t writers1 = line_writer_count(sy);
                if (writers1 == 0 && gen0 == gen1)
                {
                    stable = true;
                    stable_gen = gen1;
                    break;
                }

                ++attempt;
                ++retries_total;
            }
        }

        bool pixel_changed = true;
        if (!stable)
        {
            ++unstable_total;
            /* Keep the old generation so this row is retried next frame.  The
             * previous implementation still displayed the last attempted row;
             * preserve that behavior when using the scratch line. */
            if (render0 != dst0)
                tab5_pie_graphics_copy(dst0, render0, out_w * sizeof(uint16_t));
        }
        else
        {
            s_aspect_seen_generation[sy] = stable_gen;

            /* Build 5.98g8: generation dirtiness is conservative.  Compare the
             * newly scaled row against the last row actually presented.  This
             * is the high-leverage point: if the pixels are identical we avoid
             * both cache replication and the much more expensive LCD push. */
            if (!force_full && render0 != dst0)
            {
                ++s_vdiff_tested_src_rows;
                pixel_changed = tab5_pie_graphics_diff(
                    render0, dst0, out_w * sizeof(uint16_t)) != 0;
                if (pixel_changed)
                    ++s_vdiff_changed_src_rows;
                else
                {
                    ++s_vdiff_same_src_rows;
                    s_vdiff_identical_dst_rows += (uint64_t)(dy_end - dy);
                }
            }

            if (pixel_changed && render0 != dst0)
                tab5_pie_graphics_copy(dst0, render0, out_w * sizeof(uint16_t));
        }

        if (!pixel_changed)
        {
            note_clean_group(dy, dy_end);
            dy = dy_end;
            continue;
        }

        for (uint32_t copy_dy = dy + 1u; copy_dy < dy_end; ++copy_dy)
        {
            tab5_pie_graphics_copy(s_aspect_frame + (size_t)copy_dy * out_w,
                                   dst0, out_w * sizeof(uint16_t));
        }

        if (band_start == 0xffffffffu)
            band_start = dy;
        /* If a tiny clean gap preceded this dirty group, include those cached
         * rows in the same push by simply extending band_end to dy_end. */
        band_end = dy_end;
        clean_gap_rows = 0;
        dy = dy_end;
    }

    if (band_start != 0xffffffffu)
        flush_band(band_start, band_end);

    ++s_aspect_dirty_frames;
    if (force_full) ++s_aspect_full_frames;
    s_aspect_dirty_bands += frame_dirty_bands;
    s_aspect_dirty_rows += frame_dirty_rows;

    if (force_full)
    {
        ESP_LOGI(TAG,
                 "Build 5.98a DIRTY full refresh #%lu: %lu LCD rows in %lu band(s); subsequent frames use generation-dirty only",
                 (unsigned long)s_aspect_full_frames,
                 (unsigned long)frame_dirty_rows,
                 (unsigned long)frame_dirty_bands);
    }
    else if ((s_aspect_dirty_frames % 300u) == 0u)
    {
        const uint32_t avg_rows = s_aspect_dirty_frames
            ? (s_aspect_dirty_rows / s_aspect_dirty_frames) : 0u;
        const uint32_t avg_bands = s_aspect_dirty_frames
            ? (s_aspect_dirty_bands / s_aspect_dirty_frames) : 0u;
        ESP_LOGI(TAG,
                 "Build 5.98a DIRTY stats: frames=%lu full=%lu avgLCDrows=%lu/720 avgBands=%lu lastRows=%lu lastBands=%lu",
                 (unsigned long)s_aspect_dirty_frames,
                 (unsigned long)s_aspect_full_frames,
                 (unsigned long)avg_rows,
                 (unsigned long)avg_bands,
                 (unsigned long)frame_dirty_rows,
                 (unsigned long)frame_dirty_bands);
        ESP_LOGI(TAG,
                 "PX68K_VDIFF598G8: tested_src=%llu same=%llu changed=%llu identical_dst_rows=%llu scratch=%s",
                 (unsigned long long)s_vdiff_tested_src_rows,
                 (unsigned long long)s_vdiff_same_src_rows,
                 (unsigned long long)s_vdiff_changed_src_rows,
                 (unsigned long long)s_vdiff_identical_dst_rows,
                 s_aspect_line_scratch ? "PIE-DIFF" : "DISABLED");
    }

    if (retry_count) *retry_count = retries_total;
    if (unstable_count) *unstable_count = unstable_total;
}

static inline uint32_t video_pace_period_us(void)
{
    /* Same guest cadence used by the production speed meter in main.c. */
    return (CRTC_Regs[0x29] & 0x10u) ? 18031u : 16271u;
}

static void video_pace_timer_cb(void *)
{
    TaskHandle_t task = s_present_task;
    if (task)
        xTaskNotifyGive(task);
}

static inline void video_pace_reset(void)
{
    s_pace_next_us = 0;
    s_pace_last_present_us = 0;
}

static void video_pace_keep_latest_live(present_request_t *req)
{
    if (!req || req->mode != PRESENT_LIVE_FB || !s_ready_requests)
        return;

    present_request_t newer = {};
    while (xQueueReceive(s_ready_requests, &newer, 0) == pdTRUE)
    {
        if (newer.mode != PRESENT_LIVE_FB)
        {
            /* Preserve mode/UI transition ordering; this queue is only two
             * deep, so returning the non-live request to the front is safe. */
            (void)xQueueSendToFront(s_ready_requests, &newer, 0);
            break;
        }
        *req = newer;
        portENTER_CRITICAL(&s_stats_mux);
        ++s_pace_coalesced_frames;
        portEXIT_CRITICAL(&s_stats_mux);
    }
}

static void video_pace_wait_live(const present_request_t &req)
{
    if (req.mode != PRESENT_LIVE_FB || s_panic_compat_enabled || !s_pace_timer)
    {
        video_pace_reset();
        return;
    }

    const uint32_t period = video_pace_period_us();
    if (!s_pace_active_logged)
    {
        ESP_LOGI(TAG,
                 "PX68K_PACE614D: CRTC-paced live presenter ACTIVE target=%uus; CPU1 remains unblocked",
                 (unsigned)period);
        s_pace_active_logged = true;
    }
    int64_t now = esp_timer_get_time();

    /* Re-lock phase after launcher/UI pauses, long stalls, or mode changes. */
    if (s_pace_next_us == 0 || now > s_pace_next_us + (int64_t)period * 4 ||
        s_pace_next_us > now + (int64_t)period * 4)
    {
        s_pace_next_us = now;
    }

    int64_t deadline = s_pace_next_us;
    uint32_t skipped = 0;
    while (now > deadline + (int64_t)(period / 2u))
    {
        deadline += period;
        ++skipped;
    }

    uint32_t waited = 0;
    if (now < deadline)
    {
        const uint64_t delay_us = (uint64_t)(deadline - now);
        /* Clear a stale notification defensively; this task has no other
         * direct-notification user. */
        (void)ulTaskNotifyTake(pdTRUE, 0);
        if (esp_timer_start_once(s_pace_timer, delay_us) == ESP_OK)
        {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            const int64_t after = esp_timer_get_time();
            waited = (after > now) ? (uint32_t)(after - now) : 0u;
            now = after;
        }
    }

    const uint32_t interval = (s_pace_last_present_us > 0 && now > s_pace_last_present_us)
        ? (uint32_t)(now - s_pace_last_present_us) : 0u;
    s_pace_last_present_us = now;
    s_pace_next_us = deadline + period;

    portENTER_CRITICAL(&s_stats_mux);
    if (waited)
    {
        ++s_pace_waits;
        s_pace_wait_us += waited;
    }
    s_pace_last_wait_us = waited;
    s_pace_skipped_slots += skipped;
    s_pace_last_interval_us = interval;
    portEXIT_CRITICAL(&s_stats_mux);
}

static void video_present_task(void *)
{
    present_request_t req = {};

    for (;;)
    {
        /* Intent: Touch controls must remain responsive even when an X68000
         * title has a static frame and stops submitting LCD updates. Poll the
         * CPU0 UI at 100 Hz instead of coupling touch to incoming video.
         * Layer8 Aug/17/2026 */
        const bool have_req = xQueueReceive(s_ready_requests, &req, pdMS_TO_TICKS(10)) == pdTRUE;

        /* Build 6.12u: runtime launcher owns the panel while CPU1 is paused.
         * Drain stale snapshots but never touch LCD/touch until ownership returns. */
        if (__atomic_load_n(&s_host_ui_exclusive, __ATOMIC_ACQUIRE)) {
            if (have_req && req.mode == PRESENT_SNAPSHOT && req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            continue;
        }

        if (s_game_controls_enabled)
            (void)tab5_guest_input_queue_touch_joypad(poll_game_controls());

        /* FILE mode owns the LCD but not the guest.  Draw/redraw it even when
         * no video request arrived, while still draining queued snapshots. */
        if (s_media_overlay_active)
        {
            display_lock();
            if (s_media_overlay_redraw)
                draw_media_overlay_unlocked();
            display_unlock();
            if (have_req && req.mode == PRESENT_SNAPSHOT && req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            continue;
        }

        if (s_softkbd_active)
        {
            display_lock();
            if (s_softkbd_redraw)
                draw_soft_keyboard_unlocked();
            display_unlock();
            if (have_req && req.mode == PRESENT_SNAPSHOT && req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            continue;
        }

        if (!have_req)
            continue;

        /* Build 6.14d: when LCD work falls behind, discard only stale LIVE
         * requests and present the newest completed framebuffer.  Then align
         * CPU0 LCD work to the guest cadence; CPU1 is never delayed here. */
        video_pace_keep_latest_live(&req);
        video_pace_wait_live(req);
        /* A newer live request may arrive while the pace timer sleeps.  Keep
         * that one instead of presenting a stale queue token. */
        video_pace_keep_latest_live(&req);

        const int64_t t0 = esp_timer_get_time();
        uint32_t live_retries = 0;
        uint32_t live_unstable = 0;

        display_lock();
        if (__atomic_load_n(&s_host_ui_exclusive, __ATOMIC_ACQUIRE)) {
            display_unlock();
            if (req.mode == PRESENT_SNAPSHOT && req.slot_index < kPresentSlots)
                (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);
            continue;
        }
        if (!s_present_started || s_force_game_redraw)
        {
            M5.Display.fillScreen(0x0000);
            draw_game_controls_unlocked();
            s_present_started = true;
            s_force_game_redraw = false;
        }
        M5.Display.startWrite();
        if (req.mode == PRESENT_LIVE_FB) {
            present_request_t compat = {};
            if (panic_render_compat_source(req, &compat)) {
                /* Build 6.12g: compat pixels are freshly rebuilt on CPU0, but
                 * forcing s_aspect_seen_frame=NULL here made every PANIC frame
                 * a 640/720-row LCD full refresh and starved IDLE0 until WDT.
                 * Scan compat source rows every frame and let the existing exact
                 * PIE destination-row diff push only rows whose pixels changed. */
                push_live_frame(compat, &live_retries, &live_unstable, true);
            } else {
                push_live_frame(req, &live_retries, &live_unstable, false);
            }
        } else
            push_snapshot(req);
        M5.Display.endWrite();
        display_unlock();

        const uint32_t push_us = (uint32_t)(esp_timer_get_time() - t0);
        portENTER_CRITICAL(&s_stats_mux);
        ++s_presented_frames;
        s_last_push_us = push_us;
        if (req.mode == PRESENT_LIVE_FB)
        {
            ++s_live_presented_frames;
            s_live_row_retries += live_retries;
            s_live_unstable_rows += live_unstable;
        }
        portEXIT_CRITICAL(&s_stats_mux);

        if (req.mode == PRESENT_SNAPSHOT && req.slot_index < kPresentSlots)
            (void)xQueueSend(s_free_slots, &req.slot_index, portMAX_DELAY);

        /* Build 6.12g: PANIC can submit continuously while YM2151 also owns
         * CPU0.  One RTOS tick of blocking after a compat frame guarantees an
         * IDLE0 scheduling window instead of merely yielding to another ready
         * higher-priority worker.  At the 100-Hz system tick this is <=10 ms. */
        if (s_panic_compat_enabled && req.mode == PRESENT_LIVE_FB)
            vTaskDelay(1);
    }
}

static bool init_async_present(void)
{
    const int lcd_w = M5.Display.width();
    const int lcd_h = M5.Display.height();
    if (lcd_w <= 0 || lcd_h <= 0)
        return false;

    s_slot_capacity_pixels = (size_t)lcd_w * (size_t)lcd_h;
    s_free_slots = xQueueCreate(kPresentSlots, sizeof(uint8_t));
    s_ready_requests = xQueueCreate(kPresentSlots, sizeof(present_request_t));
    s_ui_actions = xQueueCreate(8, sizeof(tab5_video_action_t));
    if (!s_free_slots || !s_ready_requests || !s_ui_actions)
        return false;

    for (uint8_t i = 0; i < kPresentSlots; ++i)
    {
        s_slots[i].pixels = static_cast<uint16_t *>(heap_caps_malloc(
            s_slot_capacity_pixels * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_slots[i].pixels)
            return false;
        if (xQueueSend(s_free_slots, &i, 0) != pdTRUE)
            return false;
    }

    s_aspect_frame = static_cast<uint16_t *>(heap_caps_malloc(
        (size_t)kAspectViewportWidth * (size_t)kAspectViewportHeight * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_aspect_frame)
        return false;

    s_panic_compat_frame = static_cast<uint16_t *>(heap_caps_calloc(
        (size_t)kPanicCompatPitch * (size_t)kPanicCompatHeight, sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_panic_compat_frame)
        ESP_LOGW(TAG, "PANIC compatibility framebuffer unavailable; normal PX68K presenter retained");

    /* Optional 1.9-KiB PSRAM scratch.  Match its modulo-16 address to the
     * aspect-frame rows so tab5_pie_graphics_diff/copy can use PIE-128 even if
     * the heap allocator itself returns a non-16-byte base. */
    s_aspect_line_scratch_raw = static_cast<uint8_t *>(heap_caps_malloc(
        (size_t)kAspectViewportWidth * sizeof(uint16_t) + 15u,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_aspect_line_scratch_raw)
    {
        const uintptr_t raw_mod = (uintptr_t)s_aspect_line_scratch_raw & 15u;
        const uintptr_t frame_mod = (uintptr_t)s_aspect_frame & 15u;
        const uintptr_t adjust = (frame_mod + 16u - raw_mod) & 15u;
        s_aspect_line_scratch = reinterpret_cast<uint16_t *>(
            s_aspect_line_scratch_raw + adjust);
        ESP_LOGI(TAG,
                 "Build 5.98g9b LCD PIE-DIFF scratch armed: bytes=%u frame/scratch mod16=%u/%u",
                 (unsigned)(kAspectViewportWidth * sizeof(uint16_t)),
                 (unsigned)((uintptr_t)s_aspect_frame & 15u),
                 (unsigned)((uintptr_t)s_aspect_line_scratch & 15u));
    }
    else
    {
        ESP_LOGW(TAG, "Build 5.98g9b LCD PIE-DIFF scratch unavailable; generation-dirty fallback retained");
    }

    esp_timer_create_args_t pace_args = {};
    pace_args.callback = &video_pace_timer_cb;
    pace_args.name = "px68k_vpace";
    if (esp_timer_create(&pace_args, &s_pace_timer) == ESP_OK)
        ESP_LOGI(TAG, "PX68K_PACE614D: host LCD cadence timer ready; latest-live-frame pacing armed");
    else
        ESP_LOGW(TAG, "PX68K_PACE614D: cadence timer unavailable; immediate presentation retained");

#if portNUM_PROCESSORS > 1
    const BaseType_t ok = xTaskCreatePinnedToCore(
        video_present_task, "px68k_lcd", 4096, nullptr, 1, &s_present_task, 0);
#else
    const BaseType_t ok = xTaskCreate(
        video_present_task, "px68k_lcd", 4096, nullptr, 1, &s_present_task);
#endif
    if (ok != pdPASS)
        return false;

    return true;
}

extern "C" {

void tab5_video_fb_line_write_begin(uint32_t y)
{
    if (y >= kTrackedLines) return;
    (void)__atomic_add_fetch(&s_line_writers[y], 1u, __ATOMIC_ACQ_REL);
}

void tab5_video_fb_line_write_end(uint32_t y)
{
    if (y >= kTrackedLines) return;
    (void)__atomic_add_fetch(&s_line_generation[y], 1u, __ATOMIC_RELEASE);
    (void)__atomic_sub_fetch(&s_line_writers[y], 1u, __ATOMIC_RELEASE);
}

void tab5_video_fb_range_write_begin(uint32_t y, uint32_t count)
{
    uint32_t end = y + count;
    if (end > kTrackedLines) end = kTrackedLines;
    for (uint32_t row = y; row < end; ++row)
        tab5_video_fb_line_write_begin(row);
}

void tab5_video_fb_range_write_end(uint32_t y, uint32_t count)
{
    uint32_t end = y + count;
    if (end > kTrackedLines) end = kTrackedLines;
    for (uint32_t row = y; row < end; ++row)
        tab5_video_fb_line_write_end(row);
}

void tab5_video_set_game_controls_enabled(int enabled)
{
    s_game_controls_enabled = enabled != 0;
    s_utility_prev_mask = 0;
    s_touch_dpad_prev = 0;
    s_touch_dpad_press_us = 0;
    if (!s_game_controls_enabled) {
        update_game_touch_keyboard(0);
        s_media_overlay_active = false;
        s_media_overlay_redraw = false;
        s_softkbd_active = false;
        s_softkbd_redraw = false;
        s_softkbd_touch_down = false;
        s_softkbd_shift = false;
        s_softkbd_ctrl = false;
        s_soft_input_count = 0;
        s_game_touch_key_prev = 0;
        (void)tab5_guest_input_queue_touch_joypad(0);
    } else {
        s_present_started = false;
        s_aspect_seen_frame = nullptr;
    }
}

int tab5_video_poll_action(tab5_video_action_t *out)
{
    if (!out || !s_ui_actions) return 0;
    std::memset(out, 0, sizeof(*out));
    return xQueueReceive(s_ui_actions, out, 0) == pdTRUE ? 1 : 0;
}

void tab5_video_set_runtime_media_paths(const char *fdd0,
                                        const char *fdd1,
                                        const char *hdd0)
{
    portENTER_CRITICAL(&s_runtime_media_mux);
    std::snprintf(s_runtime_fdd0, sizeof(s_runtime_fdd0), "%s", fdd0 ? fdd0 : "");
    std::snprintf(s_runtime_fdd1, sizeof(s_runtime_fdd1), "%s", fdd1 ? fdd1 : "");
    std::snprintf(s_runtime_hdd0, sizeof(s_runtime_hdd0), "%s", hdd0 ? hdd0 : "");
    /* While FILE is open, CPU1 is the source of truth after each immediate
     * mount/eject action. Mirror the actual result back into the shared UI so
     * a failed mount cannot leave a stale filename on screen. */
    if (s_media_overlay_active) {
        std::snprintf(s_media_pending_fdd0, sizeof(s_media_pending_fdd0), "%s", s_runtime_fdd0);
        std::snprintf(s_media_pending_fdd1, sizeof(s_media_pending_fdd1), "%s", s_runtime_fdd1);
        std::snprintf(s_media_pending_hdd0, sizeof(s_media_pending_hdd0), "%s", s_runtime_hdd0);
    }
    portEXIT_CRITICAL(&s_runtime_media_mux);
    if (s_media_overlay_active)
        s_media_overlay_redraw = true;
}

void tab5_video_set_runtime_boot_source(int boot_source)
{
    portENTER_CRITICAL(&s_runtime_media_mux);
    s_runtime_boot_source = boot_source == TAB5_LAUNCH_BOOT_HDD0 ?
                            TAB5_LAUNCH_BOOT_HDD0 : TAB5_LAUNCH_BOOT_FLOPPY0;
    portEXIT_CRITICAL(&s_runtime_media_mux);
}

static void drain_present_requests_for_host_ui(void)
{
    if (!s_ready_requests) return;
    present_request_t stale = {};
    while (xQueueReceive(s_ready_requests, &stale, 0) == pdTRUE) {
        if (stale.mode == PRESENT_SNAPSHOT && stale.slot_index < kPresentSlots && s_free_slots)
            (void)xQueueSend(s_free_slots, &stale.slot_index, 0);
    }
}

void tab5_video_begin_host_ui(void)
{
    /* Set ownership before waiting for the display mutex. A CPU0 presenter that
     * was already between queue receive and display_lock() re-checks this flag
     * after acquiring the mutex and discards its stale frame. */
    __atomic_store_n(&s_host_ui_exclusive, true, __ATOMIC_RELEASE);
    drain_present_requests_for_host_ui();
    display_lock();
    M5.Display.fillScreen(TFT_BLACK);
    s_present_started = false;
    s_force_game_redraw = false;
    s_aspect_seen_frame = nullptr;
    std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
    s_aspect_seen_src_w = s_aspect_seen_src_h = s_aspect_seen_pitch = 0;
    display_unlock();
    ESP_LOGI(TAG, "Host UI exclusive: LCD presenter paused; ESP/audio/USB remain alive");
}

void tab5_video_end_host_ui(void)
{
    drain_present_requests_for_host_ui();
    display_lock();
    s_present_started = false;
    s_force_game_redraw = true;
    s_aspect_seen_frame = nullptr;
    std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
    s_aspect_seen_src_w = s_aspect_seen_src_h = s_aspect_seen_pitch = 0;
    display_unlock();
    __atomic_store_n(&s_host_ui_exclusive, false, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Host UI exclusive released: guest LCD presentation resumed");
}

void tab5_video_set_panic_compat_enabled(int enabled)
{
    const bool was_enabled = s_panic_compat_enabled;
    const bool now_enabled = enabled != 0;
    s_panic_compat_enabled = now_enabled;
    s_panic_compat_last_w = s_panic_compat_last_h = 0;
    s_panic_compat_last_mode = s_panic_compat_last_enable = 0xff;

    /* Build 6.12t: an in-place game -> PANIC switch intentionally keeps the
     * ESP32-P4 host, LCD task and ES8388/I2S alive.  That also means the LCD
     * still contains the game's last 960x720 frame.  PANIC can start at a
     * smaller square/letterboxed viewport, so merely forcing a dirty refresh
     * leaves the old game visible around/under the new image.
     *
     * On the OFF -> ON transition, wait for any in-flight CPU0 LCD push to
     * finish, erase the physical panel, and forget every cached presentation
     * surface/generation.  The next PANIC frame is therefore a true full
     * refresh onto black, while audio/USB/FreeRTOS host services are untouched.
     * Layer8 Aug/18/2026 */
    if (now_enabled && !was_enabled)
    {
        display_lock();
        M5.Display.fillScreen(TFT_BLACK);
        if (s_aspect_frame)
            std::memset(s_aspect_frame, 0,
                        (size_t)kAspectViewportWidth * (size_t)kAspectViewportHeight * sizeof(uint16_t));
        if (s_panic_compat_frame)
            std::memset(s_panic_compat_frame, 0,
                        (size_t)kPanicCompatPitch * (size_t)kPanicCompatHeight * sizeof(uint16_t));
        std::memset(s_aspect_seen_generation, 0, sizeof(s_aspect_seen_generation));
        s_aspect_seen_frame = nullptr;
        s_aspect_seen_src_w = 0;
        s_aspect_seen_src_h = 0;
        s_aspect_seen_pitch = 0;
        s_aspect_map_src_w = s_aspect_map_dst_w = 0;
        s_aspect_map_src_h = s_aspect_map_dst_h = 0;
        s_aspect_log_src_w = s_aspect_log_src_h = 0;
        s_force_game_redraw = false;
        s_present_started = true;
        display_unlock();
        ESP_LOGI(TAG, "PANIC display transition: previous guest frame cleared; host LCD/audio kept alive");
    }

    if (now_enabled)
        ESP_LOGI(TAG, "PANIC compatibility presenter armed on CPU0");
}

void tab5_video_init(void)
{
    auto cfg = M5.config();
    M5.begin(cfg);

    if (M5.Display.width() < M5.Display.height())
        M5.Display.setRotation(1);

    M5.Display.setSwapBytes(true);
    M5.Display.setBrightness(255);

    s_display_mutex = xSemaphoreCreateMutex();

    display_lock();
    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(24, 24);
    M5.Display.println(X68K_TAB_APP_NAME);
    M5.Display.println(X68K_TAB_SUBTITLE);
    display_unlock();

    ESP_LOGI(TAG, "Display initialized: %d x %d", M5.Display.width(), M5.Display.height());

    s_async_ready = init_async_present();
    if (s_async_ready)
    {
        ESP_LOGI(TAG,
                 "Build 5.98g9b zero-copy+4:3 DIRTY+PIE-DIFF LCD: CPU1 posts raw ScrBuf, CPU0 scales generation-dirty rows then suppresses pixel-identical LCD bands; text snapshot slots=%u",
                 (unsigned)kPresentSlots);
    }
    else
    {
        ESP_LOGW(TAG, "Build 5.45 async LCD unavailable; synchronous fallback active");
    }
}

void tab5_video_show_message(const char *line1, const char *line2)
{
    display_lock();
    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(24, 24);
    if (line1) M5.Display.println(line1);
    if (line2) M5.Display.println(line2);
    display_unlock();
}

void tab5_video_status(const char *line1, const char *line2)
{
    /* Build 5.98a: status banners are useful during launcher/boot, but once
     * emulator presentation starts they become debug text painted over the
     * top of the game.  Runtime state is still reported on serial logs.
     * Check while holding the display mutex so the first emulator full-clear
     * and a late USB status message cannot race each other. */
    const int lcd_w = M5.Display.width();
    display_lock();
    if (s_present_started || s_media_overlay_active)
    {
        display_unlock();
        return;
    }
    M5.Display.startWrite();
    M5.Display.fillRect(0, 0, lcd_w, 96, 0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(20, 12);
    if (line1) M5.Display.println(line1);
    if (line2) M5.Display.println(line2);
    M5.Display.endWrite();
    display_unlock();
}

int tab5_video_width(void) { return M5.Display.width(); }
int tab5_video_height(void) { return M5.Display.height(); }

/* Packed-snapshot path retained for the host TEXT view. */
void tab5_video_present_px68k(const uint16_t *frame,
                              uint32_t width,
                              uint32_t height,
                              uint32_t pitch_pixels)
{
    if (!frame || !width || !height || !pitch_pixels)
        return;

    const uint32_t lcd_w = (uint32_t)M5.Display.width();
    const uint32_t lcd_h = (uint32_t)M5.Display.height();
    if (width > lcd_w) width = lcd_w;
    if (height > lcd_h) height = lcd_h;

    if (s_async_ready)
    {
        uint8_t slot_index = 0;
        if (xQueueReceive(s_free_slots, &slot_index, 0) != pdTRUE)
        {
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        present_slot_t &slot = s_slots[slot_index];
        const size_t pixels = (size_t)width * (size_t)height;
        if (pixels > s_slot_capacity_pixels)
        {
            (void)xQueueSend(s_free_slots, &slot_index, 0);
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        const int64_t t0 = esp_timer_get_time();
        uint16_t *dst = slot.pixels;
        if (pitch_pixels == width)
        {
            std::memcpy(dst, frame, pixels * sizeof(uint16_t));
        }
        else
        {
            for (uint32_t row = 0; row < height; ++row)
            {
                std::memcpy(dst + (size_t)row * width,
                            frame + (size_t)row * pitch_pixels,
                            (size_t)width * sizeof(uint16_t));
            }
        }
        slot.width = width;
        slot.height = height;
        const uint32_t copy_us = (uint32_t)(esp_timer_get_time() - t0);

        present_request_t req = {};
        req.mode = PRESENT_SNAPSHOT;
        req.slot_index = slot_index;
        req.width = width;
        req.height = height;
        if (xQueueSend(s_ready_requests, &req, 0) != pdTRUE)
        {
            (void)xQueueSend(s_free_slots, &slot_index, 0);
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        portENTER_CRITICAL(&s_stats_mux);
        ++s_submitted_frames;
        s_last_copy_us = copy_us;
        portEXIT_CRITICAL(&s_stats_mux);
        return;
    }

    /* Allocation/task failure fallback: preserve the proven synchronous path. */
    const int x = ((int)lcd_w - (int)width) / 2;
    const int y = ((int)lcd_h - (int)height) / 2;
    display_lock();
    if (!s_present_started || s_force_game_redraw)
    {
        M5.Display.fillScreen(0x0000);
        if (s_game_controls_enabled) draw_game_controls_unlocked();
        s_present_started = true;
        s_force_game_redraw = false;
    }
    M5.Display.startWrite();
    if (pitch_pixels == width)
    {
        M5.Display.pushImage(x, y, (int)width, (int)height, frame);
    }
    else
    {
        for (uint32_t row = 0; row < height; ++row)
            M5.Display.pushImage(x, y + (int)row, (int)width, 1, frame + (size_t)row * pitch_pixels);
    }
    M5.Display.endWrite();
    display_unlock();
}

/* Build 5.45 zero-copy live framebuffer path. */
void tab5_video_present_px68k_live(const uint16_t *frame,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t pitch_pixels)
{
    if (!frame || !width || !height || !pitch_pixels)
        return;

    const uint32_t lcd_w = (uint32_t)M5.Display.width();
    const uint32_t lcd_h = (uint32_t)M5.Display.height();
    if (width > lcd_w) width = lcd_w;
    if (height > lcd_h) height = lcd_h;
    if (height > kTrackedLines) height = kTrackedLines;

    if (s_async_ready)
    {
        present_request_t req = {};
        req.mode = PRESENT_LIVE_FB;
        req.slot_index = 0xff;
        req.frame = frame;
        req.width = width;
        req.height = height;
        req.pitch_pixels = pitch_pixels;

        if (xQueueSend(s_ready_requests, &req, 0) != pdTRUE)
        {
            portENTER_CRITICAL(&s_stats_mux);
            ++s_dropped_frames;
            portEXIT_CRITICAL(&s_stats_mux);
            return;
        }

        portENTER_CRITICAL(&s_stats_mux);
        ++s_submitted_frames;
        s_last_copy_us = 0;
        portEXIT_CRITICAL(&s_stats_mux);
        return;
    }

    tab5_video_present_px68k(frame, width, height, pitch_pixels);
}

void tab5_video_get_async_stats(tab5_video_async_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_stats_mux);
    out->submitted_frames = s_submitted_frames;
    out->presented_frames = s_presented_frames;
    out->dropped_frames = s_dropped_frames;
    out->last_copy_us = s_last_copy_us;
    out->last_push_us = s_last_push_us;
    out->live_presented_frames = s_live_presented_frames;
    out->live_row_retries = s_live_row_retries;
    out->live_unstable_rows = s_live_unstable_rows;
    out->pace_waits = s_pace_waits;
    out->pace_wait_us = s_pace_wait_us;
    out->pace_last_wait_us = s_pace_last_wait_us;
    out->pace_coalesced_frames = s_pace_coalesced_frames;
    out->pace_skipped_slots = s_pace_skipped_slots;
    out->pace_last_interval_us = s_pace_last_interval_us;
    portEXIT_CRITICAL(&s_stats_mux);
    out->queued_frames = s_ready_requests ? (uint32_t)uxQueueMessagesWaiting(s_ready_requests) : 0u;
}

} // extern "C"
