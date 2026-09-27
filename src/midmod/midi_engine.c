#include "midi_engine.h"

#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0u) return;
    if (src == NULL) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int is_reset_message(const uint8_t *bytes, size_t len)
{
    if (bytes == NULL || len == 0u) return 0;
    if (len == 6u && bytes[0] == 0xf0u && bytes[1] == 0x7eu &&
        bytes[3] == 0x09u && bytes[4] == 0x01u && bytes[5] == 0xf7u) return 1;
    if (len == 11u && bytes[0] == 0xf0u && bytes[1] == 0x41u &&
        bytes[3] == 0x42u && bytes[4] == 0x12u && bytes[5] == 0x40u &&
        bytes[6] == 0x00u && bytes[7] == 0x7fu && bytes[8] == 0x00u &&
        bytes[10] == 0xf7u) return 1;
    return 0;
}

static int output_observe_reset(midi_engine_t *e, uint8_t byte)
{
    if (byte == 0xffu) {
        e->out_in_sysex = 0u;
        e->out_sysex_len = 0u;
        return 1;
    }
    if (byte >= 0xf8u) return 0; /* realtime can appear inside SysEx */
    if (byte == 0xf0u) {
        e->out_in_sysex = 1u;
        e->out_sysex_len = 1u;
        e->out_sysex[0] = byte;
        return 0;
    }
    if (!e->out_in_sysex) return 0;

    if (e->out_sysex_len < sizeof(e->out_sysex))
        e->out_sysex[e->out_sysex_len++] = byte;
    else {
        /* Oversize SysEx cannot be one of the reset frames we recognize. */
        e->out_in_sysex = 0u;
        e->out_sysex_len = 0u;
        return 0;
    }

    if (byte == 0xf7u) {
        const int reset = is_reset_message(e->out_sysex, e->out_sysex_len);
        e->out_in_sysex = 0u;
        e->out_sysex_len = 0u;
        return reset;
    }
    return 0;
}

void midi_engine_output(void *user, const uint8_t *bytes, size_t len)
{
    midi_engine_t *e = (midi_engine_t *)user;
    int reset_seen = 0;
    if (e == NULL || bytes == NULL || len == 0u) return;
    if (e->backend.write != NULL) e->backend.write(e->backend.user, bytes, len);
    for (size_t i = 0u; i < len; ++i)
        if (output_observe_reset(e, bytes[i])) reset_seen = 1;
    if (reset_seen && e->backend.set_master_volume != NULL)
        e->backend.set_master_volume(e->backend.user, e->view.master_volume);
}

static void passthrough_reset(void *user)
{
    (void)user;
}

static void passthrough_feed(void *user, uint8_t byte)
{
    midi_engine_t *e = (midi_engine_t *)user;
    midi_engine_output(e, &byte, 1u);
}

static uint8_t msg_data_len(uint8_t status)
{
    switch (status & 0xf0u) {
    case 0xc0u:
    case 0xd0u: return 1u;
    case 0x80u:
    case 0x90u:
    case 0xa0u:
    case 0xb0u:
    case 0xe0u: return 2u;
    default: return 0u;
    }
}

static void observe_channel_message(midi_engine_t *e, uint8_t status,
                                    const uint8_t *data, uint8_t len)
{
    const uint8_t ch = status & 0x0fu;
    const uint8_t op = status & 0xf0u;
    if (ch >= MIDI_ENGINE_CHANNELS) return;

    if (op == 0x90u && len >= 2u && data[1] != 0u) {
        const uint8_t note = data[0] & 0x7fu;
        const uint8_t vel = data[1] & 0x7fu;
        if (vel > e->view.channel_activity[ch]) e->view.channel_activity[ch] = vel;

        uint8_t bin;
        if (note <= 24u) bin = 0u;
        else if (note >= 108u) bin = MIDI_ENGINE_SPECTRUM_BINS - 1u;
        else bin = (uint8_t)(((uint16_t)(note - 24u) * MIDI_ENGINE_SPECTRUM_BINS) / 84u);
        if (bin >= MIDI_ENGINE_SPECTRUM_BINS) bin = MIDI_ENGINE_SPECTRUM_BINS - 1u;
        if (vel > e->view.spectrum[bin]) e->view.spectrum[bin] = vel;

        if (ch == 9u) {
            const uint8_t side = (uint8_t)(vel * 3u / 4u);
            if (bin > 0u && side > e->view.spectrum[bin - 1u])
                e->view.spectrum[bin - 1u] = side;
            if (bin + 1u < MIDI_ENGINE_SPECTRUM_BINS && side > e->view.spectrum[bin + 1u])
                e->view.spectrum[bin + 1u] = side;
        }
    }
}

static void observer_reset(midi_engine_t *e)
{
    e->obs_running_status = 0u;
    e->obs_status = 0u;
    e->obs_have = 0u;
    e->obs_need = 0u;
    e->obs_in_sysex = 0u;
}

static void observer_feed(midi_engine_t *e, uint8_t byte)
{
    if (byte >= 0xf8u) return;

    if (e->obs_in_sysex) {
        if (byte == 0xf7u) e->obs_in_sysex = 0u;
        return;
    }
    if (byte == 0xf0u) {
        e->obs_in_sysex = 1u;
        e->obs_running_status = 0u;
        e->obs_have = e->obs_need = 0u;
        return;
    }
    if ((byte & 0x80u) != 0u) {
        if (byte >= 0x80u && byte <= 0xefu) {
            e->obs_running_status = byte;
            e->obs_status = byte;
            e->obs_have = 0u;
            e->obs_need = msg_data_len(byte);
        } else {
            e->obs_running_status = 0u;
            e->obs_have = e->obs_need = 0u;
        }
        return;
    }

    if (e->obs_need == 0u) {
        if (e->obs_running_status == 0u) return;
        e->obs_status = e->obs_running_status;
        e->obs_have = 0u;
        e->obs_need = msg_data_len(e->obs_status);
    }
    if (e->obs_need == 0u) return;
    e->obs_data[e->obs_have++] = byte & 0x7fu;
    if (e->obs_have >= e->obs_need) {
        observe_channel_message(e, e->obs_status, e->obs_data, e->obs_need);
        e->obs_have = 0u;
        e->obs_need = msg_data_len(e->obs_running_status);
    }
}

void midi_engine_init(midi_engine_t *e,
                      const midi_engine_backend_t *backend,
                      uint8_t master_volume)
{
    if (e == NULL) return;
    memset(e, 0, sizeof(*e));
    if (backend != NULL) e->backend = *backend;
    e->view.master_volume = master_volume > 127u ? 127u : master_volume;
    e->view.tempo_bpm = 120u;
    observer_reset(e);
    midi_engine_use_passthrough(e);
    if (e->backend.set_master_volume != NULL)
        e->backend.set_master_volume(e->backend.user, e->view.master_volume);
}

void midi_engine_set_processor(midi_engine_t *e, const midi_processor_t *processor)
{
    if (e == NULL) return;
    if (processor == NULL || processor->feed_byte == NULL) {
        midi_engine_use_passthrough(e);
        return;
    }
    e->processor = *processor;
    if (e->processor.reset != NULL) e->processor.reset(e->processor.user);
    observer_reset(e);
    e->out_in_sysex = 0u;
    e->out_sysex_len = 0u;
}

void midi_engine_use_passthrough(midi_engine_t *e)
{
    if (e == NULL) return;
    e->processor.reset = passthrough_reset;
    e->processor.feed_byte = passthrough_feed;
    e->processor.user = e;
    e->out_in_sysex = 0u;
    e->out_sysex_len = 0u;
}

void midi_engine_reset(midi_engine_t *e)
{
    if (e == NULL) return;
    if (e->processor.reset != NULL) e->processor.reset(e->processor.user);
    observer_reset(e);
    e->out_in_sysex = 0u;
    e->out_sysex_len = 0u;
    memset(e->view.channel_activity, 0, sizeof(e->view.channel_activity));
    memset(e->view.spectrum, 0, sizeof(e->view.spectrum));
}

void midi_engine_feed_byte(midi_engine_t *e, uint8_t byte)
{
    if (e == NULL) return;
    observer_feed(e, byte);
    if (e->processor.feed_byte != NULL) e->processor.feed_byte(e->processor.user, byte);
}

void midi_engine_feed_buffer(midi_engine_t *e, const uint8_t *bytes, size_t len)
{
    if (e == NULL || bytes == NULL) return;
    for (size_t i = 0u; i < len; ++i) midi_engine_feed_byte(e, bytes[i]);
}

void midi_engine_feed_channel(midi_engine_t *e,
                              uint8_t status, const uint8_t *data, uint8_t data_len)
{
    if (e == NULL || status < 0x80u || status > 0xefu) return;
    midi_engine_feed_byte(e, status);
    for (uint8_t i = 0u; i < data_len; ++i) midi_engine_feed_byte(e, data[i]);
}

void midi_engine_set_title(midi_engine_t *e, const char *title)
{
    if (e != NULL) copy_text(e->view.title, sizeof(e->view.title), title);
}

void midi_engine_set_profile_label(midi_engine_t *e, const char *label)
{
    if (e != NULL) copy_text(e->view.profile, sizeof(e->view.profile), label);
}

void midi_engine_set_play_state(midi_engine_t *e, midi_engine_play_state_t state)
{
    if (e != NULL) e->view.play_state = state;
}

void midi_engine_set_timeline(midi_engine_t *e, uint32_t elapsed_ms, uint32_t total_ms)
{
    if (e == NULL) return;
    e->view.elapsed_ms = elapsed_ms;
    e->view.total_ms = total_ms;
}

void midi_engine_set_tempo(midi_engine_t *e, uint16_t bpm)
{
    if (e != NULL && bpm != 0u) e->view.tempo_bpm = bpm;
}

void midi_engine_set_loop(midi_engine_t *e, uint8_t enabled)
{
    if (e != NULL) e->view.loop_enabled = enabled ? 1u : 0u;
}

void midi_engine_set_master_volume(midi_engine_t *e, uint8_t volume)
{
    if (e == NULL) return;
    if (volume > 127u) volume = 127u;
    e->view.master_volume = volume;
    if (e->backend.set_master_volume != NULL)
        e->backend.set_master_volume(e->backend.user, volume);
}

uint8_t midi_engine_get_master_volume(const midi_engine_t *e)
{
    return e != NULL ? e->view.master_volume : 0u;
}

void midi_engine_tick(midi_engine_t *e, uint32_t elapsed_ms)
{
    if (e == NULL) return;
    e->decay_accum_ms += elapsed_ms;
    while (e->decay_accum_ms >= 40u) {
        e->decay_accum_ms -= 40u;
        for (size_t i = 0u; i < MIDI_ENGINE_CHANNELS; ++i) {
            const uint8_t v = e->view.channel_activity[i];
            e->view.channel_activity[i] = v > 7u ? (uint8_t)(v - 7u) : 0u;
        }
        for (size_t i = 0u; i < MIDI_ENGINE_SPECTRUM_BINS; ++i) {
            const uint8_t v = e->view.spectrum[i];
            e->view.spectrum[i] = v > 5u ? (uint8_t)(v - 5u) : 0u;
        }
    }
}

void midi_engine_get_snapshot(const midi_engine_t *e, midi_engine_snapshot_t *out)
{
    if (e == NULL || out == NULL) return;
    *out = e->view;
}
