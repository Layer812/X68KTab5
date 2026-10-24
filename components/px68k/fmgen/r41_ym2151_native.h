#pragma once
// R41: YM2151-only native-chip-rate PCM bridge (host-testable C API).
// Core generates at round(chip_clock / 64) Hz; caller mixes at unchanged output rate.
// Uses a causal linear interpolation (one native sample of latency), fixed-point Q32.
// No allocation, floating point, division, logging or I2S operations in tick().
#include <stdint.h>

typedef void (*R41NativeTick)(void* context, int32_t* l, int32_t* r);

typedef struct {
    uint32_t output_hz;
    uint32_t native_hz;
    uint32_t steps_whole;
    uint32_t phase_q32;
    uint32_t step_fraction_q32;
    int32_t previous_l, previous_r, latest_l, latest_r;
    uint32_t primed;
    uint32_t enabled;
} R41YM2151Native;

static inline void r41_native_init(R41YM2151Native* s, uint32_t chip_clock, uint32_t output_hz) {
    // reset all temporal state between tracks, including playback of the same file
    s->phase_q32=0; s->previous_l=s->previous_r=s->latest_l=s->latest_r=0;
    s->primed=0; s->enabled=0; s->output_hz=output_hz;
    s->native_hz=0; s->steps_whole=0; s->step_fraction_q32=0;
    if (!chip_clock || !output_hz) return;
    uint32_t hz = (chip_clock + 32u) / 64u; // 4 MHz -> 62,500 Hz
    // Avoid invalid or unreasonable VGM clocks overwhelming the audio worker.
    if (!hz || hz > 4u * output_hz) return;
    s->native_hz=hz;
    s->steps_whole=hz / output_hz;
    s->step_fraction_q32=(uint32_t)(((uint64_t)(hz % output_hz) << 32) / output_hz);
    s->enabled=1;
}

static inline void r41_native_tick(R41YM2151Native* s, R41NativeTick callback, void* ctx,
                                    int32_t* out_l, int32_t* out_r) {
    if (!s->enabled) { callback(ctx,out_l,out_r); return; }
    if (!s->primed) {
        callback(ctx,&s->latest_l,&s->latest_r);
        s->previous_l=s->latest_l;
        s->previous_r=s->latest_r;
        s->primed=1;
    } else {
        uint32_t previous_phase=s->phase_q32;
        s->phase_q32+=s->step_fraction_q32;
        uint32_t steps=s->steps_whole+(s->phase_q32 < previous_phase);
        while (steps--) {
            s->previous_l=s->latest_l;
            s->previous_r=s->latest_r;
            // Native FM callback overwrites sample with a new sample using addition
            s->latest_l=0; s->latest_r=0;
            callback(ctx,&s->latest_l,&s->latest_r);
        }
    }
    uint32_t alpha=s->phase_q32 >> 16;
    // int64 protects multiplication (FM peak may exceed signed 16-bit).
    // This avoids expensive per-frame floating point/division.
    *out_l += s->previous_l + (int32_t)((((int64_t)s->latest_l-s->previous_l)*alpha)>>16);
    *out_r += s->previous_r + (int32_t)((((int64_t)s->latest_r-s->previous_r)*alpha)>>16);
}
