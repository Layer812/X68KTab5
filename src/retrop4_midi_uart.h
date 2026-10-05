#ifndef RETROP4_MIDI_UART_H
#define RETROP4_MIDI_UART_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rp_midi_uart_set_master_volume(uint8_t level);
void rp_midi_uart_adjust_master_volume(int direction);

bool rp_midi_uart_start(void);
bool rp_midi_uart_ready(void);
void *rp_midi_uart_handle(void);

void rp_midi_uart_annotate_next_guest_cycle(uint32_t guest_cycle);
bool rp_midi_uart_submit_byte(uint8_t byte);

void rp_midi_diag_event(uint8_t kind, uint32_t value);
void rp_midi_diag_guest_tdr_byte(uint8_t byte);

uint32_t rp_midi_uart_overflow_count(void);

#ifdef __cplusplus
}
#endif

#endif /* RETROP4_MIDI_UART_H */
