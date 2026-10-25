// X68KTab5 vgmM5 stable d20fa032 YM2151 KEY ON / phase-edge host regression.
// Uses the exact X68KTab5 production-oriented FM implementation (no mock FM).
// Compile: g++ -std=c++17 -O2 -Wno-attributes tests/ym2151_kon_phase_host.cpp -lm -o /tmp/ym2151_kon_phase
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../components/px68k/fmgen/vgmm5_ym2151.cpp"

static void require_test(bool ok, const char *reason, int ch, int mask) {
    if (ok) return;
    fprintf(stderr,"FAIL %s ch=%d mask=%02x\n",reason,ch,mask);
    exit(1);
}

static VgmM5YM2151 *new_engine() {
    VgmM5YM2151 *e=vgmm5_ym2151_create(4000000u,44100u);
    if(!e || e->sample_rate!=62500u || !e->r41_native.enabled) {
        fputs("FAIL native 62.5kHz synthesizer unavailable\n",stderr);exit(2);
    }
    return e;
}

static void mapping_test() {
    // Physical array: M1=0 M2=1 C1=2 C2=3.
    // Yamaha KON: 0x08=M1, 0x10=C1, 0x20=M2, 0x40=C2.
    const struct {uint8_t bits, expected;} cases[] = {
        {0x00,0x0},{0x08,0x1},{0x10,0x4},{0x20,0x2},
        {0x40,0x8},{0x58,0xD},{0x78,0xF}
    };
    int cases_run=0;
    for(int ch=0;ch<8;ch++) for(const auto &tc:cases) {
        VgmM5YM2151 *e=new_engine();
        for(int op=0;op<4;op++) e->ops[ch*4+op].phase=0x11223344u;
        vgmm5_ym2151_write(e,0x08,(uint8_t)(tc.bits|ch));
        unsigned observed=0;
        for(int op=0;op<4;op++){
            const YMOp &p=e->ops[ch*4+op];
            const bool on=((tc.expected>>op)&1u)!=0u;
            if(p.key_on) observed|=(1u<<op);
            require_test(on ? (p.env_state==EG_ATTACK && p.phase==0u)
                            : (p.env_state==EG_OFF && p.phase==0x11223344u),
                         "KON mapping/state/phase",ch,tc.bits);
        }
        require_test(observed==tc.expected,"KON bit map",ch,tc.bits);
        require_test(((e->active_ch_mask>>ch)&1u)==(tc.bits?1u:0u),
                     "active channel mask",ch,tc.bits);
        vgmm5_ym2151_destroy(e);
        cases_run++;
    }
    printf("PASS %d Yamaha KON mapping / phase / active-mask cases\n",cases_run);
}
static void edge_test() {
    int cases_run=0;
    for(int ch=0;ch<8;ch++) {
        VgmM5YM2151 *e=new_engine();
        YMOp &p=e->ops[ch*4+2]; // C1 is D4, not D5
        p.phase=0x11223344u;
        vgmm5_ym2151_write(e,0x08,(uint8_t)(ch|0x10));
        require_test(p.key_on && p.phase==0u && p.env_state==EG_ATTACK,
                     "rising edge reset",ch,0x10);cases_run++;
        p.phase=0x55667788u;p.env_state=EG_DECAY;
        vgmm5_ym2151_write(e,0x08,(uint8_t)(ch|0x10));
        require_test(p.key_on && p.phase==0x55667788u && p.env_state==EG_DECAY,
                     "held KON must not restart",ch,0x10);cases_run++;
        vgmm5_ym2151_write(e,0x08,(uint8_t)ch);
        require_test(!p.key_on && p.env_state==EG_RELEASE && p.phase==0x55667788u,
                     "falling edge releases without phase reset",ch,0);cases_run++;
        p.phase=0xAABBCCDDu;
        vgmm5_ym2151_write(e,0x08,(uint8_t)(ch|0x10));
        require_test(p.key_on && p.phase==0u && p.env_state==EG_ATTACK,
                     "reattack after KEY OFF",ch,0x10);cases_run++;
        p.env_state=EG_OFF;p.phase=0xABCD1234u;
        vgmm5_ym2151_write(e,0x08,(uint8_t)(ch|0x10));
        require_test(p.key_on && p.env_state==EG_OFF && p.phase==0xABCD1234u,
                     "held KEY ON is independent of EG terminal state",ch,0x10);cases_run++;
        vgmm5_ym2151_reset(e);
        require_test(e->ops[ch*4+2].key_on==0u && e->ops[ch*4+2].env_state==EG_OFF,
                     "full reset clears key latch",ch,0);cases_run++;
        vgmm5_ym2151_destroy(e);
    }
    printf("PASS %d edge / retrigger / state tests\n",cases_run);
}
static void csm_test() {
    VgmM5YM2151 *e=new_engine();
    vgmm5_ym2151_csm_pulse(e);
    require_test(e->active_ch_mask==0xffu,"CSM channels active",-1,0x78);
    for(int ch=0;ch<8;ch++)for(int op=0;op<4;op++){
        const YMOp &p=e->ops[ch*4+op];
        require_test(p.key_on && p.env_state==EG_ATTACK && p.phase==0u,
                     "CSM all-operator KEY ON",ch,0x78);
        e->ops[ch*4+op].phase=0xDEADBEEFu;
        e->ops[ch*4+op].env_state=EG_DECAY;
    }
    vgmm5_ym2151_csm_pulse(e);
    for(int ch=0;ch<8;ch++)for(int op=0;op<4;op++){
        const YMOp &p=e->ops[ch*4+op];
        require_test(p.key_on && p.env_state==EG_ATTACK && p.phase==0u,
                     "CSM repeated pulse retrigger",ch,0x78);
    }
    vgmm5_ym2151_destroy(e);
    puts("PASS CSM KEY OFF then KEY ON pulse semantics across all channels");
}
int main(){
    mapping_test();
    edge_test();
    csm_test();
    puts("PASS source-based X68KTab5 YM2151 KON mapping, phase-edge, MEM-preserving regression");
    return 0;
}
