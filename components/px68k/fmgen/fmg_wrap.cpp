/*
 * PX68K source modified for the Tab5 port.
 * Intent: Split YM2151 responsibilities so CPU1 keeps guest-visible timer/status/IRQ semantics while CPU0 owns waveform synthesis and the asynchronous PCM ring.
 * Layer8 Aug/17/2026
 */
extern "C" {

#include "common.h"
#include "winx68k.h"
#include "dswin.h"
#include "prop.h"
#include "mfp.h"
#include "adpcm.h"
#include "mercury.h"
#include "fdc.h"
#include "fmg_wrap.h"

#include "opm.h"
#include "opna.h"
};

#ifdef ESP_PLATFORM
#ifndef PX68K_TAB5_R57E63_AUDIO_AUDIT
#define PX68K_TAB5_R57E63_AUDIO_AUDIT 0
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "vgmm5_ym2151.h"
#include <string.h>

extern "C" int DSound_AbsTimelineActive(void);
extern "C" int DSound_AbsTimelineOPMWrite(uint8_t reg,uint8_t data);
extern "C" int DSound_AbsTimelineOPMCSM(void);
extern "C" int DSound_AbsTimelineOPMReset(void);
extern "C" int DSound_AbsTimelineOPMVolume(uint8_t vol);

/* Intent: Keep YM2151 waveform synthesis off CPU1; only guest-visible timer/status/IRQ state stays on the guest core.  Layer8 Aug/17/2026 */
/* Sparse CPU0 waveform-worker CSM notification; defined after the async backend. */
static void audio_csm_notify(void);
#endif

class MyOPM : public FM::OPM
{
public:
	MyOPM();
	virtual ~MyOPM() {}
	void WriteIO(uint32_t adr, uint8_t data);
	void Count2(uint32_t clock);

private:
	virtual void Intr(bool);

public:
	int CurReg;
	uint32_t CurCount;

	int StateAction(StateMem *sm, int load, int data_only);
};


MyOPM::MyOPM()
{
	CurReg = 0;
}

int MyOPM::StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] =
	{
		SFVARN(CurReg, "CurReg"),
		SFVARN(CurCount, "CurCount"),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "MYOPM", false);
	ret &= FM::OPM::StateAction(sm, load, data_only);

	return(1);
}

void MyOPM::WriteIO(uint32_t adr, uint8_t data)
{
	if( adr&1 )
	{
		if ( CurReg==0x1b ) {
			::ADPCM_SetClock((data>>5)&4);
			::FDC_SetForceReady((data>>6)&1);
		}
		SetReg((int)CurReg, (int)data);
	}
	else
		CurReg = (int)data;
}

void MyOPM::Intr(bool f)
{
	if ( f ) ::MFP_Int(12);
}

void MyOPM::Count2(uint32_t clock)
{
	CurCount += clock;
	Count(CurCount/10);
	CurCount %= 10;
}

#ifdef ESP_PLATFORM
static DRAM_ATTR MyOPM* opm = NULL; /* R23: even the fallback pointer must not live in PX68K external BSS. */
#else
static MyOPM* opm = NULL;
#endif

#ifdef ESP_PLATFORM

/* R1A18 lazy control-plane horizon.  Defined before FastOPMControl because
 * NextObservable10MHz() uses it. */
#define R1A18_OPM_LAZY_MAX_CYCLES 0x3ffff000u

/*
 * Build 5.39 / ESP32-P4:
 * The CPU1 guest task no longer carries a complete FMGEN OPM synthesizer.  Waveform/EG/LFO
 * state already lives on the CPU0 waveform worker, so duplicating every guest YM2151 register
 * write on CPU1 only wastes cycles inside m68k_execute().
 *
 * Keep only the guest-visible YM2151 control plane here: address latch,
 * timer A/B, status/IRQ, and the X68000-specific CT/FDC side effects.
 * Timer math is exactly the same 4 MHz YM2151 timing used by FMGEN:
 *   Timer A period = (1024 - TA) * 16 us
 *   Timer B period = ( 256 - TB) * 256 us
 * The old FMGEN Count() while-loops are reduced to constant-time modulo math.
 */
extern "C" void rp_midi_diag_event(uint8_t kind, uint32_t value);
/* RP_MIDI_SCHED_DIAG_V1I */
struct FastOPMControl
{
    int32_t cur_reg;
    uint32_t subus_10mhz;
    uint8_t status;
    uint8_t regtc;
    uint8_t regta[2];
    uint8_t regtb;
    uint32_t timera_us;
    uint32_t timerb_us;
    uint32_t timera_count_us;
    uint32_t timerb_count_us;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
    uint64_t audit_guest_clocks;
    uint32_t audit_timer_calls;
    uint32_t audit_tb_writes;
    uint32_t audit_tc_writes;
    uint32_t audit_tb_starts;
    uint32_t audit_tb_stops;
    uint32_t audit_tb_expires;
    uint32_t audit_tb_irq_sets;
    uint32_t audit_tb_status_clears;
#endif

    void Reset()
    {
        cur_reg = 0;
        subus_10mhz = 0;
        status = 0;
        regtc = 0;
        regta[0] = regta[1] = 0;
        regtb = 0;
        timera_us = 1024u * 16u;
        timerb_us = 256u * 256u;
        timera_count_us = 0;
        timerb_count_us = 0;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        audit_guest_clocks = 0;
        audit_timer_calls = 0;
        audit_tb_writes = 0;
        audit_tc_writes = 0;
        audit_tb_starts = 0;
        audit_tb_stops = 0;
        audit_tb_expires = 0;
        audit_tb_irq_sets = 0;
        audit_tb_status_clears = 0;
#endif
    }

    void SetStatus(uint8_t bits)
    {
        if ((status & bits) == 0)
        {
            status |= bits;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
            if (bits & 2u) (void)0;
#endif
            (void)0;
            ::MFP_Int(12);
        }
    }

    void ResetStatus(uint8_t bits)
    {
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        if ((bits & 2u) && (status & 2u)) (void)0;
#endif
        status &= (uint8_t)~bits;
        (void)0;
        /* MyOPM::Intr(false) was intentionally a no-op as well. */
    }

    void SetTimerA(uint32_t addr, uint8_t data)
    {
        regta[addr & 1u] = data;
        const uint32_t ta = ((uint32_t)regta[0] << 2) | ((uint32_t)regta[1] & 3u);
        timera_us = (1024u - ta) * 16u;
        (void)0;
    }

    void SetTimerB(uint8_t data)
    {
        regtb = data;
        timerb_us = (256u - (uint32_t)data) * 256u;
        (void)0;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        (void)0;
#endif
    }

    void SetTimerControl(uint8_t data)
    {
        const uint8_t changed = regtc ^ data;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        (void)0;
        if (changed & 0x02u) {
            if (data & 0x02u) (void)0;
            else (void)0;
        }
#endif
        regtc = data;
        (void)0;
        if (data & 0x10u) ResetStatus(1u);
        if (data & 0x20u) ResetStatus(2u);
        if (changed & 0x01u) timera_count_us = (data & 0x01u) ? timera_us : 0u;
        if (changed & 0x02u) timerb_count_us = (data & 0x02u) ? timerb_us : 0u;
    }

    static uint32_t AdvanceCounter(uint32_t count, uint32_t period,
                                   uint32_t elapsed, bool *expired)
    {
        if (!count || !elapsed)
            return count;
        if (elapsed < count)
            return count - elapsed;

        *expired = true;
        const uint32_t over = elapsed - count;
        const uint32_t rem = over % period;
        return period - rem;
    }

    void CountUs(uint32_t us)
    {
        bool a_expired = false;
        bool b_expired = false;
        timera_count_us = AdvanceCounter(timera_count_us, timera_us, us, &a_expired);
        timerb_count_us = AdvanceCounter(timerb_count_us, timerb_us, us, &b_expired);
        if (a_expired) rp_midi_diag_event(14, timera_us);
        if (b_expired) rp_midi_diag_event(15, timerb_us);
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        if (b_expired) (void)0;
#endif
        /* FMGEN OPM::TimerA() performs CSM key-off/key-on on all 8 channels.
         * 5.40 preserves that rare side effect with one sparse queue event,
         * instead of forwarding every 10 MHz timer slice to the CPU0 waveform worker. */
        if (a_expired && (regtc & 0x80u)) audio_csm_notify();
        if (a_expired && (regtc & 0x04u)) SetStatus(1u);
        if (b_expired && (regtc & 0x08u)) SetStatus(2u);
    }

    void CountGuest10MHz(uint32_t clocks)
    {
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        (void)0;
        (void)0;
#endif
        subus_10mhz += clocks;
        const uint32_t us = subus_10mhz / 10u;
        subus_10mhz %= 10u;
        if (us) CountUs(us);
    }

    uint32_t NextObservable10MHz() const
    {
        uint32_t best = R1A18_OPM_LAZY_MAX_CYCLES;

        /* Timer A must wake CPU1 only when an overflow can become visible:
         * status/IRQ enabled and not already latched, or CSM is active. */
        const bool a_visible =
            ((regtc & 0x01u) != 0u) &&
            (((regtc & 0x80u) != 0u) ||
             (((regtc & 0x04u) != 0u) && ((status & 0x01u) == 0u)));
        if (a_visible && timera_count_us)
        {
            uint32_t d = timera_count_us * 10u;
            d = (d > subus_10mhz) ? (d - subus_10mhz) : 1u;
            if (d < best) best = d;
        }

        /* Timer B has no CSM side effect. Once status is already latched,
         * further wraps are not guest-visible until software clears it. */
        const bool b_visible =
            ((regtc & 0x02u) != 0u) && ((regtc & 0x08u) != 0u) &&
            ((status & 0x02u) == 0u);
        if (b_visible && timerb_count_us)
        {
            uint32_t d = timerb_count_us * 10u;
            d = (d > subus_10mhz) ? (d - subus_10mhz) : 1u;
            if (d < best) best = d;
        }
        return best;
    }

    void WriteIO(uint32_t adr, uint8_t data)
    {
        if ((adr & 1u) == 0)
        {
            cur_reg = data;
            return;
        }

        const uint8_t reg = (uint8_t)cur_reg;
        if (reg == 0x08u && (data & 0x78u))
            (void)0;
        if (reg == 0x1bu)
        {
            ::ADPCM_SetClock((data >> 5) & 4);
            ::FDC_SetForceReady((data >> 6) & 1);
        }

        switch (reg)
        {
            case 0x10: case 0x11: SetTimerA(reg, data); break;
            case 0x12: SetTimerB(data); break;
            case 0x14: SetTimerControl(data); break;
            default: break; /* Waveform state is CPU0-waveform-worker-only. */
        }
    }

    int StateAction(StateMem *sm, int load, int data_only)
    {
        SFORMAT StateRegs[] =
        {
            SFVARN(cur_reg, "CurReg"),
            SFVARN(subus_10mhz, "CurCount"),
            SFVAR(status),
            SFVAR(regtc),
            SFARRAY(regta, 2),
            SFVAR(regtb),
            SFVAR(timera_us),
            SFVAR(timerb_us),
            SFVAR(timera_count_us),
            SFVAR(timerb_count_us),
            SFEND
        };
        return PX68KSS_StateAction(sm, load, data_only, StateRegs, "MYOPM_FAST", false);
    }
};

#if defined(SPM_DRAM_ATTR)
static SPM_DRAM_ATTR FastOPMControl s_fast_opm;
static SPM_DRAM_ATTR int s_fast_opm_ready = 0;
#elif defined(TCM_DRAM_ATTR)
static TCM_DRAM_ATTR FastOPMControl s_fast_opm;
static TCM_DRAM_ATTR int s_fast_opm_ready = 0;
#else
static DRAM_ATTR FastOPMControl s_fast_opm;
static DRAM_ATTR int s_fast_opm_ready = 0;
#endif

/* X68KTAB_R1A18_VGMM5_OPM_LINEPOLL_RETIRE
 * vgmM5 waveform synthesis is already 44.1-kHz/sample driven on CPU0.
 * CPU1 keeps only guest-visible YM2151 Timer A/B status/IRQ semantics.
 * Do not run that control-plane timer once per scanline: accumulate elapsed
 * 10-MHz guest clocks and materialize only at the next observable timer
 * event, OPM MMIO observation/change, or a very rare overflow-safety fence.
 * CPU1 remains the sole writer; CPU0 never touches these lazy-clock fields. */
extern "C" {
DRAM_ATTR volatile uint32_t g_x68p4_opm_lazy_pending = 0u;
DRAM_ATTR volatile uint32_t g_x68p4_opm_lazy_deadline = R1A18_OPM_LAZY_MAX_CYCLES;
}
static DRAM_ATTR volatile uint32_t s_r1a18_opm_materialize = 0u;
static DRAM_ATTR volatile uint32_t s_r1a18_opm_deadline_hits = 0u;
static DRAM_ATTR volatile uint32_t s_r1a18_opm_mmio_flush = 0u;
static DRAM_ATTR volatile uint32_t s_r1a18_opm_safety_flush = 0u;
/* R1A17 counters remain the public audit surface; in R1A18 these count actual
 * materializations instead of every scanline entry. */
static DRAM_ATTR volatile uint32_t s_r1a17_opm_timer_calls = 0u;
static DRAM_ATTR volatile uint32_t s_r1a17_opm_timer_active_calls = 0u;

#endif

/*
 * Build 5.42 / ESP32-P4:
 * CPU1 guest task keeps authoritative YM2151 timer/status/IRQ state. CPU0 waveform
 * synthesis prefers a stripped vgmM5-derived YM2151-only engine whose hot
 * state is allocated in internal DRAM. Legacy FMGEN remains an emergency
 * fallback only. Register/render ordering is unchanged from 5.39.
 */
#ifdef ESP_PLATFORM
class AudioOPM : public FM::OPM
{
public:
    AudioOPM() : CurCount(0) {}
    void WriteReg(uint8_t reg, uint8_t data) { SetReg((int)reg, (int)data); }
    uint32_t CurCount;
private:
    virtual void Intr(bool) { /* CPU1 guest control-plane OPM owns guest IRQs. */ }
};

enum {
    ASYNC_OPM_WRITE = 1,
    ASYNC_OPM_RENDER,
    ASYNC_OPM_RESET,
    ASYNC_OPM_VOLUME,
    ASYNC_OPM_CSM,
    ASYNC_OPM_STOP
};

struct AsyncOPMEvent {
    uint8_t type;
    uint8_t reg;
    uint8_t data;
    uint8_t profile;
    uint32_t frames;
};

/* R23: FM realtime memory is forbidden from PSRAM.  4096 stereo frames are
 * 16 KiB / ~92.9 ms at 44.1 kHz, comfortably above the 256-frame render
 * quantum while avoiding the old 64 KiB external-memory FIFO. */
#define ASYNC_OPM_RING_FRAMES 4096u
#define ASYNC_OPM_RING_MASK   (ASYNC_OPM_RING_FRAMES - 1u)
#define ASYNC_OPM_QUEUE_LEN    1024u
#define ASYNC_OPM_QUEUE_MASK   (ASYNC_OPM_QUEUE_LEN - 1u)
#define ASYNC_OPM_SCRATCH_FRAMES 256u

static DRAM_ATTR AudioOPM *s_audio_opm = NULL; /* R23 ESP path keeps this NULL; legacy fallback forbidden. */
static DRAM_ATTR VgmM5YM2151 *s_audio_vgm = NULL;
static DRAM_ATTR __attribute__((aligned(64))) AsyncOPMEvent s_audio_event_ring[ASYNC_OPM_QUEUE_LEN];
static DRAM_ATTR volatile uint32_t s_audio_event_head = 0u;
static DRAM_ATTR volatile uint32_t s_audio_event_tail = 0u;
static DRAM_ATTR TaskHandle_t s_audio_task = NULL;
/* Set only after the corresponding WithCaps allocation succeeds.  These are
 * part of the R23 hard contract: queue storage and task stack are Internal. */
static DRAM_ATTR uint8_t s_audio_task_internal_caps = 0;
static DRAM_ATTR __attribute__((aligned(64))) int16_t
    s_audio_ring_storage[ASYNC_OPM_RING_FRAMES * 2];
/* R140P4: keep the FM ring lock only around a bounded Internal-SRAM copy.
 * Saturating FM+ADPCM arithmetic runs outside the spin-critical section so
 * CPU0 interrupts and the equal-priority YM producer are not held off for
 * hundreds/thousands of sample operations. */
static DRAM_ATTR __attribute__((aligned(64))) int16_t
    s_audio_mix_read_scratch[ASYNC_OPM_SCRATCH_FRAMES * 2];
static DRAM_ATTR int16_t *s_audio_ring = NULL;
static DRAM_ATTR int s_audio_ring_placement = 0; /* R23: INTERNAL only; PSRAM fallback is forbidden. */
static DRAM_ATTR size_t s_audio_rd = 0;
static DRAM_ATTR size_t s_audio_wr = 0;
static DRAM_ATTR size_t s_audio_count = 0;
static DRAM_ATTR portMUX_TYPE s_audio_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR volatile int s_audio_async_enabled = 0;
/* R140P1: C-side production audio paths read this ready latch directly,
 * avoiding repeated cross-TU OPM_AsyncEnabled() calls.  It mirrors the
 * existing async state and changes only at backend init/cleanup. */
extern "C" {
DRAM_ATTR volatile int g_x68p4_opm_async_ready = 0;
}
static DRAM_ATTR volatile uint32_t s_r120a_audio_rate_req = 44100u;
static DRAM_ATTR uint32_t s_r120a_audio_rate_applied = 44100u;

#if defined(SPM_DRAM_ATTR)
static SPM_DRAM_ATTR uint8_t s_audio_selected_reg = 0;
#elif defined(TCM_DRAM_ATTR)
static TCM_DRAM_ATTR uint8_t s_audio_selected_reg = 0;
#else
static DRAM_ATTR uint8_t s_audio_selected_reg = 0;
#endif
static DRAM_ATTR volatile uint32_t s_audio_event_drops = 0;
static DRAM_ATTR volatile uint32_t s_audio_ring_overruns = 0;
/* Production clean: steady FM demand/work/timing counters retired.
 * Functional queue/ring state and exceptional drop counters remain. */
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
/* R57E64 focused release audit: count saturation in the actual CPU0 final
 * FM+ADPCM sum.  32-bit counters are deliberate so CPU1 can sample them with
 * one lock-free atomic load each; the audit must not block the guest core. */
static DRAM_ATTR volatile uint32_t s_r57e64_mix_samples = 0;
static DRAM_ATTR volatile uint32_t s_r57e64_mix_clips = 0;
static DRAM_ATTR volatile uint32_t s_r57e64_mix_max_raw_abs = 0;
#endif

/* R56k7/BAT177NW0: event taxonomy + chronology-preserving pressure telemetry.
 * CPU1 never blocks on CPU0. The ordered SPSC ring is sized so saturation is
 * not expected in production; any overflow is an explicit NO-WAIT fault. */
static DRAM_ATTR volatile uint32_t s_r56_write_drop = 0;
static DRAM_ATTR volatile uint32_t s_r56_write_drop_frames = 0;
static DRAM_ATTR volatile uint32_t s_r56_render_drop = 0;
static DRAM_ATTR volatile uint32_t s_r56_render_drop_frames = 0;
static DRAM_ATTR volatile uint32_t s_r56_reset_drop = 0;
static DRAM_ATTR volatile uint32_t s_r56_volume_drop = 0;
static DRAM_ATTR volatile uint32_t s_r56_csm_drop = 0;
static DRAM_ATTR volatile uint32_t s_r56_stop_drop = 0;
static DRAM_ATTR volatile uint32_t s_r56_other_drop = 0;
static DRAM_ATTR volatile uint32_t s_r56l_bp_events = 0;
static DRAM_ATTR volatile uint32_t s_r56l_bp_wait_calls = 0;
static DRAM_ATTR volatile uint32_t s_r56l_bp_timeouts = 0;
static DRAM_ATTR volatile uint32_t s_r56l_bp_max_timeouts = 0;
static DRAM_ATTR volatile uint32_t s_r56l_discard_events = 0;
static DRAM_ATTR volatile uint32_t s_r56l_discard_frames = 0;
/* Render hot loop must not store sample-by-sample into the Internal PCM ring. */
static DRAM_ATTR int16_t s_vgm_scratch[ASYNC_OPM_SCRATCH_FRAMES * 2];

static void async_ring_zero(size_t wr, uint32_t frames)
{
    size_t remain = frames;
    while (remain)
    {
        size_t n = ASYNC_OPM_RING_FRAMES - wr;
        if (n > remain) n = remain;
        memset(&s_audio_ring[wr * 2], 0, n * 2 * sizeof(int16_t));
        wr = (wr + n) & ASYNC_OPM_RING_MASK;
        remain -= n;
    }
}

static IRAM_ATTR void async_vgm_render(size_t wr, uint32_t frames)
{
    uint32_t remain = frames;
    while (remain)
    {
        uint32_t n = remain;
        if (n > ASYNC_OPM_SCRATCH_FRAMES) n = ASYNC_OPM_SCRATCH_FRAMES;
        size_t ring_n = ASYNC_OPM_RING_FRAMES - wr;
        if ((size_t)n > ring_n) n = (uint32_t)ring_n;

        /*
         * Build 5.42: synthesize entirely in internal DRAM, then use one
         * contiguous copy to the PSRAM ring.  This removes thousands of
         * sample-by-sample PSRAM stores from the YM2151 hot loop.
         */
        vgmm5_ym2151_render(s_audio_vgm, s_vgm_scratch, n);
        memcpy(&s_audio_ring[wr * 2], s_vgm_scratch,
               (size_t)n * 2u * sizeof(int16_t));
        wr = (wr + n) & ASYNC_OPM_RING_MASK;
        remain -= n;
    }
}

/* R56l: when the 4096-frame PCM FIFO is full, advance the YM synthesis
 * chronology anyway and discard only the finished PCM.  A timed WRITE must
 * still observe: render old state for ev.frames -> apply register write. */
static void r56l_render_discard(uint32_t frames, uint8_t profile)
{
    if (!frames) return;
    uint32_t remain = frames;
    while (remain)
    {
        uint32_t n = remain;
        if (n > ASYNC_OPM_SCRATCH_FRAMES) n = ASYNC_OPM_SCRATCH_FRAMES;
        if (s_audio_vgm)
        {
            vgmm5_ym2151_render(s_audio_vgm, s_vgm_scratch, n);
        }
        else if (s_audio_opm)
        {
            memset(s_vgm_scratch, 0, (size_t)n * 2u * sizeof(int16_t));
            s_audio_opm->Mix(s_vgm_scratch, (int)n,
                             (uint8_t *)s_vgm_scratch,
                             (uint8_t *)(s_vgm_scratch + ASYNC_OPM_SCRATCH_FRAMES * 2));
        }
        remain -= n;
    }
    (void)profile;
    ++s_r56l_discard_events;
    s_r56l_discard_frames += frames;
}

static int async_render_publish(uint32_t frames, uint8_t profile)
{
    if (!frames)
        return 1;

    size_t wr;
    size_t free_frames;
    portENTER_CRITICAL(&s_audio_mux);
    wr = s_audio_wr;
    free_frames = ASYNC_OPM_RING_FRAMES - s_audio_count;
    portEXIT_CRITICAL(&s_audio_mux);
    if (free_frames < frames)
    {
        ++s_audio_ring_overruns;
        r56l_render_discard(frames, profile);
        return 0;
    }

    if (!s_audio_vgm)
        async_ring_zero(wr, frames);
    if (s_audio_vgm)
        async_vgm_render(wr, frames);
    else if (s_audio_opm)
        s_audio_opm->Mix(&s_audio_ring[wr * 2], (int)frames,
                         (uint8_t *)s_audio_ring,
                         (uint8_t *)(s_audio_ring + ASYNC_OPM_RING_FRAMES * 2));
    (void)profile;

    portENTER_CRITICAL(&s_audio_mux);
    s_audio_wr = (s_audio_wr + frames) & ASYNC_OPM_RING_MASK;
    s_audio_count += frames;
    portEXIT_CRITICAL(&s_audio_mux);

    /* A completed FM chunk can satisfy the common FM/ADPCM timeline even if
     * the feeder previously went to sleep after seeing min(FM,ADPCM)==0. */
    DSound_HostSourceReady();
    return 1;
}

static inline uint32_t async_event_depth(void)
{
    const uint32_t h = __atomic_load_n(&s_audio_event_head, __ATOMIC_ACQUIRE);
    const uint32_t t = __atomic_load_n(&s_audio_event_tail, __ATOMIC_ACQUIRE);
    return h - t;
}


static int async_event_push(const AsyncOPMEvent *ev)
{
    const uint32_t h = __atomic_load_n(&s_audio_event_head, __ATOMIC_RELAXED);
    const uint32_t t = __atomic_load_n(&s_audio_event_tail, __ATOMIC_ACQUIRE);
    if ((h - t) >= ASYNC_OPM_QUEUE_LEN)
        return 0;
    s_audio_event_ring[h & ASYNC_OPM_QUEUE_MASK] = *ev;
    __atomic_store_n(&s_audio_event_head, h + 1u, __ATOMIC_RELEASE);
    if (s_audio_task)
        xTaskNotifyGive(s_audio_task);
    return 1;
}

static int async_event_pop(AsyncOPMEvent *ev)
{
    const uint32_t t = __atomic_load_n(&s_audio_event_tail, __ATOMIC_RELAXED);
    const uint32_t h = __atomic_load_n(&s_audio_event_head, __ATOMIC_ACQUIRE);
    if (t == h)
        return 0;
    *ev = s_audio_event_ring[t & ASYNC_OPM_QUEUE_MASK];
    __atomic_store_n(&s_audio_event_tail, t + 1u, __ATOMIC_RELEASE);
    return 1;
}

static void async_opm_task(void *)
{
    AsyncOPMEvent ev;
    for (;;)
    {
        while (!async_event_pop(&ev))
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (ev.type == ASYNC_OPM_STOP)
            break;
        const uint32_t r120a_rate=__atomic_load_n(&s_r120a_audio_rate_req,__ATOMIC_ACQUIRE);
        if(s_audio_vgm && r120a_rate!=s_r120a_audio_rate_applied){
            vgmm5_ym2151_set_sample_rate(s_audio_vgm,r120a_rate);
            s_r120a_audio_rate_applied=r120a_rate;
        }

        if (!s_audio_vgm && !s_audio_opm)
            continue;

        switch (ev.type)
        {
            case ASYNC_OPM_WRITE:
                /* Build 6.15b: ev.frames timestamps this register write.
                 * Render the old YM2151 state first, then apply the write,
                 * preserving the same sample boundary as the former
                 * RENDER-event + WRITE-event pair with half the queue traffic. */
                if (ev.frames)
                    (void)async_render_publish(ev.frames, ev.profile);
                if (s_audio_vgm) vgmm5_ym2151_write(s_audio_vgm, ev.reg, ev.data);
                else if (s_audio_opm) s_audio_opm->WriteReg(ev.reg, ev.data);
                break;

            case ASYNC_OPM_RENDER:
                (void)async_render_publish(ev.frames, ev.profile);
                break;

            case ASYNC_OPM_RESET:
                if (s_audio_vgm) vgmm5_ym2151_reset(s_audio_vgm);
                else if (s_audio_opm) { s_audio_opm->Reset(); s_audio_opm->CurCount = 0; }
                portENTER_CRITICAL(&s_audio_mux);
                s_audio_rd = s_audio_wr = s_audio_count = 0;
                portEXIT_CRITICAL(&s_audio_mux);
                break;

            case ASYNC_OPM_VOLUME:
            {
                const uint8_t vol = ev.data;
                if (s_audio_vgm) vgmm5_ym2151_set_px_volume(s_audio_vgm, vol);
                else if (s_audio_opm)
                {
                    const int v = vol ? ((16 - vol) * 4) : 192;
                    s_audio_opm->SetVolume(-v);
                }
                break;
            }

            case ASYNC_OPM_CSM:
                if (s_audio_vgm)
                    vgmm5_ym2151_csm_pulse(s_audio_vgm);
                else if (s_audio_opm)
                {
                    /* Match FMGEN OPM::TimerA(): all channels off -> all ops on. */
                    for (uint8_t ch = 0; ch < 8; ++ch)
                    {
                        s_audio_opm->WriteReg(0x08, ch);
                        s_audio_opm->WriteReg(0x08, (uint8_t)(ch | 0x78u));
                    }
                }
                break;

            default:
                break;
        }

        /* R56k scheduler contract: yield only after one complete semantic
         * AsyncOPMEvent. Never split a timed render+WRITE transaction. */
        taskYIELD();
    }

    if (s_audio_vgm) { vgmm5_ym2151_destroy(s_audio_vgm); s_audio_vgm = NULL; }
    /* R23 ESP build never allocates the legacy FMGEN fallback. */
    s_audio_opm = NULL;
    s_audio_task_internal_caps = 0;
    s_audio_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static int async_opm_send(uint8_t type, uint8_t reg, uint8_t data,
                          uint32_t frames, uint8_t profile)
{
    if (!s_audio_async_enabled || !s_audio_task)
        return 0;

    AsyncOPMEvent ev = {};
    ev.type = type;
    ev.reg = reg;
    ev.data = data;
    ev.frames = frames;
    ev.profile = profile;

    /* BAT177NW0 CPU1 NO-WAIT: ordered SPSC transport, zero timeout, zero
     * producer retry. The 1024-entry Internal ring doubles the former queue
     * capacity. Saturation is an explicit correctness fault to be surfaced in
     * telemetry; CPU1 guest time is never delayed by CPU0 synthesis. */
    if (async_event_push(&ev))
    {
        return 1;
    }

    ++s_audio_event_drops;
    ++s_r56l_bp_events;
    ++s_r56l_bp_timeouts;
    if (s_r56l_bp_max_timeouts < 1u) s_r56l_bp_max_timeouts = 1u;
    if (type == ASYNC_OPM_WRITE) { ++s_r56_write_drop; s_r56_write_drop_frames += frames; }
    else if (type == ASYNC_OPM_RENDER) { ++s_r56_render_drop; s_r56_render_drop_frames += frames; }
    else if (type == ASYNC_OPM_RESET) ++s_r56_reset_drop;
    else if (type == ASYNC_OPM_VOLUME) ++s_r56_volume_drop;
    else if (type == ASYNC_OPM_CSM) ++s_r56_csm_drop;
    else if (type == ASYNC_OPM_STOP) ++s_r56_stop_drop;
    else ++s_r56_other_drop;
    return 0;
}

static void audio_csm_notify(void)
{
    if (!s_audio_async_enabled) return;
    if (DSound_AbsTimelineActive()) (void)DSound_AbsTimelineOPMCSM();
    else (void)async_opm_send(ASYNC_OPM_CSM, 0, 0, 0, 0);
}

int OPM_AsyncReserveRing(void)
{
    const size_t bytes = sizeof(s_audio_ring_storage);
    if (s_audio_ring)
    {
        if (!esp_ptr_internal(s_audio_ring) || esp_ptr_external_ram(s_audio_ring))
            return 0;
        return 2;
    }
    s_audio_ring = s_audio_ring_storage;
    s_audio_ring_placement = 2;

    /* Static DRAM_ATTR is the hard guarantee; runtime pointer checks make any
     * future linker/config regression visible immediately on the serial log. */
    if (!esp_ptr_internal(s_audio_ring) || esp_ptr_external_ram(s_audio_ring))
    {
        printf("PX68K_FMSRAM_R23: FATAL FM PCM FIFO is not Internal SRAM ptr=%p bytes=%u\n",
               (void *)s_audio_ring, (unsigned)bytes);
        s_audio_ring = NULL;
        s_audio_ring_placement = 0;
        return 0;
    }

    memset(s_audio_ring, 0, bytes);
    printf("PX68K_FMSRAM_R23: PCM FIFO INTERNAL-only bytes=%u frames=%u (~%ums) ptr=%p PSRAM=FORBIDDEN internalFree=%u largest=%u\n",
           (unsigned)bytes, (unsigned)ASYNC_OPM_RING_FRAMES,
           (unsigned)((ASYNC_OPM_RING_FRAMES * 1000u) / 44100u),
           (void *)s_audio_ring,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return 2;
}

int OPM_AsyncRingPlacement(void)
{
    return s_audio_ring_placement;
}

void OPM_AsyncSetSourceRate(uint32_t rate)
{
    const uint32_t r=(rate <= 11025u) ? 11025u : ((rate <= 22050u) ? 22050u : 44100u);
    __atomic_store_n(&s_r120a_audio_rate_req,r,__ATOMIC_RELEASE);
    if(s_audio_task)xTaskNotifyGive(s_audio_task);
}

static int async_opm_init(int clock)
{
    if (s_audio_async_enabled)
        return 1;

    s_audio_task_internal_caps = 0;
    if (!OPM_AsyncReserveRing())
        return 0;

    /* YM2151 hot state is allocated with MALLOC_CAP_INTERNAL by vgmm5; all
     * lookup tables are DRAM_ATTR.  R23 forbids the legacy `new AudioOPM()`
     * fallback because default C++ heap placement cannot satisfy the hard
     * "FM never in PSRAM" contract. */
    s_audio_vgm = vgmm5_ym2151_create((uint32_t)clock, 44100u);
    if (s_audio_vgm)
        printf("PX68K_FMBLOCK_R57E70B: render-call zero-depth specialization ACTIVE; existing 256-frame Internal scratch retained; sample/state order unchanged\n");
    if (!s_audio_vgm || !esp_ptr_internal(s_audio_vgm) || esp_ptr_external_ram(s_audio_vgm))
    {
        if (s_audio_vgm) vgmm5_ym2151_destroy(s_audio_vgm);
        s_audio_vgm = NULL;
        printf("PX68K_FMSRAM_R23: FATAL YM2151 state Internal allocation failed; legacy/PSRAM fallback forbidden\n");
        return 0;
    }
    s_audio_opm = NULL;

    __atomic_store_n(&s_audio_event_head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_audio_event_tail, 0u, __ATOMIC_RELAXED);

#if portNUM_PROCESSORS > 1
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        async_opm_task, "px68k_ym2151", 6144, NULL, 4, &s_audio_task, 0,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    BaseType_t ok = xTaskCreateWithCaps(
        async_opm_task, "px68k_ym2151", 6144, NULL, 4, &s_audio_task,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    if (ok != pdPASS)
    {
        vgmm5_ym2151_destroy(s_audio_vgm); s_audio_vgm = NULL;
        printf("PX68K_FMSRAM_R23: FATAL YM2151 task Internal stack allocation failed; PSRAM fallback forbidden\n");
        return 0;
    }
    s_audio_task_internal_caps = 1u;

    s_audio_rd = s_audio_wr = s_audio_count = 0;
    s_audio_selected_reg = 0;
    s_audio_event_drops = 0;
    s_audio_ring_overruns = 0;
    s_r56_write_drop = s_r56_write_drop_frames = 0;
    s_r56_render_drop = s_r56_render_drop_frames = 0;
    s_r56_reset_drop = s_r56_volume_drop = s_r56_csm_drop = 0;
    s_r56_stop_drop = s_r56_other_drop = 0;
    s_r56l_bp_events = s_r56l_bp_wait_calls = s_r56l_bp_timeouts = 0;
    s_r56l_bp_max_timeouts = 0;
    s_r56l_discard_events = s_r56l_discard_frames = 0;
    __atomic_store_n(&s_r120a_audio_rate_req,44100u,__ATOMIC_RELEASE);
    s_r120a_audio_rate_applied=44100u;
    s_audio_async_enabled = 1;
    g_x68p4_opm_async_ready = 1;
    printf("PX68K_FMHOST0: Build 6.15h17R23 YM2151-only core=0; ALL realtime FM memory INTERNAL/SPM, PSRAM forbidden; ring=%u queue=%u quantum=256 prio=4\n",
           (unsigned)ASYNC_OPM_RING_FRAMES, (unsigned)ASYNC_OPM_QUEUE_LEN);
    return 1;
}
#endif /* ESP_PLATFORM */

#ifdef ESP_PLATFORM
static inline void r1a18_opm_rearm_deadline(void)
{
    uint32_t d = R1A18_OPM_LAZY_MAX_CYCLES;
    if (s_fast_opm_ready) d = s_fast_opm.NextObservable10MHz();
    if (!d || d > R1A18_OPM_LAZY_MAX_CYCLES) d = R1A18_OPM_LAZY_MAX_CYCLES;
    g_x68p4_opm_lazy_deadline = d;
}

static inline void r1a18_opm_advance_exact(uint32_t step)
{
    (void)0;
    if (s_fast_opm_ready && (s_fast_opm.regtc & 0x03u))
        (void)0;
    if (s_fast_opm_ready) s_fast_opm.CountGuest10MHz(step);
    if (!s_audio_async_enabled && opm) opm->Count2(step);
}

extern "C" void FASTCALL OPM_R1A18Materialize(void)
{
    const uint32_t pending = g_x68p4_opm_lazy_pending;
    const uint32_t deadline = g_x68p4_opm_lazy_deadline;
    if (pending)
    {
        g_x68p4_opm_lazy_pending = 0u;
        (void)0;
        if (pending >= deadline)
        {
            if (deadline == R1A18_OPM_LAZY_MAX_CYCLES)
                (void)0;
            else
                (void)0;
        }
        r1a18_opm_advance_exact(pending);
    }
    r1a18_opm_rearm_deadline();
}

extern "C" void OPM_R1A18AuditGet(uint32_t out[6])
{
    if (!out) return;
    for (unsigned i=0;i<6u;++i) out[i]=0u;
}
#endif

int OPM_StateAction(StateMem *sm, int load, int data_only)
{
#ifdef ESP_PLATFORM
    /* Saving must serialize the exact current timer phase.  Loading must NOT
     * materialize the pre-load state first, because that could emit a stale
     * IRQ/CSM edge immediately before the saved state overwrites it. */
    if (!load)
        OPM_R1A18Materialize();
    else
        g_x68p4_opm_lazy_pending = 0u;
    if (s_fast_opm_ready)
    {
        const int ret = s_fast_opm.StateAction(sm, load, data_only);
        g_x68p4_opm_lazy_pending = 0u;
        r1a18_opm_rearm_deadline();
        return ret;
    }
    return 1;
#else
    if (opm)
        return opm->StateAction(sm, load, data_only);
    return 1;
#endif
}

int OPM_Init(int clock)
{
#ifdef ESP_PLATFORM
    s_fast_opm.Reset();
    s_fast_opm_ready = 1;
    /* CPU0 host worker remains the waveform authority; 5.40 prefers the YM2151-only vgmM5 backend. */
    if (!async_opm_init(clock))
    {
        /* R23 contract: do not silently allocate any FM object from a default
         * heap that could resolve to PSRAM.  Audio init fails loudly instead. */
        opm = NULL;
        printf("PX68K_OPMCTL: R23 FATAL CPU0 YM2151 Internal-only backend unavailable; FM PSRAM/default-heap fallback forbidden\n");
        return 0;
    }
    g_x68p4_opm_lazy_pending = 0u;
    r1a18_opm_rearm_deadline();
    printf("PX68K_OPMCTL: R1A18 vgmM5 44.1k waveform + lazy guest Timer A/B control plane; scanline OPM_Timer retired\n");
    return 1;
#else
    opm = new MyOPM();
    if (!opm) return 0;
    if (!opm->Init(clock, 44100)) {
        delete opm;
        opm = NULL;
        return 0;
    }
    return 1;
#endif
}


void OPM_Cleanup(void)
{
#ifdef ESP_PLATFORM
    g_x68p4_opm_async_ready = 0;
    s_audio_async_enabled = 0;
    s_fast_opm_ready = 0;
    g_x68p4_opm_lazy_pending = 0u;
    g_x68p4_opm_lazy_deadline = R1A18_OPM_LAZY_MAX_CYCLES;
    /* R23 ESP path never constructs legacy MyOPM. */
    opm = NULL;
#else
    delete opm;
    opm = NULL;
#endif
}

void OPM_Reset(void)
{
#ifdef ESP_PLATFORM
    s_fast_opm.Reset();
    s_fast_opm_ready = 1;
    g_x68p4_opm_lazy_pending = 0u;
    r1a18_opm_rearm_deadline();
    if (opm) opm->Reset();
    if (s_audio_async_enabled)
    {
        if (DSound_AbsTimelineActive()) (void)DSound_AbsTimelineOPMReset();
        else (void)async_opm_send(ASYNC_OPM_RESET, 0, 0, 0, 0);
    }
#else
    if (opm) opm->Reset();
#endif
}


uint8_t FASTCALL OPM_Read(void)
{
#ifdef ESP_PLATFORM
    if (g_x68p4_opm_lazy_pending) (void)0;
    OPM_R1A18Materialize();
    return s_fast_opm_ready ? s_fast_opm.status : 0;
#else
    if (opm) return opm->ReadStatus();
    return 0;
#endif
}


#ifdef ESP_PLATFORM
static DRAM_ATTR uint32_t s_debug_opm_data_writes = 0;
static DRAM_ATTR uint32_t s_debug_opm_keyons = 0;
#else
static uint32_t s_debug_opm_data_writes = 0;
static uint32_t s_debug_opm_keyons = 0;
#endif

extern "C" uint32_t OPM_DebugDataWriteCount(void) { return s_debug_opm_data_writes; }
extern "C" uint32_t OPM_DebugKeyOnCount(void) { return s_debug_opm_keyons; }
extern "C" void OPM_R1A17AuditGet(uint32_t out[3])
{
    if (!out) return;
    out[0]=out[1]=out[2]=0u;
}

extern "C" void OPM_R57E63TimerAuditGet(
    uint64_t *guest_clocks, uint32_t *timer_calls,
    uint32_t *tb_writes, uint32_t *tc_writes,
    uint32_t *tb_starts, uint32_t *tb_stops,
    uint32_t *tb_expires, uint32_t *tb_irq_sets,
    uint32_t *tb_status_clears,
    uint32_t *last_tb, uint32_t *last_tc,
    uint32_t *tb_period_us, uint32_t *tb_count_us)
{
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
    if (guest_clocks) *guest_clocks = s_fast_opm.audit_guest_clocks;
    if (timer_calls) *timer_calls = s_fast_opm.audit_timer_calls;
    if (tb_writes) *tb_writes = s_fast_opm.audit_tb_writes;
    if (tc_writes) *tc_writes = s_fast_opm.audit_tc_writes;
    if (tb_starts) *tb_starts = s_fast_opm.audit_tb_starts;
    if (tb_stops) *tb_stops = s_fast_opm.audit_tb_stops;
    if (tb_expires) *tb_expires = s_fast_opm.audit_tb_expires;
    if (tb_irq_sets) *tb_irq_sets = s_fast_opm.audit_tb_irq_sets;
    if (tb_status_clears) *tb_status_clears = s_fast_opm.audit_tb_status_clears;
    if (last_tb) *last_tb = s_fast_opm.regtb;
    if (last_tc) *last_tc = s_fast_opm.regtc;
    if (tb_period_us) *tb_period_us = s_fast_opm.timerb_us;
    if (tb_count_us) *tb_count_us = s_fast_opm.timerb_count_us;
#else
    if (guest_clocks) *guest_clocks = 0;
    if (timer_calls) *timer_calls = 0;
    if (tb_writes) *tb_writes = 0;
    if (tc_writes) *tc_writes = 0;
    if (tb_starts) *tb_starts = 0;
    if (tb_stops) *tb_stops = 0;
    if (tb_expires) *tb_expires = 0;
    if (tb_irq_sets) *tb_irq_sets = 0;
    if (tb_status_clears) *tb_status_clears = 0;
    if (last_tb) *last_tb = 0;
    if (last_tc) *last_tc = 0;
    if (tb_period_us) *tb_period_us = 0;
    if (tb_count_us) *tb_count_us = 0;
#endif
}

void FASTCALL OPM_WriteTimed(uint32_t adr, uint8_t data, uint32_t frames)
{
#ifdef ESP_PLATFORM
    if (s_fast_opm_ready)
    {
        /* Data-port writes can alter timer period/control/status. Advance the
         * old control state to this existing line-granularity boundary first.
         * Address-latch writes are timer-neutral and stay fast. */
        if (adr & 1u)
        {
            if (g_x68p4_opm_lazy_pending) (void)0;
            OPM_R1A18Materialize();
        }
        s_fast_opm.WriteIO(adr, data);
        if (adr & 1u) r1a18_opm_rearm_deadline();
    }
    if (s_audio_async_enabled)
    {
        if ((adr & 1) == 0)
        {
            s_audio_selected_reg = data;
        }
        else
        {
            ++s_debug_opm_data_writes;
            if (s_audio_selected_reg == 0x08 && (data & 0x78u))
                ++s_debug_opm_keyons;
            if (DSound_AbsTimelineActive())
                (void)DSound_AbsTimelineOPMWrite(s_audio_selected_reg,data);
            else
                (void)async_opm_send(ASYNC_OPM_WRITE, s_audio_selected_reg, data,
                                     frames, 0);
        }
    }
    else if (opm)
    {
        /* Caller flushes the synchronous PCM path before a timed write. */
        opm->WriteIO(adr, data);
    }
#else
    (void)frames;
    if (opm) opm->WriteIO(adr, data);
#endif
}

void FASTCALL OPM_Write(uint32_t adr, uint8_t data)
{
    OPM_WriteTimed(adr, data, 0);
}


void OPM_Update(int16_t *buffer, int length, uint8_t *pbsp, uint8_t *pbep)
{
#ifdef ESP_PLATFORM
    if (opm)
        opm->Mix((int16_t*)buffer, length, pbsp, pbep);
#else
    if (opm) opm->Mix((int16_t*)buffer, length, pbsp, pbep);
#endif
}


void FASTCALL OPM_Timer(uint32_t step)
{
#ifdef ESP_PLATFORM
    /* Compatibility entry point: exact timer math is retained, but the R1A18
     * Machine Kernel no longer calls this once per scanline. */
    r1a18_opm_advance_exact(step);
    r1a18_opm_rearm_deadline();
#else
    if (opm) opm->Count2(step);
#endif
}


void OPM_SetVolume(uint8_t vol)
{
#ifdef ESP_PLATFORM
    if (s_audio_async_enabled)
    {
        if (DSound_AbsTimelineActive()) (void)DSound_AbsTimelineOPMVolume(vol);
        else (void)async_opm_send(ASYNC_OPM_VOLUME, 0, vol, 0, 0);
    }
    else if (opm)
    {
        const int v = (vol)?((16-vol)*4):192;
        opm->SetVolume(-v);
    }
#else
    int v = (vol)?((16-vol)*4):192;
    if (opm) opm->SetVolume(-v);
#endif
}

#ifdef ESP_PLATFORM
/* R140P4S3: CPU0 absolute-timeline waveform entry points.
 * The timeline task is the only caller while active, so the legacy YM task
 * remains asleep.  Rendering waits for FIFO space instead of discarding,
 * preserving exact FM/ADPCM sample-position alignment in this correctness
 * reference build. */
extern "C" int OPM_AbsTimelineRenderCPU0(uint32_t frames)
{
    if (!s_audio_async_enabled || (!s_audio_vgm && !s_audio_opm)) return 0;
    const uint32_t req=__atomic_load_n(&s_r120a_audio_rate_req,__ATOMIC_ACQUIRE);
    if(s_audio_vgm && req!=s_r120a_audio_rate_applied){
        vgmm5_ym2151_set_sample_rate(s_audio_vgm,req);
        s_r120a_audio_rate_applied=req;
    }
    while (frames)
    {
        uint32_t n=frames>ASYNC_OPM_SCRATCH_FRAMES?ASYNC_OPM_SCRATCH_FRAMES:frames;
        for (;;)
        {
            size_t free_frames;
            portENTER_CRITICAL(&s_audio_mux);
            free_frames=ASYNC_OPM_RING_FRAMES-s_audio_count;
            portEXIT_CRITICAL(&s_audio_mux);
            if (free_frames>=n) break;
            vTaskDelay(1);
        }
        (void)async_render_publish(n,0);
        frames-=n;
    }
    return 1;
}
extern "C" int OPM_AbsTimelineWriteCPU0(uint8_t reg,uint8_t data)
{
    if (!s_audio_async_enabled || (!s_audio_vgm && !s_audio_opm)) return 0;
    if (s_audio_vgm) vgmm5_ym2151_write(s_audio_vgm,reg,data);
    else s_audio_opm->WriteReg(reg,data);
    return 1;
}
extern "C" int OPM_AbsTimelineResetCPU0(void)
{
    if (!s_audio_async_enabled) return 0;
    if (s_audio_vgm) vgmm5_ym2151_reset(s_audio_vgm);
    else if (s_audio_opm) { s_audio_opm->Reset(); s_audio_opm->CurCount=0; }
    portENTER_CRITICAL(&s_audio_mux);
    s_audio_rd=s_audio_wr=s_audio_count=0;
    portEXIT_CRITICAL(&s_audio_mux);
    return 1;
}
extern "C" int OPM_AbsTimelineVolumeCPU0(uint8_t vol)
{
    if (!s_audio_async_enabled) return 0;
    if (s_audio_vgm) vgmm5_ym2151_set_px_volume(s_audio_vgm,vol);
    else if (s_audio_opm) { const int v=vol?((16-vol)*4):192; s_audio_opm->SetVolume(-v); }
    return 1;
}
extern "C" int OPM_AbsTimelineCSMCPU0(void)
{
    if (!s_audio_async_enabled) return 0;
    if (s_audio_vgm) vgmm5_ym2151_csm_pulse(s_audio_vgm);
    else if (s_audio_opm) for(uint8_t ch=0;ch<8;++ch){s_audio_opm->WriteReg(0x08,ch);s_audio_opm->WriteReg(0x08,(uint8_t)(ch|0x78u));}
    return 1;
}
extern "C" void OPM_AbsTimelinePerfGet(uint32_t *frames,uint32_t *work_us)
{
    if(frames)*frames=0u;
    if(work_us)*work_us=0u;
}
extern "C" void OPM_AbsTimelinePerfGetEx(uint32_t *frames,uint32_t *work_us,uint32_t *wait_us,uint32_t *wait_events)
{
    OPM_AbsTimelinePerfGet(frames,work_us);
    if(wait_us)*wait_us=0u;
    if(wait_events)*wait_events=0u;
}


int OPM_AsyncEnabled(void)
{
	return s_audio_async_enabled ? 1 : 0;
}

int OPM_AsyncRender(uint32_t frames, int profile)
{
	if (!s_audio_async_enabled || !frames)
		return 0;
	return async_opm_send(ASYNC_OPM_RENDER, 0, 0, frames, profile ? 1 : 0);
}

uint32_t OPM_AsyncFramesAvail(void)
{
	if (!s_audio_async_enabled)
		return 0;
	/* BAT177NW0 CPU1 NO-WAIT: this is diagnostic/flow telemetry only.
	 * CPU0 owns the ring mutation; CPU1 must never spin on the CPU0 audio mux. */
	return (uint32_t)__atomic_load_n(&s_audio_count, __ATOMIC_ACQUIRE);
}

/* Production clean: historical silent flight-recorder ABI retained as zero. */
extern "C" void OPM_R140P4D2MixClipGet(uint64_t *samples, uint64_t *clips)
{
    if (samples) *samples = 0u;
    if (clips) *clips = 0u;
}

extern "C" void OPM_R140P4D2SilentGet(uint32_t *generated_frames, uint32_t *opm_writes)
{
    if (generated_frames) *generated_frames = 0u;
    if (opm_writes) *opm_writes = 0u;
}

extern "C" void OPM_R140P4D7DemandGet(uint32_t *requested_frames,
                                        uint32_t *accepted_frames,
                                        uint32_t *rendered_frames,
                                        uint32_t *queue_depth,
                                        uint32_t *queue_max)
{
    if (requested_frames) *requested_frames = 0u;
    if (accepted_frames) *accepted_frames = 0u;
    if (rendered_frames) *rendered_frames = 0u;
    if (queue_depth) *queue_depth = async_event_depth();
    if (queue_max) *queue_max = 0u;
}

extern "C" void OPM_R140P4D7WindowBegin(void) {}

int OPM_AsyncMixRead(int16_t *dst, int frames)
{
	if (!s_audio_async_enabled || !dst || frames <= 0)
		return 0;

    int copied = 0;
    while (copied < frames)
    {
        size_t n = 0;

        /* Linearize consumption under the existing ring lock, but copy only
         * one <=256-frame Internal-SRAM chunk while locked.  The expensive
         * saturating add happens after the lock is released. */
        portENTER_CRITICAL(&s_audio_mux);
        if (s_audio_count)
        {
            n = s_audio_count;
            const size_t want = (size_t)(frames - copied);
            if (n > want) n = want;
            if (n > ASYNC_OPM_SCRATCH_FRAMES) n = ASYNC_OPM_SCRATCH_FRAMES;
            const size_t contiguous = ASYNC_OPM_RING_FRAMES - s_audio_rd;
            if (n > contiguous) n = contiguous;
            memcpy(s_audio_mix_read_scratch, &s_audio_ring[s_audio_rd * 2],
                   n * 2u * sizeof(int16_t));
            s_audio_rd = (s_audio_rd + n) & ASYNC_OPM_RING_MASK;
            s_audio_count -= n;
        }
        portEXIT_CRITICAL(&s_audio_mux);

        if (!n)
            break;

        int16_t *out = dst + (size_t)copied * 2u;
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        uint32_t clip_local = 0;
        uint32_t max_raw_local = 0;
#endif
        for (size_t i = 0; i < n * 2u; ++i)
        {
            int v2 = (int)out[i] + (int)s_audio_mix_read_scratch[i];
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
            uint32_t raw_abs = (uint32_t)(v2 < 0 ? -v2 : v2);
            if (raw_abs > max_raw_local) max_raw_local = raw_abs;
            if (v2 > 32767 || v2 < -32768) ++clip_local;
#endif
            if (v2 > 32767) v2 = 32767;
            else if (v2 < -32768) v2 = -32768;
            out[i] = (int16_t)v2;
        }
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
        s_r57e64_mix_samples += (uint32_t)(n * 2u);
        s_r57e64_mix_clips += clip_local;
        if (max_raw_local > s_r57e64_mix_max_raw_abs)
            s_r57e64_mix_max_raw_abs = max_raw_local;
#endif
        copied += (int)n;
    }
    return copied;
}

extern "C" void OPM_R57E64MixClipAuditGet(uint64_t *mixed_samples,
                                            uint64_t *clipped_samples,
                                            uint32_t *max_raw_abs)
{
#if PX68K_TAB5_R57E63_AUDIO_AUDIT
	if (mixed_samples)
		*mixed_samples = (uint64_t)__atomic_load_n(&s_r57e64_mix_samples, __ATOMIC_RELAXED);
	if (clipped_samples)
		*clipped_samples = (uint64_t)__atomic_load_n(&s_r57e64_mix_clips, __ATOMIC_RELAXED);
	if (max_raw_abs)
		*max_raw_abs = __atomic_load_n(&s_r57e64_mix_max_raw_abs, __ATOMIC_RELAXED);
#else
	if (mixed_samples) *mixed_samples = 0;
	if (clipped_samples) *clipped_samples = 0;
	if (max_raw_abs) *max_raw_abs = 0;
#endif
}

void OPM_AsyncPerfBegin(void) {}

void OPM_AsyncPerfGet(uint32_t *us, uint32_t *calls, uint32_t *frames,
                      uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns)
{
    if (us) *us = 0u;
    if (calls) *calls = 0u;
    if (frames) *frames = 0u;
    if (qdepth) *qdepth = async_event_depth();
    if (event_drops) *event_drops = s_audio_event_drops;
    if (ring_overruns) *ring_overruns = s_audio_ring_overruns;
}

extern "C" void WinX68k_AudioAsyncGetQueueTaxonomy(
    uint32_t *write_attempt, uint32_t *write_drop, uint32_t *write_drop_frames,
    uint32_t *render_attempt, uint32_t *render_drop, uint32_t *render_drop_frames,
    uint32_t *reset_drop, uint32_t *volume_drop, uint32_t *csm_drop,
    uint32_t *stop_drop, uint32_t *other_drop)
{
    if (write_attempt) *write_attempt = 0u;
    if (write_drop) *write_drop = s_r56_write_drop;
    if (write_drop_frames) *write_drop_frames = s_r56_write_drop_frames;
    if (render_attempt) *render_attempt = 0u;
    if (render_drop) *render_drop = s_r56_render_drop;
    if (render_drop_frames) *render_drop_frames = s_r56_render_drop_frames;
    if (reset_drop) *reset_drop = s_r56_reset_drop;
    if (volume_drop) *volume_drop = s_r56_volume_drop;
    if (csm_drop) *csm_drop = s_r56_csm_drop;
    if (stop_drop) *stop_drop = s_r56_stop_drop;
    if (other_drop) *other_drop = s_r56_other_drop;
}

extern "C" void WinX68k_AudioAsyncGetBackpressureStats(
    uint32_t *bp_events, uint32_t *wait_calls, uint32_t *timeouts,
    uint32_t *max_timeouts, uint32_t *discard_events, uint32_t *discard_frames)
{
    if (bp_events) *bp_events = s_r56l_bp_events;
    if (wait_calls) *wait_calls = s_r56l_bp_wait_calls;
    if (timeouts) *timeouts = s_r56l_bp_timeouts;
    if (max_timeouts) *max_timeouts = s_r56l_bp_max_timeouts;
    if (discard_events) *discard_events = s_r56l_discard_events;
    if (discard_frames) *discard_frames = s_r56l_discard_frames;
}

void OPM_AsyncWorkGet(uint32_t *us, uint32_t *calls, uint32_t *frames)
{
    if (us) *us = 0u;
    if (calls) *calls = 0u;
    if (frames) *frames = 0u;
}

void OPM_AsyncDetailProfileGet(OPMAsyncDetailProfile *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

void OPM_AsyncSemanticAlgoGet(uint32_t out[8])
{
    if (!out) return;
    for (int i = 0; i < 8; ++i) out[i] = 0u;
}

uint32_t OPM_AsyncStackHighWater(void)
{
    return s_audio_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_audio_task) : 0u;
}

uint32_t OPM_AsyncRingBytes(void)
{
    return (uint32_t)sizeof(s_audio_ring_storage);
}

int OPM_AsyncSpmControlOk(void)
{
    return esp_ptr_in_tcm(&s_fast_opm) && esp_ptr_in_tcm(&s_fast_opm_ready) &&
           esp_ptr_in_tcm(&s_audio_selected_reg);
}

int OPM_AsyncInternalOnly(void)
{
    if (!s_audio_ring || !s_audio_vgm || !s_audio_task ||
        !s_audio_task_internal_caps)
        return 0;
    /* xQueueCreateWithCaps() and xTaskCreate*WithCaps() are the allocator-level
     * guarantees for queue storage and task stack.  Do not depend on
     * INCLUDE_pxTaskGetStackStart just to re-discover the task-stack pointer. */
    const void *ptrs[] = {
        s_audio_ring, s_audio_vgm, s_vgm_scratch,
        &s_fast_opm, &s_fast_opm_ready, &s_audio_selected_reg
    };
    for (unsigned i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); ++i) {
        if (!ptrs[i] || esp_ptr_external_ram(ptrs[i]))
            return 0;
        /* ESP-IDF 5.4.2 reports P4 SPM/TCM separately from ordinary
         * esp_ptr_internal(); both are on-chip and satisfy the R23/R24 rule. */
        if (!esp_ptr_internal(ptrs[i]) && !esp_ptr_in_tcm(ptrs[i]))
            return 0;
    }
    return vgmm5_ym2151_memory_internal(s_audio_vgm) ? 1 : 0;
}
#else
int OPM_AsyncReserveRing(void) { return 0; }
int OPM_AsyncRingPlacement(void) { return 0; }
int OPM_AsyncEnabled(void) { return 0; }
int OPM_AsyncRender(uint32_t frames, int profile) { (void)frames; (void)profile; return 0; }
uint32_t OPM_AsyncFramesAvail(void) { return 0; }
int OPM_AsyncMixRead(int16_t *dst, int frames) { (void)dst; (void)frames; return 0; }
extern "C" void OPM_R57E64MixClipAuditGet(uint64_t *mixed_samples,
                                            uint64_t *clipped_samples,
                                            uint32_t *max_raw_abs)
{
    if (mixed_samples) *mixed_samples = 0;
    if (clipped_samples) *clipped_samples = 0;
    if (max_raw_abs) *max_raw_abs = 0;
}
void OPM_AsyncPerfBegin(void) {}
void OPM_AsyncPerfGet(uint32_t *us, uint32_t *calls, uint32_t *frames, uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns)
{
	if (us) *us = 0; if (calls) *calls = 0; if (frames) *frames = 0;
	if (qdepth) *qdepth = 0; if (event_drops) *event_drops = 0; if (ring_overruns) *ring_overruns = 0;
}
extern "C" void WinX68k_AudioAsyncGetQueueTaxonomy(
    uint32_t *write_attempt, uint32_t *write_drop, uint32_t *write_drop_frames,
    uint32_t *render_attempt, uint32_t *render_drop, uint32_t *render_drop_frames,
    uint32_t *reset_drop, uint32_t *volume_drop, uint32_t *csm_drop,
    uint32_t *stop_drop, uint32_t *other_drop)
{
    uint32_t *p[] = {write_attempt, write_drop, write_drop_frames, render_attempt, render_drop,
                     render_drop_frames, reset_drop, volume_drop, csm_drop, stop_drop, other_drop};
    for (unsigned i = 0; i < sizeof(p)/sizeof(p[0]); ++i) if (p[i]) *p[i] = 0;
}
extern "C" void WinX68k_AudioAsyncGetBackpressureStats(
    uint32_t *bp_events, uint32_t *wait_calls, uint32_t *timeouts,
    uint32_t *max_timeouts, uint32_t *discard_events, uint32_t *discard_frames)
{
    uint32_t *p[] = {bp_events, wait_calls, timeouts, max_timeouts, discard_events, discard_frames};
    for (unsigned i = 0; i < sizeof(p)/sizeof(p[0]); ++i) if (p[i]) *p[i] = 0;
}
void OPM_AsyncWorkGet(uint32_t *us, uint32_t *calls, uint32_t *frames)
{
	if (us) *us = 0; if (calls) *calls = 0; if (frames) *frames = 0;
}
void OPM_AsyncDetailProfileGet(OPMAsyncDetailProfile *out)
{
    if (out) *out = {};
}

void OPM_AsyncSemanticAlgoGet(uint32_t out[8])
{
    if (!out) return;
    for (int i = 0; i < 8; ++i) out[i] = 0;
}

uint32_t OPM_AsyncStackHighWater(void) { return 0u; }
uint32_t OPM_AsyncRingBytes(void) { return 0u; }
int OPM_AsyncInternalOnly(void) { return 0; }
int OPM_AsyncSpmControlOk(void) { return 0; }
#endif


// ----------------------------------------------------------
// ---------------------------- YMF288 (Ǥޏꏢ)
// ----------------------------------------------------------
// TODO : ROMEO288á

class YMF288 : public FM::Y288
{
public:
	YMF288();
	virtual ~YMF288() {}
	void WriteIO(uint32_t adr, uint8_t data);
	uint8_t ReadIO(uint32_t adr);
	void Count2(uint32_t clock);
	void SetInt(int f) { IntrFlag = f; };
private:
	virtual void Intr(bool);
	int CurReg[2];
	uint32_t CurCount;
	int IntrFlag;
};

YMF288::YMF288()
{
	CurReg[0] = 0;
	CurReg[1] = 0;
	IntrFlag = 0;
}

void YMF288::WriteIO(uint32_t adr, uint8_t data)
{
	if( adr&1 )
		SetReg(((adr&2)?(CurReg[1]+0x100):CurReg[0]), (int)data);
   else
		CurReg[(adr>>1)&1] = (int)data;
}


uint8_t YMF288::ReadIO(uint32_t adr)
{
	if ( adr&1 )
		return GetReg(((adr&2)?(CurReg[1]+0x100):CurReg[0]));
   return ((adr)?(ReadStatusEx()):(ReadStatus()));
}

void YMF288::Intr(bool f)
{
   if ( (f)&&(IntrFlag) )
      ::Mcry_Int();
}


void YMF288::Count2(uint32_t clock)
{
	CurCount += clock;
	Count(CurCount/10);
	CurCount %= 10;
}

static YMF288* ymf288a = NULL;
static YMF288* ymf288b = NULL;

int M288_Init(int clock, const char* path)
{
	ymf288a = new YMF288();
	ymf288b = new YMF288();
	if ( (!ymf288a)||(!ymf288b) )
      goto error;
   if ( (!ymf288a->Init(clock, 44100, path))||(!ymf288b->Init(clock, 44100, path)) )
      goto error;
	ymf288a->SetInt(1);
	ymf288b->SetInt(0);
	return 1;

error:
   M288_Cleanup();
   return 0;
}


void M288_Cleanup(void)
{
	delete ymf288a;
	delete ymf288b;
	ymf288a = ymf288b = NULL;
}

void M288_Reset(void)
{
	if ( ymf288a ) ymf288a->Reset();
	if ( ymf288b ) ymf288b->Reset();
}


uint8_t FASTCALL M288_Read(uint16_t adr)
{
	if ( adr<=3 )
   {
		if ( ymf288a )
			return ymf288a->ReadIO(adr);
	}
   else
   {
		if ( ymf288b )
			return ymf288b->ReadIO(adr&3);
	}
   return 0;
}


void FASTCALL M288_Write(uint32_t adr, uint8_t data)
{
	if ( adr<=3 )
   {
      if ( ymf288a )
         ymf288a->WriteIO(adr, data);
	}
   else
   {
		if ( ymf288b )
         ymf288b->WriteIO(adr&3, data);
	}
}


void M288_Update(int16_t *buffer, size_t length)
{
	if ( ymf288a ) ymf288a->Mix((int16_t*)buffer, length);
	if ( ymf288b ) ymf288b->Mix((int16_t*)buffer, length);
}


void FASTCALL M288_Timer(uint32_t step)
{
	if ( ymf288a ) ymf288a->Count2(step);
	if ( ymf288b ) ymf288b->Count2(step);
}


void M288_SetVolume(uint8_t vol)
{
	int v1 = (vol)?((16-vol)*4-24):192;		// Τ餤ʤ
	int v2 = (vol)?((16-vol)*4):192;		// 
	if ( ymf288a ) {
		ymf288a->SetVolumeFM(-v1);
		ymf288a->SetVolumePSG(-v2);
	}
	if ( ymf288b ) {
		ymf288b->SetVolumeFM(-v1);
		ymf288b->SetVolumePSG(-v2);
	}
}
