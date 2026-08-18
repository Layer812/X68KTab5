/*
 * Tab5 port-specific implementation.
 * Intent: Translate USB HID keyboard usages to the X68000 keyboard matrix used by the guest.
 * Layer8 Aug/17/2026
 */
#include "tab5_usb_keymap.h"

#include "libretro.h"

uint32_t tab5_usb_modifier_to_retro(uint8_t modifier_bit)
{
    switch (modifier_bit)
    {
        case 0: return RETROK_LCTRL;
        case 1: return RETROK_LSHIFT;
        case 2: return RETROK_LALT;
        case 4: return RETROK_RCTRL;
        case 5: return RETROK_RSHIFT;
        case 6: return RETROK_RALT;
        default: return RETROK_UNKNOWN;
    }
}

uint32_t tab5_usb_hid_usage_to_retro(uint8_t usage)
{
    if (usage >= 0x04u && usage <= 0x1Du)
        return (uint32_t)RETROK_a + (uint32_t)(usage - 0x04u);

    if (usage >= 0x1Eu && usage <= 0x26u)
        return (uint32_t)RETROK_1 + (uint32_t)(usage - 0x1Eu);

    if (usage == 0x27u)
        return RETROK_0;

    switch (usage)
    {
        case 0x28: return RETROK_RETURN;
        case 0x29: return RETROK_ESCAPE;
        case 0x2A: return RETROK_BACKSPACE;
        case 0x2B: return RETROK_TAB;
        case 0x2C: return RETROK_SPACE;
        /*
         * Tab5 default physical keyboard layout: Japanese 106/109 (JIS).
         *
         * USB HID usage names are based on the US keyboard legends, but the
         * usages identify physical key positions.  Feeding those US semantic
         * names directly into PX68K therefore produces the wrong X68000 key
         * for several punctuation keys on a JIS keyboard.
         *
         * Translate the JIS physical positions to the corresponding X68000
         * Japanese-layout keys instead.  Shift is still delivered separately
         * by tab5_usb_modifier_to_retro(), so e.g. usage 0x34 becomes ':'
         * unshifted and '*' shifted, exactly like the JIS/X68000 key.
         */
        case 0x2D: return RETROK_MINUS;        /* JIS - / =   */
        case 0x2E: return RETROK_CARET;        /* JIS ^ / ~   */
        case 0x2F: return RETROK_AT;           /* JIS @ / `   */
        case 0x30: return RETROK_LEFTBRACKET;  /* JIS [ / {   */
        case 0x31: return RETROK_RIGHTBRACKET; /* JIS ] / }   */
        case 0x32: return RETROK_RIGHTBRACKET; /* non-US/JIS fallback */
        case 0x33: return RETROK_SEMICOLON;    /* JIS ; / +   */
        case 0x34: return RETROK_COLON;        /* JIS : / *   */
        case 0x35: return RETROK_BACKQUOTE;    /* half/full-width key varies by keyboard */
        case 0x36: return RETROK_COMMA;
        case 0x37: return RETROK_PERIOD;
        case 0x38: return RETROK_SLASH;
        case 0x39: return RETROK_CAPSLOCK;

        case 0x3A: return RETROK_F1;
        case 0x3B: return RETROK_F2;
        case 0x3C: return RETROK_F3;
        case 0x3D: return RETROK_F4;
        case 0x3E: return RETROK_F5;
        case 0x3F: return RETROK_F6;
        case 0x40: return RETROK_F7;
        case 0x41: return RETROK_F8;
        case 0x42: return RETROK_F9;
        case 0x43: return RETROK_F10;

        case 0x49: return RETROK_INSERT;
        case 0x4A: return RETROK_HOME;
        case 0x4B: return RETROK_PAGEUP;
        case 0x4C: return RETROK_DELETE;
        case 0x4D: return RETROK_END;
        case 0x4E: return RETROK_PAGEDOWN;
        case 0x4F: return RETROK_RIGHT;
        case 0x50: return RETROK_LEFT;
        case 0x51: return RETROK_DOWN;
        case 0x52: return RETROK_UP;

        case 0x53: return RETROK_NUMLOCK;
        case 0x54: return RETROK_KP_DIVIDE;
        case 0x55: return RETROK_KP_MULTIPLY;
        case 0x56: return RETROK_KP_MINUS;
        case 0x57: return RETROK_KP_PLUS;
        case 0x58: return RETROK_KP_ENTER;
        case 0x59: return RETROK_KP1;
        case 0x5A: return RETROK_KP2;
        case 0x5B: return RETROK_KP3;
        case 0x5C: return RETROK_KP4;
        case 0x5D: return RETROK_KP5;
        case 0x5E: return RETROK_KP6;
        case 0x5F: return RETROK_KP7;
        case 0x60: return RETROK_KP8;
        case 0x61: return RETROK_KP9;
        case 0x62: return RETROK_KP0;
        case 0x63: return RETROK_KP_PERIOD;

        /* Japanese keyboard international usages. */
        case 0x87: return RETROK_UNDERSCORE; /* JIS \ / _ ("Ro" key) */
        case 0x89: return RETROK_BACKSLASH;  /* JIS Yen / | key */

        default:   return RETROK_UNKNOWN;
    }
}
