#ifndef MIDI_ENGINE_H
#define MIDI_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_ENGINE_CHANNELS 16u
#define MIDI_ENGINE_SPECTRUM_BINS 24u
#define MIDI_ENGINE_TITLE_MAX 64u
#define MIDI_ENGINE_PROFILE_LABEL_MAX 48u

/* Generic host/output boundary. The engine is intentionally agnostic to the
 * target synth and to the transformation policy placed in front of it. */
typedef void (*midi_engine_write_fn)(void *user, const uint8_t *bytes, size_t len);
typedef void (*midi_engine_volume_fn)(void *user, uint8_t volume);

typedef struct midi_engine_backend {
    midi_engine_write_fn write;
    midi_engine_volume_fn set_master_volume; /* optional */
    void *user;
} midi_engine_backend_t;

/* Pluggable byte-stream processor. A processor may pass bytes through,
 * translate them, expand one source message into several target messages, or
 * suppress unsupported messages. Processor implementations receive an output
 * callback from midi_engine_output(). */
typedef void (*midi_processor_reset_fn)(void *user);
typedef void (*midi_processor_feed_fn)(void *user, uint8_t byte);

typedef struct midi_processor {
    midi_processor_reset_fn reset;
    midi_processor_feed_fn feed_byte;
    void *user;
} midi_processor_t;

typedef enum midi_engine_play_state {
    MIDI_ENGINE_STOPPED = 0,
    MIDI_ENGINE_PLAYING = 1,
    MIDI_ENGINE_PAUSED  = 2
} midi_engine_play_state_t;

typedef struct midi_engine_snapshot {
    char title[MIDI_ENGINE_TITLE_MAX];
    char profile[MIDI_ENGINE_PROFILE_LABEL_MAX];
    midi_engine_play_state_t play_state;
    uint8_t master_volume;
    uint8_t loop_enabled;
    uint16_t tempo_bpm;
    uint32_t elapsed_ms;
    uint32_t total_ms; /* 0 means unknown/live source */
    uint8_t channel_activity[MIDI_ENGINE_CHANNELS]; /* 0..127 */
    uint8_t spectrum[MIDI_ENGINE_SPECTRUM_BINS];   /* 0..127, MIDI-derived */
} midi_engine_snapshot_t;

typedef struct midi_engine {
    midi_engine_backend_t backend;
    midi_processor_t processor;
    midi_engine_snapshot_t view;

    /* Input observer parser, independent from the active processor. */
    uint8_t obs_running_status;
    uint8_t obs_status;
    uint8_t obs_data[2];
    uint8_t obs_have;
    uint8_t obs_need;
    uint8_t obs_in_sysex;

    /* Small output-side reset detector so pass-through and processors that
     * emit SysEx byte-by-byte still restore host master volume. */
    uint8_t out_sysex[16];
    uint8_t out_sysex_len;
    uint8_t out_in_sysex;

    uint32_t decay_accum_ms;
} midi_engine_t;

/* Starts in lossless pass-through mode. A processor can be installed later. */
void midi_engine_init(midi_engine_t *e,
                      const midi_engine_backend_t *backend,
                      uint8_t master_volume);

void midi_engine_set_processor(midi_engine_t *e, const midi_processor_t *processor);
void midi_engine_use_passthrough(midi_engine_t *e);
void midi_engine_reset(midi_engine_t *e);

/* Preferred raw-byte boundary for emulators / virtual UARTs. */
void midi_engine_feed_byte(midi_engine_t *e, uint8_t byte);
void midi_engine_feed_buffer(midi_engine_t *e, const uint8_t *bytes, size_t len);

/* Event boundary for hosts that already parsed channel messages. */
void midi_engine_feed_channel(midi_engine_t *e,
                              uint8_t status, const uint8_t *data, uint8_t data_len);

/* Output callback intended for processors. Pass the midi_engine_t pointer as
 * user. It also restores the host master volume after reset messages. */
void midi_engine_output(void *user, const uint8_t *bytes, size_t len);

void midi_engine_set_title(midi_engine_t *e, const char *title);
void midi_engine_set_profile_label(midi_engine_t *e, const char *label);
void midi_engine_set_play_state(midi_engine_t *e, midi_engine_play_state_t state);
void midi_engine_set_timeline(midi_engine_t *e, uint32_t elapsed_ms, uint32_t total_ms);
void midi_engine_set_tempo(midi_engine_t *e, uint16_t bpm);
void midi_engine_set_loop(midi_engine_t *e, uint8_t enabled);
void midi_engine_set_master_volume(midi_engine_t *e, uint8_t volume);
uint8_t midi_engine_get_master_volume(const midi_engine_t *e);

/* Periodic service for visualization decay only. */
void midi_engine_tick(midi_engine_t *e, uint32_t elapsed_ms);
void midi_engine_get_snapshot(const midi_engine_t *e, midi_engine_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_ENGINE_H */
