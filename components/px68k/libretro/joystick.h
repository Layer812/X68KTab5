/*
 * PX68K source modified for the Tab5 port.
 * Intent: Tab5 standalone joystick-state bridge declarations.
 * Layer8 Aug/17/2026
 */
#ifndef _WINX68K_JOY_H
#define _WINX68K_JOY_H

#include <stdint.h>
#include "common.h"

#define PAD_2BUTTON      0
#define PAD_CPSF_MD      1
#define PAD_CPSF_SFC     2
#define PAD_CYBERSTICK_D 3
#define PAD_CYBERSTICK_A 4

#define	JOY_UP		0x01
#define	JOY_DOWN	0x02
#define	JOY_LEFT	0x04
#define	JOY_RIGHT	0x08
#define	JOY_TRG2	0x20
#define	JOY_TRG1	0x40

#define	JOY_TRG5	0x01
#define	JOY_TRG4	0x02
#define	JOY_TRG3	0x04
#define	JOY_TRG7	0x08
#define	JOY_TRG8	0x20
#define	JOY_TRG6	0x40

/* button combination for start/select button */
#define JOY_SELECT      0x03 /* up + down */
#define JOY_START       0x0c /* left + right */

void Joystick_Init(void);
void Joystick_Cleanup(void);
uint8_t FASTCALL Joystick_Read(uint8_t num);
void FASTCALL Joystick_Write(uint8_t num, uint8_t data);
void FASTCALL Joystick_Update(int is_menu, int key, int port);
/* Standalone host bridge. Low byte keeps the legacy active-high JOY_* bits.
 * The upper bits carry six-button extension inputs learned by the Tab5 USB
 * host.  The bridge presents them through PX68K's existing CPSF/MD two-bank
 * protocol, while ordinary two-button software keeps seeing bank 0. */
#define JOY_HOST_BTN3   0x0100u
#define JOY_HOST_BTN4   0x0200u
#define JOY_HOST_BTN5   0x0400u
#define JOY_HOST_BTN6   0x0800u
#define JOY_HOST_L      0x1000u
#define JOY_HOST_R      0x2000u
/* Tab5 software-side virtual controls.  Keep START separate from physical L/R
 * in the host bitmask, then present it as the CPSF/MD bank-1 Start line. */
#define JOY_HOST_START  0x4000u
#define JOY_HOST_MODE   0x8000u

void Joystick_SetHost2ButtonState(int port, uint8_t pressed_mask);
void Joystick_SetHost8ButtonState(int port, uint16_t pressed_mask);

uint8_t get_joy_downstate(void);
void reset_joy_downstate(void);

extern uint8_t JoyKeyState;

#endif /* WINX68K_JOY_H */
