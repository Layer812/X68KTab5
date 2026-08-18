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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "vgmm5_ym2151.h"
#include <string.h>

/* Build 5.23b: avoid a direct esp_timer component-header dependency here.
 * dswin.c already uses the same IDF symbol this way; the app links esp_timer. */
extern "C" int64_t esp_timer_get_time(void);

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

static MyOPM* opm = NULL;

#ifdef ESP_PLATFORM
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
    }

    void SetStatus(uint8_t bits)
    {
        if ((status & bits) == 0)
        {
            status |= bits;
            ::MFP_Int(12);
        }
    }

    void ResetStatus(uint8_t bits)
    {
        status &= (uint8_t)~bits;
        /* MyOPM::Intr(false) was intentionally a no-op as well. */
    }

    void SetTimerA(uint32_t addr, uint8_t data)
    {
        regta[addr & 1u] = data;
        const uint32_t ta = ((uint32_t)regta[0] << 2) | ((uint32_t)regta[1] & 3u);
        timera_us = (1024u - ta) * 16u;
    }

    void SetTimerB(uint8_t data)
    {
        regtb = data;
        timerb_us = (256u - (uint32_t)data) * 256u;
    }

    void SetTimerControl(uint8_t data)
    {
        const uint8_t changed = regtc ^ data;
        regtc = data;
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
        /* FMGEN OPM::TimerA() performs CSM key-off/key-on on all 8 channels.
         * 5.40 preserves that rare side effect with one sparse queue event,
         * instead of forwarding every 10 MHz timer slice to the CPU0 waveform worker. */
        if (a_expired && (regtc & 0x80u)) audio_csm_notify();
        if (a_expired && (regtc & 0x04u)) SetStatus(1u);
        if (b_expired && (regtc & 0x08u)) SetStatus(2u);
    }

    void CountGuest10MHz(uint32_t clocks)
    {
        subus_10mhz += clocks;
        const uint32_t us = subus_10mhz / 10u;
        subus_10mhz %= 10u;
        if (us) CountUs(us);
    }

    void WriteIO(uint32_t adr, uint8_t data)
    {
        if ((adr & 1u) == 0)
        {
            cur_reg = data;
            return;
        }

        const uint8_t reg = (uint8_t)cur_reg;
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

static DRAM_ATTR FastOPMControl s_fast_opm;
static DRAM_ATTR int s_fast_opm_ready = 0;
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

#define ASYNC_OPM_RING_FRAMES 32768u
#define ASYNC_OPM_RING_MASK   (ASYNC_OPM_RING_FRAMES - 1u)
#define ASYNC_OPM_QUEUE_LEN     512u
#define ASYNC_OPM_SCRATCH_FRAMES 64u

static AudioOPM *s_audio_opm = NULL; /* legacy fallback only */
static VgmM5YM2151 *s_audio_vgm = NULL;
static DRAM_ATTR QueueHandle_t s_audio_q = NULL;
static DRAM_ATTR TaskHandle_t s_audio_task = NULL;
static DRAM_ATTR int16_t *s_audio_ring = NULL;
static DRAM_ATTR size_t s_audio_rd = 0;
static DRAM_ATTR size_t s_audio_wr = 0;
static DRAM_ATTR size_t s_audio_count = 0;
static DRAM_ATTR portMUX_TYPE s_audio_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR volatile int s_audio_async_enabled = 0;
static DRAM_ATTR uint8_t s_audio_selected_reg = 0;
static DRAM_ATTR volatile uint32_t s_audio_event_drops = 0;
static DRAM_ATTR volatile uint32_t s_audio_ring_overruns = 0;
static DRAM_ATTR volatile uint32_t s_audio_profile_us = 0;
static DRAM_ATTR volatile uint32_t s_audio_profile_calls = 0;
static DRAM_ATTR volatile uint32_t s_audio_profile_frames = 0;
/* Render hot loop must not store sample-by-sample into the PSRAM ring. */
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

static void async_opm_task(void *)
{
    AsyncOPMEvent ev;
    for (;;)
    {
        if (xQueueReceive(s_audio_q, &ev, portMAX_DELAY) != pdTRUE)
            continue;
        if (ev.type == ASYNC_OPM_STOP)
            break;
        if (!s_audio_vgm && !s_audio_opm)
            continue;

        switch (ev.type)
        {
            case ASYNC_OPM_WRITE:
                if (s_audio_vgm) vgmm5_ym2151_write(s_audio_vgm, ev.reg, ev.data);
                else if (s_audio_opm) s_audio_opm->WriteReg(ev.reg, ev.data);
                break;

            case ASYNC_OPM_RENDER:
            {
                if (!ev.frames) break;
                size_t wr;
                size_t free_frames;
                portENTER_CRITICAL(&s_audio_mux);
                wr = s_audio_wr;
                free_frames = ASYNC_OPM_RING_FRAMES - s_audio_count;
                portEXIT_CRITICAL(&s_audio_mux);
                if (free_frames < ev.frames)
                {
                    ++s_audio_ring_overruns;
                    break;
                }

                /* vgmM5 backend overwrites every output sample; only the legacy
                 * additive FMGEN fallback requires a zeroed destination. */
                if (!s_audio_vgm)
                    async_ring_zero(wr, ev.frames);
                int64_t t0 = ev.profile ? esp_timer_get_time() : 0;
                if (s_audio_vgm)
                    async_vgm_render(wr, ev.frames);
                else if (s_audio_opm)
                    s_audio_opm->Mix(&s_audio_ring[wr * 2], (int)ev.frames,
                                     (uint8_t *)s_audio_ring,
                                     (uint8_t *)(s_audio_ring + ASYNC_OPM_RING_FRAMES * 2));
                if (ev.profile)
                {
                    s_audio_profile_us += (uint32_t)(esp_timer_get_time() - t0);
                    ++s_audio_profile_calls;
                    s_audio_profile_frames += ev.frames;
                }

                portENTER_CRITICAL(&s_audio_mux);
                s_audio_wr = (s_audio_wr + ev.frames) & ASYNC_OPM_RING_MASK;
                s_audio_count += ev.frames;
                portEXIT_CRITICAL(&s_audio_mux);
                break;
            }

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
    }

    if (s_audio_vgm) { vgmm5_ym2151_destroy(s_audio_vgm); s_audio_vgm = NULL; }
    delete s_audio_opm;
    s_audio_opm = NULL;
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static int async_opm_send(uint8_t type, uint8_t reg, uint8_t data,
                          uint32_t frames, uint8_t profile)
{
    if (!s_audio_async_enabled || !s_audio_q)
        return 0;
    AsyncOPMEvent ev = {};
    ev.type = type;
    ev.reg = reg;
    ev.data = data;
    ev.frames = frames;
    ev.profile = profile;
    if (xQueueSend(s_audio_q, &ev, 0) != pdTRUE)
    {
        ++s_audio_event_drops;
        return 0;
    }
    return 1;
}

static void audio_csm_notify(void)
{
    if (s_audio_async_enabled)
        (void)async_opm_send(ASYNC_OPM_CSM, 0, 0, 0, 0);
}

static int async_opm_init(int clock)
{
    if (s_audio_async_enabled)
        return 1;

    s_audio_ring = (int16_t *)heap_caps_malloc(
        ASYNC_OPM_RING_FRAMES * 2 * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_audio_ring)
        return 0;
    memset(s_audio_ring, 0, ASYNC_OPM_RING_FRAMES * 2 * sizeof(int16_t));

    /* YM2151 hot state is internal-DRAM allocated; lookup tables are DRAM_ATTR. */
    s_audio_vgm = vgmm5_ym2151_create((uint32_t)clock, 44100u);
    if (!s_audio_vgm)
    {
        s_audio_opm = new AudioOPM();
        if (!s_audio_opm || !s_audio_opm->Init(clock, 44100))
        {
            delete s_audio_opm;
            s_audio_opm = NULL;
            heap_caps_free(s_audio_ring);
            s_audio_ring = NULL;
            return 0;
        }
    }

    s_audio_q = xQueueCreate(ASYNC_OPM_QUEUE_LEN, sizeof(AsyncOPMEvent));
    if (!s_audio_q)
    {
        if (s_audio_vgm) { vgmm5_ym2151_destroy(s_audio_vgm); s_audio_vgm = NULL; }
        delete s_audio_opm;
        s_audio_opm = NULL;
        heap_caps_free(s_audio_ring);
        s_audio_ring = NULL;
        return 0;
    }

#if portNUM_PROCESSORS > 1
    BaseType_t ok = xTaskCreatePinnedToCore(async_opm_task, "px68k_ym2151", 6144,
                                            NULL, 2, &s_audio_task, 0);
#else
    BaseType_t ok = xTaskCreate(async_opm_task, "px68k_ym2151", 6144,
                                NULL, 2, &s_audio_task);
#endif
    if (ok != pdPASS)
    {
        vQueueDelete(s_audio_q);
        s_audio_q = NULL;
        if (s_audio_vgm) { vgmm5_ym2151_destroy(s_audio_vgm); s_audio_vgm = NULL; }
        delete s_audio_opm;
        s_audio_opm = NULL;
        heap_caps_free(s_audio_ring);
        s_audio_ring = NULL;
        return 0;
    }

    s_audio_rd = s_audio_wr = s_audio_count = 0;
    s_audio_selected_reg = 0;
    s_audio_event_drops = 0;
    s_audio_ring_overruns = 0;
    s_audio_async_enabled = 1;
    if (s_audio_vgm)
        printf("PX68K_FMHOST0: Build 5.47 YM2151-only ready core=0 direct-algo+active-mask+LFO0-bypass+DRAM-scratch64 ring=%u queue=%u chunk=64\\n",
               (unsigned)ASYNC_OPM_RING_FRAMES, (unsigned)ASYNC_OPM_QUEUE_LEN);
    else
        printf("PX68K_FMHOST0: Build 5.47 internal alloc fallback -> legacy FMGEN core=0 ring=%u queue=%u chunk=64\\n",
               (unsigned)ASYNC_OPM_RING_FRAMES, (unsigned)ASYNC_OPM_QUEUE_LEN);
    return 1;
}
#endif /* ESP_PLATFORM */

int OPM_StateAction(StateMem *sm, int load, int data_only)
{
#ifdef ESP_PLATFORM
    if (s_fast_opm_ready)
        return s_fast_opm.StateAction(sm, load, data_only);
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
        /* Rare compatibility fallback: retain synchronous FMGEN if CPU0 host worker setup fails. */
        opm = new MyOPM();
        if (!opm || !opm->Init(clock, 44100))
        {
            delete opm;
            opm = NULL;
            return 0;
        }
        printf("PX68K_OPMCTL: Build 5.47 CPU0 host YM2151 init failed; synchronous FMGEN fallback active\n");
    }
    else
    {
        printf("PX68K_OPMCTL: Build 5.47 CPU1 lightweight YM2151 timer/status shim + CPU0 direct-algorithm YM2151 backend active\n");
    }
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
    s_fast_opm_ready = 0;
    delete opm;
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
    if (opm) opm->Reset();
    if (s_audio_async_enabled)
        async_opm_send(ASYNC_OPM_RESET, 0, 0, 0, 0);
#else
    if (opm) opm->Reset();
#endif
}


uint8_t FASTCALL OPM_Read(void)
{
#ifdef ESP_PLATFORM
    return s_fast_opm_ready ? s_fast_opm.status : 0;
#else
    if (opm) return opm->ReadStatus();
    return 0;
#endif
}


static uint32_t s_debug_opm_data_writes = 0;
static uint32_t s_debug_opm_keyons = 0;

extern "C" uint32_t OPM_DebugDataWriteCount(void) { return s_debug_opm_data_writes; }
extern "C" uint32_t OPM_DebugKeyOnCount(void) { return s_debug_opm_keyons; }

void FASTCALL OPM_Write(uint32_t adr, uint8_t data)
{
#ifdef ESP_PLATFORM
    if (s_fast_opm_ready)
        s_fast_opm.WriteIO(adr, data);
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
            async_opm_send(ASYNC_OPM_WRITE, s_audio_selected_reg, data, 0, 0);
        }
    }
    else if (opm)
    {
        opm->WriteIO(adr, data);
    }
#else
    if (opm) opm->WriteIO(adr, data);
#endif
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
    if (s_fast_opm_ready)
        s_fast_opm.CountGuest10MHz(step);
    if (!s_audio_async_enabled && opm)
        opm->Count2(step);
#else
    if (opm) opm->Count2(step);
#endif
}


void OPM_SetVolume(uint8_t vol)
{
#ifdef ESP_PLATFORM
    if (s_audio_async_enabled)
        async_opm_send(ASYNC_OPM_VOLUME, 0, vol, 0, 0);
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
	uint32_t n = 0;
	if (!s_audio_async_enabled)
		return 0;
	portENTER_CRITICAL(&s_audio_mux);
	n = (uint32_t)s_audio_count;
	portEXIT_CRITICAL(&s_audio_mux);
	return n;
}

int OPM_AsyncMixRead(int16_t *dst, int frames)
{
	if (!s_audio_async_enabled || !dst || frames <= 0)
		return 0;

	portENTER_CRITICAL(&s_audio_mux);
	if ((size_t)frames > s_audio_count)
		frames = (int)s_audio_count;
	int remain = frames;
	int16_t *out = dst;
	while (remain > 0)
	{
		size_t n = ASYNC_OPM_RING_FRAMES - s_audio_rd;
		if (n > (size_t)remain) n = (size_t)remain;
		const int16_t *fm = &s_audio_ring[s_audio_rd * 2];
		for (size_t i = 0; i < n * 2; ++i)
		{
			int v2 = (int)out[i] + (int)fm[i];
			if (v2 > 32767) v2 = 32767;
			else if (v2 < -32768) v2 = -32768;
			out[i] = (int16_t)v2;
		}
		out += n * 2;
		s_audio_rd = (s_audio_rd + n) & ASYNC_OPM_RING_MASK;
		s_audio_count -= n;
		remain -= (int)n;
	}
	portEXIT_CRITICAL(&s_audio_mux);
	return frames;
}

void OPM_AsyncPerfBegin(void)
{
	s_audio_profile_us = 0;
	s_audio_profile_calls = 0;
	s_audio_profile_frames = 0;
}

void OPM_AsyncPerfGet(uint32_t *us, uint32_t *calls, uint32_t *frames,
                      uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns)
{
	if (us) *us = s_audio_profile_us;
	if (calls) *calls = s_audio_profile_calls;
	if (frames) *frames = s_audio_profile_frames;
	if (qdepth) *qdepth = s_audio_q ? (uint32_t)uxQueueMessagesWaiting(s_audio_q) : 0;
	if (event_drops) *event_drops = s_audio_event_drops;
	if (ring_overruns) *ring_overruns = s_audio_ring_overruns;
}
#else
int OPM_AsyncEnabled(void) { return 0; }
int OPM_AsyncRender(uint32_t frames, int profile) { (void)frames; (void)profile; return 0; }
uint32_t OPM_AsyncFramesAvail(void) { return 0; }
int OPM_AsyncMixRead(int16_t *dst, int frames) { (void)dst; (void)frames; return 0; }
void OPM_AsyncPerfBegin(void) {}
void OPM_AsyncPerfGet(uint32_t *us, uint32_t *calls, uint32_t *frames, uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns)
{
	if (us) *us = 0; if (calls) *calls = 0; if (frames) *frames = 0;
	if (qdepth) *qdepth = 0; if (event_drops) *event_drops = 0; if (ring_overruns) *ring_overruns = 0;
}
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
