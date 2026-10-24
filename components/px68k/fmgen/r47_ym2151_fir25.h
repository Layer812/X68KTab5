#pragma once
// R47 evaluation candidate: YM2151 ONLY / 4 MHz clock / native 62,500 Hz.
// R46 25-tap symmetric Q15 low-pass, fc=19.5 kHz at 62.5 kHz.
// NOT generic rate conversion: caller MUST guard native_hz==62500,
// output_hz==44100 before enabling FIR/native path.
// FIR input is already mixed FM L/R; never applies to PCM, OPN, OPLL or MDX.
#include <stdint.h>

static inline uint32_t r47_ym2151_select_native_clock(uint32_t clock_hz, uint32_t output_hz, int is_ym2151) {
    return (is_ym2151 && clock_hz == 4000000u && output_hz == 44100u) ? clock_hz : 0u;
}

typedef struct { int32_t l[25]; int32_t r[25]; uint32_t write; } R47YM2151Fir25;

static const int16_t r47_ym2151_fir_q15[13] = {
    -70,38,101,-233,10,575,-678,-507,2012,-1180,-3445,9516,20490
};

static inline void r47_ym2151_fir_reset(R47YM2151Fir25 *s) {
    for (uint32_t i=0;i<25u;++i) { s->l[i]=0; s->r[i]=0; }
    s->write=0;
}

static inline void r47_ym2151_fir_tick(R47YM2151Fir25 *s,
    int32_t in_l, int32_t in_r, int32_t *out_l, int32_t *out_r) {
    const uint32_t w=s->write;
    s->l[w]=in_l; s->r[w]=in_r;
    int64_t al=0,ar=0;
    uint32_t newer=w,older=(w+1u)%25u;
    for(uint32_t j=0;j<12u;++j) {
        al+=((int64_t)s->l[newer]+s->l[older])*r47_ym2151_fir_q15[j];
        ar+=((int64_t)s->r[newer]+s->r[older])*r47_ym2151_fir_q15[j];
        newer=(newer==0u)?24u:newer-1u;
        older=(older==24u)?0u:older+1u;
    }
    al+=(int64_t)s->l[newer]*r47_ym2151_fir_q15[12];
    ar+=(int64_t)s->r[newer]*r47_ym2151_fir_q15[12];
    *out_l=(int32_t)(al>>15);
    *out_r=(int32_t)(ar>>15);
    s->write=(w==24u)?0u:w+1u;
}
