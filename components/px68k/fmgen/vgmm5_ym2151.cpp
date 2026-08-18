/*
 * Build 5.42: algorithm-specialized YM2151-only Core1 synthesizer for PX68K Tab5.
 *
 * The synthesis structure and fixed-point hot path are adapted from the
 * vgmM5 fm_engine YM2151 path by Layer812:
 *   https://github.com/Layer812/vgmM5
 *
 * vgmM5's upstream README marks its original source/modifications under MIT.
 * This file deliberately strips all non-YM2151 chips, PSG/PCM routing,
 * generic operator branches, and guest timer/status logic.  PX68K's CPU0
 * FastOPMControl remains authoritative for guest-visible timer/status/IRQ.
 */
/*
 * PX68K source modified for the Tab5 port.
 * Intent: Tab5 CPU0 YM2151 hot path: reduce host waveform cost while preserving the register-driven synthesis behavior used by PX68K.
 * Layer8 Aug/17/2026
 */
#include "vgmm5_ym2151.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define YM_CH 8
#define YM_OPS 32
#define EG_FRACTION_BITS 16
#define EG_MAX ((int32_t)(4096u << EG_FRACTION_BITS))

enum { EG_OFF=0, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

struct YMOp {
    uint32_t phase;
    uint32_t phase_step;
    int32_t tl_atten;
    int32_t env_level;
    int32_t ar_step, dr_step, d2r_step, sl_level, rr_step;
    uint8_t env_state;
    uint8_t mul, dt1, dt2;
    uint8_t am_enable;
    uint8_t ar, dr, d2r, rr, ks;
};

struct VgmM5YM2151 {
    uint32_t sample_rate;
    uint32_t clock;
    float phase_step_factor;
    uint32_t output_tick_counter;

    uint8_t pan_l[YM_CH], pan_r[YM_CH];
    uint8_t algo[YM_CH], fb_shift[YM_CH];
    int32_t fb_memory[YM_CH][2];
    int32_t mem_value[YM_CH];
    uint8_t kc[YM_CH], kf[YM_CH], pms[YM_CH], ams[YM_CH];
    YMOp ops[YM_OPS];
    uint32_t freq_tab[13][64];

    uint32_t lfo_phase, lfo_step;
    int32_t cached_pm, cached_am;
    uint8_t pmd, amd, lfo_wave;

    uint8_t noise_enable;
    uint32_t noise_phase, noise_step, noise_rng;

    int32_t prev_l, prev_r;
    int32_t volume_q14;
    uint8_t active_ch_mask;
};

static DRAM_ATTR uint16_t s_sin[1024];
static DRAM_ATTR uint16_t s_exp[256];
static DRAM_ATTR uint32_t s_rate[128];
static DRAM_ATTR int8_t s_lfo_pm[4][256];
static DRAM_ATTR uint8_t s_lfo_am[4][256];
static DRAM_ATTR int s_tables_ready = 0;

/*
 * Build 5.42: these are touched for every active channel of every 44.1 kHz
 * sample.  Keep them writable/DRAM_ATTR deliberately: a const table may live
 * in flash and turn the IRAM synth loop into repeated flash-cache reads.
 */
static DRAM_ATTR int32_t s_pms_mult[8] = {0,1,2,4,8,16,32,64};
static DRAM_ATTR int32_t s_ams_mult[4] = {0,2,4,8};

static void init_tables(void)
{
    if (s_tables_ready) return;
    for (int i=0;i<1024;i++) {
        float sv = sinf(((float)i + 0.5f) * (float)M_PI / 2048.0f);
        s_sin[i] = (uint16_t)(-256.0f * log2f(sv));
    }
    for (int i=0;i<256;i++)
        s_exp[i] = (uint16_t)(16384.0f * powf(2.0f, -(float)i / 256.0f));
    for (int i=0;i<128;i++) {
        if (!i) s_rate[i]=0;
        else {
            double v=64.0*pow(2.0,(double)i/4.0);
            const double cap=(double)((uint32_t)EG_MAX/8u);
            s_rate[i]=(uint32_t)(v>cap?cap:v);
        }
    }
    for (int p=0;p<256;p++) {
        s_lfo_pm[0][p]=(int8_t)(p-128); s_lfo_am[0][p]=(uint8_t)(255-p);
        s_lfo_pm[1][p]=(int8_t)(p<128?127:-128); s_lfo_am[1][p]=(uint8_t)(p<128?255:0);
        int tri;
        if (p<64) tri=p*2;
        else if (p<192) tri=127-(p-64)*2;
        else tri=-127+(p-192)*2;
        s_lfo_pm[2][p]=(int8_t)tri;
        s_lfo_am[2][p]=(uint8_t)(p<128?(255-p*2):((p-128)*2));
        uint32_t r=((uint32_t)p*1103515245u+12345u)>>16;
        s_lfo_pm[3][p]=(int8_t)((r&255u)-128); s_lfo_am[3][p]=(uint8_t)(r&255u);
    }
    s_tables_ready=1;
}

static void update_phase(VgmM5YM2151 *e,int ch)
{
    static const uint8_t kc_to_n[16]={1,2,3,3,4,5,6,6,7,8,9,9,10,11,12,12};
    static const float dt1_tab[8]={1.0f,1.002f,1.004f,1.006f,1.0f,0.998f,0.996f,0.994f};
    static const float dt2_tab[4]={0.0f,384.0f,500.0f,608.0f};
    int note=e->kc[ch]&15, oct=(e->kc[ch]>>4)&7, kf=e->kf[ch]&63;
    uint32_t base=e->freq_tab[kc_to_n[note]][kf] << oct;
    float bf=(float)base;
    for(int o=0;o<4;o++) {
        YMOp *op=&e->ops[ch*4+o];
        float mul=op->mul? (float)op->mul : 0.5f;
        float dt2=dt2_tab[op->dt2&3] * e->phase_step_factor * 0.5f;
        op->phase_step=(uint32_t)(bf*mul*dt1_tab[op->dt1&7]+dt2);
    }
}

static void update_rates(VgmM5YM2151 *e,int ch)
{
    for(int o=0;o<4;o++) {
        YMOp *p=&e->ops[ch*4+o];
        int ks=e->kc[ch] >> (5-p->ks);
        int r=p->ar? p->ar*2+ks:0; if(r>127)r=127; p->ar_step=(int32_t)(s_rate[r]*8u); if(!p->ar_step&&r)p->ar_step=1;
        r=p->dr*2+ks; if(r>127)r=127; p->dr_step=(int32_t)s_rate[r]; if(!p->dr_step&&r)p->dr_step=1;
        r=p->d2r*2+ks; if(r>127)r=127; p->d2r_step=(int32_t)s_rate[r]; if(!p->d2r_step&&r)p->d2r_step=1;
        r=p->rr*2+ks; if(r>127)r=127; p->rr_step=(int32_t)s_rate[r]; if(!p->rr_step&&r)p->rr_step=1;
    }
}

static IRAM_ATTR __attribute__((noinline,optimize("O3"))) void update_envelopes(VgmM5YM2151 *e)
{
    for(int i=0;i<YM_OPS;i++) {
        YMOp *p=&e->ops[i];
        if(p->env_state==EG_OFF)continue;
        switch(p->env_state) {
        case EG_ATTACK: {
            uint32_t d=(uint32_t)p->ar_step;
            if(d>=s_rate[127]) { p->env_level=0; p->env_state=EG_DECAY; break; }
            uint32_t ev=(uint32_t)p->env_level;
            uint32_t drop=(d>65535u)? d+(ev>>1) : d+((d*(ev>>12))>>5);
            if(!drop&&d)drop=1;
            if(ev<=drop) { p->env_level=0; p->env_state=EG_DECAY; } else p->env_level=(int32_t)(ev-drop);
            break; }
        case EG_DECAY:
            p->env_level+=p->dr_step;
            if(p->env_level>=p->sl_level){p->env_level=p->sl_level;p->env_state=EG_SUSTAIN;}
            break;
        case EG_SUSTAIN:
            p->env_level+=p->d2r_step; if(p->env_level>=EG_MAX)p->env_level=EG_MAX; break;
        case EG_RELEASE:
            p->env_level+=p->rr_step; if(p->env_level>=EG_MAX){p->env_level=EG_MAX;p->env_state=EG_OFF;} break;
        default: break;
        }
    }

    /* Build 5.42: synth() no longer reloads four envelope states for every
     * channel on every sample. EG only changes every fourth sample, so cache
     * the exact channel-active mask here. Key-on updates it immediately. */
    uint8_t mask=0;
    for(int ch=0;ch<YM_CH;ch++) {
        const YMOp *p=&e->ops[ch*4];
        if(p[0].env_state!=EG_OFF || p[1].env_state!=EG_OFF ||
           p[2].env_state!=EG_OFF || p[3].env_state!=EG_OFF)
            mask |= (uint8_t)(1u<<ch);
    }
    e->active_ch_mask=mask;
}

static IRAM_ATTR __attribute__((always_inline,optimize("O3"))) inline int32_t feedback(const VgmM5YM2151 *e,int ch)
{
    uint32_t f=e->fb_shift[ch]&7u; if(!f)return 0;
    return (e->fb_memory[ch][0]+e->fb_memory[ch][1]) >> (9u-f);
}

static IRAM_ATTR __attribute__((always_inline,optimize("O3"))) inline int32_t calc_op(
    VgmM5YM2151 *e,YMOp *p,int32_t mod,int32_t pm,int32_t am,int noise)
{
    if(p->env_state==EG_OFF)return 0;
    uint32_t step=p->phase_step;
    mod*=2;
    if(pm) step+=(uint32_t)((((int32_t)(step>>10))*pm)>>3);
    p->phase+=step;
    int32_t atten=(p->env_level>>EG_FRACTION_BITS)+p->tl_atten;
    if(p->am_enable && am) atten+=am;
    if(atten>=3840)return 0;
    uint32_t ph=p->phase>>20;
    uint32_t wi=(ph+(uint32_t)mod)&0xfffu;
    if(noise) {
        int32_t nv=((int32_t)(e->noise_rng&0x7fffu)-16384)>>1;
        nv=(nv*3)>>2;
        uint32_t ex=s_exp[(uint32_t)atten&255u] >> ((uint32_t)atten>>8);
        return (nv*(int32_t)ex)>>14;
    }
    uint32_t si=wi&0x3ffu; if(wi&0x400u)si=1023u-si;
    int neg=(wi&0x800u)!=0;
    int32_t ta=atten+(int32_t)s_sin[si];
    if(ta<0)ta=0;
    if(ta>=3840)return 0;
    int32_t out=(int32_t)(s_exp[(uint32_t)ta&255u] >> ((uint32_t)ta>>8));
    return neg?-out:out;
}

/* Build 5.42: YM2151-only algorithm routing.
 * 5.40/5.41 mirrored vgmM5's generic six-bus implementation: clear bus[6],
 * perform five routing-table lookups, then indexed loads/stores for every
 * active channel of every output sample.  PX68K only needs OPM, so expand the
 * eight hardware algorithms directly.  The arithmetic/order below is exactly
 * the same as the previous bus routing, just with dead buses removed. */
static IRAM_ATTR __attribute__((always_inline,optimize("O3"))) inline int32_t synth_channel(
    VgmM5YM2151 *e,int ch,int32_t pm,int32_t am)
{
    YMOp *p=&e->ops[ch*4];
    const int32_t mem=e->mem_value[ch];
    const int noise=(ch==7 && e->noise_enable);
    const int algo=e->algo[ch]&7;

    const int32_t o0=calc_op(e,&p[0],feedback(e,ch),pm,am,0);
    e->fb_memory[ch][0]=e->fb_memory[ch][1];
    e->fb_memory[ch][1]=o0;

    int32_t o1,o2,o3,out;
    switch(algo) {
    case 0:
        o1=calc_op(e,&p[1],mem,pm,am,0);
        o2=calc_op(e,&p[2],o0,pm,am,0);
        o3=calc_op(e,&p[3],o1,pm,am,noise);
        e->mem_value[ch]=o2;
        out=o3;
        break;
    case 1:
        o1=calc_op(e,&p[1],mem,pm,am,0);
        o2=calc_op(e,&p[2],0,pm,am,0);
        o3=calc_op(e,&p[3],o1,pm,am,noise);
        e->mem_value[ch]=o0+o2;
        out=o3;
        break;
    case 2:
        o1=calc_op(e,&p[1],mem,pm,am,0);
        o2=calc_op(e,&p[2],0,pm,am,0);
        o3=calc_op(e,&p[3],o0+o1,pm,am,noise);
        e->mem_value[ch]=o2;
        out=o3;
        break;
    case 3:
        o1=calc_op(e,&p[1],0,pm,am,0);
        o2=calc_op(e,&p[2],o0,pm,am,0);
        o3=calc_op(e,&p[3],mem+o1,pm,am,noise);
        e->mem_value[ch]=o2;
        out=o3;
        break;
    case 4:
        o1=calc_op(e,&p[1],0,pm,am,0);
        o2=calc_op(e,&p[2],o0,pm,am,0);
        o3=calc_op(e,&p[3],o1,pm,am,noise);
        out=o2+o3;
        break;
    case 5:
        o1=calc_op(e,&p[1],mem,pm,am,0);
        o2=calc_op(e,&p[2],o0,pm,am,0);
        o3=calc_op(e,&p[3],o0,pm,am,noise);
        e->mem_value[ch]=o0;
        out=o1+o2+o3;
        break;
    case 6:
        o1=calc_op(e,&p[1],0,pm,am,0);
        o2=calc_op(e,&p[2],o0,pm,am,0);
        o3=calc_op(e,&p[3],0,pm,am,noise);
        out=o1+o2+o3;
        break;
    default: /* 7 */
        o1=calc_op(e,&p[1],0,pm,am,0);
        o2=calc_op(e,&p[2],0,pm,am,0);
        o3=calc_op(e,&p[3],0,pm,am,noise);
        out=o0+o1+o2+o3;
        break;
    }
    return out;
}

static IRAM_ATTR __attribute__((noinline,optimize("O3"))) void synth(VgmM5YM2151 *e,int32_t *ml,int32_t *mr)
{
    int32_t l=0,r=0;
    const int32_t gpm=e->cached_pm;
    const int32_t gam=e->cached_am;
    const uint8_t active=e->active_ch_mask;
    for(int ch=0;ch<YM_CH;ch++) {
        if((active&(uint8_t)(1u<<ch))==0) continue;
        const uint8_t pms=e->pms[ch]&7u;
        const uint8_t ams=e->ams[ch]&3u;
        const int32_t pm=(gpm&&pms)?((gpm*s_pms_mult[pms])>>4):0;
        const int32_t am=(gam&&ams)?((gam*s_ams_mult[ams])>>2):0;
        const int32_t co=synth_channel(e,ch,pm,am);
        if(e->pan_l[ch]) l+=co;
        if(e->pan_r[ch]) r+=co;
    }
    *ml=l;*mr=r;
}

static IRAM_ATTR __attribute__((always_inline,optimize("O3"))) inline void tick(VgmM5YM2151 *e,int32_t *ol,int32_t *orr)
{
    if((e->output_tick_counter++&3u)==0u)update_envelopes(e);
    if(e->noise_enable) {
        e->noise_phase+=e->noise_step;
        if(!e->noise_step)e->noise_step=0x00400000u;
        if(e->noise_phase<e->noise_step) {
            uint32_t bit=((e->noise_rng>>0)^(e->noise_rng>>3))&1u;
            e->noise_rng=(e->noise_rng>>1)|(bit<<16); if(!e->noise_rng)e->noise_rng=1;
        }
    }
    e->lfo_phase+=e->lfo_step;
    /* Build 5.42: YM2151's LFO oscillator can keep running with depth zero,
     * but depth-zero PM/AM is audibly and numerically zero. Avoid two table
     * loads and two multiplies on every 44.1 kHz sample in that common case. */
    if(e->pmd||e->amd) {
        const uint32_t pos=(e->lfo_phase>>24)&255u;
        const int w=e->lfo_wave&3;
        e->cached_pm=e->pmd?(((int32_t)s_lfo_pm[w][pos]*(int32_t)e->pmd+63)>>7):0;
        e->cached_am=e->amd?(((int32_t)s_lfo_am[w][pos]*(int32_t)e->amd+63)>>7):0;
    } else {
        e->cached_pm=0;
        e->cached_am=0;
    }
    int32_t l=0,r=0;synth(e,&l,&r);
    l=(l+e->prev_l)/2;r=(r+e->prev_r)/2;e->prev_l=l;e->prev_r=r;
    *ol=l;*orr=r;
}

static void init_state(VgmM5YM2151 *e,uint32_t clock,uint32_t sr,int32_t volume)
{
    memset(e,0,sizeof(*e));
    e->clock=clock;e->sample_rate=sr;e->volume_q14=volume;e->noise_rng=1;
    e->phase_step_factor=4294967296.0f/(float)sr;
    e->lfo_step=(uint32_t)((5.0*4294967296.0)/(double)sr);
    for(int ch=0;ch<8;ch++){e->pan_l[ch]=1;e->pan_r[ch]=1;}
    for(int n=0;n<=12;n++)for(int k=0;k<64;k++){
        float semi=(float)n+(float)k/64.0f;
        float f=16.3516f*((float)clock/3579545.0f)*powf(1.059463094f,semi);
        e->freq_tab[n][k]=(uint32_t)(f*e->phase_step_factor);
    }
    for(int i=0;i<YM_OPS;i++){e->ops[i].env_state=EG_OFF;e->ops[i].env_level=EG_MAX;}
}

extern "C" VgmM5YM2151 *vgmm5_ym2151_create(uint32_t clock,uint32_t sr)
{
    init_tables();
#ifdef ESP_PLATFORM
    VgmM5YM2151 *e=(VgmM5YM2151*)heap_caps_calloc(1,sizeof(VgmM5YM2151),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
    VgmM5YM2151 *e=(VgmM5YM2151*)calloc(1,sizeof(VgmM5YM2151));
#endif
    if(e)init_state(e,clock,sr,16384);
    return e;
}
extern "C" void vgmm5_ym2151_destroy(VgmM5YM2151 *e){if(!e)return;
#ifdef ESP_PLATFORM
    heap_caps_free(e);
#else
    free(e);
#endif
}
extern "C" void vgmm5_ym2151_reset(VgmM5YM2151 *e){if(!e)return;uint32_t c=e->clock,s=e->sample_rate;int32_t v=e->volume_q14;init_state(e,c,s,v);}
extern "C" void vgmm5_ym2151_set_px_volume(VgmM5YM2151 *e,uint8_t vol){if(!e)return;int v=vol?((16-(int)vol)*4):192;if(v>=192)e->volume_q14=0;else e->volume_q14=(int32_t)(16384.0f*powf(10.0f,-(float)v/40.0f));}

extern "C" void vgmm5_ym2151_write(VgmM5YM2151 *e,uint8_t a,uint8_t d)
{
    if(!e)return;
    if(a==0x01){if(d&2)e->lfo_phase=0;return;}
    if(a==0x08){int ch=d&7;for(int o=0;o<4;o++){YMOp *p=&e->ops[ch*4+o];if((d>>(3+o))&1){if(p->env_state==EG_OFF)p->env_level=EG_MAX;p->phase=0;p->env_state=EG_ATTACK;}else if(p->env_state!=EG_OFF)p->env_state=EG_RELEASE;}const YMOp *q=&e->ops[ch*4];if(q[0].env_state!=EG_OFF||q[1].env_state!=EG_OFF||q[2].env_state!=EG_OFF||q[3].env_state!=EG_OFF)e->active_ch_mask|=(uint8_t)(1u<<ch);else e->active_ch_mask&=(uint8_t)~(1u<<ch);return;}
    if(a==0x0f){e->noise_enable=(d>>7)&1;uint8_t nr=d&31;if(nr==31)nr=30;float f=(float)e->clock/(32.0f*(32.0f-(float)nr));float st=f*e->phase_step_factor;if(st>=4294967295.0f)st=4294967295.0f;e->noise_step=(uint32_t)st;return;}
    if(a==0x18){float f=(float)(d+1)*0.15f;e->lfo_step=(uint32_t)(f*e->phase_step_factor);return;}
    if(a==0x19){if(d&0x80)e->amd=d&0x7f;else e->pmd=d&0x7f;return;}
    if(a==0x1b){e->lfo_wave=d&3;return;}
    if(a>=0x20&&a<=0x27){int ch=a&7;e->pan_l[ch]=(d>>7)&1;e->pan_r[ch]=(d>>6)&1;e->fb_shift[ch]=(d>>3)&7;e->algo[ch]=d&7;return;}
    if(a>=0x28&&a<=0x2f){int ch=a&7;e->kc[ch]=d;update_phase(e,ch);update_rates(e,ch);return;}
    if(a>=0x30&&a<=0x37){int ch=a&7;e->kf[ch]=d;update_phase(e,ch);return;}
    if(a>=0x38&&a<=0x3f){int ch=a&7;e->pms[ch]=(d>>4)&7;e->ams[ch]=d&3;return;}
    if(a>=0x40){int ch=a&7;int ot=(a>>3)&3;YMOp *p=&e->ops[ch*4+ot];switch(a&0xe0){
        case 0x40:p->mul=d&15;p->dt1=(d>>4)&7;update_phase(e,ch);break;
        case 0x60:p->tl_atten=(d&0x7f)*32;break;
        case 0x80:p->ks=d>>6;p->ar=d&31;update_rates(e,ch);break;
        case 0xa0:p->dr=d&31;p->am_enable=(d>>7)&1;update_rates(e,ch);break;
        case 0xc0:p->dt2=(d>>6)&3;p->d2r=d&31;update_phase(e,ch);update_rates(e,ch);break;
        case 0xe0:{int rr=d&15;p->rr=rr?(rr*2+1):0;int d1=(d>>4)&15;p->sl_level=(d1==15)?EG_MAX:(int32_t)(((uint32_t)d1*4u)*32u<<EG_FRACTION_BITS);update_rates(e,ch);break;}
    }}
}

extern "C" void vgmm5_ym2151_csm_pulse(VgmM5YM2151 *e)
{
    if(!e)return;
    /* FMGEN OPM::TimerA() CSM semantics: every channel KeyOff, then
     * KeyOn all four operators. Reuse the normal YM2151 key command path. */
    for(uint8_t ch=0;ch<8;ch++) {
        vgmm5_ym2151_write(e,0x08,ch);
        vgmm5_ym2151_write(e,0x08,(uint8_t)(ch|0x78u));
    }
}

extern "C" IRAM_ATTR __attribute__((noinline,optimize("O3"))) void vgmm5_ym2151_render(VgmM5YM2151 *e,int16_t *dst,uint32_t frames)
{
    if(!e||!dst)return;
    int32_t vol=e->volume_q14;
    for(uint32_t i=0;i<frames;i++){
        int32_t l,r;tick(e,&l,&r);
        if(l>65535)l=65535;else if(l<-65536)l=-65536;
        if(r>65535)r=65535;else if(r<-65536)r=-65536;
        l=(l*vol)>>14;r=(r*vol)>>14;
        if(l>32767)l=32767;else if(l<-32768)l=-32768;
        if(r>32767)r=32767;else if(r<-32768)r=-32768;
        dst[i*2]=(int16_t)l;dst[i*2+1]=(int16_t)r;
    }
}
