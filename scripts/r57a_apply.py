from pathlib import Path
import re, sys

TAG='PX68K_R57A_GUEST_RENDER_JOURNAL'
MAIN_MARK='PX68K_R57A: CPU1 guest/render posted-write journal ACTIVE; R56s5k correctness path retained'

ROOT=Path('.')
CMAKE=ROOT/'src/CMakeLists.txt'
MAIN=ROOT/'src/main.c'
COMP=ROOT/'src/tab5_compose.c'
TV=ROOT/'components/px68k/x68k/tvram.c'
BG=ROOT/'components/px68k/x68k/bg.c'
BUS_C=ROOT/'src/tab5_guest_bus.c'
BUS_H=ROOT/'src/tab5_guest_bus.h'

def fail(msg):
    print('R57a patch ERROR:', msg)
    raise SystemExit(2)

def function_span(text, signature):
    p=text.find(signature)
    if p<0: fail(f'function not found: {signature}')
    b=text.find('{',p)
    if b<0: fail(f'opening brace not found: {signature}')
    depth=0
    i=b
    in_s=in_c=in_line=in_block=False
    esc=False
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

def patch_func(text, signature, transform):
    a,b=function_span(text,signature)
    old=text[a:b]
    new=transform(old)
    if new==old: fail(f'no change in function: {signature}')
    return text[:a]+new+text[b:]

for p in (CMAKE, MAIN, COMP, TV, BG):
    if not p.is_file(): fail(f'missing {p}')

# New source files are package-owned and may be overwritten only when identical tag lineage exists.
bus_h=r'''#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TAB5_R57_DOMAIN_TVRAM  = 1,
    TAB5_R57_DOMAIN_SPRITE = 2,
    TAB5_R57_DOMAIN_BGREG  = 3,
    TAB5_R57_DOMAIN_BGMEM  = 4,
};

int tab5_guest_bus_init(void);
void tab5_guest_bus_post_write(uint32_t domain, uint32_t address, uint8_t value);
void tab5_guest_bus_drain_cpu0(void);

#ifdef __cplusplus
}
#endif
'''
bus_c=r'''/* PX68K_R57A_GUEST_RENDER_JOURNAL
 * CPU1 is the guest-authoritative producer. CPU0 is the sole consumer.
 * This first cut changes no rendering semantics: it proves a lossless ordered
 * posted-write boundary before render shadow ownership is switched in R57b. */
#include "tab5_guest_bus.h"

#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define R57_RING_SLOTS 4096u
#define R57_RING_MASK  (R57_RING_SLOTS - 1u)
#define R57_DRAIN_BUDGET 1024u

typedef struct {
    uint32_t seq;
    uint32_t addr;
    uint32_t meta; /* low8=domain, bits8..15=value */
} r57_evt_t;

static const char *TAG="TAB5_R57BUS";
static r57_evt_t *s_ring;
static volatile uint32_t s_head, s_tail;
static volatile uint32_t s_prod_seq, s_cons_seq;
static volatile uint32_t s_prod_hash=2166136261u, s_cons_hash=2166136261u;
static volatile uint32_t s_overflow, s_order_fault, s_max_depth;
static uint32_t s_next_report=65536u;
static int s_ready;

extern void tab5_compose_guest_event_kick(void);

static inline uint32_t ld_acq(volatile uint32_t *p){ return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static inline uint32_t ld_relaxed(volatile uint32_t *p){ return __atomic_load_n(p,__ATOMIC_RELAXED); }
static inline void st_rel(volatile uint32_t *p,uint32_t v){ __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static inline uint32_t mix32(uint32_t h,uint32_t v){ h^=v; h*=16777619u; h^=v>>16; h*=16777619u; return h; }

int tab5_guest_bus_init(void)
{
    if (s_ready) return 1;
    s_ring=(r57_evt_t*)heap_caps_aligned_calloc(64,R57_RING_SLOTS,sizeof(r57_evt_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGW(TAG,"PX68K_R57A_JOURNAL: allocation failed; observation disabled, R56s5k semantics retained");
        return 0;
    }
    s_head=s_tail=s_prod_seq=s_cons_seq=s_overflow=s_order_fault=s_max_depth=0u;
    s_prod_hash=s_cons_hash=2166136261u;
    s_next_report=65536u;
    s_ready=1;
    ESP_LOGI(TAG,"PX68K_R57A_JOURNAL: CPU1->CPU0 posted-write SPSC ready slots=%u bytes=%u; render semantics unchanged",
             (unsigned)R57_RING_SLOTS,(unsigned)(R57_RING_SLOTS*sizeof(r57_evt_t)));
    return 1;
}

void tab5_guest_bus_post_write(uint32_t domain,uint32_t address,uint8_t value)
{
    if (__builtin_expect(!s_ready,0)) return;
    const uint32_t head=ld_relaxed(&s_head);
    const uint32_t tail=ld_acq(&s_tail);
    const uint32_t depth=head-tail;
    if (__builtin_expect(depth>=R57_RING_SLOTS,0)) {
        __atomic_add_fetch(&s_overflow,1u,__ATOMIC_RELAXED);
        tab5_compose_guest_event_kick();
        return;
    }
    uint32_t seq=ld_relaxed(&s_prod_seq)+1u;
    r57_evt_t *e=&s_ring[head&R57_RING_MASK];
    e->seq=seq; e->addr=address; e->meta=(domain&0xffu)|((uint32_t)value<<8);
    uint32_t h=ld_relaxed(&s_prod_hash);
    h=mix32(h,seq); h=mix32(h,address); h=mix32(h,e->meta);
    __atomic_store_n(&s_prod_hash,h,__ATOMIC_RELAXED);
    __atomic_store_n(&s_prod_seq,seq,__ATOMIC_RELAXED);
    uint32_t d=depth+1u, old=ld_relaxed(&s_max_depth);
    while (d>old && !__atomic_compare_exchange_n(&s_max_depth,&old,d,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    st_rel(&s_head,head+1u);
    if (depth==0u) tab5_compose_guest_event_kick();
}

void tab5_guest_bus_drain_cpu0(void)
{
    if (!s_ready) return;
    uint32_t tail=ld_relaxed(&s_tail), head=ld_acq(&s_head), n=0u;
    uint32_t cseq=ld_relaxed(&s_cons_seq), h=ld_relaxed(&s_cons_hash);
    while (tail!=head && n<R57_DRAIN_BUDGET) {
        const r57_evt_t e=s_ring[tail&R57_RING_MASK];
        if (__builtin_expect(e.seq!=cseq+1u,0)) __atomic_add_fetch(&s_order_fault,1u,__ATOMIC_RELAXED);
        cseq=e.seq;
        h=mix32(h,e.seq); h=mix32(h,e.addr); h=mix32(h,e.meta);
        ++tail; ++n;
    }
    if (n) {
        __atomic_store_n(&s_cons_hash,h,__ATOMIC_RELAXED);
        __atomic_store_n(&s_cons_seq,cseq,__ATOMIC_RELAXED);
        st_rel(&s_tail,tail);
    }
    if (cseq>=s_next_report) {
        const uint32_t pseq=ld_acq(&s_prod_seq), ph=ld_relaxed(&s_prod_hash);
        const uint32_t ov=ld_relaxed(&s_overflow), of=ld_relaxed(&s_order_fault);
        const int exact=(pseq==cseq && ph==h && ov==0u && of==0u);
        ESP_LOGI(TAG,"PX68K_R57A_JOURNAL: %s prod=%lu cons=%lu hash=%08lX/%08lX overflow=%lu order=%lu maxDepth=%lu",
                 exact?"COHERENT":"ACTIVE",
                 (unsigned long)pseq,(unsigned long)cseq,(unsigned long)ph,(unsigned long)h,
                 (unsigned long)ov,(unsigned long)of,(unsigned long)ld_relaxed(&s_max_depth));
        s_next_report=((cseq/65536u)+1u)*65536u;
    }
}
'''

# Idempotent check first.
cm=CMAKE.read_text(encoding='utf-8')
ma=MAIN.read_text(encoding='utf-8')
co=COMP.read_text(encoding='utf-8')
tv=TV.read_text(encoding='utf-8')
bg=BG.read_text(encoding='utf-8')
if TAG in co and MAIN_MARK in ma and 'tab5_guest_bus.c' in cm and 'TAB5_R57_POST' in tv and 'TAB5_R57_BGPOST' in bg:
    BUS_H.write_text(bus_h,encoding='utf-8',newline='')
    BUS_C.write_text(bus_c,encoding='utf-8',newline='')
    print('R57a already applied and verified')
    raise SystemExit(0)
if any(x in co+ma+tv+bg+cm for x in [TAG, MAIN_MARK]) or ('tab5_guest_bus.c' in cm):
    fail('partial R57a state')

# CMake: compile new bus source.
needle='        "tab5_compose.c"'
if cm.count(needle)!=1: fail(f'CMake tab5_compose entry count={cm.count(needle)}')
cm2=cm.replace(needle,needle+'\n        "tab5_guest_bus.c"',1)

# main lineage marker.
anchor='    ESP_LOGW(TAG, "PX68K_R56S5K: visible TEXT host-BT quarantined after false one-line PASS; R56s4 exact BT authoritative");\n'
if ma.count(anchor)!=1: fail(f'R56s5k main marker count={ma.count(anchor)}')
ma2=ma.replace(anchor,anchor+f'    ESP_LOGI(TAG, "{MAIN_MARK}");\n',1)

# compose: include bus, halve burst queue while preserving power-of-two mask, init/drain/kick.
inc='#include "tab5_compose.h"\n'
if co.count(inc)!=1: fail('tab5_compose.h include anchor mismatch')
co2=co.replace(inc,inc+'#include "tab5_guest_bus.h"\n',1)
old_slots='#define TAB5_GBT65K_BURST_SLOTS 128u'
if co2.count(old_slots)!=1: fail(f'burst slot define count={co2.count(old_slots)}')
co2=co2.replace(old_slots,'#define TAB5_GBT65K_BURST_SLOTS 64u /* R57a: reclaim ~547KB PSRAM for render-shadow boundary */',1)
# kick function after task handle declaration.
task_anchor='static TaskHandle_t s_task;\n'
if co2.count(task_anchor)!=1: fail(f's_task declaration count={co2.count(task_anchor)}')
co2=co2.replace(task_anchor,task_anchor+r'''/* PX68K_R57A_GUEST_RENDER_JOURNAL: cross-core producer wake only; never waits. */
void tab5_compose_guest_event_kick(void)
{
    TaskHandle_t t=s_task;
    if (t) xTaskNotifyGive(t);
}
''',1)
# init bus inside compose init.
def add_init(fn):
    m=re.search(r'(if\s*\(s_ready\)\s*\n\s*return\s+1\s*;)',fn)
    if not m: fail('s_ready init guard not found')
    ins=m.group(1)+'\n\n    (void)tab5_guest_bus_init(); /* observation-only; allocation failure keeps R56s5k exact path */'
    return fn[:m.start()]+ins+fn[m.end():]
co2=patch_func(co2,'int tab5_compose_init(void)',add_init)
# drain at top of worker loop.
def add_drain(fn):
    old='    for (;;) {\n        uint8_t idx;'
    if old not in fn: fail('compose worker loop anchor not found')
    return fn.replace(old,'    for (;;) {\n        tab5_guest_bus_drain_cpu0();\n        uint8_t idx;',1)
co2=patch_func(co2,'static void compose_task(void *arg)',add_drain)
# source tag comment near top.
co2=co2.replace('#include "tab5_guest_bus.h"\n','#include "tab5_guest_bus.h"\n/* PX68K_R57A_GUEST_RENDER_JOURNAL */\n',1)

# TVRAM producer hook.
tv_inc='#include\t"tvram.h"\n'
if tv_inc not in tv: tv_inc='#include "tvram.h"\n'
if tv_inc not in tv: fail('tvram include anchor missing')
tv2=tv.replace(tv_inc,tv_inc+r'''#ifdef ESP_PLATFORM
extern void tab5_guest_bus_post_write(uint32_t domain, uint32_t address, uint8_t value);
#define TAB5_R57_POST(a,v) tab5_guest_bus_post_write(1u,(uint32_t)(a),(uint8_t)(v))
#else
#define TAB5_R57_POST(a,v) ((void)0)
#endif
''',1)
def tv_hook(fn):
    old='\t\tTVRAM[adr] = data;'
    if fn.count(old)!=1:
        old='        TVRAM[adr] = data;'
    if fn.count(old)!=1: fail('TVRAM assignment anchor mismatch')
    return fn.replace(old,old+'\n\t\tTAB5_R57_POST(adr, data);',1)
tv2=patch_func(tv2,'static INLINE void TVRAM_WriteByte(uint32_t adr, uint8_t data)',tv_hook)
tv2=patch_func(tv2,'static INLINE void TVRAM_WriteByteMask(uint32_t adr, uint8_t data)',tv_hook)

# BG/Sprite producer hook; normal writes keep existing correctness/barrier for R57a.
bg_anchor='#include "m68000.h"\n'
if bg.count(bg_anchor)!=1: fail('bg m68000 include anchor mismatch')
bg2=bg.replace(bg_anchor,bg_anchor+r'''#ifdef ESP_PLATFORM
extern void tab5_guest_bus_post_write(uint32_t domain, uint32_t address, uint8_t value);
#define TAB5_R57_BGPOST(d,a,v) tab5_guest_bus_post_write((uint32_t)(d),(uint32_t)(a),(uint8_t)(v))
#else
#define TAB5_R57_BGPOST(d,a,v) ((void)0)
#endif
''',1)
def bg_hook(fn):
    reps=[
        ('\t\t\tSprite_Regs[adr] = data;','\t\t\tSprite_Regs[adr] = data;\n\t\t\tTAB5_R57_BGPOST(2u, adr, data);'),
        ('\t\tBG_Regs[adr] = data;','\t\tBG_Regs[adr] = data;\n\t\tTAB5_R57_BGPOST(3u, adr, data);'),
        ('\t\tBG[adr] = data;','\t\tBG[adr] = data;\n\t\tTAB5_R57_BGPOST(4u, adr, data);'),
    ]
    out=fn
    for old,new in reps:
        if out.count(old)!=1:
            # tolerate spaces
            s=old.replace('\t','    '); n=new.replace('\t','    ')
            if out.count(s)!=1: fail(f'BG assignment anchor mismatch: {old.strip()} count={out.count(old)} spaces={out.count(s)}')
            out=out.replace(s,n,1)
        else: out=out.replace(old,new,1)
    return out
bg2=patch_func(bg2,'void FASTCALL BG_Write(uint32_t adr, uint8_t data)',bg_hook)

# In-memory audit.
checks=[
 ('tab5_guest_bus.c' in cm2,'CMake bus source missing'),
 (MAIN_MARK in ma2,'main marker missing'),
 ('#define TAB5_GBT65K_BURST_SLOTS 64u' in co2,'64-slot rebalance missing'),
 ('tab5_guest_bus_drain_cpu0();' in co2,'CPU0 drain missing'),
 ('tab5_compose_guest_event_kick' in co2,'kick missing'),
 (tv2.count('TAB5_R57_POST(adr, data);')==2,'TVRAM hooks !=2'),
 (bg2.count('TAB5_R57_BGPOST(')>=4,'BG hooks missing'),
 ('PX68K_R56S5K' in ma2,'R56s5k correctness lineage missing'),
]
for ok,msg in checks:
    if not ok: fail(msg)

CMAKE.write_text(cm2,encoding='utf-8',newline='')
MAIN.write_text(ma2,encoding='utf-8',newline='')
COMP.write_text(co2,encoding='utf-8',newline='')
TV.write_text(tv2,encoding='utf-8',newline='')
BG.write_text(bg2,encoding='utf-8',newline='')
BUS_H.write_text(bus_h,encoding='utf-8',newline='')
BUS_C.write_text(bus_c,encoding='utf-8',newline='')
print('R57a applied: posted-write journal boundary + 64-slot PSRAM rebalance; rendering remains R56s5k exact')
