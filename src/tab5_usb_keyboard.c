/*
 * Tab5 port-specific implementation.
 * Intent: USB host implementation for Tab5 keyboard, mouse, and retro JoyPAD, including reconnect-safe fixed mapping for the validated 8-byte pad.
 * Layer8 Aug/17/2026
 */
#include "tab5_usb_keyboard.h"

#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"

#include "libretro.h"
#include "libretro/joystick.h"
#include "tab5_guest_input.h"
#include "tab5_usb_keymap.h"
#include "esp_rom_sys.h"

#define TAB5_USB_CTRL_QUEUE_DEPTH 8
#define TAB5_USB_BOOT_REPORT_SIZE 8u
#define TAB5_USB_BOOT_KEYS        6u
#define TAB5_USB_BOOT_MOUSE_SIZE  3u
#define TAB5_USB_PAD_REPORT_MAX   64u
#define TAB5_USB_PAD_RAW_LOG_MAX  16u
#define TAB5_USB_PAD_AXIS_DEADZONE 48
#define TAB5_USB_LIB_TASK_PRIO   6
#define TAB5_USB_HID_TASK_PRIO   6
#define TAB5_USB_CTRL_TASK_PRIO  5

#ifndef PX68K_TAB5_INPUT_TRACE
#define PX68K_TAB5_INPUT_TRACE 0
#endif

static const char *TAG = "TAB5_USB_KBD";

typedef enum
{
    USB_CTRL_CONNECT = 1
} usb_ctrl_type_t;

typedef struct
{
    usb_ctrl_type_t type;
    hid_host_device_handle_t handle;
} usb_ctrl_event_t;

static QueueHandle_t s_ctrl_queue = NULL;
static TaskHandle_t s_usb_lib_task = NULL;
static TaskHandle_t s_usb_ctl_task = NULL;
static hid_host_device_handle_t s_keyboard_handle = NULL;
static hid_host_device_handle_t s_mouse_handle = NULL;
static hid_host_device_handle_t s_joypad_handle = NULL;
static uint8_t s_prev_modifier = 0;
static uint8_t s_prev_keys[TAB5_USB_BOOT_KEYS];

static uint32_t s_connected = 0;
static uint32_t s_key_events = 0;
static uint32_t s_reports = 0;
static uint32_t s_mouse_connected = 0;
static uint32_t s_mouse_events = 0;
static uint32_t s_joypad_connected = 0;
static uint32_t s_joypad_reports = 0;
static uint32_t s_joypad_events = 0;
static uint32_t s_joypad_recognized = 0;
static uint32_t s_hotkeys = 0;
static esp_err_t s_usb_install_result = ESP_FAIL;

typedef struct
{
    bool neutral_valid;
    size_t report_len;
    uint8_t neutral[TAB5_USB_PAD_REPORT_MAX];
    uint8_t previous[TAB5_USB_PAD_REPORT_MAX];
    int axis_x;
    int axis_y;
    int hat;
    int button_byte[8];
    uint8_t button_mask[8];
    bool learning_ready;
    uint16_t last_joy;
    uint32_t raw_logs;
} tab5_generic_pad_t;

typedef struct
{
    bool valid;
    size_t report_len;
    uint8_t neutral[TAB5_USB_PAD_REPORT_MAX];
    int axis_x;
    int axis_y;
    int hat;
    int button_byte[8];
    uint8_t button_mask[8];
} tab5_generic_pad_profile_t;

static tab5_generic_pad_t s_pad;
/* Build 6.00: keep one learned generic-pad profile across transient USB
 * disconnect/re-enumeration.  The HID host supports only one generic pad at a
 * time, so the profile is intentionally per-boot rather than persistent flash.
 * A report-length change is treated as a different layout and relearned. */
static tab5_generic_pad_profile_t s_pad_profile;

static void pad_live_reset(void)
{
    memset(&s_pad, 0, sizeof(s_pad));
    s_pad.axis_x = -1;
    s_pad.axis_y = -1;
    s_pad.hat = -1;
    for (unsigned b = 0; b < 8u; ++b)
        s_pad.button_byte[b] = -1;
}

static void pad_profile_reset(void)
{
    memset(&s_pad_profile, 0, sizeof(s_pad_profile));
    s_pad_profile.axis_x = -1;
    s_pad_profile.axis_y = -1;
    s_pad_profile.hat = -1;
    for (unsigned b = 0; b < 8u; ++b)
        s_pad_profile.button_byte[b] = -1;
}

static unsigned pad_learned_button_count(void)
{
    unsigned n = 0;
    for (unsigned b = 0; b < 8u; ++b)
        if (s_pad.button_byte[b] >= 0 && s_pad.button_mask[b])
            ++n;
    return n;
}

static void pad_profile_capture(void)
{
    if (!s_pad.neutral_valid || !s_pad.report_len ||
        s_pad.report_len > TAB5_USB_PAD_REPORT_MAX)
        return;

    s_pad_profile.valid = true;
    s_pad_profile.report_len = s_pad.report_len;
    memcpy(s_pad_profile.neutral, s_pad.neutral, s_pad.report_len);
    s_pad_profile.axis_x = s_pad.axis_x;
    s_pad_profile.axis_y = s_pad.axis_y;
    s_pad_profile.hat = s_pad.hat;
    memcpy(s_pad_profile.button_byte, s_pad.button_byte, sizeof(s_pad.button_byte));
    memcpy(s_pad_profile.button_mask, s_pad.button_mask, sizeof(s_pad.button_mask));
}

static int pad_profile_restore(const uint8_t *current, size_t length)
{
    if (!s_pad_profile.valid || !current ||
        length != s_pad_profile.report_len ||
        length > TAB5_USB_PAD_REPORT_MAX)
        return 0;

    pad_live_reset();
    s_pad.neutral_valid = true;
    s_pad.report_len = length;
    memcpy(s_pad.neutral, s_pad_profile.neutral, length);
    memcpy(s_pad.previous, current, length);
    s_pad.axis_x = s_pad_profile.axis_x;
    s_pad.axis_y = s_pad_profile.axis_y;
    s_pad.hat = s_pad_profile.hat;
    memcpy(s_pad.button_byte, s_pad_profile.button_byte, sizeof(s_pad.button_byte));
    memcpy(s_pad.button_mask, s_pad_profile.button_mask, sizeof(s_pad.button_mask));
    s_pad.learning_ready = false; /* avoid learning transient bits from first reconnect report */
    ESP_LOGI(TAG,
             "USB PAD learned profile RESTORED after reconnect: len=%u X=%d Y=%d HAT=%d buttons=%u/8",
             (unsigned)length, s_pad.axis_x, s_pad.axis_y, s_pad.hat,
             pad_learned_button_count());
    return 1;
}

static int pad_center_like(uint8_t v)
{
    return v >= 0x50u && v <= 0xb0u;
}

/* Intent: Use the validated fixed map for this pad so transient USB re-enumeration cannot erase B/C or shift button assignments.  Layer8 Aug/17/2026 */
/* Build 6.00: the low-speed 8-byte retro pad used during Tab5 bring-up
 * has a stable report layout even though the ESP-IDF USB host may transiently
 * re-enumerate it.  Install the already-proven physical A/B/C, upper A/B/C,
 * L/R mapping immediately instead of requiring an uninterrupted learning run.
 * Other generic HID pads still use the normal first-press learner. */
static int pad_install_known_8byte_profile(const uint8_t *data, size_t length)
{
    static const uint8_t neutral[8] = {0x7f, 0x7f, 0x00, 0x80, 0x80, 0x0f, 0x00, 0x00};
    static const int button_byte[8] = {6, 5, 5, 6, 5, 5, 6, 6};
    static const uint8_t button_mask[8] = {0x01, 0x40, 0x20, 0x10, 0x80, 0x10, 0x04, 0x08};

    if (!data || length != sizeof(neutral))
        return 0;

    /* Match the characteristic neutral/report shape while allowing a button
     * to be held during enumeration.  Axes must be near center; byte 5 low
     * nibble is the hat and byte 6 contains only the known button bits. */
    if (!pad_center_like(data[0]) || !pad_center_like(data[1]) ||
        data[2] != 0x00u || data[3] != 0x80u || data[4] != 0x80u ||
        ((data[5] & 0x0fu) > 7u && (data[5] & 0x0fu) != 0x0fu) ||
        (data[6] & (uint8_t)~0x1du) != 0u || data[7] != 0x00u)
        return 0;

    pad_live_reset();
    s_pad.neutral_valid = true;
    s_pad.report_len = sizeof(neutral);
    memcpy(s_pad.neutral, neutral, sizeof(neutral));
    memcpy(s_pad.previous, data, sizeof(neutral));
    s_pad.axis_x = 0;
    s_pad.axis_y = 1;
    s_pad.hat = 5;
    memcpy(s_pad.button_byte, button_byte, sizeof(button_byte));
    memcpy(s_pad.button_mask, button_mask, sizeof(button_mask));
    s_pad.learning_ready = false;
    pad_profile_capture();
    __atomic_store_n(&s_joypad_recognized, 1u, __ATOMIC_RELEASE);
    ESP_LOGI(TAG,
             "USB PAD known 8-byte profile APPLIED: A/B/C + upper A/B/C + L/R fixed; reconnect-safe");
    return 1;
}

static void pad_log_raw(const uint8_t *data, size_t length)
{
#if PX68K_TAB5_INPUT_TRACE
    char line[(TAB5_USB_PAD_REPORT_MAX * 3u) + 1u];
    size_t pos = 0;

    if (!data || !length || s_pad.raw_logs >= TAB5_USB_PAD_RAW_LOG_MAX)
        return;

    for (size_t i = 0; i < length && i < TAB5_USB_PAD_REPORT_MAX; ++i)
    {
        int n = snprintf(&line[pos], sizeof(line) - pos,
                         (i + 1u == length) ? "%02X" : "%02X ", data[i]);
        if (n <= 0 || (size_t)n >= sizeof(line) - pos)
            break;
        pos += (size_t)n;
    }
    line[sizeof(line) - 1u] = '\0';
    ++s_pad.raw_logs;
    ESP_LOGI(TAG, "USB PAD RAW #%lu len=%u: %s",
             (unsigned long)s_pad.raw_logs, (unsigned)length, line);
#else
    (void)data;
    (void)length;
#endif
}

static void pad_discover_layout(const uint8_t *data, size_t length)
{
    s_pad.axis_x = -1;
    s_pad.axis_y = -1;
    s_pad.hat = -1;
    for (unsigned b = 0; b < 8u; ++b)
    {
        s_pad.button_byte[b] = -1;
        s_pad.button_mask[b] = 0;
    }

    /* Most standard HID pads expose X/Y as the first adjacent pair centered
     * near 0x7f/0x80. This also covers the common DragonRise/retro-pad family
     * when buttons or a report ID precede the axes. */
    for (size_t i = 0; i + 1u < length; ++i)
    {
        if (pad_center_like(data[i]) && pad_center_like(data[i + 1u]))
        {
            s_pad.axis_x = (int)i;
            s_pad.axis_y = (int)(i + 1u);
            break;
        }
    }

    /* HID hat neutral is commonly 8 or 15. Find it outside the chosen axes. */
    for (size_t i = 0; i < length; ++i)
    {
        const uint8_t low = (uint8_t)(data[i] & 0x0fu);
        if ((int)i == s_pad.axis_x || (int)i == s_pad.axis_y)
            continue;
        if ((low == 8u || low == 15u) && (data[i] & 0xf0u) == 0u)
        {
            s_pad.hat = (int)i;
            break;
        }
    }

    ESP_LOGI(TAG,
             "USB PAD auto-map: len=%u X=%d Y=%d HAT=%d buttons=learn B1..B6,L,R; learned profile survives transient reconnect -> X68000 JOY1 CPSF/MD",
             (unsigned)length, s_pad.axis_x, s_pad.axis_y, s_pad.hat);
    pad_profile_capture();
}

static void pad_learn_buttons(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        uint8_t allowed = 0xffu;
        uint8_t pressed;

        if ((int)i == s_pad.axis_x || (int)i == s_pad.axis_y)
            continue;
        if ((int)i == s_pad.hat)
            allowed &= 0xf0u;

        if (s_pad.neutral[i] == 0x00u)
            pressed = (uint8_t)(data[i] & allowed);
        else if (s_pad.neutral[i] == 0xffu)
            pressed = (uint8_t)((~data[i]) & allowed);
        else if ((int)i == s_pad.hat && (s_pad.neutral[i] & 0xf0u) == 0u)
            pressed = (uint8_t)(data[i] & 0xf0u);
        else
            continue;

        for (unsigned bit = 0; bit < 8u && pressed; ++bit)
        {
            const uint8_t mask = (uint8_t)(1u << bit);
            if (!(pressed & mask))
                continue;

            bool known = false;
            for (unsigned b = 0; b < 8u; ++b)
            {
                if (s_pad.button_byte[b] == (int)i && s_pad.button_mask[b] == mask)
                    known = true;
            }
            if (known)
                continue;

            for (unsigned b = 0; b < 8u; ++b)
            {
                if (s_pad.button_byte[b] < 0)
                {
                    s_pad.button_byte[b] = (int)i;
                    s_pad.button_mask[b] = mask;
                    {
                        static const char *const labels[8] =
                            {"B1", "B2", "B3", "B4", "B5", "B6", "L", "R"};
                        ESP_LOGI(TAG, "USB PAD learned %s: byte=%u mask=%02X",
                                 labels[b], (unsigned)i, (unsigned)mask);
                    }
                    __atomic_store_n(&s_joypad_recognized, 1u, __ATOMIC_RELEASE);
                    pad_profile_capture();
                    break;
                }
            }
        }
    }
}

static int pad_button_pressed(const uint8_t *data, size_t length, unsigned which)
{
    int byte;
    uint8_t mask;

    if (which >= 8u)
        return 0;
    byte = s_pad.button_byte[which];
    mask = s_pad.button_mask[which];
    if (byte < 0 || (size_t)byte >= length || !mask)
        return 0;

    return ((data[byte] ^ s_pad.neutral[byte]) & mask) != 0u;
}

static uint16_t pad_decode_state(const uint8_t *data, size_t length)
{
    uint16_t joy = 0;

    if (s_pad.axis_x >= 0 && s_pad.axis_y >= 0 &&
        (size_t)s_pad.axis_y < length)
    {
        const int dx = (int)data[s_pad.axis_x] - (int)s_pad.neutral[s_pad.axis_x];
        const int dy = (int)data[s_pad.axis_y] - (int)s_pad.neutral[s_pad.axis_y];
        if (dx <= -TAB5_USB_PAD_AXIS_DEADZONE) joy |= JOY_LEFT;
        if (dx >=  TAB5_USB_PAD_AXIS_DEADZONE) joy |= JOY_RIGHT;
        if (dy <= -TAB5_USB_PAD_AXIS_DEADZONE) joy |= JOY_UP;
        if (dy >=  TAB5_USB_PAD_AXIS_DEADZONE) joy |= JOY_DOWN;
    }

    if (s_pad.hat >= 0 && (size_t)s_pad.hat < length)
    {
        const uint8_t h = (uint8_t)(data[s_pad.hat] & 0x0fu);
        switch (h)
        {
            case 0: joy |= JOY_UP; break;
            case 1: joy |= JOY_UP | JOY_RIGHT; break;
            case 2: joy |= JOY_RIGHT; break;
            case 3: joy |= JOY_RIGHT | JOY_DOWN; break;
            case 4: joy |= JOY_DOWN; break;
            case 5: joy |= JOY_DOWN | JOY_LEFT; break;
            case 6: joy |= JOY_LEFT; break;
            case 7: joy |= JOY_LEFT | JOY_UP; break;
            default: break;
        }
    }

    if (pad_button_pressed(data, length, 0u)) joy |= JOY_TRG1;
    if (pad_button_pressed(data, length, 1u)) joy |= JOY_TRG2;
    if (pad_button_pressed(data, length, 2u)) joy |= JOY_HOST_BTN3;
    if (pad_button_pressed(data, length, 3u)) joy |= JOY_HOST_BTN4;
    if (pad_button_pressed(data, length, 4u)) joy |= JOY_HOST_BTN5;
    if (pad_button_pressed(data, length, 5u)) joy |= JOY_HOST_BTN6;
    if (pad_button_pressed(data, length, 6u)) joy |= JOY_HOST_L;
    if (pad_button_pressed(data, length, 7u)) joy |= JOY_HOST_R;

    if ((joy & (JOY_LEFT | JOY_RIGHT)) == (JOY_LEFT | JOY_RIGHT))
        joy &= (uint16_t)~(JOY_LEFT | JOY_RIGHT);
    if ((joy & (JOY_UP | JOY_DOWN)) == (JOY_UP | JOY_DOWN))
        joy &= (uint16_t)~(JOY_UP | JOY_DOWN);

    return joy;
}

static void process_generic_joypad_report(const uint8_t *data, size_t length)
{
    uint16_t joy;

    if (!data || !length || length > TAB5_USB_PAD_REPORT_MAX)
        return;

    __atomic_add_fetch(&s_joypad_reports, 1u, __ATOMIC_RELAXED);

    if (!s_pad.neutral_valid || s_pad.report_len != length)
    {
        /* A transient disconnect can deliver the first report with a button or
         * direction already asserted.  Never make that report the new neutral
         * baseline when a compatible learned profile exists. */
        if (pad_profile_restore(data, length))
        {
            pad_log_raw(data, length);
            joy = pad_decode_state(data, length);
            s_pad.last_joy = joy;
            if (s_pad.axis_x >= 0 || s_pad.hat >= 0 || pad_learned_button_count())
                __atomic_store_n(&s_joypad_recognized, 1u, __ATOMIC_RELEASE);
            (void)tab5_guest_input_queue_joypad(joy);
            return;
        }

        if (pad_install_known_8byte_profile(data, length))
        {
            pad_log_raw(data, length);
            joy = pad_decode_state(data, length);
            s_pad.last_joy = joy;
            (void)tab5_guest_input_queue_joypad(joy);
            return;
        }

        /* Report shape changed: treat it as a new generic pad/layout. */
        pad_profile_reset();
        pad_live_reset();
        s_pad.report_len = length;
        memcpy(s_pad.neutral, data, length);
        memcpy(s_pad.previous, data, length);
        s_pad.neutral_valid = true;
        s_pad.learning_ready = true;
        pad_log_raw(data, length);
        pad_discover_layout(data, length);
        if (s_pad.axis_x >= 0 || s_pad.hat >= 0)
            __atomic_store_n(&s_joypad_recognized, 1u, __ATOMIC_RELEASE);
        (void)tab5_guest_input_queue_joypad(0);
        return;
    }

    if (memcmp(s_pad.previous, data, length) != 0)
        pad_log_raw(data, length);

    if (!s_pad.learning_ready)
    {
        if (memcmp(s_pad.neutral, data, length) == 0)
        {
            s_pad.learning_ready = true;
            ESP_LOGI(TAG, "USB PAD reconnect neutral observed; learning remaining buttons re-armed");
        }
    }
    else
    {
        pad_learn_buttons(data, length);
    }
    joy = pad_decode_state(data, length);

    if (joy != s_pad.last_joy)
    {
        s_pad.last_joy = joy;
        __atomic_add_fetch(&s_joypad_events, 1u, __ATOMIC_RELAXED);
        (void)tab5_guest_input_queue_joypad(joy);
#if PX68K_TAB5_INPUT_TRACE
        ESP_LOGI(TAG, "USB PAD -> X68000 JOY1 state=%04X%s%s%s%s%s%s%s%s%s%s%s%s",
                 (unsigned)joy,
                 (joy & JOY_UP) ? " U" : "",
                 (joy & JOY_DOWN) ? " D" : "",
                 (joy & JOY_LEFT) ? " LEFT" : "",
                 (joy & JOY_RIGHT) ? " RIGHT" : "",
                 (joy & JOY_TRG1) ? " B1" : "",
                 (joy & JOY_TRG2) ? " B2" : "",
                 (joy & JOY_HOST_BTN3) ? " B3" : "",
                 (joy & JOY_HOST_BTN4) ? " B4" : "",
                 (joy & JOY_HOST_BTN5) ? " B5" : "",
                 (joy & JOY_HOST_BTN6) ? " B6" : "",
                 (joy & JOY_HOST_L) ? " L" : "",
                 (joy & JOY_HOST_R) ? " R" : "");
#endif
    }

    memcpy(s_pad.previous, data, length);
}

static int host_hotkey_usage(uint8_t usage)
{
    /* USB HID Keyboard F7..F12 usages. Reserved for Tab5 host controls. */
    return usage == 0x40u || usage == 0x41u || usage == 0x42u ||
           usage == 0x43u || usage == 0x44u || usage == 0x45u;
}

static void queue_host_hotkey(uint8_t usage)
{
    uint32_t bit = 0;

    if (usage == 0x40u)
        bit = TAB5_USB_HOTKEY_A_BOOT_NEXT;
    else if (usage == 0x41u)
        bit = TAB5_USB_HOTKEY_A_BOOT_TOGGLE;
    else if (usage == 0x42u)
        bit = TAB5_USB_HOTKEY_VIDEO_TOGGLE;
    else if (usage == 0x43u)
        bit = TAB5_USB_HOTKEY_B_WP;
    else if (usage == 0x44u)
        bit = TAB5_USB_HOTKEY_B_TOGGLE;
    else if (usage == 0x45u)
        bit = TAB5_USB_HOTKEY_B_NEXT;

    if (bit)
        __atomic_fetch_or(&s_hotkeys, bit, __ATOMIC_RELEASE);
}

static int key_found(const uint8_t *keys, uint8_t key)
{
    unsigned i;
    for (i = 0; i < TAB5_USB_BOOT_KEYS; ++i)
    {
        if (keys[i] == key)
            return 1;
    }
    return 0;
}

static void queue_retro_event(uint32_t retro_key, int pressed)
{
    if (retro_key == RETROK_UNKNOWN)
        return;

    if (tab5_guest_input_queue_key(retro_key, pressed))
        __atomic_add_fetch(&s_key_events, 1u, __ATOMIC_RELAXED);
}

static void release_previous_report(void)
{
    unsigned i;

    for (i = 0; i < TAB5_USB_BOOT_KEYS; ++i)
    {
        const uint8_t usage = s_prev_keys[i];
        if (usage > 3u && !host_hotkey_usage(usage))
            queue_retro_event(tab5_usb_hid_usage_to_retro(usage), 0);
    }

    for (i = 0; i < 8u; ++i)
    {
        if (s_prev_modifier & (1u << i))
            queue_retro_event(tab5_usb_modifier_to_retro((uint8_t)i), 0);
    }

    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    s_prev_modifier = 0;
}

static void process_boot_keyboard_report(const uint8_t *data, size_t length)
{
    uint8_t modifier;
    const uint8_t *keys;
    unsigned i;

    if (!data || length < TAB5_USB_BOOT_REPORT_SIZE)
        return;

    modifier = data[0];
    keys = &data[2];

    __atomic_add_fetch(&s_reports, 1u, __ATOMIC_RELAXED);

    /* Release ordinary keys that disappeared from the six-key array. */
    for (i = 0; i < TAB5_USB_BOOT_KEYS; ++i)
    {
        const uint8_t old_usage = s_prev_keys[i];
        if (old_usage > 3u && !key_found(keys, old_usage) &&
            !host_hotkey_usage(old_usage))
            queue_retro_event(tab5_usb_hid_usage_to_retro(old_usage), 0);
    }

    /* Release modifiers before pressing a replacement modifier. */
    for (i = 0; i < 8u; ++i)
    {
        const uint8_t mask = (uint8_t)(1u << i);
        if ((s_prev_modifier & mask) && !(modifier & mask))
            queue_retro_event(tab5_usb_modifier_to_retro((uint8_t)i), 0);
    }

    /* Modifiers must be down before a newly pressed character. */
    for (i = 0; i < 8u; ++i)
    {
        const uint8_t mask = (uint8_t)(1u << i);
        if (!(s_prev_modifier & mask) && (modifier & mask))
            queue_retro_event(tab5_usb_modifier_to_retro((uint8_t)i), 1);
    }

    for (i = 0; i < TAB5_USB_BOOT_KEYS; ++i)
    {
        const uint8_t usage = keys[i];
        /* 0x01..0x03 are USB HID ErrorRollOver/POSTFail/ErrorUndefined. */
        if (usage > 3u && !key_found(s_prev_keys, usage))
        {
            if (host_hotkey_usage(usage))
                queue_host_hotkey(usage);
            else
                queue_retro_event(tab5_usb_hid_usage_to_retro(usage), 1);
        }
    }

    memcpy(s_prev_keys, keys, sizeof(s_prev_keys));
    s_prev_modifier = modifier;
}

static void process_boot_mouse_report(const uint8_t *data, size_t length)
{
    int dx, dy;
    uint8_t buttons;

    if (!data || length < TAB5_USB_BOOT_MOUSE_SIZE)
        return;

    buttons = (uint8_t)(data[0] & 0x03u);
    dx = (int)(int8_t)data[1];
    dy = (int)(int8_t)data[2];

    if (tab5_guest_input_queue_mouse(dx, dy, buttons))
        __atomic_add_fetch(&s_mouse_events, 1u, __ATOMIC_RELAXED);
}

static void hid_interface_callback(hid_host_device_handle_t handle,
                                   const hid_host_interface_event_t event,
                                   void *arg)
{
    (void)arg;

    switch (event)
    {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        {
            uint8_t data[64];
            size_t length = 0;
            hid_host_dev_params_t params;

            if (hid_host_device_get_params(handle, &params) != ESP_OK)
                return;

            if (hid_host_device_get_raw_input_report_data(handle,
                                                          data,
                                                          sizeof(data),
                                                          &length) != ESP_OK)
                return;

            if (params.proto == HID_PROTOCOL_KEYBOARD)
                process_boot_keyboard_report(data, length);
            else if (params.proto == HID_PROTOCOL_MOUSE)
                process_boot_mouse_report(data, length);
            else if (handle == s_joypad_handle)
                process_generic_joypad_report(data, length);
            break;
        }

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        {
            /* Build 5.98g6: close in the HID interface callback, matching the
             * Espressif HID-host example.  The former zero-wait handoff to a
             * separate control queue delayed release of endpoint/DMA objects;
             * repeated low-speed JoyPAD reconnects then reached device_open
             * with a badly fragmented internal heap. */
            const char *kind = "Interface";
            esp_err_t close_err;

            if (handle == s_keyboard_handle)
            {
                release_previous_report();
                __atomic_store_n(&s_connected, 0u, __ATOMIC_RELEASE);
                s_keyboard_handle = NULL;
                kind = "Keyboard";
            }
            else if (handle == s_mouse_handle)
            {
                (void)tab5_guest_input_queue_mouse(0, 0, 0);
                __atomic_store_n(&s_mouse_connected, 0u, __ATOMIC_RELEASE);
                s_mouse_handle = NULL;
                kind = "Mouse";
            }
            else if (handle == s_joypad_handle)
            {
                (void)tab5_guest_input_queue_joypad(0);
                __atomic_store_n(&s_joypad_connected, 0u, __ATOMIC_RELEASE);
                __atomic_store_n(&s_joypad_recognized, 0u, __ATOMIC_RELEASE);
                s_joypad_handle = NULL;
                pad_live_reset(); /* keep s_pad_profile across re-enumeration */
                kind = "JoyPAD candidate";
            }

            close_err = hid_host_device_close(handle);
            {
                const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
                const size_t free_dma = heap_caps_get_free_size(caps);
                const size_t largest_dma = heap_caps_get_largest_free_block(caps);
                if (close_err == ESP_OK)
                    ESP_LOGI(TAG, "USB HID %s DISCONNECTED close=OK dma_free=%u largest=%u",
                             kind, (unsigned)free_dma, (unsigned)largest_dma);
                else
                    ESP_LOGW(TAG, "USB HID %s DISCONNECTED close=%s dma_free=%u largest=%u",
                             kind, esp_err_to_name(close_err),
                             (unsigned)free_dma, (unsigned)largest_dma);
            }
            break;
        }

        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "USB HID transfer error");
            break;

        default:
            break;
    }
}

static void hid_driver_callback(hid_host_device_handle_t handle,
                                const hid_host_driver_event_t event,
                                void *arg)
{
    usb_ctrl_event_t ctrl;
    (void)arg;

    if (event != HID_HOST_DRIVER_EVENT_CONNECTED || !s_ctrl_queue)
        return;

    ctrl.type = USB_CTRL_CONNECT;
    ctrl.handle = handle;
    (void)xQueueSend(s_ctrl_queue, &ctrl, 0);
}

static void usb_control_task(void *arg)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    {
        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);
        esp_rom_printf("R56K5_TASKSELF name=tab5_usb_ctl core=%d base=0x%08x top=0x%08x bytes=4096 hwm=%u\\n",
                       (int)xPortGetCoreID(), (unsigned)r56k5_base,
                       (unsigned)(r56k5_base + 4096u),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif

    usb_ctrl_event_t ctrl;
    (void)arg;

    for (;;)
    {
        if (xQueueReceive(s_ctrl_queue, &ctrl, portMAX_DELAY) != pdTRUE)
            continue;

        if (ctrl.type == USB_CTRL_CONNECT)
        {
            hid_host_dev_params_t params;
            hid_host_device_config_t device_config = {
                .callback = hid_interface_callback,
                .callback_arg = NULL,
            };
            esp_err_t err;

            if (hid_host_device_get_params(ctrl.handle, &params) != ESP_OK)
                continue;

            if (params.proto != HID_PROTOCOL_KEYBOARD &&
                params.proto != HID_PROTOCOL_MOUSE &&
                params.proto != HID_PROTOCOL_NONE)
            {
                ESP_LOGI(TAG, "HID interface ignored: proto=%d", (int)params.proto);
                continue;
            }

            if (params.proto == HID_PROTOCOL_KEYBOARD && s_keyboard_handle)
            {
                ESP_LOGW(TAG, "Additional USB keyboard ignored");
                continue;
            }
            if (params.proto == HID_PROTOCOL_MOUSE && s_mouse_handle)
            {
                ESP_LOGW(TAG, "Additional USB mouse ignored");
                continue;
            }
            if (params.proto == HID_PROTOCOL_NONE && s_joypad_handle)
            {
                ESP_LOGW(TAG, "Additional generic HID interface ignored");
                continue;
            }

            err = hid_host_device_open(ctrl.handle, &device_config);
            if (err != ESP_OK)
            {
                const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
                ESP_LOGE(TAG, "hid_host_device_open failed: %s dma_free=%u largest=%u",
                         esp_err_to_name(err),
                         (unsigned)heap_caps_get_free_size(caps),
                         (unsigned)heap_caps_get_largest_free_block(caps));
                continue;
            }

            if (params.proto == HID_PROTOCOL_KEYBOARD || params.proto == HID_PROTOCOL_MOUSE)
            {
                if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE)
                {
                    ESP_LOGW(TAG,
                             "HID keyboard/mouse has no Boot subclass; unsupported");
                    (void)hid_host_device_close(ctrl.handle);
                    continue;
                }

                err = hid_class_request_set_protocol(ctrl.handle,
                                                     HID_REPORT_PROTOCOL_BOOT);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "set BOOT protocol failed: %s", esp_err_to_name(err));
                    (void)hid_host_device_close(ctrl.handle);
                    continue;
                }

                /* 0 duration/report-id asks the device for its normal idle behavior. */
                (void)hid_class_request_set_idle(ctrl.handle, 0, 0);
            }

            err = hid_host_device_start(ctrl.handle);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "hid_host_device_start failed: %s", esp_err_to_name(err));
                (void)hid_host_device_close(ctrl.handle);
                continue;
            }

            if (params.proto == HID_PROTOCOL_KEYBOARD)
            {
                memset(s_prev_keys, 0, sizeof(s_prev_keys));
                s_prev_modifier = 0;
                s_keyboard_handle = ctrl.handle;
                __atomic_store_n(&s_connected, 1u, __ATOMIC_RELEASE);
                ESP_LOGI(TAG, "USB HID Boot Keyboard CONNECTED");
            }
            else if (params.proto == HID_PROTOCOL_MOUSE)
            {
                s_mouse_handle = ctrl.handle;
                __atomic_store_n(&s_mouse_connected, 1u, __ATOMIC_RELEASE);
                (void)tab5_guest_input_queue_mouse(0, 0, 0);
                ESP_LOGI(TAG, "USB HID Boot Mouse CONNECTED");
            }
            else
            {
                pad_live_reset(); /* first input report restores compatible learned profile */
                s_joypad_handle = ctrl.handle;
                __atomic_store_n(&s_joypad_connected, 1u, __ATOMIC_RELEASE);
                __atomic_store_n(&s_joypad_recognized, 0u, __ATOMIC_RELEASE);
                (void)tab5_guest_input_queue_joypad(0);
                ESP_LOGI(TAG,
                         "USB HID Generic interface CONNECTED: gamepad candidate (raw auto-map enabled)");
            }
        }
        /* Disconnect is handled synchronously in hid_interface_callback(). */
    }
}


/* Build 6.00: passive topology/enumeration observer removed.
 * HID host callbacks are the sole USB client; reconnect behavior does not
 * depend on the retired descriptor logger. */

static void usb_library_task(void *arg)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    {
        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);
        esp_rom_printf("R56K5_TASKSELF name=tab5_usb_lib core=%d base=0x%08x top=0x%08x bytes=4096 hwm=%u\\n",
                       (int)xPortGetCoreID(), (unsigned)r56k5_base,
                       (unsigned)(r56k5_base + 4096u),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif

    TaskHandle_t starter = (TaskHandle_t)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    s_usb_install_result = usb_host_install(&config);
    xTaskNotifyGive(starter);

    if (s_usb_install_result != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "usb_host_install failed: %s",
                 esp_err_to_name(s_usb_install_result));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB Host library installed");
#if CONFIG_USB_HOST_HUBS_SUPPORTED
    ESP_LOGI(TAG, "USB external HUB support: ENABLED (CONFIG_USB_HOST_HUBS_SUPPORTED=y)");
#else
    ESP_LOGW(TAG, "USB external HUB support: DISABLED - direct devices only");
#endif

    for (;;)
    {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            (void)usb_host_device_free_all();
    }
}

int tab5_usb_keyboard_start(void)
{
    hid_host_driver_config_t hid_config;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    BaseType_t task_ok;
    esp_err_t err;

    __atomic_store_n(&s_connected, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_key_events, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_reports, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_connected, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_events, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_joypad_connected, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_joypad_reports, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_joypad_events, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_joypad_recognized, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_hotkeys, 0u, __ATOMIC_RELAXED);
    s_keyboard_handle = NULL;
    s_mouse_handle = NULL;
    s_joypad_handle = NULL;
    pad_profile_reset();
    pad_live_reset();
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    s_prev_modifier = 0;

    if (!s_ctrl_queue)
        s_ctrl_queue = xQueueCreate(TAB5_USB_CTRL_QUEUE_DEPTH,
                                    sizeof(usb_ctrl_event_t));
    if (!s_ctrl_queue)
    {
        ESP_LOGE(TAG, "Unable to allocate USB control queue");
        return 0;
    }
    xQueueReset(s_ctrl_queue);

    #if portNUM_PROCESSORS > 1
    task_ok = xTaskCreatePinnedToCore(usb_library_task,
                          "tab5_usb_lib",
                          4096,
                          (void *)self,
                          TAB5_USB_LIB_TASK_PRIO,
                          &s_usb_lib_task,
                          0);
#else
    task_ok = xTaskCreate(usb_library_task,
                          "tab5_usb_lib",
                          4096,
                          (void *)self,
                          TAB5_USB_LIB_TASK_PRIO,
                          &s_usb_lib_task);
#endif
    if (task_ok != pdPASS)
    {
        ESP_LOGE(TAG, "Unable to create USB library task");
        return 0;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0)
    {
        ESP_LOGE(TAG, "USB Host install timed out");
        return 0;
    }

    if (s_usb_install_result != ESP_OK)
        return 0;

    #if portNUM_PROCESSORS > 1
    task_ok = xTaskCreatePinnedToCore(usb_control_task,
                          "tab5_usb_ctl",
                          4096,
                          NULL,
                          TAB5_USB_CTRL_TASK_PRIO,
                          &s_usb_ctl_task,
                          0);
#else
    task_ok = xTaskCreate(usb_control_task,
                          "tab5_usb_ctl",
                          4096,
                          NULL,
                          TAB5_USB_CTRL_TASK_PRIO,
                          &s_usb_ctl_task);
#endif
    if (task_ok != pdPASS)
    {
        ESP_LOGE(TAG, "Unable to create USB HID control task");
        return 0;
    }

    memset(&hid_config, 0, sizeof(hid_config));
    hid_config.create_background_task = true;
    hid_config.task_priority = TAB5_USB_HID_TASK_PRIO;
    hid_config.stack_size = 4096;
    hid_config.core_id = 0; /* Build 5.47: host-side USB work follows ESP-IDF/system work on CPU0. */
    hid_config.callback = hid_driver_callback;
    hid_config.callback_arg = NULL;

    err = hid_host_install(&hid_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(err));
        return 0;
    }

    ESP_LOGI(TAG, "USB HID host ready on CPU0; waiting for keyboard/mouse/standard HID JoyPAD on USB-A");
    ESP_LOGI(TAG, "USB RC stability: lib/hid/ctl priority=%u/%u/%u; root reset-recovery=80ms",
             (unsigned)TAB5_USB_LIB_TASK_PRIO, (unsigned)TAB5_USB_HID_TASK_PRIO,
             (unsigned)TAB5_USB_CTRL_TASK_PRIO);
    return 1;
}

int tab5_usb_keyboard_connected(void)
{
    return __atomic_load_n(&s_connected, __ATOMIC_ACQUIRE) ? 1 : 0;
}

uint32_t tab5_usb_keyboard_event_count(void)
{
    return __atomic_load_n(&s_key_events, __ATOMIC_RELAXED);
}

uint32_t tab5_usb_keyboard_report_count(void)
{
    return __atomic_load_n(&s_reports, __ATOMIC_RELAXED);
}

int tab5_usb_mouse_connected(void)
{
    return __atomic_load_n(&s_mouse_connected, __ATOMIC_ACQUIRE) ? 1 : 0;
}

uint32_t tab5_usb_mouse_event_count(void)
{
    return __atomic_load_n(&s_mouse_events, __ATOMIC_RELAXED);
}

int tab5_usb_joypad_connected(void)
{
    return __atomic_load_n(&s_joypad_connected, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int tab5_usb_joypad_recognized(void)
{
    return __atomic_load_n(&s_joypad_recognized, __ATOMIC_ACQUIRE) ? 1 : 0;
}

uint32_t tab5_usb_joypad_report_count(void)
{
    return __atomic_load_n(&s_joypad_reports, __ATOMIC_RELAXED);
}

uint32_t tab5_usb_joypad_event_count(void)
{
    return __atomic_load_n(&s_joypad_events, __ATOMIC_RELAXED);
}

uint32_t tab5_usb_keyboard_take_hotkeys(void)
{
    return __atomic_exchange_n(&s_hotkeys, 0u, __ATOMIC_ACQ_REL);
}

void tab5_usb_keyboard_stack_highwater(uint32_t *lib_bytes, uint32_t *ctl_bytes)
{
    if (lib_bytes) *lib_bytes = s_usb_lib_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_usb_lib_task) : 0u;
    if (ctl_bytes) *ctl_bytes = s_usb_ctl_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_usb_ctl_task) : 0u;
}
