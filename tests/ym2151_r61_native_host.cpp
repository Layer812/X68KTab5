// X68KTab5 R61 YM2151 standalone HOST QA, NOT a Tab5 firmware target.
// Build: g++ -std=c++17 -O2 -Wno-attributes -o /tmp/ym2151_r61_qa tests/ym2151_r61_native_host.cpp -lm
// Run: /tmp/ym2151_r61_qa
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "../components/px68k/fmgen/vgmm5_ym2151.cpp"

static std::vector<int16_t> generate(int algorithm,int mode,int kf,int detune,int step) {
    VgmM5YM2151* e=vgmm5_ym2151_create(4000000u,44100u);
    if(!e || !e->r41_native.enabled || e->sample_rate!=62500u) abort();
    const int ch=mode==2?7:0;
    auto w=[&](int r,int v){vgmm5_ym2151_write(e,(uint8_t)r,(uint8_t)v);};
    w(0x20+ch,0xc0|0x30|algorithm); w(0x28+ch,0x4c); w(0x30+ch,kf);
    if(mode==1){w(0x18,0x10);w(0x19,0x50);w(0x19,0xa0);w(0x38+ch,0x73);}
    if(mode==2)w(0x0f,0x9b);
    for(int op=0;op<4;op++){
        const int off=op*8;
        w(0x40+off+ch,(detune<<4)|1); w(0x60+off+ch,op?0x10:0x20);
        w(0x80+off+ch,0x1f);w(0xa0+off+ch,2);
        w(0xc0+off+ch,(detune%4)<<6);w(0xe0+off+ch,0x0f);
    }
    w(0x08,0x78|ch);
    const int frames=4000;
    std::vector<int16_t> out(frames*2);
    for(int pos=0;pos<frames;pos+=step) {
        const int size=(frames-pos<step)?frames-pos:step;
        vgmm5_ym2151_render(e,out.data()+pos*2,(uint32_t)size);
    }
    vgmm5_ym2151_destroy(e);
    return out;
}
int main() {
    int checked=0,failures=0;
    for(int algo=0;algo<8;algo++)
      for(int mode=0;mode<3;mode++)
       for(int kf:{0,64,252})
        for(int dt:{0,3}) {
            auto reference=generate(algo,mode,kf,dt,4000);
            if(reference!=generate(algo,mode,kf,dt,1)||
               reference!=generate(algo,mode,kf,dt,127)) {
                fprintf(stderr,"stream-continuity failed: alg=%d mode=%d kf=%d dt=%d\n",
                    algo,mode,kf,dt);
                failures++;
            }
            checked++;
        }
    VgmM5YM2151* e=vgmm5_ym2151_create(4000000,44100);
    if(!e || !e->r41_native.enabled || e->sample_rate!=62500) return 3;
    vgmm5_ym2151_set_sample_rate(e,22050);
    if(e->r41_native.enabled || e->sample_rate!=22050) return 4;
    vgmm5_ym2151_set_sample_rate(e,44100);
    if(!e->r41_native.enabled || e->sample_rate!=62500) return 5;
    int16_t audio[256*2]={0};
    vgmm5_ym2151_render(e,audio,256);
    vgmm5_ym2151_reset(e);
    if(!e->r41_native.enabled || e->sample_rate!=62500 || e->r47_fir.write!=0) return 6;
    vgmm5_ym2151_write(e,0x28,0x7e);
    vgmm5_ym2151_write(e,0x30,0xfc);
    vgmm5_ym2151_write(e,0x40,0x3f); // previous R58 float-cast-overflow trigger
    vgmm5_ym2151_destroy(e);
    e=vgmm5_ym2151_create(3579545,44100);
    if(!e || e->r41_native.enabled) return 7;
    vgmm5_ym2151_destroy(e);
    printf("R61 YM2151: %d PCM segmentation cases, %d failures; rate/reset/high-KC PASS\n",
           checked,failures);
    return failures?1:0;
}
