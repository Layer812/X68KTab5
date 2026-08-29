#include <stdint.h>
#define N 128u
#define H 600u
#define INIT  0x4C504232u /* LPB2 */
#define READY 0x4C504252u /* LPBR */
typedef struct __attribute__((aligned(4))) {
    volatile uint32_t ready;
    volatile uint32_t lp_echo;
    volatile uint32_t hp_probe;
    volatile uint32_t tail;
    volatile uint32_t head;
    volatile uint32_t frontier;
    volatile uint32_t duplicates;
    volatile uint32_t consumed;
    volatile uint32_t posts;
    volatile uint32_t heartbeat;
    volatile uint32_t drops;
    volatile uint32_t dirty_mask[H];
    volatile uint32_t latest_seq[H];
    volatile uint32_t ring[N*3u];
} lpbr_shared_t;
__attribute__((section(".lp_shared"),aligned(4))) volatile lpbr_shared_t g_lpbr;
static inline void fence_all(void){ __asm__ __volatile__("fence rw,rw":::"memory"); }
int main(void){
    /* Preserve LPB1 until C is alive, then publish LPB2 before table init. */
    g_lpbr.lp_echo=0; g_lpbr.hp_probe=0; g_lpbr.tail=0; g_lpbr.head=0;
    g_lpbr.frontier=0; g_lpbr.duplicates=0; g_lpbr.consumed=0; g_lpbr.posts=0;
    g_lpbr.heartbeat=0; g_lpbr.drops=0;
    fence_all(); g_lpbr.ready=INIT; fence_all();
    for(uint32_t i=0;i<H;i++){ g_lpbr.dirty_mask[i]=0; g_lpbr.latest_seq[i]=0; }
    fence_all(); g_lpbr.ready=READY; fence_all();
    for(;;){
        g_lpbr.heartbeat++;
        uint32_t p=g_lpbr.hp_probe;
        if(g_lpbr.lp_echo!=p){ fence_all(); g_lpbr.lp_echo=p; fence_all(); }
        uint32_t tail=g_lpbr.tail, head=g_lpbr.hp_probe;
        while(tail!=head){
            uint32_t i=(tail&(N-1u))*3u;
            uint32_t y=g_lpbr.ring[i], m=g_lpbr.ring[i+1u], s=g_lpbr.ring[i+2u];
            fence_all();
            if(y<H && m){
                uint32_t old=g_lpbr.dirty_mask[y];
                if((old&m)==m) g_lpbr.duplicates++;
                g_lpbr.dirty_mask[y]=old|m;
                if((int32_t)(s-g_lpbr.latest_seq[y])>0) g_lpbr.latest_seq[y]=s;
                if((int32_t)(s-g_lpbr.frontier)>0) g_lpbr.frontier=s;
            }
            tail++; g_lpbr.consumed++; g_lpbr.tail=tail; fence_all();
        }
    }
}
