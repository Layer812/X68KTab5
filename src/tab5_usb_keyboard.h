/*
 * Tab5 port-specific implementation.
 * Intent: USB HID host interface and Tab5 hotkey/joypad state definitions.
 * Layer8 Aug/17/2026
 */
#ifndef TAB5_USB_KEYBOARD_H
#define TAB5_USB_KEYBOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int tab5_usb_keyboard_start(void);
int tab5_usb_keyboard_connected(void);
uint32_t tab5_usb_keyboard_event_count(void);
uint32_t tab5_usb_keyboard_report_count(void);
int tab5_usb_mouse_connected(void);
uint32_t tab5_usb_mouse_event_count(void);
int tab5_usb_joypad_connected(void);
int tab5_usb_joypad_recognized(void);
uint32_t tab5_usb_joypad_report_count(void);
uint32_t tab5_usb_joypad_event_count(void);

/*
 * Host-only hotkeys. These are consumed in the USB HID layer and are never
 * forwarded to the X68000 keyboard. The emulation task polls/takes them.
 */
enum
{
    TAB5_USB_HOTKEY_B_TOGGLE    = 1u << 0, /* F11: eject/reinsert B: */
    TAB5_USB_HOTKEY_B_NEXT      = 1u << 1, /* F12: next XDF in B: */
    TAB5_USB_HOTKEY_B_WP         = 1u << 2, /* F10: toggle B: write protect */
    TAB5_USB_HOTKEY_VIDEO_TOGGLE = 1u << 3, /* F9: color text/composite */
    TAB5_USB_HOTKEY_A_BOOT_TOGGLE = 1u << 4, /* F8: Human68k / DISKMAG boot */
    TAB5_USB_HOTKEY_A_BOOT_NEXT   = 1u << 5  /* F7: next root .XDF/.DIM + reset */
};
uint32_t tab5_usb_keyboard_take_hotkeys(void);

#ifdef __cplusplus
}
#endif

#endif /* TAB5_USB_KEYBOARD_H */
