/*
 * Tab5 port-specific implementation.
 * Intent: Cross-core input handoff: convert CPU0 USB HID state into X68000 keyboard, mouse, and joystick state consumed by the guest.
 * Layer8 Aug/17/2026
 */
#include "tab5_guest_input.h"

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "libretro.h"
#include "libretro/keyboard.h"
#include "libretro/mouse.h"
#include "libretro/joystick.h"

#define TAB5_GUEST_INPUT_TEXT_QUEUE_SIZE 128u
#define TAB5_GUEST_INPUT_TEXT_QUEUE_MASK (TAB5_GUEST_INPUT_TEXT_QUEUE_SIZE - 1u)
#define TAB5_GUEST_INPUT_REALTIME_DEPTH  128u
#define TAB5_GUEST_INPUT_REALTIME_BURST    8u
#define TAB5_GUEST_INPUT_SOFTKBD_DEPTH      96u
#define TAB5_GUEST_INPUT_SOFTKBD_INTERVAL    2u

typedef struct
{
    uint32_t key;
    uint8_t pressed;
    uint8_t direct_scancode;
} tab5_key_event_t;

typedef struct
{
    uint8_t scancode;
    uint8_t modifiers; /* bit0=SHIFT, bit1=CTRL */
} tab5_softkbd_event_t;

static uint8_t s_text_queue[TAB5_GUEST_INPUT_TEXT_QUEUE_SIZE];
static uint32_t s_text_rp = 0;
static uint32_t s_text_wp = 0;
static uint32_t s_next_frame = 0;
static uint32_t s_interval_frames = 12;
static uint32_t s_sent_chars = 0;

static QueueHandle_t s_realtime_queue = NULL;
static QueueHandle_t s_softkbd_queue = NULL;
static uint32_t s_softkbd_next_frame = 0;
static uint32_t s_realtime_events = 0;
static uint32_t s_realtime_dropped = 0;

/* USB mouse reports are accumulated from the HID task and consumed only
 * by the emulation task.  This keeps PX68K mouse/SCC state single-threaded. */
static int32_t s_mouse_dx = 0;
static int32_t s_mouse_dy = 0;
static uint32_t s_mouse_buttons = 0;
static uint32_t s_mouse_events = 0;
static uint8_t s_mouse_applied_buttons = 0;

/* USB joypad reports arrive on HID tasks; only the emulation task touches
 * PX68K Joystick state. Bits use JOY_* active-high semantics. */
static uint32_t s_joy_bits = 0;
static uint32_t s_touch_joy_bits = 0;
static uint32_t s_joy_events = 0;
static uint16_t s_joy_applied_bits = 0;

static uint32_t text_queue_next(uint32_t p)
{
    return (p + 1u) & TAB5_GUEST_INPUT_TEXT_QUEUE_MASK;
}

static int text_queue_push(uint8_t c)
{
    const uint32_t next = text_queue_next(s_text_wp);
    if (next == s_text_rp)
        return 0;

    s_text_queue[s_text_wp] = c;
    s_text_wp = next;
    return 1;
}

static int text_queue_pop(uint8_t *out)
{
    if (!out || s_text_rp == s_text_wp)
        return 0;

    *out = s_text_queue[s_text_rp];
    s_text_rp = text_queue_next(s_text_rp);
    return 1;
}

static uint32_t ascii_to_retro_key(uint8_t c)
{
    switch (c)
    {
        case '\r':
        case '\n':
            return RETROK_RETURN;
        case '\t':
            return RETROK_TAB;
        case 0x08:
            return RETROK_BACKSPACE;
        default:
            if (c >= 0x20u && c <= 0x7eu)
                return (uint32_t)c;
            return RETROK_UNKNOWN;
    }
}

static void tap_key(uint32_t key)
{
    if (key == RETROK_UNKNOWN)
        return;

    Keyboard_KeyDown(key);
    Keyboard_KeyUp(key);
}

void tab5_guest_input_init(void)
{
    memset(s_text_queue, 0, sizeof(s_text_queue));
    s_text_rp = 0;
    s_text_wp = 0;
    s_next_frame = 0;
    s_interval_frames = 12;
    s_sent_chars = 0;

    __atomic_store_n(&s_realtime_events, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_realtime_dropped, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_dx, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_dy, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_buttons, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_events, 0u, __ATOMIC_RELAXED);
    s_mouse_applied_buttons = 0;
    __atomic_store_n(&s_joy_bits, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_touch_joy_bits, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_joy_events, 0u, __ATOMIC_RELAXED);
    s_joy_applied_bits = 0;

    if (!s_realtime_queue)
        s_realtime_queue = xQueueCreate(TAB5_GUEST_INPUT_REALTIME_DEPTH,
                                        sizeof(tab5_key_event_t));
    else
        xQueueReset(s_realtime_queue);

    if (!s_softkbd_queue)
        s_softkbd_queue = xQueueCreate(TAB5_GUEST_INPUT_SOFTKBD_DEPTH,
                                       sizeof(tab5_softkbd_event_t));
    else
        xQueueReset(s_softkbd_queue);
    s_softkbd_next_frame = 0;
}

void tab5_guest_input_set_interval_frames(uint32_t frames)
{
    s_interval_frames = frames ? frames : 1u;
}

int tab5_guest_input_queue_text(const char *text)
{
    size_t len;
    size_t free_slots;

    if (!text)
        return 0;

    len = strlen(text);

    if (s_text_wp >= s_text_rp)
        free_slots = (TAB5_GUEST_INPUT_TEXT_QUEUE_SIZE - 1u) -
                     (s_text_wp - s_text_rp);
    else
        free_slots = (s_text_rp - s_text_wp) - 1u;

    if (len > free_slots)
        return 0;

    while (*text)
    {
        if (!text_queue_push((uint8_t)*text++))
            return 0;
    }

    return 1;
}

void tab5_guest_input_cancel_text(void)
{
    s_text_rp = s_text_wp;
}

int tab5_guest_input_queue_key(uint32_t retro_key, int pressed)
{
    tab5_key_event_t event;

    if (!s_realtime_queue || retro_key == RETROK_UNKNOWN)
        return 0;

    event.key = retro_key;
    event.pressed = pressed ? 1u : 0u;
    event.direct_scancode = 0u;

    if (xQueueSend(s_realtime_queue, &event, 0) != pdTRUE)
    {
        __atomic_add_fetch(&s_realtime_dropped, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    __atomic_add_fetch(&s_realtime_events, 1u, __ATOMIC_RELAXED);
    return 1;
}

int tab5_guest_input_queue_x68k_scancode(uint8_t scancode, int pressed)
{
    tab5_key_event_t event;

    if (!s_realtime_queue || scancode == 0u || scancode >= 0x80u)
        return 0;

    event.key = scancode;
    event.pressed = pressed ? 1u : 0u;
    event.direct_scancode = 1u;

    if (xQueueSend(s_realtime_queue, &event, 0) != pdTRUE)
    {
        __atomic_add_fetch(&s_realtime_dropped, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    __atomic_add_fetch(&s_realtime_events, 1u, __ATOMIC_RELAXED);
    return 1;
}

int tab5_guest_input_queue_x68k_tap(uint8_t scancode, uint8_t modifiers)
{
    tab5_softkbd_event_t event;

    if (!s_softkbd_queue || scancode == 0u || scancode >= 0x80u)
        return 0;

    event.scancode = scancode;
    event.modifiers = (uint8_t)(modifiers & 0x03u);
    if (xQueueSend(s_softkbd_queue, &event, 0) != pdTRUE)
        return 0;
    return 1;
}

int tab5_guest_input_queue_mouse(int dx, int dy, uint8_t buttons)
{
    __atomic_add_fetch(&s_mouse_dx, (int32_t)dx, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_mouse_dy, (int32_t)dy, __ATOMIC_RELAXED);
    __atomic_store_n(&s_mouse_buttons, (uint32_t)(buttons & 0x03u), __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_mouse_events, 1u, __ATOMIC_RELAXED);
    return 1;
}

int tab5_guest_input_queue_joypad(uint16_t joy_bits)
{
    const uint16_t mask = (uint16_t)(JOY_UP | JOY_DOWN | JOY_LEFT | JOY_RIGHT |
                                     JOY_TRG1 | JOY_TRG2 |
                                     JOY_HOST_BTN3 | JOY_HOST_BTN4 |
                                     JOY_HOST_BTN5 | JOY_HOST_BTN6 |
                                     JOY_HOST_L | JOY_HOST_R | JOY_HOST_START | JOY_HOST_MODE);
    __atomic_store_n(&s_joy_bits, (uint32_t)(joy_bits & mask), __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_joy_events, 1u, __ATOMIC_RELAXED);
    return 1;
}

int tab5_guest_input_queue_touch_joypad(uint16_t joy_bits)
{
    const uint16_t mask = (uint16_t)(JOY_UP | JOY_DOWN | JOY_LEFT | JOY_RIGHT |
                                     JOY_TRG1 | JOY_TRG2 |
                                     JOY_HOST_BTN3 | JOY_HOST_BTN4 |
                                     JOY_HOST_BTN5 | JOY_HOST_BTN6 |
                                     JOY_HOST_L | JOY_HOST_R | JOY_HOST_START | JOY_HOST_MODE);
    __atomic_store_n(&s_touch_joy_bits, (uint32_t)(joy_bits & mask), __ATOMIC_RELEASE);
    return 1;
}

void tab5_guest_input_tick(uint32_t frame)
{
    tab5_key_event_t event;
    uint8_t c;
    unsigned drained = 0;

    /* Apply the latest USB joypad state on the emulation task. */
    {
        /*
         * Intent: USB PAD and the Tab5 side-bar touch controller are peers;
         * OR their active-high states so either can be used, including
         * simultaneous direction + button touches.
         * Layer8 Aug/17/2026
         */
        uint16_t joy = (uint16_t)(
            __atomic_load_n(&s_joy_bits, __ATOMIC_ACQUIRE) |
            __atomic_load_n(&s_touch_joy_bits, __ATOMIC_ACQUIRE));
        if ((joy & (JOY_UP | JOY_DOWN)) == (JOY_UP | JOY_DOWN))
            joy &= (uint16_t)~(JOY_UP | JOY_DOWN);
        if ((joy & (JOY_LEFT | JOY_RIGHT)) == (JOY_LEFT | JOY_RIGHT))
            joy &= (uint16_t)~(JOY_LEFT | JOY_RIGHT);
        if (joy != s_joy_applied_bits)
        {
            Joystick_SetHost8ButtonState(0, joy);
            s_joy_applied_bits = joy;
        }
    }

    /* Apply the latest accumulated USB mouse state on the emulation task. */
    {
        const int32_t mdx = __atomic_exchange_n(&s_mouse_dx, 0, __ATOMIC_ACQ_REL);
        const int32_t mdy = __atomic_exchange_n(&s_mouse_dy, 0, __ATOMIC_ACQ_REL);
        const uint8_t buttons = (uint8_t)__atomic_load_n(&s_mouse_buttons, __ATOMIC_ACQUIRE);

        if (mdx || mdy)
            Mouse_Event(0, (float)mdx, (float)mdy);

        if ((buttons ^ s_mouse_applied_buttons) & 0x01u)
            Mouse_Event(1, (buttons & 0x01u) ? 1.0f : 0.0f, 0.0f);
        if ((buttons ^ s_mouse_applied_buttons) & 0x02u)
            Mouse_Event(2, (buttons & 0x02u) ? 1.0f : 0.0f, 0.0f);

        s_mouse_applied_buttons = buttons;
    }

    /*
     * Physical input is delivered first and only from the emulation task.
     * This keeps PX68K's KeyBuf/MFP state single-threaded even though USB
     * reports arrive from HID/USB tasks.
     */
    while (s_realtime_queue &&
           drained < TAB5_GUEST_INPUT_REALTIME_BURST &&
           xQueueReceive(s_realtime_queue, &event, 0) == pdTRUE)
    {
        if (event.direct_scancode)
        {
            if (event.pressed)
                Keyboard_ScanCodeDown((uint8_t)event.key);
            else
                Keyboard_ScanCodeUp((uint8_t)event.key);
        }
        else
        {
            if (event.pressed)
                Keyboard_KeyDown(event.key);
            else
                Keyboard_KeyUp(event.key);
        }
        ++drained;
    }

    /* Buffered on-screen keyboard SEND path.  It has its own queue so a long
     * command cannot starve physical USB/touch events.  Emit one X68000 key
     * tap every two guest frames, including modifier make/break around it. */
    if (s_softkbd_queue && frame >= s_softkbd_next_frame)
    {
        tab5_softkbd_event_t sk;
        if (xQueueReceive(s_softkbd_queue, &sk, 0) == pdTRUE)
        {
            if (sk.modifiers & 0x01u) Keyboard_ScanCodeDown(0x70u);
            if (sk.modifiers & 0x02u) Keyboard_ScanCodeDown(0x71u);
            Keyboard_ScanCodeDown(sk.scancode);
            Keyboard_ScanCodeUp(sk.scancode);
            if (sk.modifiers & 0x02u) Keyboard_ScanCodeUp(0x71u);
            if (sk.modifiers & 0x01u) Keyboard_ScanCodeUp(0x70u);
            s_softkbd_next_frame = frame + TAB5_GUEST_INPUT_SOFTKBD_INTERVAL;
        }
    }

    if (s_text_rp == s_text_wp || frame < s_next_frame)
        return;

    if (!text_queue_pop(&c))
        return;

    tap_key(ascii_to_retro_key(c));
    ++s_sent_chars;
    s_next_frame = frame + s_interval_frames;
}

int tab5_guest_input_busy(void)
{
    return s_text_rp != s_text_wp;
}

size_t tab5_guest_input_pending(void)
{
    if (s_text_wp >= s_text_rp)
        return (size_t)(s_text_wp - s_text_rp);

    return (size_t)(TAB5_GUEST_INPUT_TEXT_QUEUE_SIZE -
                    (s_text_rp - s_text_wp));
}

uint32_t tab5_guest_input_sent_chars(void)
{
    return s_sent_chars;
}

size_t tab5_guest_input_realtime_pending(void)
{
    return s_realtime_queue ? (size_t)uxQueueMessagesWaiting(s_realtime_queue) : 0u;
}

uint32_t tab5_guest_input_realtime_events(void)
{
    return __atomic_load_n(&s_realtime_events, __ATOMIC_RELAXED);
}

uint32_t tab5_guest_input_realtime_dropped(void)
{
    return __atomic_load_n(&s_realtime_dropped, __ATOMIC_RELAXED);
}

uint32_t tab5_guest_input_mouse_events(void)
{
    return __atomic_load_n(&s_mouse_events, __ATOMIC_RELAXED);
}

uint32_t tab5_guest_input_joypad_events(void)
{
    return __atomic_load_n(&s_joy_events, __ATOMIC_RELAXED);
}

uint16_t tab5_guest_input_joypad_state(void)
{
    return (uint16_t)(
        __atomic_load_n(&s_joy_bits, __ATOMIC_ACQUIRE) |
        __atomic_load_n(&s_touch_joy_bits, __ATOMIC_ACQUIRE));
}
