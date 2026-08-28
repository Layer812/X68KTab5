from pathlib import Path
import re

ROOT=Path('.')
BUS_C=ROOT/'src/tab5_guest_bus.c'
BUS_H=ROOT/'src/tab5_guest_bus.h'
BG=ROOT/'components/px68k/x68k/bg.c'
MAIN=ROOT/'src/main.c'
TAG='PX68K_R57B_BG_SHADOW'
MARK='PX68K_R57B: CPU0 BG/Sprite render-shadow mirror ACTIVE; ownership switch deferred to raster-token phase'

def fail(msg):
    print('R57b patch ERROR:',msg)
    raise SystemExit(2)

def function_span(text, signature):
    p=text.find(signature)
    if p<0: fail(f'function not found: {signature}')
    b=text.find('{',p)
    if b<0: fail(f'opening brace not found: {signature}')
    depth=0; i=b; in_s=in_c=in_line=in_block=False; esc=False
    while i<len(text):
        ch=text[i]; nx=text[i+1] if i+1<len(text) else ''
        if in_line:
            if ch=='\n': in_line=False
        elif in_block:
            if ch=='*' and nx=='/': in_block=False; i+=1
        elif in_s:
            if esc: esc=False
            elif ch=='\\': esc=True
            elif ch=='"': in_s=False
        elif in_c:
            if esc: esc=False
            elif ch=='\\': esc=True
            elif ch=="'": in_c=False
        else:
            if ch=='/' and nx=='/': in_line=True; i+=1
            elif ch=='/' and nx=='*': in_block=True; i+=1
            elif ch=='"': in_s=True
            elif ch=="'": in_c=True
            elif ch=='{': depth+=1
            elif ch=='}':
                depth-=1
                if depth==0: return p,i+1
        i+=1
    fail(f'unclosed function: {signature}')

for p in (BUS_C,BUS_H,BG,MAIN):
    if not p.is_file(): fail(f'missing {p}')

old=BUS_C.read_text(encoding='utf-8')
ma=MAIN.read_text(encoding='utf-8')
bg=BG.read_text(encoding='utf-8')
if 'PX68K_R57A2_DEDICATED_CONSUMER' not in old:
    fail('R57a2 dedicated consumer is not present')
if 'PX68K_R57A2:' not in ma:
    fail('R57a2 main lineage missing')
if 'TAB5_R57_BGPOST' not in bg:
    fail('R57a BG producer hooks missing')

bus_h=r'''#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TAB5_R57_DOMAIN_TVRAM    = 1,
    TAB5_R57_DOMAIN_SPRITE   = 2,
    TAB5_R57_DOMAIN_BGREG    = 3,
    TAB5_R57_DOMAIN_BGMEM    = 4,
    TAB5_R57_DOMAIN_BG_RESET = 5,
};

int tab5_guest_bus_init(void);
void tab5_guest_bus_post_write(uint32_t domain, uint32_t address, uint8_t value);

/* R57b: CPU0-owned mirror only. Production renderer does not use these yet;
 * R57c will consume them at ordered raster-token boundaries. */
int tab5_guest_bus_bg_shadow_ready(void);
const uint8_t *tab5_guest_bus_bg_shadow(void);
const uint8_t *tab5_guest_bus_bgchr8_shadow(void);
const uint8_t *tab5_guest_bus_bgchr16_shadow(void);
const uint8_t *tab5_guest_bus_sprite_shadow(void);
const uint8_t *tab5_guest_bus_bgreg_shadow(void);
uint32_t tab5_guest_bus_shadow_seq(void);

#ifdef __cplusplus
}
#endif
'''

bus_c=r'''/* PX68K_R57A_GUEST_RENDER_JOURNAL
 * PX68K_R57A2_DEDICATED_CONSUMER
 * PX68K_R57B_BG_SHADOW
 * CPU1 remains guest-authoritative and never waits. CPU0's dedicated journal
 * consumer now owns a private BG/Sprite source mirror plus exact BGCHR8/16
 * derived caches. Production rendering is intentionally still R56s5k exact;
 * the mirror is not consumed by the renderer until R57c raster-token ordering. */
#include "tab5_guest_bus.h"

#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define R57_RING_SLOTS 8192u
#define R57_RING_MASK  (R57_RING_SLOTS - 1u)
#define R57_DRAIN_QUANTUM 2048u
#define R57_NOTIFY_MASK 63u

#define R57_BG_BYTES       0x8000u
#define R57_SPRITE_BYTES   0x0800u
#define R57_BGREG_BYTES    0x0012u
#define R57_BGCHR8_BYTES   (8u*8u*256u)
#define R57_BGCHR16_BYTES  (16u*16u*256u)
#define R57_SHADOW_BYTES   (R57_BG_BYTES+R57_SPRITE_BYTES+R57_BGREG_BYTES+R57_BGCHR8_BYTES+R57_BGCHR16_BYTES)

typedef struct {
    uint32_t seq;
    uint32_t addr;
    uint32_t meta; /* low8=domain, bits8..15=value */
} r57_evt_t;

static const char *TAG="TAB5_R57BUS";
static r57_evt_t *s_ring;
static uint8_t *s_shadow_mem;
static uint8_t *s_bg_shadow, *s_sprite_shadow, *s_bgreg_shadow;
static uint8_t *s_bgchr8_shadow, *s_bgchr16_shadow;
static TaskHandle_t s_consumer_task;
static volatile uint32_t s_head, s_tail;
static volatile uint32_t s_attempt_seq, s_prod_seq, s_cons_seq, s_shadow_seq;
static volatile uint32_t s_prod_hash=2166136261u, s_cons_hash=2166136261u;
static volatile uint32_t s_overflow, s_order_fault, s_max_depth, s_notify_count;
static volatile uint32_t s_drain_batches;
static volatile uint32_t s_shadow_sprite_w, s_shadow_bgreg_w, s_shadow_bg_w;
static volatile uint32_t s_shadow_reset, s_shadow_chr8_w, s_shadow_chr16_w;
static uint32_t s_next_report=65536u;
static int s_ready, s_shadow_ready;

/* Guest arrays are read only once during host initialization, before guest
 * execution starts. After that, CPU0 shadow state changes only by ordered
 * journal events. */
extern uint8_t BG[0x8000];
extern uint8_t Sprite_Regs[0x800];
extern uint8_t BG_Regs[0x12];
extern uint8_t BGCHR8[8*8*256];
extern uint8_t BGCHR16[16*16*256];

static inline uint32_t ld_acq(volatile uint32_t *p){ return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static inline uint32_t ld_relaxed(volatile uint32_t *p){ return __atomic_load_n(p,__ATOMIC_RELAXED); }
static inline void st_rel(volatile uint32_t *p,uint32_t v){ __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static inline uint32_t mix32(uint32_t h,uint32_t v){ h^=v; h*=16777619u; h^=v>>16; h*=16777619u; return h; }

static void shadow_zero_bg(void)
{
    if (!s_shadow_ready) return;
    memset(s_bg_shadow,0,R57_BG_BYTES);
    memset(s_sprite_shadow,0,R57_SPRITE_BYTES);
    memset(s_bgreg_shadow,0,R57_BGREG_BYTES);
    memset(s_bgchr8_shadow,0,R57_BGCHR8_BYTES);
    memset(s_bgchr16_shadow,0,R57_BGCHR16_BYTES);
    __atomic_add_fetch(&s_shadow_reset,1u,__ATOMIC_RELAXED);
}

static inline void shadow_apply(const r57_evt_t *e)
{
    if (!s_shadow_ready || !e) return;
    const uint32_t domain=e->meta&0xffu;
    const uint8_t value=(uint8_t)(e->meta>>8);
    if (domain==TAB5_R57_DOMAIN_SPRITE) {
        const uint32_t a=e->addr&(R57_SPRITE_BYTES-1u);
        s_sprite_shadow[a]=value;
        __atomic_add_fetch(&s_shadow_sprite_w,1u,__ATOMIC_RELAXED);
    } else if (domain==TAB5_R57_DOMAIN_BGREG) {
        const uint32_t a=e->addr;
        if (a<R57_BGREG_BYTES) s_bgreg_shadow[a]=value;
        __atomic_add_fetch(&s_shadow_bgreg_w,1u,__ATOMIC_RELAXED);
    } else if (domain==TAB5_R57_DOMAIN_BGMEM) {
        const uint32_t a=e->addr&0x7fffu;
        s_bg_shadow[a]=value;
        if (a<0x2000u) {
            const uint32_t i=a*2u;
            s_bgchr8_shadow[i]=(uint8_t)(value>>4);
            s_bgchr8_shadow[i+1u]=(uint8_t)(value&15u);
            __atomic_add_fetch(&s_shadow_chr8_w,1u,__ATOMIC_RELAXED);
        }
        const uint32_t i=((a&3u)*2u)+((a&0x3cu)*4u)+((a&0x40u)>>3)+((a&0x7f80u)*2u);
        if (i+1u<R57_BGCHR16_BYTES) {
            s_bgchr16_shadow[i]=(uint8_t)(value>>4);
            s_bgchr16_shadow[i+1u]=(uint8_t)(value&15u);
        }
        __atomic_add_fetch(&s_shadow_chr16_w,1u,__ATOMIC_RELAXED);
        __atomic_add_fetch(&s_shadow_bg_w,1u,__ATOMIC_RELAXED);
    } else if (domain==TAB5_R57_DOMAIN_BG_RESET) {
        shadow_zero_bg();
    }
}

static uint32_t drain_once(void)
{
    uint32_t tail=ld_relaxed(&s_tail), head=ld_acq(&s_head), n=0u;
    uint32_t cseq=ld_relaxed(&s_cons_seq), h=ld_relaxed(&s_cons_hash);
    while (tail!=head && n<R57_DRAIN_QUANTUM) {
        const r57_evt_t e=s_ring[tail&R57_RING_MASK];
        if (__builtin_expect(e.seq!=cseq+1u,0))
            __atomic_add_fetch(&s_order_fault,1u,__ATOMIC_RELAXED);
        cseq=e.seq;
        shadow_apply(&e);
        h=mix32(h,e.seq); h=mix32(h,e.addr); h=mix32(h,e.meta);
        ++tail; ++n;
    }
    if (n) {
        __atomic_store_n(&s_cons_hash,h,__ATOMIC_RELAXED);
        __atomic_store_n(&s_cons_seq,cseq,__ATOMIC_RELAXED);
        __atomic_store_n(&s_shadow_seq,cseq,__ATOMIC_RELEASE);
        st_rel(&s_tail,tail);
        __atomic_add_fetch(&s_drain_batches,1u,__ATOMIC_RELAXED);
    }
    return n;
}

static void report_if_due(void)
{
    const uint32_t cseq=ld_relaxed(&s_cons_seq);
    if (cseq<s_next_report) return;
    if (ld_relaxed(&s_tail)!=ld_acq(&s_head)) return;
    const uint32_t pseq=ld_acq(&s_prod_seq), attempt=ld_acq(&s_attempt_seq);
    const uint32_t ph=ld_relaxed(&s_prod_hash), ch=ld_relaxed(&s_cons_hash);
    const uint32_t ov=ld_relaxed(&s_overflow), of=ld_relaxed(&s_order_fault);
    const uint32_t depth=ld_acq(&s_head)-ld_acq(&s_tail);
    const int exact=(attempt==pseq && pseq==cseq && ph==ch && ov==0u && of==0u && depth==0u && ld_acq(&s_shadow_seq)==cseq);
    ESP_LOGI(TAG,"PX68K_R57B_SHADOW: %s attempt=%lu prod=%lu cons=%lu shadowSeq=%lu overflow=%lu order=%lu depth=%lu maxDepth=%lu shadow{spr=%lu reg=%lu bg=%lu c8=%lu c16=%lu reset=%lu}",
             exact?"COHERENT":"ACTIVE",
             (unsigned long)attempt,(unsigned long)pseq,(unsigned long)cseq,(unsigned long)ld_acq(&s_shadow_seq),
             (unsigned long)ov,(unsigned long)of,(unsigned long)depth,(unsigned long)ld_relaxed(&s_max_depth),
             (unsigned long)ld_relaxed(&s_shadow_sprite_w),(unsigned long)ld_relaxed(&s_shadow_bgreg_w),
             (unsigned long)ld_relaxed(&s_shadow_bg_w),(unsigned long)ld_relaxed(&s_shadow_chr8_w),
             (unsigned long)ld_relaxed(&s_shadow_chr16_w),(unsigned long)ld_relaxed(&s_shadow_reset));
    s_next_report=((cseq/65536u)+1u)*65536u;
}

static void r57_consumer_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        for (;;) {
            const uint32_t n=drain_once();
            report_if_due();
            if (!n || ld_relaxed(&s_tail)==ld_acq(&s_head)) break;
            taskYIELD();
        }
    }
}

int tab5_guest_bus_init(void)
{
    if (s_ready) return 1;
    s_ring=(r57_evt_t*)heap_caps_aligned_calloc(64,R57_RING_SLOTS,sizeof(r57_evt_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_shadow_mem=(uint8_t*)heap_caps_aligned_calloc(64,1,R57_SHADOW_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_ring || !s_shadow_mem) {
        ESP_LOGW(TAG,"PX68K_R57B_SHADOW: allocation failed ring=%p shadow=%p; R56s5k semantics retained",(void*)s_ring,(void*)s_shadow_mem);
        if (s_ring) heap_caps_free(s_ring);
        if (s_shadow_mem) heap_caps_free(s_shadow_mem);
        s_ring=NULL; s_shadow_mem=NULL;
        return 0;
    }
    uint8_t *p=s_shadow_mem;
    s_bg_shadow=p; p+=R57_BG_BYTES;
    s_sprite_shadow=p; p+=R57_SPRITE_BYTES;
    s_bgreg_shadow=p; p+=R57_BGREG_BYTES;
    s_bgchr8_shadow=p; p+=R57_BGCHR8_BYTES;
    s_bgchr16_shadow=p;
    memcpy(s_bg_shadow,BG,R57_BG_BYTES);
    memcpy(s_sprite_shadow,Sprite_Regs,R57_SPRITE_BYTES);
    memcpy(s_bgreg_shadow,BG_Regs,R57_BGREG_BYTES);
    memcpy(s_bgchr8_shadow,BGCHR8,R57_BGCHR8_BYTES);
    memcpy(s_bgchr16_shadow,BGCHR16,R57_BGCHR16_BYTES);
    s_shadow_ready=1;
    s_head=s_tail=s_attempt_seq=s_prod_seq=s_cons_seq=s_shadow_seq=0u;
    s_overflow=s_order_fault=s_max_depth=s_notify_count=s_drain_batches=0u;
    s_shadow_sprite_w=s_shadow_bgreg_w=s_shadow_bg_w=s_shadow_reset=s_shadow_chr8_w=s_shadow_chr16_w=0u;
    s_prod_hash=s_cons_hash=2166136261u;
    s_next_report=65536u;
#if portNUM_PROCESSORS > 1
    BaseType_t ok=xTaskCreatePinnedToCore(r57_consumer_task,"px68k_r57bus",4096,NULL,2,&s_consumer_task,0);
#else
    BaseType_t ok=xTaskCreate(r57_consumer_task,"px68k_r57bus",4096,NULL,2,&s_consumer_task);
#endif
    if (ok!=pdPASS || !s_consumer_task) {
        ESP_LOGW(TAG,"PX68K_R57B_SHADOW: consumer task create failed; R56s5k semantics retained");
        heap_caps_free(s_ring); heap_caps_free(s_shadow_mem);
        s_ring=NULL; s_shadow_mem=NULL; s_shadow_ready=0; s_consumer_task=NULL;
        return 0;
    }
    s_ready=1;
    ESP_LOGI(TAG,"PX68K_R57A2_JOURNAL: dedicated CPU0 consumer ready slots=%u bytes=%u prio=2 notifyEvery<=64; CPU1 never blocks; render semantics unchanged",
             (unsigned)R57_RING_SLOTS,(unsigned)(R57_RING_SLOTS*sizeof(r57_evt_t)));
    ESP_LOGI(TAG,"PX68K_R57B_SHADOW: CPU0-owned BG/Sprite mirror ready bytes=%u BG=%u SPR=%u REG=%u CHR8=%u CHR16=%u; renderer switch=DEFERRED until raster-token ordering",
             (unsigned)R57_SHADOW_BYTES,(unsigned)R57_BG_BYTES,(unsigned)R57_SPRITE_BYTES,(unsigned)R57_BGREG_BYTES,
             (unsigned)R57_BGCHR8_BYTES,(unsigned)R57_BGCHR16_BYTES);
    return 1;
}

void tab5_guest_bus_post_write(uint32_t domain,uint32_t address,uint8_t value)
{
    if (__builtin_expect(!s_ready,0)) return;
    const uint32_t seq=__atomic_add_fetch(&s_attempt_seq,1u,__ATOMIC_RELAXED);
    const uint32_t head=ld_relaxed(&s_head);
    const uint32_t tail=ld_acq(&s_tail);
    const uint32_t depth=head-tail;
    if (__builtin_expect(depth>=R57_RING_SLOTS,0)) {
        __atomic_add_fetch(&s_overflow,1u,__ATOMIC_RELAXED);
        TaskHandle_t t=s_consumer_task;
        if (t) { xTaskNotifyGive(t); __atomic_add_fetch(&s_notify_count,1u,__ATOMIC_RELAXED); }
        return;
    }
    r57_evt_t *e=&s_ring[head&R57_RING_MASK];
    e->seq=seq; e->addr=address; e->meta=(domain&0xffu)|((uint32_t)value<<8);
    uint32_t h=ld_relaxed(&s_prod_hash);
    h=mix32(h,seq); h=mix32(h,address); h=mix32(h,e->meta);
    __atomic_store_n(&s_prod_hash,h,__ATOMIC_RELAXED);
    __atomic_store_n(&s_prod_seq,seq,__ATOMIC_RELAXED);
    uint32_t d=depth+1u, old=ld_relaxed(&s_max_depth);
    while (d>old && !__atomic_compare_exchange_n(&s_max_depth,&old,d,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    st_rel(&s_head,head+1u);
    if (depth==0u || ((head&R57_NOTIFY_MASK)==0u)) {
        TaskHandle_t t=s_consumer_task;
        if (t) { xTaskNotifyGive(t); __atomic_add_fetch(&s_notify_count,1u,__ATOMIC_RELAXED); }
    }
}

int tab5_guest_bus_bg_shadow_ready(void){ return s_shadow_ready; }
const uint8_t *tab5_guest_bus_bg_shadow(void){ return s_bg_shadow; }
const uint8_t *tab5_guest_bus_bgchr8_shadow(void){ return s_bgchr8_shadow; }
const uint8_t *tab5_guest_bus_bgchr16_shadow(void){ return s_bgchr16_shadow; }
const uint8_t *tab5_guest_bus_sprite_shadow(void){ return s_sprite_shadow; }
const uint8_t *tab5_guest_bus_bgreg_shadow(void){ return s_bgreg_shadow; }
uint32_t tab5_guest_bus_shadow_seq(void){ return ld_acq(&s_shadow_seq); }
'''

# BG reset event: exact ordered shadow reset for future repeated guest resets.
a,b=function_span(bg,'void BG_Init(void)')
fn=bg[a:b]
reset_call='\tTAB5_R57_BGPOST(5u, 0u, 0u); /* R57b ordered CPU0 shadow reset */'
if reset_call not in fn:
    anchor='\tBG_CHREND = 0x8000;'
    if anchor not in fn:
        anchor='    BG_CHREND = 0x8000;'
    if anchor not in fn: fail('BG_Init end anchor missing')
    indent='\t' if anchor.startswith('\t') else '    '
    fn2=fn.replace(anchor,anchor+'\n'+indent+'TAB5_R57_BGPOST(5u, 0u, 0u); /* R57b ordered CPU0 shadow reset */',1)
    bg=bg[:a]+fn2+bg[b:]

anchor='    ESP_LOGI(TAG, "PX68K_R57A2: dedicated CPU0 journal consumer ACTIVE; CPU1 posted writes never wait; R56s5k correctness retained");\n'
if MARK not in ma:
    if ma.count(anchor)!=1: fail(f'main R57a2 marker anchor count={ma.count(anchor)}')
    ma=ma.replace(anchor,anchor+f'    ESP_LOGI(TAG, "{MARK}");\n',1)

BUS_C.write_text(bus_c,encoding='utf-8',newline='')
BUS_H.write_text(bus_h,encoding='utf-8',newline='')
BG.write_text(bg,encoding='utf-8',newline='')
MAIN.write_text(ma,encoding='utf-8',newline='')
print('R57b applied: CPU0 BG/Sprite source + BGCHR8/16 shadow mirror active; renderer ownership intentionally unchanged')
