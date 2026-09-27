/*
 *  MIDI.C - MIDI Board (CZ-6BM1) emulator
 */

#include "common.h"
#include "prop.h"
#include "winx68k.h"
#include "../libretro/dosio.h"
#include "../libretro/mmsystem.h"
#include "x68kmemory.h"
#include "irqh.h"
#include "midi.h"
#include "hostfs.h"
#include "m68000.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
/* R1A4 ownership contract: CPU1 publishes raw guest TDR bytes only.
 * CPU0 retrop4_midi_uart worker owns MIDI parsing/diagnostics and UART. */
extern bool rp_midi_uart_submit_byte(uint8_t byte);
extern void rp_midi_uart_annotate_next_guest_cycle(uint32_t guest_cycle);
/* R1A7: CPU0 diagnostic snapshot only; MFP guest semantics remain CPU1-owned. */
extern void MFP_R1A7DiagSnapshot(uint32_t out[16]);
/* X68KTAB_R1A17_SYSTEM_COST_AUDIT observation-only snapshots. */
extern void m68k_tab5_r1a17_system_audit_get(uint32_t out[25]);
extern void MemWrap_R1A17AuditGet(uint32_t out_r[41], uint32_t out_w[41]);
extern void OPM_R1A17AuditGet(uint32_t out[3]);
extern void OPM_R1A18AuditGet(uint32_t out[6]);
extern void DSound_R1A17AuditGet(uint32_t out[4]);
extern void MFP_Tab5TimerFastStats(uint32_t *exact_bc, uint32_t *fallback);
extern uint32_t GVRAM_DebugWriteCount(void);
extern uint32_t TVRAM_Tab5TextWriteEpoch(void);
extern uint32_t OPM_DebugDataWriteCount(void);
extern uint32_t ADPCM_DebugControlWriteCount(void);
extern uint32_t ADPCM_DebugDataWriteCount(void);
extern void ADPCM_R1A20AuditGet(uint32_t out[6]);
extern uint32_t Keyboard_DebugIntDelivered(void);
#endif

#define MIDIBUFFERS 1024			/* 1024 (should never needed more than this) */
#define MIDIBUFTIMER 3200			/* 10MHz / (31.25K / 10bit) = 3200 */
#define MIDIFIFOSIZE 16			/* YM3802 FIFO-Tx depth (Midiori-compatible) */
#define MIDIDELAYBUF 4096			/* is it ok to have 31250/10 = 3125 byts (1sec)?  */

enum {
	MIDI_NOTUSED,
	MIDI_DEFAULT,
	MIDI_MT32,
	MIDI_CM32L,
	MIDI_CM64,
	MIDI_CM300,
	MIDI_CM500,
	MIDI_SC55,
	MIDI_SC88,
	MIDI_LA,
	MIDI_GM,
	MIDI_GS,
	MIDI_XG
};

#ifdef ESP_PLATFORM
/* R57E127 scheduler gates: public ABI consumed by current Musashi hot loop. */
uint8_t MIDI_R127DelayPending = 0u;
uint8_t MIDI_R127TimerActive = 0u;
#endif

static void *hOut = NULL;
static int		MIDI_CTRL;
static int		MIDI_POS;
static int		MIDI_SYSCOUNT;
static uint8_t		MIDI_LAST;
static uint8_t		MIDI_BUF[MIDIBUFFERS];
static uint8_t		MIDI_EXCVBUF[MIDIBUFFERS];

static uint8_t		MIDI_RegHigh = 0;
static uint8_t		MIDI_Vector = 0;
static uint8_t		MIDI_IntEnable = 0;
static uint8_t		MIDI_IntVect = 0;
static uint8_t		MIDI_IntFlag = 0;
static uint32_t		MIDI_Buffered = 0;
static int32_t		MIDI_BufTimer = 3333;
static uint8_t		MIDI_R05 = 0;
static uint8_t        MIDI_TxEnable = 0;
static uint8_t        MIDI_TxIdle = 0;
static uint8_t        MIDI_BreakEnable = 0;
static uint8_t        MIDI_CCR = 0;
static uint8_t        MIDI_GTimerLowLatch = 0;
static uint8_t        MIDI_MTimerLowLatch = 0;
static uint32_t       MIDI_TxIdleTimer = 0;
static uint32_t		MIDI_GTimerMax = 0;
static uint32_t		MIDI_MTimerMax = 0;
static int32_t        MIDI_GTimerVal = 0;

/* X68KTAB_R1A11_RCD_GT_ISR_SHIFTUI_ONEPASS
 * Diagnostic-only RCD/YM3802 playback pass.  RCP timebase and the 8 us GTR
 * clock are already proven, so this build does not change timer/FIFO/IRQ/UART
 * semantics.  CPU1 only records compact General-Timer IACK / ICR80 / TDR
 * burst-boundary facts; CPU0 prints them from the existing one-pass task.
 * The package also patches the host virtual-keyboard Shift-face display.
 */

/* X68KTAB_R1A9_PROCOWNER_FIX_ONEPASS
 * Corrects R1A8 process ownership telemetry.
 * $1C04 is kept as the OSWORK memory-start snapshot, but the current PMB is
 * resolved through *($1C28), cross-checked against current thread +$08, and
 * against the process child chain / MMB list.
 * R1A7 timer/YM3802 semantics are preserved exactly.
 */

/* X68KTAB_R1A8_PROCOWNER_ONEPASS lineage retained. */

/* X68KTAB_R1A7_TIMERC_ONTIME_ONEPASS
 * R66 is CCR (Click Counter Control), NOT a timer clock selector.
 *
 * Static analysis of the actual RCD 3.01q binary establishes its CZ-6BM1
 * tempo equation as approximately 7,500,000 / (tempo * timebase).  Since one
 * musical tick is 60 / (tempo * timebase) seconds, the YM3802 General Timer
 * count used by RCD is 8 us.  In the exact 10 MHz X68000 peripheral domain
 * this is 80 guest cycles.  R1A6's 40-cycle diagnostic clock is therefore
 * retired here; MTR remains at the already-established 80-cycle scale.
 *
 * R84/R86 low-byte writes remain latches. R85/R87 commit the 14-bit preset
 * and bit7 reloads the corresponding counter, as established in R1A5.
 */
#define MIDI_GTR_TICK_CYCLES 80u
#define MIDI_MTR_TICK_CYCLES 80u
static uint32_t MIDI_GTimerPeriodCycles(void)
{
    const uint32_t n = MIDI_GTimerMax & 0x3fffu;
    return (n + 1u) * MIDI_GTR_TICK_CYCLES;
}
static uint32_t MIDI_MTimerPeriodCycles(void)
{
    const uint32_t n = MIDI_MTimerMax & 0x3fffu;
    return (n + 1u) * MIDI_MTR_TICK_CYCLES;
}
static int32_t		MIDI_MTimerVal = 0;
static uint8_t        MIDI_MTimerRunning = 0;
#ifdef ESP_PLATFORM
static inline __attribute__((always_inline)) void MIDI_R127SyncTimerActive(void)
{
    MIDI_R127TimerActive = (MIDI_Buffered != 0u || (MIDI_MTimerRunning && (MIDI_MTimerMax & 0x3fffu) > 1u) || (MIDI_GTimerMax & 0x3fffu) > 1u) ? 1u : 0u;
}
#endif
static uint8_t		MIDI_MODULE = MIDI_NOTUSED;

static uint8_t MIDI_ResetType[5] = {
	MIDI_LA, MIDI_GM, MIDI_GS, MIDI_XG
};

typedef struct {
	uint32_t time;
	uint8_t msg;
} DELAYBUFITEM;

static DELAYBUFITEM DelayBuf[MIDIDELAYBUF];
static int DBufPtrW = 0;
static int DBufPtrR = 0;

#ifdef ESP_PLATFORM
/* X68KTAB_R1A4_HF1_ONEPASS_FINAL_ORACLE_R2
 * One hardware run must identify the owning layer. CPU1 is the sole producer
 * and performs no printf, wait, mutex, or CPU0 handshake. CPU0 periodically
 * summarizes and automatically dumps the recent causal window when guest TDR
 * production stops. This is observation only: no YM3802 semantics are changed. */
#define MIDI_OP_RING_CAP 8192u
#define MIDI_OP_RING_MASK (MIDI_OP_RING_CAP - 1u)
typedef struct {
    uint32_t seq;
    uint32_t gc;
    uint32_t x;
    uint32_t y;
    uint16_t z;
    uint8_t type;
    uint8_t a;
} MIDI_OnePassEvent;
static MIDI_OnePassEvent *s_op_ring;
static volatile uint32_t s_op_head;
static uint32_t s_op_seq;
static uint32_t s_op_guest_cycles;
static volatile uint32_t s_op_last_pc;
static volatile uint32_t s_op_lost;
static volatile uint32_t s_op_tdr_count;
static volatile uint32_t s_op_last_tdr_gc;
static volatile uint32_t s_op_gt_count;
static volatile uint32_t s_op_mt_count;
static volatile uint32_t s_op_tx_count;
static volatile uint32_t s_op_iack_count;
static volatile uint32_t s_op_hfs_req_count;
static volatile uint32_t s_op_hfs_open_count;
static volatile uint32_t s_op_hfs_read_count;
static volatile uint32_t s_op_host_sysex_count;
static volatile uint32_t s_op_submit_ok;
static volatile uint32_t s_op_submit_fail;
static volatile uint32_t s_op_tdr_hash = 2166136261u;
static volatile uint32_t s_op_gt_delta_last, s_op_gt_delta_min, s_op_gt_delta_max, s_op_gt_prev_gc;
static volatile uint32_t s_op_mt_delta_last, s_op_mt_delta_min, s_op_mt_delta_max, s_op_mt_prev_gc;
static volatile uint32_t s_op_tdr_delta_last, s_op_tdr_delta_min, s_op_tdr_delta_max, s_op_tdr_prev_gc;
static volatile uint32_t s_op_irq_gc, s_op_irq_src;
static volatile uint32_t s_op_iack_lat_last, s_op_iack_lat_min, s_op_iack_lat_max;
static volatile uint32_t s_op_started;
static TaskHandle_t s_op_task;

/* R1A11: compact cross-core diagnostic ring.  CPU1 never printf/waits here.
 * One entry is emitted only for the first TDR write after one or more actual
 * General-Timer IACKs, so a multi-byte MIDI burst remains one timing event. */
#define MIDI_R1A11_EVT_CAP 64u
#define MIDI_R1A11_EVT_MASK (MIDI_R1A11_EVT_CAP - 1u)
typedef struct {
    uint32_t seq;
    uint32_t gc;
    uint32_t pc;
    uint32_t gt_iack;
    uint32_t icr80;
    uint32_t period;
    uint16_t gt_delta;
    uint16_t gtr;
    uint8_t data;
    uint8_t flags;
    uint16_t reserved;
} MIDI_R1A11Event;
static MIDI_R1A11Event s_r1a11_evt[MIDI_R1A11_EVT_CAP];
static volatile uint32_t s_r1a11_evt_head;
static uint32_t s_r1a11_evt_seq;
static volatile uint32_t s_r1a11_gt_iack;
static volatile uint32_t s_r1a11_icr80;
static volatile uint32_t s_r1a11_last_iack_pc;
static volatile uint32_t s_r1a11_last_icr_pc;
static volatile uint32_t s_r1a11_last_iack_gc;
static volatile uint32_t s_r1a11_last_icr_gc;
static uint32_t s_r1a11_last_tdr_gt_iack;
static volatile uint32_t s_r1a11_last_evt_delta;
static volatile uint32_t s_r1a11_evt_overwrite;

/* X68KTAB_R1A12_DEFERRED_REASSERT state.
 * See helper block next to MIDI_ProdPendingVector(). */
static volatile uint8_t  s_r1a12_irq_line;
static volatile uint8_t  s_r1a12_deferred;
static volatile uint32_t s_r1a12_defer_set;
static volatile uint32_t s_r1a12_defer_assert;
static volatile uint32_t s_r1a12_defer_blocked;
static volatile uint32_t s_r1a12_defer_cancel;

/* X68KTAB_R1A13_TXE_ONESHOT_1MS_YM_HORIZON
 * R1A12 restored correct average tempo but could re-deliver the same latched
 * Tx-empty source after IER toggles.  R1A13 makes each delivered pending source one-shot until its raw flag is
 * cleared or a fresh source transition occurs, and coalesces YM3802 timer service to a
 * 1,000 us (10,000 cycle @ 10 MHz) horizon.  Any guest YM3802 MMIO access
 * materializes accumulated time first.  R140J2 MFP/DMA/CRTC deadlines remain
 * unchanged; the 1 ms quantum is deliberately YM3802-only. */
#define MIDI_R1A13_QUANTUM_CYCLES 10000u
#define MIDI_R1A13_LIGHT_TRACE 1
static volatile uint8_t  s_r1a13_delivered_mask;
volatile uint32_t g_x68p4_midi_lazy_pending = 0u;
volatile uint32_t g_x68p4_midi_lazy_deadline = MIDI_R1A13_QUANTUM_CYCLES;
#define s_r1a13_timer_pending g_x68p4_midi_lazy_pending
static volatile uint32_t s_r1a20_midi_deadline_hits;
static volatile uint32_t s_r1a13_timer_calls;
static volatile uint32_t s_r1a13_timer_process_calls;
static volatile uint32_t s_r1a13_quantum_flushes;
static volatile uint32_t s_r1a13_mmio_flushes;
static volatile uint32_t s_r1a13_dup_suppressed;
static volatile uint32_t s_r1a13_pending_max;

static void MIDI_R1A13TimerProcess(uint32_t clk);
static inline void MIDI_R1A13MaterializePendingTime(void);
static inline uint8_t MIDI_R1A13EligiblePending(void);

static inline uint32_t MIDI_R1A11PC(void)
{
    return m68000_get_reg(M68K_PC) & 0x00ffffffu;
}

static inline void MIDI_R1A11OnIack(void)
{
    /* MIDI_IntVect is the source selected for this IACK.  Count only an
     * actual General Timer vector (offset 0x0e); a pending GT hidden behind
     * Tx-empty is intentionally not counted until it is really acknowledged. */
    if (MIDI_IntVect != 0x0eu) return;
    ++s_r1a11_gt_iack;
    s_r1a11_last_iack_gc = s_op_guest_cycles;
    s_r1a11_last_iack_pc = MIDI_R1A11PC();
}

static inline void MIDI_R1A11OnIcr(uint8_t data)
{
    if (!(data & 0x80u)) return;
    ++s_r1a11_icr80;
    s_r1a11_last_icr_gc = s_op_guest_cycles;
    s_r1a11_last_icr_pc = MIDI_R1A11PC();
}

static inline void MIDI_R1A11OnTdr(uint8_t data)
{
    const uint32_t gt = s_r1a11_gt_iack;
    const uint32_t prev = s_r1a11_last_tdr_gt_iack;
    if (gt == prev) return;
    s_r1a11_last_tdr_gt_iack = gt;
    uint32_t d = gt - prev;
    if (d > 0xffffu) d = 0xffffu;
    s_r1a11_last_evt_delta = d;
#if MIDI_R1A13_LIGHT_TRACE
    (void)data;
    return;
#endif
    const uint32_t seq = ++s_r1a11_evt_seq;
    MIDI_R1A11Event *e = &s_r1a11_evt[seq & MIDI_R1A11_EVT_MASK];
    e->gc = s_op_guest_cycles;
    e->pc = MIDI_R1A11PC();
    e->gt_iack = gt;
    e->icr80 = s_r1a11_icr80;
    e->period = MIDI_GTimerPeriodCycles();
    e->gt_delta = (uint16_t)d;
    e->gtr = (uint16_t)(MIDI_GTimerMax & 0xffffu);
    e->data = data;
    e->flags = MIDI_IntFlag;
    __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&s_r1a11_evt_head, seq, __ATOMIC_RELEASE);
}

static const char *MIDI_OnePassTypeName(uint8_t t)
{
    switch (t) {
        case MIDI_OP_HFS_REQ: return "HFS_REQ";
        case MIDI_OP_HFS_DONE: return "HFS_DONE";
        case MIDI_OP_HFS_PATH: return "HFS_PATH";
        case MIDI_OP_HFS_NAME: return "HFS_NAME";
        case MIDI_OP_HFS_FILE: return "HFS_FILE";
        case MIDI_OP_HFS_OPEN: return "HFS_OPEN";
        case MIDI_OP_HFS_READ: return "HFS_READ";
        case MIDI_OP_HFS_CLOSE: return "HFS_CLOSE";
        case MIDI_OP_YM_W: return "YM_W";
        case MIDI_OP_YM_R: return "YM_R";
        case MIDI_OP_GT_FIRE: return "GT_FIRE";
        case MIDI_OP_MT_FIRE: return "MT_FIRE";
        case MIDI_OP_TX_EMPTY: return "TX_EMPTY";
        case MIDI_OP_IRQ_ASSERT: return "IRQ_ASSERT";
        case MIDI_OP_IACK: return "IACK";
        case MIDI_OP_TDR: return "TDR";
        case MIDI_OP_HOST_SYSEX: return "HOST_SYSEX";
        case MIDI_OP_MIDI_INIT: return "MIDI_INIT";
        case MIDI_OP_TIMER_ARM: return "TIMER_ARM";
        case MIDI_OP_IRQ_CLEAR: return "IRQ_CLEAR";
        case MIDI_OP_HOST_SUBMIT: return "HOST_SUBMIT";
        case MIDI_OP_HFS_ENUM: return "HFS_ENUM";
        case MIDI_OP_HFS_FAIL: return "HFS_FAIL";
        default: return "?";
    }
}

static inline void MIDI_OnePassDeltaUpdate(volatile uint32_t *prev, volatile uint32_t *last,
                                           volatile uint32_t *minv, volatile uint32_t *maxv)
{
    const uint32_t now = s_op_guest_cycles;
    const uint32_t p = *prev;
    if (p) {
        const uint32_t d = now - p;
        *last = d;
        if (!*minv || d < *minv) *minv = d;
        if (d > *maxv) *maxv = d;
    }
    *prev = now;
}

static volatile uint32_t s_r1a15_hfs_quiet_trace;

void MIDI_OnePassTrace(uint8_t type, uint8_t a, uint16_t z, uint32_t x, uint32_t y)
{
    /* QUIET_AB_R1: diagnostic-only event trace disabled.
     * Guest timing/IRQ/YM semantics are not changed. */
    (void)type;
    (void)a;
    (void)z;
    (void)x;
    (void)y;
}

void MIDI_OnePassAdvanceGuestCycles(uint32_t cycles, uint32_t pc)
{
    s_op_guest_cycles += cycles;
    s_op_last_pc = pc & 0x00ffffffu;
}

static void MIDI_OnePassPrintEvent(const MIDI_OnePassEvent *e)
{
    if (!e || !e->seq) return;
    if (e->type == MIDI_OP_HFS_NAME) {
        char n[9];
        uint32_t x=e->x, y=e->y;
        memcpy(n, &x, 4); memcpy(n+4, &y, 4); n[8]=0;
        for (int i=0;i<8;i++) if ((unsigned char)n[i] < 0x20u || (unsigned char)n[i] > 0x7eu) n[i]='.';
        (printf)("RP_ONEPASS E s=%lu gc=%lu t=%s kind=%u name8='%s' h16=%04X\n",
               (unsigned long)e->seq, (unsigned long)e->gc, MIDI_OnePassTypeName(e->type),
               (unsigned)e->a, n, (unsigned)e->z);
        return;
    }
    (printf)("RP_ONEPASS E s=%lu gc=%lu t=%s a=%02X z=%04X x=%08lX y=%08lX\n",
           (unsigned long)e->seq, (unsigned long)e->gc, MIDI_OnePassTypeName(e->type),
           (unsigned)e->a, (unsigned)e->z, (unsigned long)e->x, (unsigned long)e->y);
}

static void MIDI_OnePassDumpRecent(uint32_t head, uint32_t want)
{
    if (!s_op_ring || !head) return;
    uint32_t first = head > want ? head - want + 1u : 1u;
    unsigned tdr_kept = 0u, sub_kept = 0u;
    (printf)("RP_ONEPASS DUMP_BEGIN head=%lu first=%lu\n", (unsigned long)head, (unsigned long)first);
    for (uint32_t seq=first; seq<=head; ++seq) {
        MIDI_OnePassEvent *src = &s_op_ring[seq & MIDI_OP_RING_MASK];
        const uint32_t before = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (before != seq) continue;
        MIDI_OnePassEvent snap = *src;
        const uint32_t after = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (after != seq || snap.seq != seq) continue;
        /* Keep all causal control/FS/timer events; thin dense byte/submit pairs only. */
        if ((snap.type == MIDI_OP_TDR || snap.type == MIDI_OP_HOST_SUBMIT) && seq + 64u < head) continue;
        if (snap.type == MIDI_OP_TDR && ++tdr_kept > 32u) continue;
        if (snap.type == MIDI_OP_HOST_SUBMIT && ++sub_kept > 32u) continue;
        MIDI_OnePassPrintEvent(&snap);
    }
    (printf)("RP_ONEPASS DUMP_END head=%lu\n", (unsigned long)head);
}

static void MIDI_R1A11DrainEvents(uint32_t *last_seq)
{
    if (!last_seq) return;
    const uint32_t head = __atomic_load_n(&s_r1a11_evt_head, __ATOMIC_ACQUIRE);
    if (!head || head == *last_seq) return;
    uint32_t first = *last_seq + 1u;
    if (head - first + 1u > MIDI_R1A11_EVT_CAP) {
        s_r1a11_evt_overwrite += (head - first + 1u) - MIDI_R1A11_EVT_CAP;
        first = head - MIDI_R1A11_EVT_CAP + 1u;
    }
    for (uint32_t seq = first; seq <= head; ++seq) {
        MIDI_R1A11Event *src = &s_r1a11_evt[seq & MIDI_R1A11_EVT_MASK];
        const uint32_t before = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (before != seq) continue;
        MIDI_R1A11Event e = *src;
        const uint32_t after = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (after != seq || e.seq != seq) continue;
        const uint32_t lag = e.gt_iack - e.icr80;
        (printf)("RP_R1A11_EVT s=%lu gc=%lu pc=%06lX data=%02X gtDelta=%u gtAck=%lu icr80=%lu lag=%lu gtr=%04X period=%lu flags=%02X\n",
               (unsigned long)e.seq, (unsigned long)e.gc, (unsigned long)e.pc,
               (unsigned)e.data, (unsigned)e.gt_delta, (unsigned long)e.gt_iack,
               (unsigned long)e.icr80, (unsigned long)lag, (unsigned)e.gtr,
               (unsigned long)e.period, (unsigned)e.flags);
    }
    *last_seq = head;
}

static inline uint8_t MIDI_R1A7GuestU8(uint32_t a)
{
    return MEM ? MEM[(a & 0x00ffffffu) ^ 1u] : 0xffu;
}

static uint16_t MIDI_R1A7GuestU16(uint32_t a)
{
    return (uint16_t)(((uint16_t)MIDI_R1A7GuestU8(a) << 8) |
                      (uint16_t)MIDI_R1A7GuestU8(a + 1u));
}

static uint32_t MIDI_R1A7GuestU32(uint32_t a)
{
    return ((uint32_t)MIDI_R1A7GuestU16(a) << 16) |
           (uint32_t)MIDI_R1A7GuestU16(a + 2u);
}

static void MIDI_R1A8GuestAscii(uint32_t a, char *out, unsigned cap)
{
    if (!out || cap < 2u) return;
    unsigned w = 0u;
    for (unsigned i = 0u; i + 1u < cap; ++i) {
        const uint8_t c = MIDI_R1A7GuestU8(a + i);
        if (!c) break;
        out[w++] = (c >= 0x20u && c <= 0x7eu) ? (char)c : '.';
    }
    out[w] = 0;
}

static uint32_t MIDI_R1A8GuestFNV1a(uint32_t a, unsigned n)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0u; i < n; ++i) {
        h ^= MIDI_R1A7GuestU8(a + i);
        h *= 16777619u;
    }
    return h;
}

static int MIDI_R1A9PtrOK(uint32_t p)
{
    return p >= 0x100u && p < 0x00fff000u;
}

static int MIDI_R1A9PcInPmb(uint32_t pmb, uint32_t pc)
{
    if (!MIDI_R1A9PtrOK(pmb)) return 0;
    const uint32_t e = MIDI_R1A7GuestU32(pmb + 0x08u) & 0x00ffffffu;
    const uint32_t t = (pmb + 0x100u) & 0x00ffffffu;
    return e > t && pc >= t && pc < e;
}

static unsigned MIDI_R1A9NameScore(uint32_t pmb)
{
    if (!MIDI_R1A9PtrOK(pmb)) return 0u;
    unsigned n = 0u, good = 0u;
    for (unsigned i = 0u; i < 24u; ++i) {
        const uint8_t c = MIDI_R1A7GuestU8(pmb + 0xc4u + i);
        if (!c) break;
        ++n;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-' || c == '$') ++good;
    }
    return (n && good + 2u >= n) ? good : 0u;
}

static uint32_t MIDI_R1A9FindPcBlock(uint32_t pc, uint32_t *owner_raw_out,
                                     uint32_t *end_out, unsigned *steps_out)
{
    uint32_t p = MIDI_R1A7GuestU32(0x00001c20u) & 0x00ffffffu;
    uint32_t prev = 0u;
    unsigned steps = 0u;
    for (; steps < 256u && MIDI_R1A9PtrOK(p); ++steps) {
        const uint32_t back = MIDI_R1A7GuestU32(p + 0x00u) & 0x00ffffffu;
        const uint32_t owner_raw = MIDI_R1A7GuestU32(p + 0x04u);
        const uint32_t e = MIDI_R1A7GuestU32(p + 0x08u) & 0x00ffffffu;
        const uint32_t next = MIDI_R1A7GuestU32(p + 0x0cu) & 0x00ffffffu;
        if (e > p + 0x10u && pc >= p + 0x10u && pc < e) {
            if (owner_raw_out) *owner_raw_out = owner_raw;
            if (end_out) *end_out = e;
            if (steps_out) *steps_out = steps + 1u;
            return p;
        }
        if (!next || next == p || next == prev) break;
        /* Corrupt/racing lists must never turn telemetry into guest semantics. */
        if (next >= 0x00fff000u) break;
        prev = p;
        p = next;
    }
    if (steps_out) *steps_out = steps;
    return 0u;
}

static uint32_t MIDI_R1A9FindChainOwner(uint32_t root, uint32_t pc,
                                        unsigned *depth_out, uint32_t *last_out)
{
    uint32_t p = root;
    uint32_t last = 0u;
    for (unsigned d = 0u; d < 16u && MIDI_R1A9PtrOK(p); ++d) {
        last = p;
        if (MIDI_R1A9PcInPmb(p, pc)) {
            if (depth_out) *depth_out = d;
            if (last_out) *last_out = last;
            return p;
        }
        const uint32_t child = MIDI_R1A7GuestU32(p + 0x68u) & 0x00ffffffu;
        if (!MIDI_R1A9PtrOK(child) || child == p) break;
        p = child;
    }
    if (last_out) *last_out = last;
    return 0u;
}

static void MIDI_R1A9PrintProcOwner(const char *tag)
{
    /* Human68k OSWORK (verified against OS WORK MANUAL):
     *   $1C04 current process memory-start snapshot (R1A8 read this directly)
     *   $1C20 first memory-management block
     *   $1C28 address of the work variable that stores CURRENT PMB
     *   $1C54 current thread-management structure; +$08 = PSP/PMB
     * PMB +$68 child PMB, +$82 path, +$C4 executable filename, +$100 text.
     * Observation only on CPU0. No guest state is modified. */
    const uint32_t pc = s_op_last_pc & 0x00ffffffu;
    const uint32_t os_end = MIDI_R1A7GuestU32(0x00001c00u) & 0x00ffffffu;
    const uint32_t os_mem = MIDI_R1A7GuestU32(0x00001c04u) & 0x00ffffffu;
    const uint16_t indos = MIDI_R1A7GuestU16(0x00001c08u);
    const uint8_t doscall = MIDI_R1A7GuestU8(0x00001c0au);
    const uint32_t pmb_slot = MIDI_R1A7GuestU32(0x00001c28u) & 0x00ffffffu;
    const uint32_t cur_pmb = MIDI_R1A9PtrOK(pmb_slot) ?
        (MIDI_R1A7GuestU32(pmb_slot) & 0x00ffffffu) : 0u;
    const uint32_t cur_thr = MIDI_R1A7GuestU32(0x00001c54u) & 0x00ffffffu;
    const uint32_t thr_pmb = MIDI_R1A9PtrOK(cur_thr) ?
        (MIDI_R1A7GuestU32(cur_thr + 0x08u) & 0x00ffffffu) : 0u;

    unsigned chain_depth = 0u;
    uint32_t chain_last = 0u;
    const uint32_t chain_pmb = MIDI_R1A9FindChainOwner(os_mem, pc, &chain_depth, &chain_last);
    uint32_t mmb_owner_raw = 0u, mmb_end = 0u;
    unsigned mmb_steps = 0u;
    const uint32_t pc_block = MIDI_R1A9FindPcBlock(pc, &mmb_owner_raw, &mmb_end, &mmb_steps);

    uint32_t owner = 0u;
    const char *src = "NONE";
    if (MIDI_R1A9PcInPmb(cur_pmb, pc)) { owner = cur_pmb; src = "CUR"; }
    else if (MIDI_R1A9PcInPmb(thr_pmb, pc)) { owner = thr_pmb; src = "THR"; }
    else if (chain_pmb) { owner = chain_pmb; src = "CHAIN"; }
    else if (pc_block && MIDI_R1A9NameScore(pc_block)) { owner = pc_block; src = "MMBSELF"; }
    else if (pc_block) {
        const uint32_t op = mmb_owner_raw & 0x00ffffffu;
        if (MIDI_R1A9PtrOK(op) && MIDI_R1A9NameScore(op)) { owner = op; src = "MMBOWNER"; }
    }
    /* Even when PC is in resident/RCD/ROM code, current process identity is still useful. */
    if (!owner && MIDI_R1A9PtrOK(cur_pmb) && MIDI_R1A9NameScore(cur_pmb)) { owner = cur_pmb; src = "CURCTX"; }
    if (!owner && MIDI_R1A9PtrOK(thr_pmb) && MIDI_R1A9NameScore(thr_pmb)) { owner = thr_pmb; src = "THRCTX"; }

    char path[67] = {0};
    char name[25] = {0};
    uint32_t block_end = 0u, child = 0u, parent_raw = 0u, text = 0u, off = 0xffffffffu;
    if (MIDI_R1A9PtrOK(owner)) {
        parent_raw = MIDI_R1A7GuestU32(owner + 0x04u);
        block_end = MIDI_R1A7GuestU32(owner + 0x08u) & 0x00ffffffu;
        child = MIDI_R1A7GuestU32(owner + 0x68u) & 0x00ffffffu;
        MIDI_R1A8GuestAscii(owner + 0x82u, path, sizeof(path));
        MIDI_R1A8GuestAscii(owner + 0xc4u, name, sizeof(name));
        text = (owner + 0x100u) & 0x00ffffffu;
        if (pc >= text && block_end > text && pc < block_end) off = pc - text;
    }

    const uint32_t page = pc & 0x00ffff00u;
    const uint32_t code_hash = MIDI_R1A8GuestFNV1a(page, 0x100u);
    const uint32_t c0 = MIDI_R1A7GuestU32(pc & 0x00fffffeu);
    const uint32_t c1 = MIDI_R1A7GuestU32((pc & 0x00fffffeu) + 4u);
    const uint32_t c2 = MIDI_R1A7GuestU32((pc & 0x00fffffeu) + 8u);
    const uint32_t c3 = MIDI_R1A7GuestU32((pc & 0x00fffffeu) + 12u);

    (printf)("RP_R1A9_OWNER tag=%s gc=%lu pc=%06lX osEnd=%06lX osMem=%06lX pmbSlot=%06lX cur=%06lX thr=%06lX thrPmb=%06lX owner=%06lX src=%s text=%06lX off=%08lX blockEnd=%06lX parentRaw=%08lX child=%06lX pcBlock=%06lX mmbOwner=%08lX mmbEnd=%06lX mmbSteps=%u chainDepth=%u chainLast=%06lX inDOS=%u dos=%02X name='%s' path='%s' page=%06lX hash=%08lX code=%08lX/%08lX/%08lX/%08lX\n",
             tag ? tag : "?", (unsigned long)s_op_guest_cycles, (unsigned long)pc,
             (unsigned long)os_end, (unsigned long)os_mem, (unsigned long)pmb_slot,
             (unsigned long)cur_pmb, (unsigned long)cur_thr, (unsigned long)thr_pmb,
             (unsigned long)owner, src, (unsigned long)text, (unsigned long)off,
             (unsigned long)block_end, (unsigned long)parent_raw, (unsigned long)child,
             (unsigned long)pc_block, (unsigned long)mmb_owner_raw, (unsigned long)mmb_end,
             mmb_steps, chain_depth, (unsigned long)chain_last,
             (unsigned)indos, (unsigned)doscall, name, path,
             (unsigned long)page, (unsigned long)code_hash,
             (unsigned long)c0, (unsigned long)c1, (unsigned long)c2, (unsigned long)c3);
}

static void MIDI_R1A7PrintTimeChain(const char *tag)
{
    uint32_t d[16] = {0};
    MFP_R1A7DiagSnapshot(d);
    const uint16_t w09ca = MIDI_R1A7GuestU16(0x0009cau);
    const uint16_t w09cc = MIDI_R1A7GuestU16(0x0009ccu);
    const uint16_t w09d6 = MIDI_R1A7GuestU16(0x0009d6u);
    const uint32_t l09d6 = MIDI_R1A7GuestU32(0x0009d6u);
    const uint32_t vec45 = MIDI_R1A7GuestU32(0x00000114u);
    (printf)("RP_R1A7_TIME tag=%s gc=%lu pc=%06lX tcUF=%lu tcReq=%lu tcAssert=%lu tcIack45=%lu tcPendCoal=%lu tcMask=%lu tcInSvc=%lu tcDis=%lu mfpIack=%lu regs=%08lX timer=%08lX tick=%ld deadline=%ld active=%08lX vec45=%08lX w09ca=%04X w09cc=%04X w09d6=%04X l09d6=%08lX\n",
             tag ? tag : "?", (unsigned long)s_op_guest_cycles, (unsigned long)s_op_last_pc,
             (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2], (unsigned long)d[3],
             (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6], (unsigned long)d[7],
             (unsigned long)d[8], (unsigned long)d[9], (unsigned long)d[10], (long)(int32_t)d[11],
             (long)(int32_t)d[12], (unsigned long)d[13], (unsigned long)vec45,
             (unsigned)w09ca, (unsigned)w09cc, (unsigned)w09d6, (unsigned long)l09d6);
}

static void MIDI_OnePassTask(void *arg)
{
    (void)arg;
    uint32_t last_tdr = __atomic_load_n(&s_op_tdr_count, __ATOMIC_ACQUIRE);
    uint32_t last_host_sysex = __atomic_load_n(&s_op_host_sysex_count, __ATOMIC_ACQUIRE);
    uint32_t base_gt=0u, base_mt=0u, base_txe=0u, base_iack=0u, base_hfs=0u, base_open=0u, base_read=0u;
    int64_t last_change = esp_timer_get_time();
    int64_t startup_arm = 0;
    int64_t last_summary = 0;
    uint32_t last_summary_gc = s_op_guest_cycles;
    int64_t last_summary_wall = esp_timer_get_time();
    uint32_t first_tdr_dumped = 0u;
    uint32_t r1a11_last_evt = 0u;
    unsigned stalled = 0u, stall_no = 0u, startup_dumped = 0u;
    (printf)("RP_ONEPASS_FINAL READY CPU=0 cap=%u semantics=R1A13R2_IRQ_ONESHOT_GLOBAL/R1A13_TXE_ONESHOT_1MS_YM_HORIZON GTR8US MTR8US CCR_R66 FIFO16 LATCHED_PRESET HF1 CPU1_WAIT=0 fields=HFS+YM+TC+IRQ6+V45+BIOS_TIME+TDR+UART+GTIACK+ICR80\n", (unsigned)MIDI_OP_RING_CAP);
    MIDI_R1A7PrintTimeChain("READY");
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const uint32_t tdr = __atomic_load_n(&s_op_tdr_count, __ATOMIC_ACQUIRE);
        const uint32_t host_sysex = __atomic_load_n(&s_op_host_sysex_count, __ATOMIC_ACQUIRE);
        MIDI_R1A11DrainEvents(&r1a11_last_evt);
        if (host_sysex != last_host_sysex) {
            last_host_sysex = host_sysex;
            if (!tdr) {
                startup_arm = now; startup_dumped = 0u;
                base_gt=s_op_gt_count; base_mt=s_op_mt_count; base_txe=s_op_tx_count; base_iack=s_op_iack_count;
                base_hfs=s_op_hfs_req_count; base_open=s_op_hfs_open_count; base_read=s_op_hfs_read_count;
            }
        }
        if (!tdr && host_sysex && startup_arm && !startup_dumped && now-startup_arm >= 1500000LL) {
            startup_dumped = 1u;
            const uint32_t head = __atomic_load_n(&s_op_head, __ATOMIC_ACQUIRE);
            (printf)("RP_ONEPASS STARTUP_STALL wall_us=%lld gc=%lu pc=%06lX tdr=0 hostSysex=%lu head=%lu dGT=%lu dMT=%lu dTXE=%lu dIACK=%lu dHFS=%lu dOpen=%lu dRead=%lu lost=%lu active=%u bank=%u ier=%02X flags=%02X gtr=%04lX gtVal=%ld mtr=%04lX mtVal=%ld buf=%lu\n",
                   (long long)(now-startup_arm), (unsigned long)s_op_guest_cycles, (unsigned long)s_op_last_pc,
                   (unsigned long)host_sysex, (unsigned long)head,
                   (unsigned long)(s_op_gt_count-base_gt), (unsigned long)(s_op_mt_count-base_mt),
                   (unsigned long)(s_op_tx_count-base_txe), (unsigned long)(s_op_iack_count-base_iack),
                   (unsigned long)(s_op_hfs_req_count-base_hfs), (unsigned long)(s_op_hfs_open_count-base_open),
                   (unsigned long)(s_op_hfs_read_count-base_read), (unsigned long)s_op_lost,
                   (unsigned)MIDI_R127TimerActive, (unsigned)MIDI_RegHigh, (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntFlag,
                   (unsigned long)(MIDI_GTimerMax & 0xffffu), (long)MIDI_GTimerVal,
                   (unsigned long)(MIDI_MTimerMax & 0xffffu), (long)MIDI_MTimerVal, (unsigned long)MIDI_Buffered);
            MIDI_R1A7PrintTimeChain("STARTUP_STALL");
            MIDI_R1A9PrintProcOwner("STARTUP_STALL");
            MIDI_OnePassDumpRecent(head, 320u);
        }
        if (tdr != last_tdr) {
            if (!first_tdr_dumped) {
                first_tdr_dumped = 1u;
                const uint32_t head0 = __atomic_load_n(&s_op_head, __ATOMIC_ACQUIRE);
                (printf)("RP_ONEPASS FIRST_TDR gc=%lu pc=%06lX tdr=%lu head=%lu hash=%08lX submit=%lu/%lu\n",
                         (unsigned long)s_op_guest_cycles, (unsigned long)s_op_last_pc, (unsigned long)tdr,
                         (unsigned long)head0, (unsigned long)s_op_tdr_hash,
                         (unsigned long)s_op_submit_ok, (unsigned long)s_op_submit_fail);
                MIDI_R1A7PrintTimeChain("FIRST_TDR");
                MIDI_OnePassDumpRecent(head0, 768u);
            }
            if (stalled) {
                (printf)("RP_ONEPASS STALL_END n=%u wall_us=%lld gc=%lu pc=%06lX tdr=%lu head=%lu\n",
                       stall_no, (long long)(now-last_change),
                       (unsigned long)s_op_guest_cycles, (unsigned long)s_op_last_pc, (unsigned long)tdr,
                       (unsigned long)__atomic_load_n(&s_op_head, __ATOMIC_ACQUIRE));
                MIDI_R1A7PrintTimeChain("STALL_END");
                MIDI_R1A9PrintProcOwner("STALL_END");
                MIDI_OnePassDumpRecent(__atomic_load_n(&s_op_head, __ATOMIC_ACQUIRE), 256u);
            }
            last_tdr = tdr; last_change = now; stalled = 0u;
            base_gt=s_op_gt_count; base_mt=s_op_mt_count; base_txe=s_op_tx_count; base_iack=s_op_iack_count;
            base_hfs=s_op_hfs_req_count; base_open=s_op_hfs_open_count; base_read=s_op_hfs_read_count;
        } else if (tdr && !stalled && now-last_change >= 1500000LL && stall_no < 6u) {
            stalled = 1u; ++stall_no;
            const uint32_t head = __atomic_load_n(&s_op_head, __ATOMIC_ACQUIRE);
            (printf)("RP_ONEPASS STALL_BEGIN n=%u wall_us=%lld gc=%lu pc=%06lX tdr=%lu head=%lu dGT=%lu dMT=%lu dTXE=%lu dIACK=%lu dHFS=%lu dOpen=%lu dRead=%lu gt=%lu mt=%lu txe=%lu iack=%lu hfsReq=%lu open=%lu read=%lu lost=%lu active=%u bank=%u ier=%02X flags=%02X gtr=%04lX gtVal=%ld mtr=%04lX mtVal=%ld buf=%lu\n",
                   stall_no, (long long)(now-last_change), (unsigned long)s_op_guest_cycles, (unsigned long)s_op_last_pc,
                   (unsigned long)tdr, (unsigned long)head,
                   (unsigned long)(s_op_gt_count-base_gt), (unsigned long)(s_op_mt_count-base_mt),
                   (unsigned long)(s_op_tx_count-base_txe), (unsigned long)(s_op_iack_count-base_iack),
                   (unsigned long)(s_op_hfs_req_count-base_hfs), (unsigned long)(s_op_hfs_open_count-base_open),
                   (unsigned long)(s_op_hfs_read_count-base_read),
                   (unsigned long)s_op_gt_count, (unsigned long)s_op_mt_count,
                   (unsigned long)s_op_tx_count, (unsigned long)s_op_iack_count,
                   (unsigned long)s_op_hfs_req_count, (unsigned long)s_op_hfs_open_count,
                   (unsigned long)s_op_hfs_read_count, (unsigned long)s_op_lost,
                   (unsigned)MIDI_R127TimerActive, (unsigned)MIDI_RegHigh, (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntFlag,
                   (unsigned long)(MIDI_GTimerMax & 0xffffu), (long)MIDI_GTimerVal,
                   (unsigned long)(MIDI_MTimerMax & 0xffffu), (long)MIDI_MTimerVal, (unsigned long)MIDI_Buffered);
            MIDI_R1A7PrintTimeChain("STALL_BEGIN");
            MIDI_R1A9PrintProcOwner("STALL_BEGIN");
            MIDI_OnePassDumpRecent(head, 320u);
        }
        if (now-last_summary >= 1000000LL) {
            const uint32_t gc_now = s_op_guest_cycles;
            const uint32_t dgc = gc_now - last_summary_gc;
            const uint32_t wall_us = (uint32_t)(now - last_summary_wall);
            last_summary = now; last_summary_gc = gc_now; last_summary_wall = now;
            (printf)("RP_ONEPASS STAT gc=%lu dgc=%lu wallUs=%lu pc=%06lX tdr=%lu tdrD=%lu/%lu/%lu hash=%08lX submit=%lu/%lu lastTdrGc=%lu hostSysex=%lu gt=%lu gtD=%lu/%lu/%lu expGT=%lu mt=%lu mtD=%lu/%lu/%lu expMT=%lu txe=%lu iack=%lu iackLat=%lu/%lu/%lu irqSrc=%02lX hfsReq=%lu open=%lu read=%lu head=%lu lost=%lu stalled=%u active=%u bank=%u ier=%02X flags=%02X gtr=%04lX gtVal=%ld mtr=%04lX mtVal=%ld buf=%lu\n",
                   (unsigned long)gc_now, (unsigned long)dgc, (unsigned long)wall_us, (unsigned long)s_op_last_pc,
                   (unsigned long)tdr, (unsigned long)s_op_tdr_delta_last, (unsigned long)s_op_tdr_delta_min, (unsigned long)s_op_tdr_delta_max,
                   (unsigned long)s_op_tdr_hash, (unsigned long)s_op_submit_ok, (unsigned long)s_op_submit_fail,
                   (unsigned long)s_op_last_tdr_gc, (unsigned long)host_sysex, (unsigned long)s_op_gt_count,
                   (unsigned long)s_op_gt_delta_last, (unsigned long)s_op_gt_delta_min, (unsigned long)s_op_gt_delta_max,
                   (unsigned long)MIDI_GTimerPeriodCycles(), (unsigned long)s_op_mt_count,
                   (unsigned long)s_op_mt_delta_last, (unsigned long)s_op_mt_delta_min, (unsigned long)s_op_mt_delta_max,
                   (unsigned long)MIDI_MTimerPeriodCycles(), (unsigned long)s_op_tx_count,
                   (unsigned long)s_op_iack_count, (unsigned long)s_op_iack_lat_last,
                   (unsigned long)s_op_iack_lat_min, (unsigned long)s_op_iack_lat_max, (unsigned long)s_op_irq_src,
                   (unsigned long)s_op_hfs_req_count, (unsigned long)s_op_hfs_open_count, (unsigned long)s_op_hfs_read_count,
                   (unsigned long)__atomic_load_n(&s_op_head, __ATOMIC_ACQUIRE), (unsigned long)s_op_lost, stalled,
                   (unsigned)MIDI_R127TimerActive, (unsigned)MIDI_RegHigh, (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntFlag,
                   (unsigned long)(MIDI_GTimerMax & 0xffffu), (long)MIDI_GTimerVal,
                   (unsigned long)(MIDI_MTimerMax & 0xffffu), (long)MIDI_MTimerVal, (unsigned long)MIDI_Buffered);
            (printf)("RP_R1A11_STAT gtFire=%lu gtAck=%lu icr80=%lu lag=%ld lastDelta=%lu lastIackPc=%06lX lastIcrPc=%06lX lastIackGc=%lu lastIcrGc=%lu evt=%lu overwrite=%lu gtr=%04lX period=%lu\n",
                   (unsigned long)s_op_gt_count, (unsigned long)s_r1a11_gt_iack,
                   (unsigned long)s_r1a11_icr80, (long)((int32_t)(s_r1a11_gt_iack - s_r1a11_icr80)),
                   (unsigned long)s_r1a11_last_evt_delta, (unsigned long)s_r1a11_last_iack_pc,
                   (unsigned long)s_r1a11_last_icr_pc, (unsigned long)s_r1a11_last_iack_gc,
                   (unsigned long)s_r1a11_last_icr_gc, (unsigned long)s_r1a11_evt_head,
                   (unsigned long)s_r1a11_evt_overwrite, (unsigned long)(MIDI_GTimerMax & 0xffffu),
                   (unsigned long)MIDI_GTimerPeriodCycles());
            (printf)("RP_R1A12_STAT pending=%02X line=%u deferred=%u set=%lu assert=%lu blocked=%lu cancel=%lu vect=%02X ier=%02X flags=%02X\n",
                   (unsigned)(MIDI_IntFlag & MIDI_IntEnable), (unsigned)s_r1a12_irq_line,
                   (unsigned)s_r1a12_deferred, (unsigned long)s_r1a12_defer_set,
                   (unsigned long)s_r1a12_defer_assert, (unsigned long)s_r1a12_defer_blocked,
                   (unsigned long)s_r1a12_defer_cancel, (unsigned)MIDI_IntVect,
                   (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntFlag);
            (printf)("RP_R1A13_STAT raw=%02X eligible=%02X delivered=%02X dupSupp=%lu timerCalls=%lu proc=%lu qFlush=%lu mmioFlush=%lu pend=%lu pendMax=%lu quantum=%u\n",
                   (unsigned)(MIDI_IntFlag & MIDI_IntEnable), (unsigned)MIDI_R1A13EligiblePending(),
                   (unsigned)s_r1a13_delivered_mask, (unsigned long)s_r1a13_dup_suppressed,
                   (unsigned long)s_r1a13_timer_calls, (unsigned long)s_r1a13_timer_process_calls,
                   (unsigned long)s_r1a13_quantum_flushes, (unsigned long)s_r1a13_mmio_flushes,
                   (unsigned long)s_r1a13_timer_pending, (unsigned long)s_r1a13_pending_max,
                   (unsigned)MIDI_R1A13_QUANTUM_CYCLES);
            {
                uint32_t irq_calls=0u, irq_effective=0u, irq_dup=0u, irq_lvl[8]={0};
                IRQH_R1A13GetStats(&irq_calls, &irq_effective, &irq_dup, irq_lvl);
                (printf)("RP_R1A13_IRQSTAT calls=%lu effective=%lu dup=%lu irq1=%lu irq3=%lu irq4=%lu irq5=%lu irq6=%lu\n",
                         (unsigned long)irq_calls, (unsigned long)irq_effective, (unsigned long)irq_dup,
                         (unsigned long)irq_lvl[1], (unsigned long)irq_lvl[3],
                         (unsigned long)irq_lvl[4], (unsigned long)irq_lvl[5],
                         (unsigned long)irq_lvl[6]);
            }
            {
                uint32_t hs[9]={0};
                HostFS_R1A15GetStats(hs);
                (printf)("RP_R1A15_HFSSTAT media57=%lu drv51=%lu flushNoop=%lu flushFiles=%lu rSeekSkip=%lu rSeekDo=%lu wSeekSkip=%lu wSeekDo=%lu closeFlushSkip=%lu quietTrace=%lu\n",
                         (unsigned long)hs[0], (unsigned long)hs[1], (unsigned long)hs[2],
                         (unsigned long)hs[3], (unsigned long)hs[4], (unsigned long)hs[5],
                         (unsigned long)hs[6], (unsigned long)hs[7], (unsigned long)hs[8],
                         (unsigned long)__atomic_load_n(&s_r1a15_hfs_quiet_trace, __ATOMIC_RELAXED));
            }
            {
                uint32_t a[25]={0}, mr[41]={0}, mw[41]={0}, oa[3]={0}, da[4]={0};
                m68k_tab5_r1a17_system_audit_get(a);
                MemWrap_R1A17AuditGet(mr,mw);
                OPM_R1A17AuditGet(oa);
                DSound_R1A17AuditGet(da);
                uint32_t mfpExact=0u, mfpFallback=0u;
                MFP_Tab5TimerFastStats(&mfpExact, &mfpFallback);
                (printf)("RP_R1A17_SCHED lines=%lu slices=%lu none=%lu mfpD=%lu rtcD=%lu dmaD=%lu pollD=%lu mfpAct=%lu rtcOpen=%lu dmaAct=%lu dmaCh=%lu/%lu/%lu hsync=%lu/%lu raster=%lu vblank=%lu ta=%lu/%lu midiLines=%lu adDue=%lu keySvc=%lu fdd=%lu mfpExact=%lu fallback=%lu\n",
                    (unsigned long)a[0],(unsigned long)a[1],(unsigned long)a[2],
                    (unsigned long)a[3],(unsigned long)a[4],(unsigned long)a[5],
                    (unsigned long)a[6],(unsigned long)a[7],(unsigned long)a[8],
                    (unsigned long)a[9],(unsigned long)a[20],(unsigned long)a[21],
                    (unsigned long)a[22],(unsigned long)a[10],(unsigned long)a[11],
                    (unsigned long)a[23],(unsigned long)a[24],(unsigned long)a[12],
                    (unsigned long)a[13],(unsigned long)a[17],(unsigned long)a[15],
                    (unsigned long)a[18],(unsigned long)a[19],
                    (unsigned long)mfpExact,(unsigned long)mfpFallback);
                (printf)("RP_R1A17_AUDIO onepass=%lu opmLine=%lu opmTimer=%lu active=%lu tc=%02lX send0=%lu zero=%lu frames=%lu flush=%lu opmW=%lu adCtl=%lu adData=%lu\n",
                    (unsigned long)a[14],(unsigned long)a[16],(unsigned long)oa[0],
                    (unsigned long)oa[1],(unsigned long)oa[2],(unsigned long)da[0],
                    (unsigned long)da[1],(unsigned long)da[2],(unsigned long)da[3],
                    (unsigned long)OPM_DebugDataWriteCount(),
                    (unsigned long)ADPCM_DebugControlWriteCount(),
                    (unsigned long)ADPCM_DebugDataWriteCount());
                {
                    uint32_t r18[6]={0};
                    OPM_R1A18AuditGet(r18);
                    (printf)("RP_R1A18_OPM mat=%lu deadline=%lu mmio=%lu safety=%lu pending=%lu next=%lu\n",
                        (unsigned long)r18[0],(unsigned long)r18[1],
                        (unsigned long)r18[2],(unsigned long)r18[3],
                        (unsigned long)r18[4],(unsigned long)r18[5]);
                }
                {
                    uint32_t ad20[6]={0};
                    ADPCM_R1A20AuditGet(ad20);
                    (printf)("RP_R1A20_EVT midiDeadline=%lu midiPend=%lu midiNext=%lu adMat=%lu adDeadline=%lu adClkFlush=%lu adDueLoops=%lu adPend=%lu adNext=%lu\n",
                        (unsigned long)s_r1a20_midi_deadline_hits,
                        (unsigned long)s_r1a13_timer_pending,
                        (unsigned long)g_x68p4_midi_lazy_deadline,
                        (unsigned long)ad20[0],(unsigned long)ad20[1],
                        (unsigned long)ad20[2],(unsigned long)ad20[3],
                        (unsigned long)ad20[4],(unsigned long)ad20[5]);
                }
                #define R1A17_R(slot) mr[(slot)-0x40u]
                #define R1A17_W(slot) mw[(slot)-0x40u]
                const uint32_t bg_r=R1A17_R(0x58)+R1A17_R(0x59)+R1A17_R(0x5a)+R1A17_R(0x5b)+R1A17_R(0x5c)+R1A17_R(0x5d)+R1A17_R(0x5e)+R1A17_R(0x5f);
                const uint32_t bg_w=R1A17_W(0x58)+R1A17_W(0x59)+R1A17_W(0x5a)+R1A17_W(0x5b)+R1A17_W(0x5c)+R1A17_W(0x5d)+R1A17_W(0x5e)+R1A17_W(0x5f);
                (printf)("RP_R1A17_MMIO crtc=%lu/%lu vctl=%lu/%lu dma=%lu/%lu mfp=%lu/%lu rtc=%lu/%lu sys=%lu/%lu opm=%lu/%lu adpcm=%lu/%lu fdc=%lu/%lu sasi=%lu/%lu scc=%lu/%lu pia=%lu/%lu ioc=%lu/%lu scsi=%lu/%lu midi=%lu/%lu bg=%lu/%lu mercury=%lu/%lu sram=%lu/%lu\n",
                    (unsigned long)R1A17_R(0x40),(unsigned long)R1A17_W(0x40),
                    (unsigned long)R1A17_R(0x41),(unsigned long)R1A17_W(0x41),
                    (unsigned long)R1A17_R(0x42),(unsigned long)R1A17_W(0x42),
                    (unsigned long)R1A17_R(0x44),(unsigned long)R1A17_W(0x44),
                    (unsigned long)R1A17_R(0x45),(unsigned long)R1A17_W(0x45),
                    (unsigned long)R1A17_R(0x47),(unsigned long)R1A17_W(0x47),
                    (unsigned long)R1A17_R(0x48),(unsigned long)R1A17_W(0x48),
                    (unsigned long)R1A17_R(0x49),(unsigned long)R1A17_W(0x49),
                    (unsigned long)R1A17_R(0x4a),(unsigned long)R1A17_W(0x4a),
                    (unsigned long)R1A17_R(0x4b),(unsigned long)R1A17_W(0x4b),
                    (unsigned long)R1A17_R(0x4c),(unsigned long)R1A17_W(0x4c),
                    (unsigned long)R1A17_R(0x4d),(unsigned long)R1A17_W(0x4d),
                    (unsigned long)R1A17_R(0x4e),(unsigned long)R1A17_W(0x4e),
                    (unsigned long)(R1A17_R(0x4f)+R1A17_R(0x50)),(unsigned long)(R1A17_W(0x4f)+R1A17_W(0x50)),
                    (unsigned long)R1A17_R(0x57),(unsigned long)R1A17_W(0x57),
                    (unsigned long)bg_r,(unsigned long)bg_w,
                    (unsigned long)R1A17_R(0x66),(unsigned long)R1A17_W(0x66),
                    (unsigned long)R1A17_R(0x68),(unsigned long)R1A17_W(0x68));
                (printf)("RP_R1A17_MEM gvW=%lu tvChange=%lu keyDelivered=%lu\n",
                    (unsigned long)GVRAM_DebugWriteCount(),
                    (unsigned long)TVRAM_Tab5TextWriteEpoch(),
                    (unsigned long)Keyboard_DebugIntDelivered());
                #undef R1A17_R
                #undef R1A17_W
            }
            MIDI_R1A7PrintTimeChain("STAT");
            if (stalled) MIDI_R1A9PrintProcOwner("STALL_STAT");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void MIDI_OnePassEnsureStarted(void)
{
    /* QUIET_AB_R1: keep startup call harmless; no ring, no task, no serial. */
    __atomic_store_n(&s_op_started, 1u, __ATOMIC_RELEASE);
}
#endif

/* Nekomichi 6, MIMPI tone map compatibility */

enum {
	MIMPI_LA = 0,
	MIMPI_PCM,
	MIMPI_GS,
	MIMPI_RHYTHM
};

static	uint8_t		LOADED_TONEMAP = 0;
static	uint8_t		ENABLE_TONEMAP = 0;
static	uint8_t		TONE_CH[16];
static	uint8_t		TONEBANK[3][128];
static	uint8_t		TONEMAP[3][128];

static uint8_t EXCV_MTRESET[] = { 0xf0, 0x41, 0x10, 0x16, 0x12, 0x7f, 0x00, 0x00, 0x00, 0x01, 0xf7 };
static uint8_t EXCV_GMRESET[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
static uint8_t EXCV_GSRESET[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7};
static uint8_t EXCV_XGRESET[] = { 0xf0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xf7};

#define	MIDICTRL_READY		0
#define	MIDICTRL_2BYTES		1
#define	MIDICTRL_3BYTES		2
#define	MIDICTRL_EXCLUSIVE	3
#define	MIDICTRL_TIMECODE	4
#define MIDICTRL_SYSTEM		5

#define	MIDI_EXCLUSIVE		0xf0
#define MIDI_TIMECODE		0xf1
#define MIDI_SONGPOS		0xf2
#define MIDI_SONGSELECT		0xf3
#define	MIDI_TUNEREQUEST	0xf6
#define	MIDI_EOX			0xf7
#define	MIDI_TIMING			0xf8
#define MIDI_START			0xfa
#define MIDI_CONTINUE		0xfb
#define	MIDI_STOP			0xfc
#define	MIDI_ACTIVESENSE	0xfe
#define	MIDI_SYSTEMRESET	0xff

#define MIDIOUTS(a,b,c) (((size_t)c << 16) | ((size_t)b << 8) | (size_t)a)

int MIDI_StateAction(StateMem *sm, int load, int data_only)
{
#ifdef ESP_PLATFORM
    if (!load) MIDI_R1A13MaterializePendingTime();
#endif
	SFORMAT StateRegs[] = 
	{
		SFVAR(MIDI_CTRL),
		SFVAR(MIDI_POS),
		SFVAR(MIDI_SYSCOUNT),

		SFVAR(MIDI_LAST),
		SFARRAY(MIDI_BUF, 1024),
		SFARRAY(MIDI_EXCVBUF, 1024),

		SFVAR(MIDI_RegHigh),
		SFVAR(MIDI_Vector),
		SFVAR(MIDI_IntEnable),
		SFVAR(MIDI_IntVect),
		SFVAR(MIDI_IntFlag),
		SFVAR(MIDI_Buffered),
		SFVAR(MIDI_BufTimer),
		SFVAR(MIDI_R05),
		SFVAR(MIDI_TxEnable),
		SFVAR(MIDI_TxIdle),
		SFVAR(MIDI_BreakEnable),
		SFVAR(MIDI_CCR),
		SFVAR(MIDI_GTimerLowLatch),
		SFVAR(MIDI_MTimerLowLatch),
		SFVAR(MIDI_TxIdleTimer),
		SFVAR(MIDI_GTimerMax),
		SFVAR(MIDI_MTimerMax),
		SFVAR(MIDI_GTimerVal),
		SFVAR(MIDI_MTimerVal),
		SFVAR(MIDI_MTimerRunning),
		SFVAR(MIDI_MODULE),

		SFARRAY(DelayBuf, ((4 + 1) * MIDIDELAYBUF)),
		SFVAR(DBufPtrW),
		SFVAR(DBufPtrR),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_MIDI", false);
#ifdef ESP_PLATFORM
    if (load) {
        MIDI_R127DelayPending = (DBufPtrW != DBufPtrR) ? 1u : 0u;
        MIDI_R127SyncTimerActive();
        s_r1a13_timer_pending = 0u;
        s_r1a13_delivered_mask = 0u;
    }
#endif

	return ret;
}


/* X68KTAB_FULLPASS_RESULTS_PROD_R1A1_SOURCE_OVERWRITE */
/* X68KTAB_FULLPASS_RESULTS_PROD_R1A2_SOURCE_OVERWRITE: R57E127 hot-loop ABI restored. */
/* X68KTAB_FULLPASS_RESULTS_PROD_R1A3_ADDRESS_ERROR_FIX: source-driven IRQ assertion only. */
/* X68KTAB_MIDI_CPU0_RAW_R1A4
 * Hardware-backed YM3802 corrections:
 *  - ISR is pending source state; ICR is write-one-to-clear.
 *  - Tx-empty pending clears immediately on each TDR write and rises only
 *    when the emulated Tx FIFO actually becomes empty.
 *  - IRQ vector priority follows YM3802/Midiori priority encoder: bit0 first.
 *  - IER/ICR never synchronously inject IRQ4 from the guest write path.
 *  - ESP TDR data is not parsed on CPU1: one raw byte is published to the
 *    CPU0 RetroP4 SPSC backend; CPU0 owns protocol observation and UART.
 *  - General Timer uses the hardware 8 us CLKM tick = 80 exact 10 MHz cycles.
 */
#define PX68K_YM3802_FULLPASS_TRACE 0
static inline uint8_t MIDI_R1A13EligiblePending(void)
{
    const uint8_t raw = (uint8_t)(MIDI_IntFlag & MIDI_IntEnable);
    return (uint8_t)(raw & (uint8_t)~s_r1a13_delivered_mask);
}

static uint8_t MIDI_ProdPendingVector(void)
{
    const uint8_t p = MIDI_R1A13EligiblePending();
    if (p & 0x01u) return 0x00u;
    if (p & 0x02u) return 0x02u;
    if (p & 0x04u) return 0x04u;
    if (p & 0x08u) return 0x06u;
    if (p & 0x10u) return 0x08u;
    if (p & 0x20u) return 0x0au;
    if (p & 0x40u) return 0x0cu;
    if (p & 0x80u) return 0x0eu;
    return 0x10u;
}

/* X68KTAB_R1A12_DEFERRED_REASSERT
 * R1A11 proved the General Timer period and RCD's 48-GT step are correct.
 * The half-speed path was stale Tx-empty pending: RCD masks Tx while in IRQ4,
 * later re-enables it, but R1A3 correctly forbids synchronous IRQ4 re-entry
 * from guest IER/ICR writes.  The pending Tx source was therefore delivered
 * only when the next GT source event called IRQH_Int(), taking vector 0x0c
 * instead of 0x0e and stealing that timer tick.
 *
 * Keep R1A3's no-nested-MMIO rule.  Guest writes only mark a pending source
 * for deferred delivery. MIDI_Timer() reasserts it after SR IPL drops below 4.
 * This is entirely guest-cycle driven: no wall clock, no sleep, no CPU0 wait.
 */
static uint32_t FASTCALL MIDI_Int(uint8_t irq);

static inline uint8_t MIDI_R1A12PendingEnabled(void)
{
    return MIDI_R1A13EligiblePending();
}

static inline void MIDI_R1A12RefreshDeferred(void)
{
    const uint8_t raw = (uint8_t)(MIDI_IntFlag & MIDI_IntEnable);
    const uint8_t eligible = MIDI_R1A12PendingEnabled();
    if ((uint8_t)(raw & s_r1a13_delivered_mask))
        ++s_r1a13_dup_suppressed;
    if (eligible) {
        if (!s_r1a12_deferred) ++s_r1a12_defer_set;
        s_r1a12_deferred = 1u;
    } else if (s_r1a12_deferred) {
        s_r1a12_deferred = 0u;
        ++s_r1a12_defer_cancel;
    }
}

static inline void MIDI_R1A12SourceAssert(void)
{
    /* Multiple YM3802 sources can become pending inside one 1 ms service
     * quantum.  One asserted IRQ4 line is enough; leave later sources latched
     * and defer them after the current IACK instead of manufacturing another
     * IRQH_Int() call while the line is already active.
     *
     * R1A13 can also materialize pending YM time from a guest MMIO access.
     * Never inject IRQ4 synchronously while the 68000 is already at IPL4+;
     * this preserves the R1A3 no-nested-MMIO invariant that removed the
     * Address Error. */
    if (!MIDI_R1A12PendingEnabled()) return;
    if (s_r1a12_irq_line) {
        MIDI_R1A12RefreshDeferred();
        return;
    }
    const uint32_t sr = m68000_get_reg(M68K_SR);
    if (((sr >> 8) & 7u) >= 4u) {
        MIDI_R1A12RefreshDeferred();
        ++s_r1a12_defer_blocked;
        return;
    }
    s_r1a12_irq_line = 1u;
    IRQH_Int(4, &MIDI_Int);
}

static inline void MIDI_R1A12ServiceDeferred(void)
{
    if (!s_r1a12_deferred) return;
    if (!MIDI_R1A12PendingEnabled()) {
        s_r1a12_deferred = 0u;
        ++s_r1a12_defer_cancel;
        return;
    }
    if (s_r1a12_irq_line) return;

    const uint32_t sr = m68000_get_reg(M68K_SR);
    if (((sr >> 8) & 7u) >= 4u) {
        ++s_r1a12_defer_blocked;
        return;
    }

    MIDI_IntVect = MIDI_ProdPendingVector();
    s_r1a12_deferred = 0u;
    s_r1a12_irq_line = 1u;
    ++s_r1a12_defer_assert;
    MIDI_OnePassTrace(MIDI_OP_IRQ_ASSERT, 0x12u, (uint16_t)MIDI_IntVect,
                      (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8),
                      s_r1a12_defer_assert);
    IRQH_Int(4, &MIDI_Int);
}

/* RP_YM3802_FULLPASS_R1A5
 * R1A5A1 robust final one-pass observation layer.
 * Observe complete YM3802 control/timing surface without inventing missing
 * click/playback/recording counter semantics.  R1A4 hardware-backed timer scale is active.
 */
#if defined(ESP_PLATFORM)
static uint32_t s_rp_ym_full_rgr;
static uint32_t s_rp_ym_full_ivr;
static uint32_t s_rp_ym_full_isr;
static uint32_t s_rp_ym_full_icr;
static uint32_t s_rp_ym_full_ier;
static uint32_t s_rp_ym_full_tdr;
static uint32_t s_rp_ym_full_b8;
static uint32_t s_rp_ym_full_irq_tx;
static uint32_t s_rp_ym_full_irq_mt;
static uint32_t s_rp_ym_full_irq_gt;
static uint32_t s_rp_ym_full_iack;
static uint32_t s_rp_ym_full_init;

static int MIDI_Tab5FullDiagSample(uint32_t n)
{
    return n <= 16u || (n && ((n & (n - 1u)) == 0u));
}

static void MIDI_Tab5FullDiagWritePre(uint32_t adr, uint8_t data)
{
    const uint32_t r = adr & 15u;
    if (r == 0x03u)
    {
        const uint32_t n = ++s_rp_ym_full_rgr;
        if (n <= 64u)
            (printf)("RP_YM_FULL RGR #%lu oldBank=%u data=%02X newBank=%u reset=%u\n",
                   (unsigned long)n, (unsigned)MIDI_RegHigh, (unsigned)data,
                   (unsigned)(data & 0x0fu), (unsigned)((data >> 7) & 1u));
    }
    else if (r == 0x07u)
    {
        const uint32_t n = ++s_rp_ym_full_icr;
        if (n <= 64u)
            (printf)("RP_YM_FULL ICR #%lu data=%02X shadowFlags=%02X ier=%02X vect=%02X (baseline ignores ICR)\n",
                   (unsigned long)n, (unsigned)data, (unsigned)MIDI_IntFlag,
                   (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntVect);
    }
}

static void MIDI_Tab5FullDiagWritePost(uint32_t adr, uint8_t data)
{
    const uint32_t r = adr & 15u;
    if (r == 0x0du && MIDI_RegHigh == 0u)
    {
        const uint32_t n = ++s_rp_ym_full_ier;
        if (n <= 64u)
            (printf)("RP_YM_FULL IER #%lu data=%02X ier=%02X flags=%02X vect=%02X\n",
                   (unsigned long)n, (unsigned)data, (unsigned)MIDI_IntEnable,
                   (unsigned)MIDI_IntFlag, (unsigned)MIDI_IntVect);
    }
    if (MIDI_RegHigh == 5u && r == 0x0du)
    {
        const uint32_t n = ++s_rp_ym_full_tdr;
        if (MIDI_Tab5FullDiagSample(n))
            (printf)("RP_YM_FULL TDR n=%lu data=%02X buffered=%lu ier=%02X flags=%02X\n",
                   (unsigned long)n, (unsigned)data, (unsigned long)MIDI_Buffered,
                   (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntFlag);
    }
    if (MIDI_RegHigh == 8u && (r == 0x09u || r == 0x0bu || r == 0x0du || r == 0x0fu))
    {
        const uint32_t n = ++s_rp_ym_full_b8;
        if (n <= 96u)
            (printf)("RP_YM_FULL B8 #%lu R%02u data=%02X gt=%04lX gtVal=%ld mt=%04lX mtVal=%ld ier=%02X r05=%02X\n",
                   (unsigned long)n, (unsigned)(((r - 1u) >> 1)), (unsigned)data,
                   (unsigned long)(MIDI_GTimerMax & 0xffffu), (long)MIDI_GTimerVal,
                   (unsigned long)(MIDI_MTimerMax & 0xffffu), (long)MIDI_MTimerVal,
                   (unsigned)MIDI_IntEnable, (unsigned)MIDI_R05);
    }
}

static void MIDI_Tab5FullDiagRead(uint32_t adr, uint8_t ret)
{
    const uint32_t r = adr & 15u;
    if (r == 0x01u)
    {
        const uint32_t n = ++s_rp_ym_full_ivr;
        if (n <= 32u || (n && ((n & 0xffu) == 0u)))
            (printf)("RP_YM_FULL IVR #%lu ret=%02X flags=%02X ier=%02X vect=%02X bank=%u\n",
                   (unsigned long)n, (unsigned)ret, (unsigned)MIDI_IntFlag,
                   (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntVect, (unsigned)MIDI_RegHigh);
    }
    else if (r == 0x05u)
    {
        const uint32_t n = ++s_rp_ym_full_isr;
        if (n <= 64u)
            (printf)("RP_YM_FULL ISR #%lu ret=%02X shadowFlags=%02X ier=%02X vect=%02X bank=%u\n",
                   (unsigned long)n, (unsigned)ret, (unsigned)MIDI_IntFlag,
                   (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntVect, (unsigned)MIDI_RegHigh);
    }
}

static void MIDI_Tab5FullDiagIRQ(uint8_t src)
{
    uint32_t *p = src == 0x80u ? &s_rp_ym_full_irq_gt :
                  (src == 0x40u ? &s_rp_ym_full_irq_tx : &s_rp_ym_full_irq_mt);
    const uint32_t n = ++*p;
    if (MIDI_Tab5FullDiagSample(n))
        (printf)("RP_YM_FULL IRQ src=%02X n=%lu bank=%u ier=%02X flags=%02X vect=%02X gt=%04lX/%ld mt=%04lX/%ld buf=%lu\n",
               (unsigned)src, (unsigned long)n, (unsigned)MIDI_RegHigh,
               (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntFlag, (unsigned)MIDI_IntVect,
               (unsigned long)(MIDI_GTimerMax & 0xffffu), (long)MIDI_GTimerVal,
               (unsigned long)(MIDI_MTimerMax & 0xffffu), (long)MIDI_MTimerVal,
               (unsigned long)MIDI_Buffered);
}

static void MIDI_Tab5FullDiagIack(uint8_t irq, uint32_t vector)
{
    const uint32_t n = ++s_rp_ym_full_iack;
    if (n <= 32u || (n && ((n & 0xffu) == 0u)))
        (printf)("RP_YM_FULL IACK #%lu irq=%u vector=%02lX ier=%02X flags=%02X vect=%02X bank=%u\n",
               (unsigned long)n, (unsigned)irq, (unsigned long)(vector & 0xffu),
               (unsigned)MIDI_IntEnable, (unsigned)MIDI_IntFlag,
               (unsigned)MIDI_IntVect, (unsigned)MIDI_RegHigh);
}

static void MIDI_Tab5FullDiagInit(void)
{
    const uint32_t n = ++s_rp_ym_full_init;
    (printf)("RP_YM_FULL INIT #%lu gtIrq=%lu mtIrq=%lu txIrq=%lu iack=%lu flags=%02X ier=%02X bank=%u\n",
           (unsigned long)n, (unsigned long)s_rp_ym_full_irq_gt,
           (unsigned long)s_rp_ym_full_irq_mt, (unsigned long)s_rp_ym_full_irq_tx,
           (unsigned long)s_rp_ym_full_iack, (unsigned)MIDI_IntFlag,
           (unsigned)MIDI_IntEnable, (unsigned)MIDI_RegHigh);
}
#endif

static uint32_t FASTCALL MIDI_Int(uint8_t irq)
{
	IRQH_IRQCallBack(irq);
#if defined(ESP_PLATFORM)
    if (irq == 4u && MIDI_IntVect <= 0x0eu)
        s_r1a13_delivered_mask |= (uint8_t)(1u << (MIDI_IntVect >> 1));
    if (irq == 4u) {
        s_r1a12_irq_line = 0u;
        /* If another source was already pending behind this vector, queue one
         * deferred delivery.  Service waits until guest IPL drops below 4. */
        MIDI_R1A12RefreshDeferred();
    }
    if (irq == 4u) {
        ++s_op_iack_count;
        MIDI_OnePassTrace(MIDI_OP_IACK, MIDI_RegHigh, (uint16_t)((MIDI_Vector|MIDI_IntVect) & 0xffu),
                          (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8) | ((uint32_t)MIDI_IntVect << 16),
                          s_op_iack_count);
        MIDI_R1A11OnIack();
        s_op_irq_gc = 0u;
    }
	if (PX68K_YM3802_FULLPASS_TRACE && irq == 4u)
		MIDI_Tab5FullDiagIack(irq, (uint32_t)(MIDI_Vector|MIDI_IntVect));
#endif
	if ( irq==4 )
		return (uint32_t)(MIDI_Vector|MIDI_IntVect);
	return (uint32_t)(-1);
}

static void MIDI_R1A13TimerProcess(uint32_t clk)
{
    if (!clk) return;
    ++s_r1a13_timer_process_calls;

#ifdef ESP_PLATFORM
    MIDI_R1A12ServiceDeferred();
#endif

    MIDI_BufTimer -= (int32_t)clk;
    while (MIDI_BufTimer < 0)
    {
        MIDI_BufTimer += MIDIBUFTIMER;
        if (!MIDI_Buffered)
            continue;

        MIDI_Buffered--;
        if (MIDI_Buffered==0u)
        {
#ifdef ESP_PLATFORM
            s_r1a13_delivered_mask &= (uint8_t)~0x40u;
#endif
            MIDI_IntFlag |= 0x40;
            MIDI_IntVect = MIDI_ProdPendingVector();
#if defined(ESP_PLATFORM)
            if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5FullDiagIRQ(0x40u);
            ++s_op_tx_count;
            MIDI_OnePassTrace(MIDI_OP_TX_EMPTY, MIDI_RegHigh, (uint16_t)MIDI_Buffered,
                              (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8) | ((uint32_t)MIDI_IntVect << 16),
                              s_op_tx_count);
#endif
            if (MIDI_IntEnable & 0x40)
            {
#ifdef ESP_PLATFORM
                MIDI_OnePassTrace(MIDI_OP_IRQ_ASSERT, 0x40u, (uint16_t)MIDI_IntVect, s_op_tx_count, MIDI_Buffered);
#endif
                MIDI_R1A12SourceAssert();
            }
            break;
        }
    }

    if (MIDI_MTimerRunning && (MIDI_MTimerMax & 0x3fffu) > 1u)
    {
        MIDI_MTimerVal -= (int32_t)clk;
        if (MIDI_MTimerVal<0)
        {
            while (MIDI_MTimerVal<0) MIDI_MTimerVal += (int32_t)MIDI_MTimerPeriodCycles();
            if (!(MIDI_R05&0x80))
            {
#ifdef ESP_PLATFORM
                s_r1a13_delivered_mask &= (uint8_t)~0x02u;
#endif
                MIDI_IntFlag |= 0x02;
                MIDI_IntVect = MIDI_ProdPendingVector();
#if defined(ESP_PLATFORM)
                if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5FullDiagIRQ(0x02u);
                ++s_op_mt_count;
                MIDI_OnePassTrace(MIDI_OP_MT_FIRE, MIDI_RegHigh, (uint16_t)MIDI_IntVect,
                                  (uint32_t)MIDI_MTimerMax, (uint32_t)MIDI_MTimerVal);
#endif
                if (MIDI_IntEnable & 0x02)
                {
#ifdef ESP_PLATFORM
                    MIDI_OnePassTrace(MIDI_OP_IRQ_ASSERT, 0x02u, (uint16_t)MIDI_IntVect, s_op_mt_count,
                                      (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8));
#endif
                    MIDI_R1A12SourceAssert();
                }
            }
        }
    }

    if ((MIDI_GTimerMax & 0x3fffu) > 1u)
    {
        MIDI_GTimerVal -= (int32_t)clk;
        if (MIDI_GTimerVal<0)
        {
            while (MIDI_GTimerVal<0) MIDI_GTimerVal += (int32_t)MIDI_GTimerPeriodCycles();
#ifdef ESP_PLATFORM
            s_r1a13_delivered_mask &= (uint8_t)~0x80u;
#endif
            MIDI_IntFlag |= 0x80;
            MIDI_IntVect = MIDI_ProdPendingVector();
#if defined(ESP_PLATFORM)
            if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5FullDiagIRQ(0x80u);
            ++s_op_gt_count;
            MIDI_OnePassTrace(MIDI_OP_GT_FIRE, MIDI_RegHigh, (uint16_t)MIDI_IntVect,
                              (uint32_t)MIDI_GTimerMax, (uint32_t)MIDI_GTimerVal);
#endif
            if (MIDI_IntEnable & 0x80)
            {
#ifdef ESP_PLATFORM
                MIDI_OnePassTrace(MIDI_OP_IRQ_ASSERT, 0x80u, (uint16_t)MIDI_IntVect, s_op_gt_count,
                                  (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8));
#endif
                MIDI_R1A12SourceAssert();
            }
        }
    }

    if (MIDI_TxEnable && MIDI_Buffered == 0u)
    {
        if (!MIDI_TxIdle)
        {
            if (MIDI_TxIdleTimer < 800000u)
            {
                uint32_t next = MIDI_TxIdleTimer + clk;
                MIDI_TxIdleTimer = (next < MIDI_TxIdleTimer || next >= 800000u) ? 800000u : next;
            }
            if (MIDI_TxIdleTimer >= 800000u) MIDI_TxIdle = 1u;
        }
    }
    else
    {
        MIDI_TxIdleTimer = 0u;
    }
#ifdef ESP_PLATFORM
    MIDI_R127SyncTimerActive();
#endif
}

static inline void MIDI_R1A13MaterializePendingTime(void)
{
    const uint32_t clk = s_r1a13_timer_pending;
    if (!clk) return;
    if (clk > s_r1a13_pending_max) s_r1a13_pending_max = clk;
    s_r1a13_timer_pending = 0u;
    ++s_r1a13_mmio_flushes;
    MIDI_R1A13TimerProcess(clk);
}

void FASTCALL MIDI_R1A20MaterializeDeadline(void)
{
    if (!Config.MIDI_SW) { s_r1a13_timer_pending = 0u; return; }
    const uint32_t run = s_r1a13_timer_pending;
    if (!run) return;
    if (run > s_r1a13_pending_max) s_r1a13_pending_max = run;
    s_r1a13_timer_pending = 0u;
    ++s_r1a20_midi_deadline_hits;
    ++s_r1a13_timer_calls;
    ++s_r1a13_quantum_flushes;
    MIDI_R1A13TimerProcess(run);
}

void FASTCALL MIDI_Timer(uint32_t clk)
{
    if (!Config.MIDI_SW) {
        s_r1a13_timer_pending = 0u;
        return;
    }

    ++s_r1a13_timer_calls;
    if (!clk) return;
    if (UINT32_MAX - s_r1a13_timer_pending < clk) {
        const uint32_t run = s_r1a13_timer_pending;
        s_r1a13_timer_pending = 0u;
        if (run) MIDI_R1A13TimerProcess(run);
    }
    s_r1a13_timer_pending += clk;
    if (s_r1a13_timer_pending > s_r1a13_pending_max)
        s_r1a13_pending_max = s_r1a13_timer_pending;

    if (s_r1a13_timer_pending < MIDI_R1A13_QUANTUM_CYCLES)
        return;

    const uint32_t run = s_r1a13_timer_pending;
    s_r1a13_timer_pending = 0u;
    ++s_r1a13_quantum_flushes;
    MIDI_R1A13TimerProcess(run);
}

static void MIDI_SetModule(void)
{
	if (Config.MIDI_SW)
		MIDI_MODULE = MIDI_ResetType[Config.MIDI_Type];
	else
		MIDI_MODULE = MIDI_NOTUSED;
}

static void MIDI_Sendexclusive(uint8_t *excv, size_t length)
{
#ifdef ESP_PLATFORM
    MIDI_OnePassTrace(MIDI_OP_HOST_SYSEX, length ? excv[0] : 0u, (uint16_t)(length > 0xffffu ? 0xffffu : length),
                      length > 1u ? excv[1] : 0u, length > 2u ? excv[2] : 0u);
#endif
	memcpy(MIDI_EXCVBUF, excv, length);
	midi_out_long_msg(MIDI_EXCVBUF, length);
}

void MIDI_Reset(void)
{
	memset(DelayBuf, 0, sizeof(DelayBuf));
	DBufPtrW = DBufPtrR = 0;
#ifdef ESP_PLATFORM
    MIDI_R127DelayPending = 0u;
    MIDI_R127SyncTimerActive();
#endif

	if (hOut)
	{
		size_t msg;
		switch(MIDI_MODULE)
		{
			case MIDI_NOTUSED:
				return;
			case MIDI_MT32:
			case MIDI_CM32L:
			case MIDI_CM64:
			case MIDI_LA:
				MIDI_Sendexclusive(EXCV_MTRESET, sizeof(EXCV_MTRESET));
				break;
			case MIDI_SC55:
			case MIDI_SC88:
			case MIDI_GS:
				MIDI_Sendexclusive(EXCV_GSRESET, sizeof(EXCV_GSRESET));
				break;
			case MIDI_XG:
				MIDI_Sendexclusive(EXCV_XGRESET, sizeof(EXCV_XGRESET));
				break;
			default:
				MIDI_Sendexclusive(EXCV_GMRESET, sizeof(EXCV_GMRESET));
				break;
		}
		for (msg=0x7bb0; msg<0x7bc0; msg++)
			midi_out_short_msg(msg);
	}
}

void MIDI_Init(void)
{
#if defined(ESP_PLATFORM)
    MIDI_OnePassEnsureStarted();
    MIDI_OnePassTrace(MIDI_OP_MIDI_INIT, MIDI_RegHigh, 0u, 0u, 0u);
	if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5FullDiagInit();
#endif

	memset(DelayBuf, 0, sizeof(DelayBuf));
	DBufPtrW = DBufPtrR = 0;
#ifdef ESP_PLATFORM
    MIDI_R127DelayPending = 0u;
    MIDI_R127SyncTimerActive();
    s_r1a12_irq_line = 0u;
    s_r1a12_deferred = 0u;
    s_r1a12_defer_set = 0u;
    s_r1a12_defer_assert = 0u;
    s_r1a12_defer_blocked = 0u;
    s_r1a12_defer_cancel = 0u;
    s_r1a13_delivered_mask = 0u;
    s_r1a13_timer_pending = 0u;
    s_r1a13_timer_calls = 0u;
    s_r1a13_timer_process_calls = 0u;
    s_r1a13_quantum_flushes = 0u;
    s_r1a20_midi_deadline_hits = 0u;
    g_x68p4_midi_lazy_deadline = MIDI_R1A13_QUANTUM_CYCLES;
    s_r1a13_mmio_flushes = 0u;
    s_r1a13_dup_suppressed = 0u;
    s_r1a13_pending_max = 0u;
#endif

	MIDI_SetModule();
	MIDI_RegHigh = 0;
	MIDI_Vector = 0;
	MIDI_IntEnable = 0;
	MIDI_IntVect = 0;
	MIDI_IntFlag = 0;
	MIDI_R05 = 0;
	MIDI_TxEnable = 0;
	MIDI_TxIdle = 0;
	MIDI_BreakEnable = 0;
	MIDI_CCR = 0;
	MIDI_GTimerLowLatch = 0;
	MIDI_MTimerLowLatch = 0;
	MIDI_TxIdleTimer = 0;
	MIDI_GTimerMax = 0;
	MIDI_MTimerMax = 0;
	MIDI_GTimerVal = 0;
	MIDI_MTimerVal = 0;
	MIDI_MTimerRunning = 0;
	MIDI_Buffered = 0;
	MIDI_BufTimer = MIDIBUFTIMER;

	MIDI_CTRL = MIDICTRL_READY;
	MIDI_LAST = 0x80;

	if (!hOut)
	{
		if (midi_out_open(&hOut) != 0)
			hOut = NULL;
	}
}

void MIDI_Cleanup(void)
{
	if (hOut)
	{
		MIDI_Reset();
		hOut = NULL;
	}
}

void MIDI_Message(uint8_t mes)
{
	if (!hOut)
		return;

	switch(mes)
	{
		case MIDI_TIMING:
		case MIDI_START:
		case MIDI_CONTINUE:
		case MIDI_STOP:
		case MIDI_ACTIVESENSE:
		case MIDI_SYSTEMRESET:
			return;
	}

	if (MIDI_CTRL == MIDICTRL_READY) {
		if (mes & 0x80) {
			/* status */
			MIDI_POS = 0;
			switch(mes & 0xf0) {
				case 0xc0:
				case 0xd0:
					MIDI_LAST = mes;
					MIDI_CTRL = MIDICTRL_2BYTES;
					break;
				case 0x80:
				case 0x90:
				case 0xa0:
				case 0xb0:
				case 0xe0:
					MIDI_LAST = mes;
					MIDI_CTRL = MIDICTRL_3BYTES;
					break;
				default:
					switch(mes) {
						case MIDI_EXCLUSIVE:
							MIDI_CTRL = MIDICTRL_EXCLUSIVE;
							break;
						case MIDI_TIMECODE:
							MIDI_CTRL = MIDICTRL_TIMECODE;
							break;
						case MIDI_SONGPOS:
							MIDI_CTRL = MIDICTRL_SYSTEM;
							MIDI_SYSCOUNT = 3;
							break;
						case MIDI_SONGSELECT:
							MIDI_CTRL = MIDICTRL_SYSTEM;
							MIDI_SYSCOUNT = 2;
							break;
						case MIDI_TUNEREQUEST:
							MIDI_CTRL = MIDICTRL_SYSTEM;
							MIDI_SYSCOUNT = 1;
							break;
						default:
							return;
					}
					break;
			}
		}
		else {
			/* running status */
			MIDI_BUF[0] = MIDI_LAST;
			MIDI_POS    = 1;
			switch (MIDI_LAST & 0xf0)
			{
			case 0xc0:
			case 0xd0:
				MIDI_CTRL = MIDICTRL_2BYTES;
				break;
			case 0x80:
			case 0x90:
			case 0xa0:
			case 0xb0:
			case 0xe0:
				MIDI_CTRL   = MIDICTRL_3BYTES;
				break;
			default:
				return;
			}
		}
	}
	else if ( (mes&0x80) && ((MIDI_CTRL!=MIDICTRL_EXCLUSIVE)||(mes!=MIDI_EOX)) ) /* When a control byte appears in the data section of a message... (GENOCIDE2) */
	{
		/* status */
		MIDI_POS = 0;
		switch(mes & 0xf0)
		{
			case 0xc0:
			case 0xd0:
				MIDI_LAST = mes;
				MIDI_CTRL = MIDICTRL_2BYTES;
				break;
			case 0x80:
			case 0x90:
			case 0xa0:
			case 0xb0:
			case 0xe0:
				MIDI_LAST = mes;
				MIDI_CTRL = MIDICTRL_3BYTES;
				break;
			default:
				switch(mes) {
					case MIDI_EXCLUSIVE:
						MIDI_CTRL = MIDICTRL_EXCLUSIVE;
						break;
					case MIDI_TIMECODE:
						MIDI_CTRL = MIDICTRL_TIMECODE;
						break;
					case MIDI_SONGPOS:
						MIDI_CTRL = MIDICTRL_SYSTEM;
						MIDI_SYSCOUNT = 3;
						break;
					case MIDI_SONGSELECT:
						MIDI_CTRL = MIDICTRL_SYSTEM;
						MIDI_SYSCOUNT = 2;
						break;
					case MIDI_TUNEREQUEST:
						MIDI_CTRL = MIDICTRL_SYSTEM;
						MIDI_SYSCOUNT = 1;
						break;
					default:
						return;
				}
				break;
		}
	}

	MIDI_BUF[MIDI_POS++] = mes;

	switch(MIDI_CTRL)
	{
		case MIDICTRL_2BYTES:
			if (MIDI_POS >= 2)
			{
				if (ENABLE_TONEMAP)
				{
					if (((MIDI_BUF[0] & 0xf0) == 0xc0) &&
						(TONE_CH[MIDI_BUF[0] & 0x0f] < MIMPI_RHYTHM))
						MIDI_BUF[1] = TONEMAP[ TONE_CH[MIDI_BUF[0] & 0x0f] ][ MIDI_BUF[1] & 0x7f ];
				}
				midi_out_short_msg(MIDIOUTS(MIDI_BUF[0], MIDI_BUF[1], 0));
				MIDI_CTRL = MIDICTRL_READY;
			}
			break;
		case MIDICTRL_3BYTES:
			if (MIDI_POS >= 3)
			{
				midi_out_short_msg( 
				MIDIOUTS(MIDI_BUF[0],
					MIDI_BUF[1], MIDI_BUF[2]));
				MIDI_CTRL = MIDICTRL_READY;
			}
			break;
		case MIDICTRL_EXCLUSIVE:
			if (mes == MIDI_EOX)
			{
				MIDI_Sendexclusive(MIDI_BUF, MIDI_POS);
				MIDI_CTRL = MIDICTRL_READY;
			}
			else if (MIDI_POS >= MIDIBUFFERS) /* overflow */
				MIDI_CTRL = MIDICTRL_READY;
			break;
		case MIDICTRL_TIMECODE:
			if (MIDI_POS >= 2)
			{
				/* it should be the same as exclusive */
				if ((mes == 0x7e) || (mes == 0x7f))
					MIDI_CTRL = MIDICTRL_EXCLUSIVE;
				else
					MIDI_CTRL = MIDICTRL_READY;
			}
			break;
		case MIDICTRL_SYSTEM:
			if (MIDI_POS >= MIDI_SYSCOUNT)
				MIDI_CTRL = MIDICTRL_READY;
			break;
	}
}

/* RP_YM3802_B67_DIAG_R1A4
 * Diagnostic only: observe currently-unimplemented YM3802 banks 6/7.
 * Bank 6: FSR/FCR/CCR/CDR.  Bank 7: SRR/SCR/SPR(L)/SPR(H).
 * No guest-visible register/timer/IRQ semantics are changed here.
 */
#if defined(ESP_PLATFORM)
static uint32_t s_rp_ym_b67_wdiag = 0;
static uint32_t s_rp_ym_b67_rdiag = 0;
static uint8_t s_rp_ym_b67_announced = 0;

static int MIDI_Tab5DiagBank67Reg(uint32_t adr)
{
    const uint32_t r = adr & 15u;
    return (MIDI_RegHigh == 6u || MIDI_RegHigh == 7u) &&
           (r == 0x09u || r == 0x0bu || r == 0x0du || r == 0x0fu);
}

static void MIDI_Tab5DiagBank67Announce(void)
{
    if (!s_rp_ym_b67_announced)
    {
        s_rp_ym_b67_announced = 1;
        (printf)("RP_YM_B67_DIAG armed base=R1A4_125kHz_8us Wlimit=48 Rlimit=24\n");
    }
}

static void MIDI_Tab5DiagBank67Write(uint32_t adr, uint8_t data)
{
    if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5DiagBank67Announce();
    if (MIDI_Tab5DiagBank67Reg(adr))
    {
        const uint32_t n = ++s_rp_ym_b67_wdiag;
        if (n <= 48u)
        {
            const unsigned reg = (unsigned)(((adr & 15u) - 1u) >> 1);
            (printf)("RP_YM_B67_W #%lu bank=%u R%02u data=%02X addr=%06lX\n",
                   (unsigned long)n, (unsigned)MIDI_RegHigh, reg,
                   (unsigned)data, (unsigned long)(adr & 0xffffffu));
        }
    }
}

static void MIDI_Tab5DiagBank67Read(uint32_t adr, uint8_t data)
{
    if (MIDI_Tab5DiagBank67Reg(adr))
    {
        const uint32_t n = ++s_rp_ym_b67_rdiag;
        if (n <= 24u)
        {
            const unsigned reg = (unsigned)(((adr & 15u) - 1u) >> 1);
            (printf)("RP_YM_B67_R #%lu bank=%u R%02u ret=%02X addr=%06lX\n",
                   (unsigned long)n, (unsigned)MIDI_RegHigh, reg,
                   (unsigned)data, (unsigned long)(adr & 0xffffffu));
        }
    }
}
#endif

uint8_t FASTCALL MIDI_Read(uint32_t adr)
{
	uint8_t ret = 0;

	if ( (adr<0xeafa01)||(adr>=0xeafa10)||(!Config.MIDI_SW) ) /* 変なアドレスか、 */
	{
		/* When MIDI is OFF, a bus error occurs */
		BusErrFlag = 1;
		return 0;
	}

#ifdef ESP_PLATFORM
    MIDI_R1A13MaterializePendingTime();
#endif

	switch(adr&15)
	{
		case 0x01:
			ret = (MIDI_Vector | MIDI_IntVect);
			MIDI_IntVect=0x10;
			break;
		case 0x09:			/* R04, 14, ... 94 */
			if (MIDI_RegHigh == 5)
			{
				uint8_t tsr = 0;
				if (MIDI_TxIdle) tsr |= 0x04;                 /* Tx idle */
				if (MIDI_Buffered < MIDIFIFOSIZE) tsr |= 0x40; /* Tx ready */
				if (MIDI_Buffered == 0u) tsr |= 0x80;          /* Tx empty */
				return tsr;
			}
			break;
		case 0x05:
			ret = MIDI_IntFlag;
			break;
		case 0x03:
		case 0x07:
		case 0x0b:			/* R05, 15, ... 95 */
		case 0x0d:			/* R06, 16, ... 96 */
		case 0x0f:			/* R07, 17, ... 97 */
			break;
	}
	#if defined(ESP_PLATFORM)
	if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5DiagBank67Read(adr, ret);
#endif
	#if defined(ESP_PLATFORM)
	if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5FullDiagRead(adr, ret);
    MIDI_OnePassTrace(MIDI_OP_YM_R, MIDI_RegHigh, (uint16_t)(((adr & 15u) << 8) | ret),
                      (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8) | ((uint32_t)MIDI_IntVect << 16) | ((uint32_t)MIDI_R05 << 24),
                      (MIDI_GTimerMax & 0xffffu) | ((MIDI_MTimerMax & 0xffffu) << 16));
#endif
	return ret;
}


static void AddDelayBuf(uint8_t msg)
{
	int newptr = (DBufPtrW+1)%MIDIDELAYBUF;
	if ( newptr!=DBufPtrR )
	{
		DelayBuf[DBufPtrW].time = timeGetTime();
		DelayBuf[DBufPtrW].msg  = msg;
		DBufPtrW = newptr;
#ifdef ESP_PLATFORM
        MIDI_R127DelayPending = 1u;
#endif
	}
}

void MIDI_DelayOut(unsigned int delay)
{
	while ( DBufPtrW!=DBufPtrR )
	{
		unsigned int t = timeGetTime();
		if ( (t-DelayBuf[DBufPtrR].time)>=delay )
		{
			MIDI_Message(DelayBuf[DBufPtrR].msg);
			DBufPtrR = (DBufPtrR+1)%MIDIDELAYBUF;
		}
		else
			break;
	}
#ifdef ESP_PLATFORM
    MIDI_R127DelayPending = (DBufPtrW != DBufPtrR) ? 1u : 0u;
#endif
}

void FASTCALL MIDI_Write(uint32_t adr, uint8_t data)
{
	if ( (adr<0xeafa01)||(adr>=0xeafa10)||(!Config.MIDI_SW) )
	{
		/* When MIDI is OFF, a bus error occurs */
		BusErrFlag = 1;
		return;
	}

#ifdef ESP_PLATFORM
    MIDI_R1A13MaterializePendingTime();
#endif

	#if defined(ESP_PLATFORM)
	if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5DiagBank67Write(adr, data);
#endif

	#if defined(ESP_PLATFORM)
	if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5FullDiagWritePre(adr, data);
#endif

	switch(adr&15)
	{
		case 0x03:
			MIDI_RegHigh = data&0x0f;
			if (data&0x80)
				MIDI_Init();
			break;
		case 0x01:
		case 0x05:
			break;
		case 0x07:
			/* PROD R1A3: ICR is write-one-to-clear, but MUST NOT synchronously
			 * re-enter IRQH_Int() from inside the guest's IRQ handler. RCD.X
			 * writes IER=80 then ICR=80 after each IACK; immediate reassert here
			 * can nest IRQ4 before RTE and corrupt the 68000 exception stack.
			 * New source events in MIDI_Timer() are the only IRQ4 assertion points. */
			MIDI_IntFlag &= (uint8_t)~data;
#ifdef ESP_PLATFORM
            s_r1a13_delivered_mask &= (uint8_t)~data;
#endif
			MIDI_IntVect = MIDI_ProdPendingVector();
#ifdef ESP_PLATFORM
            MIDI_R1A12RefreshDeferred();
            MIDI_R1A11OnIcr(data);
            MIDI_OnePassTrace(MIDI_OP_IRQ_CLEAR, data, (uint16_t)MIDI_IntVect,
                              (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8), s_op_iack_count);
#endif
			break;
		case 0x09:			/* R04, 14, ... 94 */
			switch(MIDI_RegHigh)
			{
				case 0:
					MIDI_Vector = (data&0xe0);
					break;
				case 1:
				case 2:
				case 3:
				case 4:
				case 5:
				case 6:
				case 7:
				case 9:
					break;
				case 8:
					/* R84 is only the low-byte latch. Do not alter a running preset. */
					MIDI_GTimerLowLatch = data;
					break;
			}
			break;
		case 0x0b:			/* R05, 15, ... 95 */
			switch(MIDI_RegHigh)
			{
				case 0:
					MIDI_R05 = data;
					break;
				case 1:
					/*
					 * R15 DCR controls the MIDI-clock timer.  A preset in
					 * R86/R87 does not by itself make the counter free-run.
					 * DCR bit5 selects a counter-control command; command 2
					 * starts/reloads the MIDI-clock timer and command 3 stops it.
					 *
					 * Z-MUSIC initializes both GTR and MTR presets.  Treating
					 * the MTR preset as an implicit START creates a second
					 * same-period timer source and can double the sequencer rate.
					 */
					if (data & 0x20u) {
						const uint8_t cmd = (uint8_t)(data & 0x07u);
						if (cmd == 2u) {
							MIDI_MTimerRunning = 1u;
							MIDI_MTimerVal = (int32_t)MIDI_MTimerPeriodCycles();
						} else if (cmd == 3u) {
							MIDI_MTimerRunning = 0u;
						}
#ifdef ESP_PLATFORM
						MIDI_R127SyncTimerActive();
#endif
					}
					break;
				case 2:
				case 3:
				case 4:
					break;
				case 5:
					/* R55 transmitter control: TxE, TxIDL clear, BRKE. */
					MIDI_TxEnable = (uint8_t)(data & 0x01u);
					if (data & 0x04u) { MIDI_TxIdle = 0u; MIDI_TxIdleTimer = 0u; }
					MIDI_BreakEnable = (uint8_t)((data >> 3) & 0x01u);
					break;
				case 6:
				case 7:
				case 9:
					break;
				case 8:
					/* R85 commits the cached low byte plus 6 high bits. */
					MIDI_GTimerMax = (uint32_t)MIDI_GTimerLowLatch | (((uint32_t)data & 0x3fu) << 8);
					if (data&0x80) {
						MIDI_GTimerVal = (int32_t)MIDI_GTimerPeriodCycles();
#ifdef ESP_PLATFORM
                        MIDI_OnePassTrace(MIDI_OP_TIMER_ARM, 0x80u, (uint16_t)(MIDI_GTimerMax & 0xffffu),
                                          MIDI_GTimerPeriodCycles(), (uint32_t)MIDI_GTimerVal);
#endif
                    }
					break;
			}
			break;
		case 0x0d:			/* R06, 16, ... 96 */
			switch(MIDI_RegHigh)
			{
				case 0:
					/* PROD R1A3: mask update only. Do not synchronously assert an
					 * already-pending source while software is still inside IRQ4. */
					MIDI_IntEnable = data;
					MIDI_IntVect = MIDI_ProdPendingVector();
#ifdef ESP_PLATFORM
                    MIDI_R1A12RefreshDeferred();
#endif
					break;
				case 5:			/* R56 FIFO-Tx data */
					/* A TDR write clears FIFO-Tx-empty pending even if FIFO is full. */
					MIDI_IntFlag &= (uint8_t)~0x40u;
#ifdef ESP_PLATFORM
                    s_r1a13_delivered_mask &= (uint8_t)~0x40u;
#endif
					MIDI_IntVect = MIDI_ProdPendingVector();
#ifdef ESP_PLATFORM
                    MIDI_R1A12RefreshDeferred();
                    ++s_op_tdr_count;
                    s_op_last_tdr_gc = s_op_guest_cycles;
                    s_op_tdr_hash ^= data; s_op_tdr_hash *= 16777619u;
                    MIDI_R1A11OnTdr(data);
#endif
					if (MIDI_Buffered < MIDIFIFOSIZE)
					{
						if (!MIDI_Buffered) MIDI_BufTimer = MIDIBUFTIMER;
						MIDI_Buffered++;
#ifdef ESP_PLATFORM
                        MIDI_OnePassTrace(MIDI_OP_TDR, data, (uint16_t)MIDI_Buffered, s_op_tdr_count,
                                          (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8) | ((uint32_t)MIDI_IntVect << 16));
						/* CPU1 ownership ends here: one non-blocking raw-byte publish only.
						 * CPU0 worker owns protocol observation and physical 31250-baud UART. */
                        {
                            rp_midi_uart_annotate_next_guest_cycle((uint32_t)s_op_guest_cycles);
                            const bool op_ok = rp_midi_uart_submit_byte(data);
                            if (op_ok) ++s_op_submit_ok; else ++s_op_submit_fail;
                            MIDI_OnePassTrace(MIDI_OP_HOST_SUBMIT, data, (uint16_t)(op_ok ? 1u : 0u),
                                              s_op_submit_ok, s_op_submit_fail);
                        }
#else
						AddDelayBuf(data);
#endif
					}
#ifdef ESP_PLATFORM
                    else
                    {
                        /* Guest wrote while FIFO full: real FIFO does not gain a byte. */
                        MIDI_OnePassTrace(MIDI_OP_TDR, data, (uint16_t)MIDI_Buffered, s_op_tdr_count, 0xFFFFFFFFu);
                    }
#endif
					break;
				case 6:
					/* R66 is CCR: Click Counter Control. It does not select GTR/MTR clocks. */
					MIDI_CCR = data;
					break;
				case 8:
					/* R86 is only the MIDI-clock-timer low-byte latch. */
					MIDI_MTimerLowLatch = data;
					break;
				case 1:
				case 2:
				case 3:
				case 4:
				case 7:
				case 9:
					break;
			}
			break;
		case 0x0f:			/* R07, 17, ... 97 */
			if (MIDI_RegHigh == 8)
			{
				/* R87 commits the cached low byte plus 6 high bits.
				 * bit7 reloads the preset; DCR controls START/STOP. */
				MIDI_MTimerMax = (uint32_t)MIDI_MTimerLowLatch | (((uint32_t)data & 0x3fu) << 8);
				if (data&0x80) {
					MIDI_MTimerRunning = 1u;
					MIDI_MTimerVal = (int32_t)MIDI_MTimerPeriodCycles();
#ifdef ESP_PLATFORM
                    MIDI_OnePassTrace(MIDI_OP_TIMER_ARM, 0x02u, (uint16_t)(MIDI_MTimerMax & 0xffffu),
                                      MIDI_MTimerPeriodCycles(), (uint32_t)MIDI_MTimerVal);
#endif
                }
			}
			break;
	}
#ifdef ESP_PLATFORM
    MIDI_R127SyncTimerActive();
    MIDI_OnePassTrace(MIDI_OP_YM_W, MIDI_RegHigh, (uint16_t)(((adr & 15u) << 8) | data),
                      (uint32_t)MIDI_IntEnable | ((uint32_t)MIDI_IntFlag << 8) | ((uint32_t)MIDI_IntVect << 16) | ((uint32_t)MIDI_R05 << 24),
                      (MIDI_GTimerMax & 0xffffu) | ((MIDI_MTimerMax & 0xffffu) << 16));
#endif
#if defined(ESP_PLATFORM)
	if (PX68K_YM3802_FULLPASS_TRACE) MIDI_Tab5FullDiagWritePost(adr, data);
#endif
}

static int exstrcmp(char *str, char *cmp)
{
	uint8_t	c;

	while(*cmp)
	{
		c = *str++;
		if ((c >= 'a') && (c <= 'z'))
			c -= 0x20;
		if (c != *cmp++)
			return 1;
	}
	return 0;
}

static void cutdelimita(char **buf)
{
	uint8_t	c;

	for(;;) {
		c = **buf;
		if (!c)
			break;
		if (c > ' ')
			break;
		(*buf)++;
	}
}

static int getvalue(char **buf, int cutspace)
{
	int	ret    = 0;
	int	valhit = 0;
	uint8_t	c;

	if (cutspace)
		cutdelimita(buf);
	for (;; valhit=1)
	{
		c = **buf;
		if (!c)
		{
			if (!valhit)
				return(-1);
			break;
		}
		if ((c < '0') || (c > '9'))
			break;
		ret = ret * 10 + (c - '0');
		(*buf)++;
	}
	return ret;
}

static int file_readline(void *fh, char *buf, int len)
{
	size_t	pos;
	size_t	readsize;
	int	i;

	if (len < 2)
		return -1;
	pos = file_seek(fh, 0, FSEEK_CUR);
	if (pos == -1)
		return -1;
	readsize = file_lread(fh, buf, len-1);
	if (readsize == -1)
		return -1;
	if (!readsize)
		return -1;
	for (i=0; i<readsize; i++)
	{
		pos++;
		if ((buf[i] == 0x0a) || (buf[i] == 0x0d))
			break;
	}
	buf[i] = '\0';
	if (file_seek(fh, pos, FSEEK_SET) != pos)
		return(-1);
	return(i);
}

static void mimpidefline_analyze(char *buf)
{
	cutdelimita(&buf);
	if (*buf == '@')
	{
		int ch;
		buf++;
		ch = getvalue(&buf, 0);
		if ((ch < 1) || (ch > 16))
			return;
		ch--;
		cutdelimita(&buf);
		if (!exstrcmp(buf, "LA"))
			TONE_CH[ch] = MIMPI_LA;
		else if (!exstrcmp(buf, "PCM"))
			TONE_CH[ch] = MIMPI_PCM;
		else if (!exstrcmp(buf, "GS"))
			TONE_CH[ch] = MIMPI_GS;
		else if (!exstrcmp(buf, "RHYTHM"))
			TONE_CH[ch] = MIMPI_RHYTHM;
	}
	else {
		int	mod, num, bank, tone;
		mod = getvalue(&buf, 0);
		if ((mod < 0) || (mod >= MIMPI_RHYTHM)) {
			return;
		}
		num = getvalue(&buf, 1);
		if ((num < 1) || (num > 128)) {
			return;
		}
		num--;
		tone = getvalue(&buf, 1);
		if ((tone < 1) || (tone > 128)) {
			return;
		}
		if (*buf == ':') {
			buf++;
			bank = tone - 1;
			tone = getvalue(&buf, 1);
			if ((tone < 1) || (tone > 128)) {
				return;
			}
			TONEBANK[mod][num] = bank;
		}
		TONEMAP[mod][num] = tone-1;
	}
}

int MIDI_SetMimpiMap(char *filename)
{
	uint8_t		b;
	void *		fh;
	char		buf[128];

	LOADED_TONEMAP = 0;
	memset(TONE_CH, 0, sizeof(TONE_CH));
	memset(TONEBANK[0], 0, sizeof(TONEBANK));
	for (b=0; b<128; b++)
   {
		TONEMAP[0][b] = b;
		TONEMAP[1][b] = b;
		TONEMAP[2][b] = b;
	}
	TONE_CH[9] = MIMPI_RHYTHM;

	if ((filename == NULL) || (!filename[0]))
	{
		ENABLE_TONEMAP = 0;
		return 0;
	}
	fh = file_open(filename);
	if (fh == (void*)-1)
	{
		ENABLE_TONEMAP = 0;
		return 0;
	}
	while(file_readline(fh, buf, sizeof(buf)) >= 0)
		mimpidefline_analyze(buf);
	file_close(fh);

	LOADED_TONEMAP = 1;
	return 1;
}

int MIDI_EnableMimpiDef(int enable)
{
	ENABLE_TONEMAP = 0;
	if ((enable) && (LOADED_TONEMAP))
	{
		ENABLE_TONEMAP = 1;
		return 1;
	}
	return 0;
}
