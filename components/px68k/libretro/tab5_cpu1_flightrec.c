/* R140P4EP2: executor front-end breakdown only.
 * No PSRAM allocation. No MMIO storage. No transport chronology.
 */
#include "tab5_cpu1_flightrec.h"
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif

volatile uint32_t g_tab5_cpu1_fr_active DRAM_ATTR=0u;
volatile uint32_t g_tab5_cpu1_fr_guest_tick DRAM_ATTR=0u;
volatile uint32_t g_tab5_cpu1_ep_active DRAM_ATTR=0u;
volatile uint32_t g_tab5_cpu1_ep_sample_counter DRAM_ATTR=0u;

static DRAM_ATTR uint32_t s_done=0u,s_dumped=0u,s_samples=0u;
static DRAM_ATTR uint64_t s_total=0u,s_front=0u;
static DRAM_ATTR uint64_t s_phase[5];
static DRAM_ATTR uint32_t s_phase_samples=0u;
static DRAM_ATTR uint32_t s_path_n[TAB5_EP_PATH_COUNT];
static DRAM_ATTR uint64_t s_path_total[TAB5_EP_PATH_COUNT],s_path_front[TAB5_EP_PATH_COUNT];
static DRAM_ATTR uint32_t s_family_n[16];
static DRAM_ATTR uint64_t s_family_total[16],s_family_front[16];
static DRAM_ATTR uint32_t s_same_page=0u,s_page_change=0u,s_decode_now=0u,s_xop_null=0u;
static DRAM_ATTR uint64_t s_same_resolve=0u,s_change_resolve=0u,s_decode_resolve=0u;

static const char * const s_path_name[TAB5_EP_PATH_COUNT]={
 "MDX622_ZERO","MDX619_FIXED","M2_EARLY","M2_FALLBACK","S2","DIRECT","CORE_FAST",
 "BNE_FAST","BEQ_FAST","BCC_FAST","DBF_FAST","POST_OTHER","OTHER"
};
static const char * const s_family_name[16]={
 "IMM/BIT","MOVE.B","MOVE.L","MOVE.W","MISC","ADDQ/SUBQ/Scc/DBcc","Bcc/BSR","MOVEQ",
 "OR/DIV/SBCD","SUB","LINE-A","CMP/EOR","AND/MUL/EXG","ADD","SHIFT/ROT","LINE-F"
};

static void ep2_reset(void){
    s_samples=0u;s_total=0u;s_front=0u;s_phase_samples=0u;
    memset((void*)s_phase,0,sizeof(s_phase));
    memset((void*)s_path_n,0,sizeof(s_path_n));
    memset((void*)s_path_total,0,sizeof(s_path_total));
    memset((void*)s_path_front,0,sizeof(s_path_front));
    memset((void*)s_family_n,0,sizeof(s_family_n));
    memset((void*)s_family_total,0,sizeof(s_family_total));
    memset((void*)s_family_front,0,sizeof(s_family_front));
    s_same_page=s_page_change=s_decode_now=s_xop_null=0u;
    s_same_resolve=s_change_resolve=s_decode_resolve=0u;
    g_tab5_cpu1_ep_sample_counter=0u;
}
int tab5_cpu1_fr_suppress_s5(uint32_t frame){
    return frame>=TAB5_EP2_START_FRAME && frame<(TAB5_EP2_START_FRAME+TAB5_EP2_CAPTURE_FRAMES);
}
void tab5_cpu1_fr_frame_begin(uint32_t frame){
    if(frame==(TAB5_EP2_START_FRAME-1u) && !s_done)
        printf("R140P4EP2 ARMED start=%u frames=%u samplePeriod=%u mode=FRONT_BREAKDOWN_NO_PSRAM\n",
               (unsigned)TAB5_EP2_START_FRAME,(unsigned)TAB5_EP2_CAPTURE_FRAMES,(unsigned)TAB5_EP2_SAMPLE_PERIOD);
    if(frame==TAB5_EP2_START_FRAME && !s_done){
        ep2_reset();g_tab5_cpu1_ep_active=1u;g_tab5_cpu1_fr_active=1u;
    }
    if(frame==(TAB5_EP2_START_FRAME+TAB5_EP2_CAPTURE_FRAMES) && !s_done){
        g_tab5_cpu1_ep_active=0u;g_tab5_cpu1_fr_active=0u;s_done=1u;
    }
}
void tab5_cpu1_fr_frame_wall_start(int64_t v){(void)v;}
void tab5_cpu1_fr_frame_end(uint32_t f,int64_t v){(void)f;(void)v;}
void tab5_cpu1_fr_add_phase(uint32_t p,uint32_t c){(void)p;(void)c;}
void tab5_cpu1_fr_note_slice(uint32_t a,uint32_t b,int c,int d,uint32_t e,uint32_t f,uint32_t g,uint32_t h,uint32_t i,uint32_t j){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;}
void tab5_cpu1_fr_note_finish(uint32_t a,uint32_t b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;}
void tab5_cpu1_fr_note_poll(uint32_t a,uint32_t b,uint32_t c){(void)a;(void)b;(void)c;}
void tab5_cpu1_fr_note_mmio(uint32_t a,uint32_t b,uint8_t c,uint8_t d){(void)a;(void)b;(void)c;(void)d;}
void tab5_cpu1_fr_audio_tick(uint32_t t){g_tab5_cpu1_fr_guest_tick=t;}
void tab5_cpu1_fr_note_audio_push(uint8_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void tab5_cpu1_fr_note_r57(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void tab5_cpu1_fr_note_compose(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){(void)a;(void)b;(void)c;(void)d;(void)e;}
void tab5_cpu1_fr_note_framepub(uint32_t a,uint32_t b){(void)a;(void)b;}
void tab5_cpu1_fr_note_wait(uint32_t a,uint32_t b){(void)a;(void)b;}

void tab5_cpu1_ep2_sample_record(uint16_t op,uint8_t path,uint8_t rf,
                                 uint32_t total,uint32_t front,
                                 uint32_t pre,uint32_t gate,uint32_t addr,
                                 uint32_t resolve,uint32_t unpack){
    if(!g_tab5_cpu1_ep_active)return;
    if(front>total)front=total;
    if(path>=TAB5_EP_PATH_COUNT)path=TAB5_EP_PATH_OTHER;
    const unsigned fam=(unsigned)(op>>12);
    ++s_samples;s_total+=total;s_front+=front;
    ++s_path_n[path];s_path_total[path]+=total;s_path_front[path]+=front;
    ++s_family_n[fam];s_family_total[fam]+=total;s_family_front[fam]+=front;

    if(!(rf&(TAB5_EP2_RF_FUSED|TAB5_EP2_RF_M2_EARLY)) && front){
        ++s_phase_samples;
        s_phase[0]+=pre;s_phase[1]+=gate;s_phase[2]+=addr;s_phase[3]+=resolve;s_phase[4]+=unpack;
        if(rf&TAB5_EP2_RF_SAME_PAGE){++s_same_page;s_same_resolve+=resolve;}
        else {++s_page_change;s_change_resolve+=resolve;}
        if(rf&TAB5_EP2_RF_DECODE_NOW){++s_decode_now;s_decode_resolve+=resolve;}
        if(rf&TAB5_EP2_RF_XOP_NULL)++s_xop_null;
    }
}
void tab5_cpu1_fr_dump_once(void){
    if(!s_done||s_dumped)return;
    s_dumped=1u;
    static const char *pn[5]={"pre","pcgate","addrcheck","resolve","unpack"};
    printf("R140P4EP2 DUMP BEGIN start=%u frames=%u samplePeriod=%u samples=%u total=%llu front=%llu body=%llu frontPct_x100=%llu\n",
           (unsigned)TAB5_EP2_START_FRAME,(unsigned)TAB5_EP2_CAPTURE_FRAMES,(unsigned)TAB5_EP2_SAMPLE_PERIOD,
           (unsigned)s_samples,(unsigned long long)s_total,(unsigned long long)s_front,
           (unsigned long long)(s_total-s_front),
           (unsigned long long)(s_total?s_front*10000ull/s_total:0ull));
    uint64_t phase_sum=0u;for(unsigned i=0;i<5;i++)phase_sum+=s_phase[i];
    printf("EP2 FRONT_SUM ordinarySamples=%u phaseSum=%llu sampledFront=%llu\n",
           (unsigned)s_phase_samples,(unsigned long long)phase_sum,(unsigned long long)s_front);
    for(unsigned i=0;i<5;i++)
        printf("EP2 FRONT_PHASE id=%u name=%s cycles=%llu shareFront_x100=%llu avg=%llu\n",
               i,pn[i],(unsigned long long)s_phase[i],
               (unsigned long long)(phase_sum?s_phase[i]*10000ull/phase_sum:0ull),
               (unsigned long long)(s_phase_samples?s_phase[i]/s_phase_samples:0ull));
    printf("EP2 RESOLVE samePage=%u pageChange=%u decodeNow=%u xopNull=%u sameResolveAvg=%llu changeResolveAvg=%llu decodeResolveAvg=%llu\n",
           (unsigned)s_same_page,(unsigned)s_page_change,(unsigned)s_decode_now,(unsigned)s_xop_null,
           (unsigned long long)(s_same_page?s_same_resolve/s_same_page:0ull),
           (unsigned long long)(s_page_change?s_change_resolve/s_page_change:0ull),
           (unsigned long long)(s_decode_now?s_decode_resolve/s_decode_now:0ull));
    for(unsigned i=0;i<TAB5_EP_PATH_COUNT;i++)if(s_path_n[i])
        printf("EP2 PATH id=%u name=%s samples=%u cycles=%llu share_x100=%llu frontPct_x100=%llu avg=%llu\n",
               i,s_path_name[i],(unsigned)s_path_n[i],(unsigned long long)s_path_total[i],
               (unsigned long long)(s_total?s_path_total[i]*10000ull/s_total:0ull),
               (unsigned long long)(s_path_total[i]?s_path_front[i]*10000ull/s_path_total[i]:0ull),
               (unsigned long long)(s_path_n[i]?s_path_total[i]/s_path_n[i]:0ull));
    for(unsigned i=0;i<16;i++)if(s_family_n[i])
        printf("EP2 FAMILY nibble=%x name=%s samples=%u cycles=%llu share_x100=%llu frontPct_x100=%llu avg=%llu\n",
               i,s_family_name[i],(unsigned)s_family_n[i],(unsigned long long)s_family_total[i],
               (unsigned long long)(s_total?s_family_total[i]*10000ull/s_total:0ull),
               (unsigned long long)(s_family_total[i]?s_family_front[i]*10000ull/s_family_total[i]:0ull),
               (unsigned long long)(s_family_n[i]?s_family_total[i]/s_family_n[i]:0ull));
    printf("R140P4EP2 DUMP END\n");
}
