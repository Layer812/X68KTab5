from pathlib import Path
import sys

ROOT=Path('.')
BUS_C=ROOT/'src/tab5_guest_bus.c'
BUS_H=ROOT/'src/tab5_guest_bus.h'
COMP=ROOT/'src/tab5_compose.c'
MAIN=ROOT/'src/main.c'
MARK='PX68K_R57A2: dedicated CPU0 journal consumer ACTIVE; CPU1 posted writes never wait; R56s5k correctness retained'
TAG='PX68K_R57A2_DEDICATED_CONSUMER'

def fail(msg):
    print('R57a2 patch ERROR:',msg)
    raise SystemExit(2)

for p in (BUS_C,BUS_H,COMP,MAIN):
    if not p.is_file(): fail(f'missing {p}')

bus_c=r'''/* PX68K_R57A_GUEST_RENDER_JOURNAL
 * PX68K_R57A2_DEDICATED_CONSUMER
 * CPU1 is the guest-authoritative producer. CPU0 has one dedicated consumer.
 * Rendering semantics remain R56s5k exact in this phase. The dedicated worker
 * removes the accidental dependency on compositor job wakeups seen in R57a. */
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

typedef struct {
    uint32_t seq;
    uint32_t addr;
    uint32_t meta; /* low8=domain, bits8..15=value */
} r57_evt_t;

static const char *TAG="TAB5_R57BUS";
static r57_evt_t *s_ring;
static TaskHandle_t s_consumer_task;
static volatile uint32_t s_head, s_tail;
static volatile uint32_t s_attempt_seq, s_prod_seq, s_cons_seq;
static volatile uint32_t s_prod_hash=2166136261u, s_cons_hash=2166136261u;
static volatile uint32_t s_overflow, s_order_fault, s_max_depth, s_notify_count;
static volatile uint32_t s_drain_batches;
static uint32_t s_next_report=65536u;
static int s_ready;

static inline uint32_t ld_acq(volatile uint32_t *p){ return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static inline uint32_t ld_relaxed(volatile uint32_t *p){ return __atomic_load_n(p,__ATOMIC_RELAXED); }
static inline void st_rel(volatile uint32_t *p,uint32_t v){ __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static inline uint32_t mix32(uint32_t h,uint32_t v){ h^=v; h*=16777619u; h^=v>>16; h*=16777619u; return h; }

static uint32_t drain_once(void)
{
    uint32_t tail=ld_relaxed(&s_tail), head=ld_acq(&s_head), n=0u;
    uint32_t cseq=ld_relaxed(&s_cons_seq), h=ld_relaxed(&s_cons_hash);
    while (tail!=head && n<R57_DRAIN_QUANTUM) {
        const r57_evt_t e=s_ring[tail&R57_RING_MASK];
        if (__builtin_expect(e.seq!=cseq+1u,0))
            __atomic_add_fetch(&s_order_fault,1u,__ATOMIC_RELAXED);
        cseq=e.seq;
        h=mix32(h,e.seq); h=mix32(h,e.addr); h=mix32(h,e.meta);
        ++tail; ++n;
    }
    if (n) {
        __atomic_store_n(&s_cons_hash,h,__ATOMIC_RELAXED);
        __atomic_store_n(&s_cons_seq,cseq,__ATOMIC_RELAXED);
        st_rel(&s_tail,tail);
        __atomic_add_fetch(&s_drain_batches,1u,__ATOMIC_RELAXED);
    }
    return n;
}

static void report_if_due(void)
{
    const uint32_t cseq=ld_relaxed(&s_cons_seq);
    if (cseq<s_next_report) return;
    /* Report only at a caught-up boundary. Otherwise a healthy producer that
     * is merely ahead of the consumer would create a misleading ACTIVE line
     * and consume the one report opportunity for this 64K interval. */
    if (ld_relaxed(&s_tail)!=ld_acq(&s_head)) return;
    const uint32_t pseq=ld_acq(&s_prod_seq), attempt=ld_acq(&s_attempt_seq);
    const uint32_t ph=ld_relaxed(&s_prod_hash), ch=ld_relaxed(&s_cons_hash);
    const uint32_t ov=ld_relaxed(&s_overflow), of=ld_relaxed(&s_order_fault);
    const uint32_t depth=ld_acq(&s_head)-ld_acq(&s_tail);
    const int exact=(attempt==pseq && pseq==cseq && ph==ch && ov==0u && of==0u && depth==0u);
    ESP_LOGI(TAG,"PX68K_R57A2_JOURNAL: %s attempt=%lu prod=%lu cons=%lu hash=%08lX/%08lX overflow=%lu order=%lu depth=%lu maxDepth=%lu notify=%lu batches=%lu",
             exact?"COHERENT":"ACTIVE",
             (unsigned long)attempt,(unsigned long)pseq,(unsigned long)cseq,
             (unsigned long)ph,(unsigned long)ch,(unsigned long)ov,(unsigned long)of,
             (unsigned long)depth,(unsigned long)ld_relaxed(&s_max_depth),
             (unsigned long)ld_relaxed(&s_notify_count),(unsigned long)ld_relaxed(&s_drain_batches));
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
            /* Bound one CPU0 run. Audio/LCD/Screen at higher priority preempt us;
             * this yield also prevents the observer from monopolizing idle headroom. */
            taskYIELD();
        }
    }
}

int tab5_guest_bus_init(void)
{
    if (s_ready) return 1;
    s_ring=(r57_evt_t*)heap_caps_aligned_calloc(64,R57_RING_SLOTS,sizeof(r57_evt_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGW(TAG,"PX68K_R57A2_JOURNAL: allocation failed; observation disabled, R56s5k semantics retained");
        return 0;
    }
    s_head=s_tail=s_attempt_seq=s_prod_seq=s_cons_seq=s_overflow=s_order_fault=s_max_depth=0u;
    s_notify_count=s_drain_batches=0u;
    s_prod_hash=s_cons_hash=2166136261u;
    s_next_report=65536u;
#if portNUM_PROCESSORS > 1
    BaseType_t ok=xTaskCreatePinnedToCore(r57_consumer_task,"px68k_r57bus",4096,NULL,2,&s_consumer_task,0);
#else
    BaseType_t ok=xTaskCreate(r57_consumer_task,"px68k_r57bus",4096,NULL,2,&s_consumer_task);
#endif
    if (ok!=pdPASS || !s_consumer_task) {
        ESP_LOGW(TAG,"PX68K_R57A2_JOURNAL: consumer task create failed; observation disabled");
        heap_caps_free(s_ring); s_ring=NULL; s_consumer_task=NULL;
        return 0;
    }
    s_ready=1;
    ESP_LOGI(TAG,"PX68K_R57A2_JOURNAL: dedicated CPU0 consumer ready slots=%u bytes=%u prio=2 notifyEvery<=64; CPU1 never blocks; render semantics unchanged",
             (unsigned)R57_RING_SLOTS,(unsigned)(R57_RING_SLOTS*sizeof(r57_evt_t)));
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

    /* Cross-core wakeup is deliberately sparse: first event after empty plus
     * one wake per 64 producer positions. CPU1 never waits for the consumer. */
    if (depth==0u || ((head&R57_NOTIFY_MASK)==0u)) {
        TaskHandle_t t=s_consumer_task;
        if (t) { xTaskNotifyGive(t); __atomic_add_fetch(&s_notify_count,1u,__ATOMIC_RELAXED); }
    }
}
'''

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

#ifdef __cplusplus
}
#endif
'''

co=COMP.read_text(encoding='utf-8')
ma=MAIN.read_text(encoding='utf-8')
oldbus=BUS_C.read_text(encoding='utf-8')
if TAG in oldbus and MARK in ma and 'tab5_guest_bus_drain_cpu0();' not in co:
    BUS_C.write_text(bus_c,encoding='utf-8',newline='')
    BUS_H.write_text(bus_h,encoding='utf-8',newline='')
    print('R57a2 already applied and verified')
    raise SystemExit(0)
if 'PX68K_R57A_GUEST_RENDER_JOURNAL' not in oldbus: fail('R57a base journal not present')
if 'PX68K_R57A:' not in ma: fail('R57a main lineage missing')
if co.count('        tab5_guest_bus_drain_cpu0();')!=1:
    fail(f'compose drain anchor count={co.count("        tab5_guest_bus_drain_cpu0();")}')
co2=co.replace('        tab5_guest_bus_drain_cpu0();\n','',1)
anchor='    ESP_LOGI(TAG, "PX68K_R57A: CPU1 guest/render posted-write journal ACTIVE; R56s5k correctness path retained");\n'
if MARK not in ma:
    if ma.count(anchor)!=1: fail(f'main R57a marker anchor count={ma.count(anchor)}')
    ma=ma.replace(anchor,anchor+f'    ESP_LOGI(TAG, "{MARK}");\n',1)

BUS_C.write_text(bus_c,encoding='utf-8',newline='')
BUS_H.write_text(bus_h,encoding='utf-8',newline='')
COMP.write_text(co2,encoding='utf-8',newline='')
MAIN.write_text(ma,encoding='utf-8',newline='')
print('R57a2 applied: journal consumer detached from compositor; dedicated CPU0 task + 8192-slot ring; CPU1 remains nonblocking')
