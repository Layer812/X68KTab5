// Cardputer R5 operator routing integration: host-only comparison harness.
// Build the same source with -DX68KTAB_YM2151_R5_ALGO0_PREVIEW=0 and =1.
// Only algorithm 0 may differ; algorithms 1..7 must have identical PCM hashes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../components/px68k/fmgen/vgmm5_ym2151.cpp"

static uint64_t hash_pcm(int algo, int mode) {
    VgmM5YM2151 *e=vgmm5_ym2151_create(4000000u,44100u);
    if(!e || !e->r41_native.enabled || e->sample_rate!=62500u) std::abort();
    auto w=[&](int reg,int value) {vgmm5_ym2151_write(e,(uint8_t)reg,(uint8_t)value);};
    const int ch=mode==2?7:0;
    w(0x20+ch,0xc0 | (3<<3) | algo);
    w(0x28+ch,0x4c);
    w(0x30+ch,0x7c);
    if(mode==1){w(0x18,0x10);w(0x19,0x50);w(0x19,0xa0);w(0x38+ch,0x73);}
    if(mode==2)w(0x0f,0x9b);
    for(int op=0;op<4;op++) {
        int off=op*8;
        w(0x40+off+ch,1);
        w(0x60+off+ch,op==0?0x14:0x08);
        w(0x80+off+ch,0x1f);
        w(0xa0+off+ch,2);
        w(0xc0+off+ch,0);
        w(0xe0+off+ch,0x0f);
    }
    w(0x08,0x78|ch);
    std::vector<int16_t> pcm(6000*2);
    for(int pos=0;pos<6000;pos+=127) {
        uint32_t n=(uint32_t)((6000-pos)<127?(6000-pos):127);
        vgmm5_ym2151_render(e,pcm.data()+2*pos,n);
    }
    vgmm5_ym2151_destroy(e);
    uint64_t hash=14695981039346656037ull;
    for(int16_t x:pcm) {
        uint16_t u=(uint16_t)x;
        hash=(hash^(uint8_t)u)*1099511628211ull;
        hash=(hash^(uint8_t)(u>>8))*1099511628211ull;
    }
    return hash;
}
int main(){
    for(int algo=0;algo<8;algo++)
        for(int mode=0;mode<3;mode++)
            std::printf("%d,%d,%016llx\n",algo,mode,(unsigned long long)hash_pcm(algo,mode));
}
