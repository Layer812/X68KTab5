/*
 * Tab5 port-specific implementation.
 * Intent: Shared input-state contract between the CPU0 USB host and the CPU1 X68000 guest.
 * Layer8 Aug/17/2026
 */
#ifndef TAB5_GUEST_INPUT_H
#define TAB5_GUEST_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void tab5_guest_input_init(void);
void tab5_guest_input_set_interval_frames(uint32_t frames);
int  tab5_guest_input_queue_text(const char *text);
void tab5_guest_input_cancel_text(void);

/* Thread-safe real-time input path. May be called from USB HID tasks. */
int  tab5_guest_input_queue_key(uint32_t retro_key, int pressed);
/* Direct X68000 scan-code path used only by the Tab5 software keyboard. */
int  tab5_guest_input_queue_x68k_scancode(uint8_t scancode, int pressed);
/* Buffered software-keyboard tap.  modifiers: bit0=SHIFT, bit1=CTRL. */
int  tab5_guest_input_queue_x68k_tap(uint8_t scancode, uint8_t modifiers);
int  tab5_guest_input_queue_mouse(int dx, int dy, uint8_t buttons);
int  tab5_guest_input_queue_joypad(uint16_t joy_bits);
/* CPU0 touchscreen side controls are a second JOY1 source, ORed with USB. */
int  tab5_guest_input_queue_touch_joypad(uint16_t joy_bits);

/* Must be called only from the PX68K/emulation task. */
void tab5_guest_input_tick(uint32_t frame);

/* Text queue status retained for the automatic console regression. */
int    tab5_guest_input_busy(void);
size_t tab5_guest_input_pending(void);
uint32_t tab5_guest_input_sent_chars(void);

/* Real-time physical-input diagnostics. */
size_t   tab5_guest_input_realtime_pending(void);
uint32_t tab5_guest_input_realtime_events(void);
uint32_t tab5_guest_input_realtime_dropped(void);
uint32_t tab5_guest_input_mouse_events(void);
uint32_t tab5_guest_input_joypad_events(void);
uint16_t tab5_guest_input_joypad_state(void);

#ifdef __cplusplus
}
#endif

#endif /* TAB5_GUEST_INPUT_H */
