/*
 * Tab5 port-specific implementation.
 * Intent: USB HID to X68000 keymap declarations for the Tab5 port.
 * Layer8 Aug/17/2026
 */
#ifndef TAB5_USB_KEYMAP_H
#define TAB5_USB_KEYMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t tab5_usb_hid_usage_to_retro(uint8_t usage);
uint32_t tab5_usb_modifier_to_retro(uint8_t modifier_bit);

#ifdef __cplusplus
}
#endif

#endif /* TAB5_USB_KEYMAP_H */
