/* PX68K_R57A_GUEST_RENDER_JOURNAL
 * PX68K_R57A2_DEDICATED_CONSUMER
 * PX68K_R57B_BG_SHADOW
 * PX68K_R57C_RASTER_TOKEN
 * PX68K_R57D_ORDERED_SHADOW_HOLD
 *
 * CPU1 is guest-authoritative and is never allowed to block on this module.
 * CPU0 owns a private BG/Sprite source mirror. R57d adds an ordered bounded
 * hold queue: producer registers the freeze sequence before the raster token
 * becomes visible, the CPU0 journal consumer stops exactly at that sequence,
 * and the CPU0 compositor releases it after using the frozen shadow. */
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
#define R57_HOLD_SLOTS 256u
#define R57_HOLD_MASK  (R57_HOLD_SLOTS - 1u)

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

typedef struct {
    uint32_t seq;
    volatile uint32_t state; /* 1=active, 2=cancelled */
} r57_hold_t;

static const char *TAG="TAB5_R57BUS";
static r57_evt_t *s_ring;
static uint8_t *s_shadow_mem;
static uint8_t *s_bg_shadow, *s_sprite_shadow, *s_bgreg_shadow;
static uint8_t *s_bgchr8_shadow, *s_bgchr16_shadow;
static TaskHandle_t s_consumer_task;
static TaskHandle_t s_waiter_task;
static volatile uint32_t s_head, s_tail;
static volatile uint32_t s_attempt_seq, s_prod_seq, s_cons_seq, s_shadow_seq;
static volatile uint32_t s_prod_hash=2166136261u, s_cons_hash=2166136261u;
static volatile uint32_t s_overflow, s_order_fault, s_max_depth, s_notify_count;
static volatile uint32_t s_drain_batches;
static volatile uint32_t s_shadow_sprite_w, s_shadow_bgreg_w, s_shadow_bg_w;
static volatile uint32_t s_shadow_reset, s_shadow_chr8_w, s_shadow_chr16_w;
static volatile uint32_t s_raster_tokens, s_last_raster_seq, s_last_raster_vline;
static r57_hold_t s_holds[R57_HOLD_SLOTS];
static volatile uint32_t s_hold_head, s_hold_tail, s_hold_reached;
static volatile uint32_t s_hold_claims, s_hold_busy, s_hold_cancel, s_hold_release, s_hold_waits;
static uint32_t s_next_report=65536u;
static int s_ready, s_shadow_ready;
static volatile uint32_t s_shadow_invalid;

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
    } else if (domain==TAB5_R57_DOMAIN_RASTER) {
        __atomic_add_fetch(&s_raster_tokens,1u,__ATOMIC_RELAXED);
        __atomic_store_n(&s_last_raster_seq,e->seq,__ATOMIC_RELAXED);
        __atomic_store_n(&s_last_raster_vline,e->addr,__ATOMIC_RELAXED);
    }
}

static uint32_t current_hold_target(void)
{
    for (;;) {
        const uint32_t t=ld_relaxed(&s_hold_tail), h=ld_acq(&s_hold_head);
        if (t==h) return 0u;
        r57_hold_t *q=&s_holds[t&R57_HOLD_MASK];
        const uint32_t st=ld_acq(&q->state);
        if (st==2u) {
            st_rel(&q->state,0u);
            st_rel(&s_hold_tail,t+1u);
            continue;
        }
        if (st==1u) return q->seq;
        return 0u;
    }
}

static uint32_t drain_once(void)
{
    if (ld_acq(&s_hold_reached)) return 0u;
    uint32_t tail=ld_relaxed(&s_tail), head=ld_acq(&s_head), n=0u;
    uint32_t cseq=ld_relaxed(&s_cons_seq), h=ld_relaxed(&s_cons_hash);
    uint32_t hold_target=current_hold_target();
    int hit_hold=0;
    while (tail!=head && n<R57_DRAIN_QUANTUM) {
        const r57_evt_t e=s_ring[tail&R57_RING_MASK];
        if (__builtin_expect(e.seq!=cseq+1u,0))
            __atomic_add_fetch(&s_order_fault,1u,__ATOMIC_RELAXED);
        cseq=e.seq;
        shadow_apply(&e);
        h=mix32(h,e.seq); h=mix32(h,e.addr); h=mix32(h,e.meta);
        ++tail; ++n;
        if (hold_target && cseq==hold_target) { hit_hold=1; break; }
    }
    if (n) {
        __atomic_store_n(&s_cons_hash,h,__ATOMIC_RELAXED);
        __atomic_store_n(&s_cons_seq,cseq,__ATOMIC_RELAXED);
        __atomic_store_n(&s_shadow_seq,cseq,__ATOMIC_RELEASE);
        st_rel(&s_tail,tail);
        __atomic_add_fetch(&s_drain_batches,1u,__ATOMIC_RELAXED);
    }
    if (hit_hold) {
        st_rel(&s_hold_reached,cseq);
        TaskHandle_t w=s_waiter_task;
        if (w) xTaskNotifyGive(w);
    }
    return n;
}

static void report_if_due(void)
{
    const uint32_t cseq=ld_relaxed(&s_cons_seq);
    if (cseq<s_next_report) return;
    if (ld_relaxed(&s_tail)!=ld_acq(&s_head) || ld_acq(&s_hold_reached)) return;
    const uint32_t pseq=ld_acq(&s_prod_seq), attempt=ld_acq(&s_attempt_seq);
    const uint32_t ph=ld_relaxed(&s_prod_hash), ch=ld_relaxed(&s_cons_hash);
    const uint32_t ov=ld_relaxed(&s_overflow), of=ld_relaxed(&s_order_fault);
    const uint32_t depth=ld_acq(&s_head)-ld_acq(&s_tail);
    const int exact=(attempt==pseq && pseq==cseq && ph==ch && ov==0u && of==0u && depth==0u && ld_acq(&s_shadow_seq)==cseq);
    ESP_LOGI(TAG,"PX68K_R57B_SHADOW: %s attempt=%lu prod=%lu cons=%lu shadowSeq=%lu overflow=%lu order=%lu depth=%lu maxDepth=%lu raster=%lu lastRaster={seq=%lu y=%lu} holds{head=%lu tail=%lu reached=%lu claim=%lu busy=%lu cancel=%lu release=%lu wait=%lu} shadow{spr=%lu reg=%lu bg=%lu c8=%lu c16=%lu reset=%lu}",
             exact?"COHERENT":"ACTIVE",
             (unsigned long)attempt,(unsigned long)pseq,(unsigned long)cseq,(unsigned long)ld_acq(&s_shadow_seq),
             (unsigned long)ov,(unsigned long)of,(unsigned long)depth,(unsigned long)ld_relaxed(&s_max_depth),
             (unsigned long)ld_relaxed(&s_raster_tokens),(unsigned long)ld_relaxed(&s_last_raster_seq),
             (unsigned long)ld_relaxed(&s_last_raster_vline),
             (unsigned long)ld_acq(&s_hold_head),(unsigned long)ld_acq(&s_hold_tail),
             (unsigned long)ld_acq(&s_hold_reached),(unsigned long)ld_relaxed(&s_hold_claims),
             (unsigned long)ld_relaxed(&s_hold_busy),(unsigned long)ld_relaxed(&s_hold_cancel),
             (unsigned long)ld_relaxed(&s_hold_release),(unsigned long)ld_relaxed(&s_hold_waits),
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
            if (!n || ld_acq(&s_hold_reached) || ld_relaxed(&s_tail)==ld_acq(&s_head)) break;
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
        ESP_LOGW(TAG,"PX68K_R57D: allocation failed ring=%p shadow=%p; R56s5k exact fallback retained",(void*)s_ring,(void*)s_shadow_mem);
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
    memset(s_holds,0,sizeof(s_holds));
    s_shadow_ready=1;
    s_head=s_tail=s_attempt_seq=s_prod_seq=s_cons_seq=s_shadow_seq=0u;
    s_overflow=s_order_fault=s_max_depth=s_notify_count=s_drain_batches=0u;
    s_shadow_sprite_w=s_shadow_bgreg_w=s_shadow_bg_w=s_shadow_reset=s_shadow_chr8_w=s_shadow_chr16_w=0u;
    s_raster_tokens=s_last_raster_seq=s_last_raster_vline=0u;
    s_hold_head=s_hold_tail=s_hold_reached=0u;
    s_shadow_invalid=0u;
    s_hold_claims=s_hold_busy=s_hold_cancel=s_hold_release=s_hold_waits=0u;
    s_prod_hash=s_cons_hash=2166136261u;
    s_next_report=65536u;
#if portNUM_PROCESSORS > 1
    BaseType_t ok=xTaskCreatePinnedToCore(r57_consumer_task,"px68k_r57bus",4096,NULL,2,&s_consumer_task,0);
#else
    BaseType_t ok=xTaskCreate(r57_consumer_task,"px68k_r57bus",4096,NULL,2,&s_consumer_task);
#endif
    if (ok!=pdPASS || !s_consumer_task) {
        ESP_LOGW(TAG,"PX68K_R57D: consumer task create failed; exact fallback retained");
        heap_caps_free(s_ring); heap_caps_free(s_shadow_mem);
        s_ring=NULL; s_shadow_mem=NULL; s_shadow_ready=0; s_consumer_task=NULL;
        return 0;
    }
    s_ready=1;
    ESP_LOGI(TAG,"PX68K_R57A2_JOURNAL: dedicated CPU0 consumer ready slots=%u bytes=%u prio=2 notifyEvery<=64; CPU1 never blocks",
             (unsigned)R57_RING_SLOTS,(unsigned)(R57_RING_SLOTS*sizeof(r57_evt_t)));
    ESP_LOGI(TAG,"PX68K_R57D_SHADOW: ordered freeze queue ready holds=%u; CPU0 BG/Sprite shadow is eligible for 65K production ownership",
             (unsigned)R57_HOLD_SLOTS);
    return 1;
}

static uint32_t post_event_common(uint32_t domain,uint32_t address,uint8_t value,uint32_t hold)
{
    if (__builtin_expect(!s_ready,0)) return 0u;
    if (hold && __builtin_expect(ld_acq(&s_shadow_invalid)!=0u,0)) return 0u;
    const uint32_t head=ld_relaxed(&s_head), tail=ld_acq(&s_tail);
    const uint32_t depth=head-tail;
    if (__builtin_expect(depth>=R57_RING_SLOTS,0)) {
        __atomic_add_fetch(&s_overflow,1u,__ATOMIC_RELAXED);
        /* Lossless ordering is mandatory once renderer ownership moves here.
         * A dropped event permanently invalidates the shadow for this boot;
         * future shadow holds are refused and WinDraw falls back to exact CPU1. */
        st_rel(&s_shadow_invalid,1u);
        TaskHandle_t t=s_consumer_task; if (t) xTaskNotifyGive(t);
        return 0u;
    }
    const uint32_t seq=ld_relaxed(&s_attempt_seq)+1u;
    if (hold) {
        const uint32_t hh=ld_relaxed(&s_hold_head), ht=ld_acq(&s_hold_tail);
        if ((uint32_t)(hh-ht)>=R57_HOLD_SLOTS) {
            __atomic_add_fetch(&s_hold_busy,1u,__ATOMIC_RELAXED);
            return 0u;
        }
        r57_hold_t *q=&s_holds[hh&R57_HOLD_MASK];
        q->seq=seq;
        st_rel(&q->state,1u);
        st_rel(&s_hold_head,hh+1u); /* publish freeze before event */
        __atomic_add_fetch(&s_hold_claims,1u,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_attempt_seq,seq,__ATOMIC_RELAXED);
    r57_evt_t *e=&s_ring[head&R57_RING_MASK];
    e->seq=seq; e->addr=address; e->meta=(domain&0xffu)|((uint32_t)value<<8);
    uint32_t h=ld_relaxed(&s_prod_hash);
    h=mix32(h,seq); h=mix32(h,address); h=mix32(h,e->meta);
    __atomic_store_n(&s_prod_hash,h,__ATOMIC_RELAXED);
    __atomic_store_n(&s_prod_seq,seq,__ATOMIC_RELAXED);
    uint32_t d=depth+1u, old=ld_relaxed(&s_max_depth);
    while (d>old && !__atomic_compare_exchange_n(&s_max_depth,&old,d,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    st_rel(&s_head,head+1u);
    if (depth==0u || ((head&R57_NOTIFY_MASK)==0u) || hold) {
        TaskHandle_t t=s_consumer_task;
        if (t) { xTaskNotifyGive(t); __atomic_add_fetch(&s_notify_count,1u,__ATOMIC_RELAXED); }
    }
    return seq;
}

void tab5_guest_bus_post_write(uint32_t domain,uint32_t address,uint8_t value)
{
    (void)post_event_common(domain,address,value,0u);
}

void tab5_guest_bus_post_raster(uint32_t vline)
{
    (void)post_event_common(TAB5_R57_DOMAIN_RASTER,vline,0u,0u);
}

uint32_t tab5_guest_bus_post_raster_hold(uint32_t vline)
{
    return post_event_common(TAB5_R57_DOMAIN_RASTER,vline,0u,1u);
}

void tab5_guest_bus_shadow_hold_cancel(uint32_t seq)
{
    if (!seq) return;
    uint32_t t=ld_acq(&s_hold_tail), h=ld_acq(&s_hold_head);
    for (uint32_t i=t;i!=h;++i) {
        r57_hold_t *q=&s_holds[i&R57_HOLD_MASK];
        if (q->seq==seq && ld_acq(&q->state)==1u) {
            st_rel(&q->state,2u);
            __atomic_add_fetch(&s_hold_cancel,1u,__ATOMIC_RELAXED);
            /* Submission can fail just after CPU0 reaches the registered hold.
             * There is then no compositor job that could release it.  Cancel
             * must therefore unfreeze the consumer itself, but only for the
             * current head hold. */
            if (ld_acq(&s_hold_reached)==seq && i==t) {
                st_rel(&s_hold_reached,0u);
                st_rel(&q->state,0u);
                st_rel(&s_hold_tail,t+1u);
            }
            TaskHandle_t c=s_consumer_task; if (c) xTaskNotifyGive(c);
            TaskHandle_t w=s_waiter_task; if (w) xTaskNotifyGive(w);
            return;
        }
    }
}

int tab5_guest_bus_shadow_hold_wait(uint32_t seq)
{
    if (!seq || !s_ready) return 0;
    s_waiter_task=xTaskGetCurrentTaskHandle();
    __atomic_add_fetch(&s_hold_waits,1u,__ATOMIC_RELAXED);
    for (;;) {
        if (ld_acq(&s_hold_reached)==seq && ld_acq(&s_shadow_seq)==seq) return 1;
        const uint32_t t=ld_acq(&s_hold_tail), h=ld_acq(&s_hold_head);
        int found=0;
        for (uint32_t i=t;i!=h;++i) {
            r57_hold_t *q=&s_holds[i&R57_HOLD_MASK];
            if (q->seq==seq && ld_acq(&q->state)==1u) { found=1; break; }
        }
        if (!found) return 0;
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));
        TaskHandle_t c=s_consumer_task; if (c) xTaskNotifyGive(c);
    }
}

void tab5_guest_bus_shadow_hold_release(uint32_t seq)
{
    if (!seq) return;
    const uint32_t t=ld_acq(&s_hold_tail), h=ld_acq(&s_hold_head);
    if (t==h) return;
    r57_hold_t *q=&s_holds[t&R57_HOLD_MASK];
    if (q->seq!=seq || ld_acq(&q->state)!=1u) return;
    st_rel(&q->state,0u);
    st_rel(&s_hold_reached,0u);
    st_rel(&s_hold_tail,t+1u);
    __atomic_add_fetch(&s_hold_release,1u,__ATOMIC_RELAXED);
    TaskHandle_t c=s_consumer_task; if (c) xTaskNotifyGive(c);
}

int tab5_guest_bus_bg_shadow_ready(void){ return s_shadow_ready && ld_acq(&s_shadow_invalid)==0u; }
const uint8_t *tab5_guest_bus_bg_shadow(void){ return s_bg_shadow; }
const uint8_t *tab5_guest_bus_bgchr8_shadow(void){ return s_bgchr8_shadow; }
const uint8_t *tab5_guest_bus_bgchr16_shadow(void){ return s_bgchr16_shadow; }
const uint8_t *tab5_guest_bus_sprite_shadow(void){ return s_sprite_shadow; }
const uint8_t *tab5_guest_bus_bgreg_shadow(void){ return s_bgreg_shadow; }
uint32_t tab5_guest_bus_shadow_seq(void){ return ld_acq(&s_shadow_seq); }
