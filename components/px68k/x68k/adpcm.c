/*
 *  ADPCM.C - ADPCM (OKI MSM6258V)
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Keep the validated scalar-hoisted ADPCM interpolation path after the experimental PIE interpolation failed exactness checks.
 * Layer8 Aug/17/2026
 */
#include "common.h"
#include "prop.h"
#include "pia.h"
#include "adpcm.h"
#include "adpcm_cpu0.h"
#include "dmac.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_memory_utils.h"
#if defined(SPM_DRAM_ATTR)
#define PX68K_ADHOT SPM_DRAM_ATTR
#elif defined(TCM_DRAM_ATTR)
#define PX68K_ADHOT TCM_DRAM_ATTR
#else
#define PX68K_ADHOT DRAM_ATTR
#endif
#define PX68K_ADTABLE DRAM_ATTR
#define PX68K_ADBUF DRAM_ATTR
#define PX68K_ADIRAM IRAM_ATTR
/* R140P4S3: legacy helper retained for fallback path; absolute timeline is authoritative when active. */
extern void DSound_FlushADPCMPending(void);
#else
#define PX68K_ADHOT
#define PX68K_ADTABLE
#define PX68K_ADBUF
#define PX68K_ADIRAM
#endif


/* Intent: Exact audio samples are more important than vector speed here; keep the scalar-hoisted path after the PIE interpolation trial failed bit-exact validation.  Layer8 Aug/17/2026 */
/* Build 5.99rc1: failed PIE interpolation trial removed; scalar-hoisted cubic path is production. */
#ifdef ESP_PLATFORM
/* R24: this is a Core1-only decode staging FIFO, not the Core1->Core0 host
 * audio ring.  R23 runtime high-water was only 24 samples, so retain >10x
 * safety margin at 256 samples = 1 KiB total.  The old 96,000-sample pair
 * consumed 375 KiB of external BSS. */
#define ADPCM_BufSize      256
#else
#define ADPCM_BufSize      96000
#endif
#define ADPCMMAX           2047
#define ADPCMMIN          -2048
#define FM_IPSCALE         256L

#define INTERPOLATE(y, x)	\
	(((((((-y[0]+3*y[1]-3*y[2]+y[3]) * x + FM_IPSCALE/2) / FM_IPSCALE \
	+ 3 * (y[0]-2*y[1]+y[2])) * x + FM_IPSCALE/2) / FM_IPSCALE \
	- 2*y[0]-3*y[1]+6*y[2]-y[3]) * x + 3*FM_IPSCALE) / (6*FM_IPSCALE) + y[1])

static PX68K_ADHOT int ADPCM_VolumeShift = 65536;
static const int32_t index_shift[8] = {
	-1, -1, -1, -1, 2, 4, 6, 8
};
static const int32_t index_table[58] = {
	0, 0,
	1, 2, 3, 4, 5, 6, 7, 8,
	9, 10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24,
	25, 26,	27,	28, 29, 30,	31, 32,
	33, 34,	35, 36, 37, 38,	39, 40,
	41, 42,	43, 44, 45, 46,	47, 48,
	48, 48, 48, 48, 48, 48,	48, 48
};
/* Build 5.60: exact floor(16 * 1.1^step) results from the original table
 * generator.  Keeping the 49 values removes pow()/floor() from ADPCM init. */
static const uint16_t ADPCM_BaseStep[49] = {
    16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
    494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552
};
static const uint8_t ADPCM_VolumeTable[17] = {
    0,1,1,1,2,2,2,3,4,4,5,6,8,9,11,13,16
};
static const int ADPCM_Clocks[8] = {
	93750, 125000, 187500, 125000, 46875, 62500, 93750, 62500 };

/* Build 5.60: both expressions below used to divide by ADPCM_ClockRate in
 * ADPCM_PreUpdate(), one of the scheduler hot paths.  ADPCM_Clock selects
 * only these eight rates, so preserve the original integer-truncation exactly
 * with precomputed tables and turn each update into two indexed loads. */
static PX68K_ADTABLE const uint16_t ADPCM_PreStep[8] = {
	3906, 5208, 7812, 5208, 1953, 2604, 3906, 2604 };
static PX68K_ADTABLE const uint16_t ADPCM_DifStep[8] = {
	2257, 1693, 1128, 1693, 4515, 3386, 2257, 3386 };

#define ADPCM_SAMPLE_RATE_X12 (44100u * 12u)
#define ADPCM_SAMPLE_RATE_DIV100 (ADPCM_SAMPLE_RATE_X12 / 100u)
/* The 3.1 KiB nibble-difference table is read once per decoded ADPCM nibble.
 * It is hot, but not hot enough to monopolize nearly half of the P4's 8 KiB
 * SPM.  R23 leaves SPM for tiny per-sample/per-instruction state. */
static PX68K_ADTABLE int dif_table[49*16];
/* R23: force the Core1 decode staging FIFO into on-chip SRAM. */
static PX68K_ADBUF int16_t ADPCM_BufR[ADPCM_BufSize];
static PX68K_ADBUF int16_t ADPCM_BufL[ADPCM_BufSize];


static PX68K_ADHOT int32_t  ADPCM_WrPtr = 0;
static PX68K_ADHOT int32_t  ADPCM_RdPtr = 0;
static PX68K_ADHOT uint32_t ADPCM_SampleRate = ADPCM_SAMPLE_RATE_X12;
static PX68K_ADHOT uint32_t ADPCM_ClockRate = 7800*12;
static PX68K_ADHOT uint32_t ADPCM_Count = 0;
static PX68K_ADHOT int ADPCM_Step = 0;
static PX68K_ADHOT int ADPCM_Out = 0;
static PX68K_ADHOT uint8_t ADPCM_Playing = 0;
static uint32_t s_debug_adpcm_control_writes = 0;
static uint32_t s_debug_adpcm_data_writes = 0;
#ifdef ESP_PLATFORM
static PX68K_ADHOT uint32_t s_adpcm_buf_highwater = 0;
static PX68K_ADHOT uint32_t s_adpcm_buf_overflows = 0;
#endif

uint32_t ADPCM_DebugControlWriteCount(void) { return s_debug_adpcm_control_writes; }
uint32_t ADPCM_DebugDataWriteCount(void) { return s_debug_adpcm_data_writes; }
int ADPCM_DebugPlaying(void) { return ADPCM_Playing ? 1 : 0; }
uint32_t ADPCM_Tab5BufferCapacity(void) { return (uint32_t)ADPCM_BufSize; }
uint32_t ADPCM_Tab5BufferHighWater(void)
{
#ifdef ESP_PLATFORM
    return s_adpcm_buf_highwater;
#else
    return 0;
#endif
}
uint32_t ADPCM_Tab5BufferOverflows(void)
{
#ifdef ESP_PLATFORM
    return s_adpcm_buf_overflows;
#else
    return 0;
#endif
}
static PX68K_ADHOT uint8_t ADPCM_Clock = 0;
#ifdef ESP_PLATFORM
PX68K_ADHOT int ADPCM_R127_PreCounter = 0;
PX68K_ADHOT uint16_t ADPCM_R127_PreStepCurrent = 3906u;
#define ADPCM_PreCounter ADPCM_R127_PreCounter

/* X68KTAB_R1A20_EVENT_DEADLINE_CORE
 *
 * The historical ESP fast path did one multiply+add+compare on every
 * scanline, then called ADPCM_PreUpdateR127Due() only when the 10,000,000
 * numerator threshold was reached.  Preserve that exact completed-scanline
 * visibility but reserve the next crossing in 10 MHz guest cycles. */
PX68K_ADHOT volatile uint32_t g_x68p4_adpcm_lazy_pending = 0u;
PX68K_ADHOT volatile uint32_t g_x68p4_adpcm_lazy_deadline = 1u;
static volatile uint32_t s_r1a20_adpcm_materialize;
static volatile uint32_t s_r1a20_adpcm_deadline_hits;
static volatile uint32_t s_r1a20_adpcm_clock_flush;
static volatile uint32_t s_r1a20_adpcm_due_loops;

static inline uint32_t ADPCM_R1A20NextDeadline(void)
{
    const uint32_t step = (uint32_t)ADPCM_R127_PreStepCurrent;
    const int c = ADPCM_PreCounter;
    if (!step || c >= 10000000L) return 1u;
    const uint32_t remain = 10000000u - (uint32_t)c;
    return (remain + step - 1u) / step;
}

static inline void ADPCM_R1A20Rearm(void)
{
    g_x68p4_adpcm_lazy_deadline = ADPCM_R1A20NextDeadline();
}
#else
static PX68K_ADHOT int ADPCM_PreCounter = 0;
#endif
static PX68K_ADHOT int ADPCM_DifBuf = 0;

static PX68K_ADHOT int ADPCM_Pan = 0x00;
static PX68K_ADHOT int OldR = 0, OldL = 0;
static PX68K_ADHOT int Outs[8];
static PX68K_ADHOT int OutsIp[4];
static PX68K_ADHOT int OutsIpR[4];
static PX68K_ADHOT int OutsIpL[4];

int ADPCM_Tab5SpmStateOk(void)
{
#ifdef ESP_PLATFORM
    const void *ptrs[] = {
        &ADPCM_VolumeShift, &ADPCM_WrPtr, &ADPCM_RdPtr, &ADPCM_SampleRate,
        &ADPCM_ClockRate, &ADPCM_Count, &ADPCM_Step, &ADPCM_Out, &ADPCM_Playing,
        &ADPCM_Clock, &ADPCM_PreCounter, &ADPCM_DifBuf, &ADPCM_Pan,
        &OldR, &OldL, Outs, OutsIp, OutsIpR, OutsIpL
    };
    for (unsigned i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); ++i)
        if (!esp_ptr_in_tcm(ptrs[i])) return 0;
    return 1;
#else
    return 0;
#endif
}

int ADPCM_StateAction(StateMem *sm, int load, int data_only)
{
#ifdef ESP_PLATFORM
    if (!load) ADPCM_R1A20Materialize();
    else g_x68p4_adpcm_lazy_pending = 0u;
#endif
	SFORMAT StateRegs[] = 
	{
		/* TODO: Some of the vars might not be necessary */
		SFARRAY16(ADPCM_BufL, ADPCM_BufSize),
		SFARRAY16(ADPCM_BufR, ADPCM_BufSize),

		SFVAR(ADPCM_VolumeShift),
		SFVAR(ADPCM_WrPtr),
		SFVAR(ADPCM_RdPtr),

		SFVAR(ADPCM_ClockRate),
		SFVAR(ADPCM_Count),

		SFVAR(ADPCM_Step),
		SFVAR(ADPCM_Out),
		SFVAR(ADPCM_Playing),
		SFVAR(ADPCM_Clock),
		SFVAR(ADPCM_PreCounter),
		SFVAR(ADPCM_DifBuf),

		SFVAR(ADPCM_Pan),
		SFVAR(OldR),
		SFVAR(OldL),
		SFARRAY32(Outs, 8),
		SFARRAY32(OutsIp, 4),
		SFARRAY32(OutsIpR, 4),
		SFARRAY32(OutsIpL, 4),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_ADPC", false);
#ifdef ESP_PLATFORM
    if (load) {
        ADPCM_R127_PreStepCurrent = ADPCM_PreStep[ADPCM_Clock & 7u];
        ADPCM_R1A20Rearm();
    }
#endif

	return ret;
}


static void ADPCM_InitTable(void)
{
	int step, n;
	static int bit[16][4] =
	{
		{ 1, 0, 0, 0}, { 1, 0, 0, 1}, { 1, 0, 1, 0}, { 1, 0, 1, 1},
		{ 1, 1, 0, 0}, { 1, 1, 0, 1}, { 1, 1, 1, 0}, { 1, 1, 1, 1},
		{-1, 0, 0, 0}, {-1, 0, 0, 1}, {-1, 0, 1, 0}, {-1, 0, 1, 1},
		{-1, 1, 0, 0}, {-1, 1, 0, 1}, {-1, 1, 1, 0}, {-1, 1, 1, 1}
	};

	for (step=0; step<=48; step++)
   {
		const int val = (int)ADPCM_BaseStep[step];
		for (n=0; n<16; n++)
      {
			dif_table[step*16+n] = bit[n][0] *
			   (val   * bit[n][1] +
				 val/2 * bit[n][2] +
				 val/4 * bit[n][3] +
				 val/8);
		}
	}
}

void PX68K_ADIRAM FASTCALL ADPCM_PreUpdateR127Due(void)
{
	const uint8_t ci = (uint8_t)(ADPCM_Clock & 7u);
	while (ADPCM_PreCounter >= 10000000L)
    {
#ifdef ESP_PLATFORM
        (void)0;
#endif
		ADPCM_DifBuf -= (int)ADPCM_DifStep[ci];
		if (ADPCM_DifBuf <= 0)
        {
			ADPCM_DifBuf = 0;
			if (!DMA_Exec3ADPCMFast()) DMA_Exec(3);
		}
		ADPCM_PreCounter -= 10000000L;
	}
}

#ifdef ESP_PLATFORM
void PX68K_ADIRAM FASTCALL ADPCM_R1A20Materialize(void)
{
    const uint32_t clk = g_x68p4_adpcm_lazy_pending;
    g_x68p4_adpcm_lazy_pending = 0u;
    if (!clk) { ADPCM_R1A20Rearm(); return; }

    (void)0;
    ADPCM_PreCounter += (int)((uint32_t)ADPCM_R127_PreStepCurrent * clk);
    if (ADPCM_PreCounter >= 10000000L) {
        (void)0;
        ADPCM_PreUpdateR127Due();
    }
    ADPCM_R1A20Rearm();
}

void ADPCM_R1A20AuditGet(uint32_t out[6])
{
    if (!out) return;
    for (unsigned i=0;i<6u;++i) out[i]=0u;
}
#endif

void PX68K_ADIRAM FASTCALL ADPCM_PreUpdate(uint32_t clock)
{
#ifdef ESP_PLATFORM
    /* Compatibility path: if a legacy executor calls direct PreUpdate while
     * completed standalone lines are pending, preserve ordering first. */
    if (g_x68p4_adpcm_lazy_pending) ADPCM_R1A20Materialize();
    ADPCM_PreCounter += (int)(ADPCM_R127_PreStepCurrent * clock);
#else
	const uint8_t ci = (uint8_t)(ADPCM_Clock & 7u);
    ADPCM_PreCounter += (int)(ADPCM_PreStep[ci] * clock);
#endif
    if (ADPCM_PreCounter >= 10000000L) ADPCM_PreUpdateR127Due();
#ifdef ESP_PLATFORM
    ADPCM_R1A20Rearm();
#endif
}

void ADPCM_Update(int16_t *buffer, size_t length, uint8_t *pbsp, uint8_t *pbep)
{
	int outs;
	int32_t outl, outr;

	if ( length<=0 ) return;

   if ( Config.Sound_LPF )
   {
      while ( length )
      {
         int tmpl, tmpr;
         if (buffer >= (int16_t *)pbep)
            buffer = (int16_t*)pbsp;

         if ( (ADPCM_WrPtr==ADPCM_RdPtr)&&(!(DMA[3].CCR&0x40)) )
            if (!DMA_Exec3ADPCMFast()) DMA_Exec(3);
         if ( ADPCM_WrPtr!=ADPCM_RdPtr )
         {
            OldR = outr = ADPCM_BufL[ADPCM_RdPtr];
            OldL = outl = ADPCM_BufR[ADPCM_RdPtr];
            ADPCM_RdPtr++;
            if ( ADPCM_RdPtr>=ADPCM_BufSize )
               ADPCM_RdPtr = 0;
         }
         else
         {
            outr = OldR;
            outl = OldL;
         }

         outr       = (int)(outr*40*ADPCM_VolumeShift);
         outs       = (outr + Outs[3]*2 + Outs[2] + Outs[1]*157 - Outs[0]*61) >> 8;
         Outs[2]    = Outs[3];
         Outs[3]    = outr;
         Outs[0]    = Outs[1];
         Outs[1]    = outs;

         OutsIpR[0] = OutsIpR[1];
         OutsIpR[1] = OutsIpR[2];
         OutsIpR[2] = OutsIpR[3];
         OutsIpR[3] = outs;

         outl       = (int)(outl*40*ADPCM_VolumeShift);
         outs       = (outl + Outs[7]*2 + Outs[6] + Outs[5]*157 - Outs[4]*61) >> 8;
         Outs[6]    = Outs[7];
         Outs[7]    = outl;
         Outs[4]    = Outs[5];
         Outs[5]    = outs;

         OutsIpL[0] = OutsIpL[1];
         OutsIpL[1] = OutsIpL[2];
         OutsIpL[2] = OutsIpL[3];
         OutsIpL[3] = outs;

         tmpr = INTERPOLATE(OutsIpR, 0);
         if ( tmpr>32767 )
            tmpr = 32767;
         else if ( tmpr<(-32768) )
            tmpr = -32768;
         *(buffer++) = (int16_t)tmpr;
         tmpl = INTERPOLATE(OutsIpL, 0);
         if ( tmpl>32767 )
            tmpl = 32767;
         else if ( tmpl<(-32768) )
            tmpl = -32768;
         *(buffer++) = (int16_t)tmpl;
         length--;
      }
   }
   else
   {
      while ( length )
      {
         int tmpl, tmpr;
         if (buffer >= (int16_t *)pbep)
            buffer = (int16_t*)pbsp;

         if ( (ADPCM_WrPtr==ADPCM_RdPtr)&&(!(DMA[3].CCR&0x40)) )
            if (!DMA_Exec3ADPCMFast()) DMA_Exec(3);
         if ( ADPCM_WrPtr!=ADPCM_RdPtr )
         {
            OldR = outr = ADPCM_BufL[ADPCM_RdPtr];
            OldL = outl = ADPCM_BufR[ADPCM_RdPtr];
            ADPCM_RdPtr++;
            if ( ADPCM_RdPtr>=ADPCM_BufSize )
               ADPCM_RdPtr = 0;
         }
         else
         {
            outr = OldR;
            outl = OldL;
         }

         outs       = (int)(outr*ADPCM_VolumeShift);

         OutsIpR[0] = OutsIpR[1];
         OutsIpR[1] = OutsIpR[2];
         OutsIpR[2] = OutsIpR[3];
         OutsIpR[3] = outs;

         outs       = (int)(outl*ADPCM_VolumeShift);

         OutsIpL[0] = OutsIpL[1];
         OutsIpL[1] = OutsIpL[2];
         OutsIpL[2] = OutsIpL[3];
         OutsIpL[3] = outs;

         tmpr = INTERPOLATE(OutsIpR, 0);
         if ( tmpr>32767 )
            tmpr = 32767;
         else if ( tmpr<(-32768) )
            tmpr = -32768;
         *(buffer++) = (int16_t)tmpr;
         tmpl = INTERPOLATE(OutsIpL, 0);
         if ( tmpl>32767 )
            tmpl = 32767;
         else if ( tmpl<(-32768) )
            tmpl = -32768;
         *(buffer++) = (int16_t)tmpl;
         length--;
      }
   }

	ADPCM_DifBuf = ADPCM_WrPtr-ADPCM_RdPtr;
	if ( ADPCM_DifBuf<0 )
      ADPCM_DifBuf += ADPCM_BufSize;
}

static INLINE int adpcm_interp_coeff_scalar(int A, int B, int C, int y1, int x)
{
    int t = (A * x + FM_IPSCALE/2) / FM_IPSCALE + B;
    t = (t * x + FM_IPSCALE/2) / FM_IPSCALE + C;
    return (t * x + 3*FM_IPSCALE) / (6*FM_IPSCALE) + y1;
}

static INLINE int adpcm_clip12(int v)
{
    if (v > ADPCMMAX) return ADPCMMAX;
    if (v < ADPCMMIN) return ADPCMMIN;
    return v;
}

static INLINE void adpcm598c_store_sample(int tmp)
{
    int32_t next = ADPCM_WrPtr + 1;
    if (next >= ADPCM_BufSize) next = 0;
#ifdef ESP_PLATFORM
    /* A full FIFO must never become indistinguishable from empty.  The old
     * 96k buffer relied on "this never fills".  Keep that assumption visible
     * after shrinking it: preserve queued samples, drop only the impossible
     * newest sample, and count it. */
    if (next == ADPCM_RdPtr) {
        ++s_adpcm_buf_overflows;
        return;
    }
#endif
    tmp = adpcm_clip12(tmp);
    if (!(ADPCM_Pan & 1)) ADPCM_BufR[ADPCM_WrPtr] = (int16_t)tmp;
    else                  ADPCM_BufR[ADPCM_WrPtr] = 0;
    if (!(ADPCM_Pan & 2)) ADPCM_BufL[ADPCM_WrPtr] = (int16_t)tmp;
    else                  ADPCM_BufL[ADPCM_WrPtr] = 0;
    ADPCM_WrPtr = next;
#ifdef ESP_PLATFORM
    int32_t used = ADPCM_WrPtr - ADPCM_RdPtr;
    if (used < 0) used += ADPCM_BufSize;
    if ((uint32_t)used > s_adpcm_buf_highwater)
        s_adpcm_buf_highwater = (uint32_t)used;
#endif
}


#ifdef ESP_PLATFORM
/* R57E107X_CPU0_ADPCM_COMMAND_ENGINE
 * CPU1 no longer computes MSM6258 sample values.  Keep only the historical
 * ADPCM ring occupancy shadow because DMA3 pacing is guest-visible.  This is
 * the proven h13 timing split, now feeding the HP-CPU0 command renderer. */
static INLINE void adpcm_cpu0_timing_write_one(void)
{
    int nr = 0;
    while (ADPCM_SampleRate > ADPCM_Count)
    {
        if (ADPCM_Playing) ++nr;
        ADPCM_Count += ADPCM_ClockRate;
    }
    ADPCM_Count -= ADPCM_SampleRate;
    if (nr > 0)
    {
        ADPCM_WrPtr += nr;
        while (ADPCM_WrPtr >= ADPCM_BufSize) ADPCM_WrPtr -= ADPCM_BufSize;
    }
}

void ADPCM_CPU0_TimingConsume(size_t length)
{
    while (length)
    {
        int avail = ADPCM_WrPtr - ADPCM_RdPtr;
        if (avail < 0) avail += ADPCM_BufSize;
        if (avail > 0)
        {
            size_t n = length;
            if (n > (size_t)avail) n = (size_t)avail;
            ADPCM_RdPtr += (int32_t)n;
            while (ADPCM_RdPtr >= ADPCM_BufSize) ADPCM_RdPtr -= ADPCM_BufSize;
            length -= n;
            continue;
        }
        if (!(DMA[3].CCR & 0x40))
        {
            const int32_t before = ADPCM_WrPtr;
            DMA_Exec(3);
            if (ADPCM_WrPtr != before) continue;
        }
        /* Match legacy ADPCM_Update: when empty, hold the previous sample. */
        break;
    }
    ADPCM_DifBuf = ADPCM_WrPtr - ADPCM_RdPtr;
    if (ADPCM_DifBuf < 0) ADPCM_DifBuf += ADPCM_BufSize;
}
#else
void ADPCM_CPU0_TimingConsume(size_t length) { (void)length; }
#endif

static INLINE void ADPCM_WriteOne(uint8_t val)
{
    ADPCM_Out += dif_table[(ADPCM_Step << 4) + val];
    if (ADPCM_Out > ADPCMMAX) ADPCM_Out = ADPCMMAX;
    else if (ADPCM_Out < ADPCMMIN) ADPCM_Out = ADPCMMIN;

    ADPCM_Step += index_shift[val & 0x07];
    ADPCM_Step = index_table[ADPCM_Step + 1];

    if (OutsIp[0] == -1) {
        OutsIp[0] = OutsIp[1] = OutsIp[2] = OutsIp[3] = ADPCM_Out;
    } else {
        OutsIp[0] = OutsIp[1];
        OutsIp[1] = OutsIp[2];
        OutsIp[2] = OutsIp[3];
        OutsIp[3] = ADPCM_Out;
    }

    /* Build 5.98c: determine all 44.1kHz fractional positions first.  The
     * original loop performed the same Count increments interleaved with the
     * sample writes; no guest-visible state can change inside this function. */
    int16_t ratios[16] __attribute__((aligned(16)));
    int nr = 0;
    while (ADPCM_SampleRate > ADPCM_Count) {
        if (ADPCM_Playing && nr < (int)(sizeof(ratios)/sizeof(ratios[0]))) {
            ratios[nr++] = (int16_t)(((ADPCM_Count / 100u) * FM_IPSCALE) /
                                     ADPCM_SAMPLE_RATE_DIV100);
        }
        ADPCM_Count += ADPCM_ClockRate;
    }
    ADPCM_Count -= ADPCM_SampleRate;

    if (!ADPCM_Playing || nr == 0)
        return;

    /* Hoist cubic coefficients once per decoded nibble.  5.98a expanded the
     * INTERPOLATE macro for every output sample. */
    const int y0 = OutsIp[0], y1 = OutsIp[1], y2 = OutsIp[2], y3 = OutsIp[3];
    const int A = -y0 + 3*y1 - 3*y2 + y3;
    const int B = 3*(y0 - 2*y1 + y2);
    const int C = -2*y0 - 3*y1 + 6*y2 - y3;

    for (int i=0; i<nr; ++i) {
        const int tmp = adpcm_interp_coeff_scalar(A, B, C, y1, ratios[i]);
        adpcm598c_store_sample(tmp);
    }
}

/* R140P4S3 absolute semantic timeline.
 * Every audible MSM6258 mutation, INCLUDING each data byte, is stamped with
 * CPU1's authoritative absolute guest sample tick.  CPU0 advances FM+ADPCM
 * together to that tick before applying the event.  The public CPU0 command
 * engine remains available as a fallback when the timeline is not active. */
#ifdef ESP_PLATFORM
extern int DSound_AbsTimelineActive(void);
extern int DSound_AbsTimelineADPCMControl(uint8_t data);
extern int DSound_AbsTimelineADPCMData(uint8_t data);
extern int DSound_AbsTimelineADPCMPan(uint8_t data);
extern int DSound_AbsTimelineADPCMClock(uint8_t data);
extern int DSound_AbsTimelineADPCMVolume(uint8_t data);
#endif

void PX68K_ADIRAM FASTCALL ADPCM_Write(uint32_t adr, uint8_t data)
{
	if ( adr==0xe92001 )
   {
      ++s_debug_adpcm_control_writes;
      if ( data&1 )
         ADPCM_Playing = 0;
      else if ( data&2 )
      {
         if ( !ADPCM_Playing )
         {
            ADPCM_Step    = 0;
            ADPCM_Out     = 0;
            OldL = OldR   = -2;
            ADPCM_Playing = 1;
         }
         OutsIp[0] = OutsIp[1] = OutsIp[2] = OutsIp[3] = -1;
      }
   }
   else if ( adr==0xe92003 )
   {
      ++s_debug_adpcm_data_writes;
		if ( ADPCM_Playing )
      {
         #ifdef ESP_PLATFORM
         if (ADPCM_CPU0_Active())
         {
            adpcm_cpu0_timing_write_one();
            adpcm_cpu0_timing_write_one();
            if (!DSound_AbsTimelineActive()) ADPCM_CPU0_Data(data);
            else (void)DSound_AbsTimelineADPCMData(data);
         }
         else
#endif
         {
            ADPCM_WriteOne((uint8_t)(data & 15));
            ADPCM_WriteOne((uint8_t)((data >> 4) & 15));
         }
      }
	}
#ifdef ESP_PLATFORM
   if (adr == 0xe92001 && ADPCM_CPU0_Active())
   {
      if (!DSound_AbsTimelineActive()) ADPCM_CPU0_Control(data);
      else (void)DSound_AbsTimelineADPCMControl(data);
   }
#endif
}

uint8_t FASTCALL ADPCM_Read(uint32_t adr)
{
	if ( adr==0xe92001 )
		return ((ADPCM_Playing)?0xc0:0x40);
	return 0x00;
}

void ADPCM_SetVolume(uint8_t vol)
{
	if ( vol>16 ) vol=16;

	ADPCM_VolumeShift = (int)ADPCM_VolumeTable[vol];
#ifdef ESP_PLATFORM
   if (ADPCM_CPU0_Active()) { if (!DSound_AbsTimelineActive()) ADPCM_CPU0_SetVolume(vol); else (void)DSound_AbsTimelineADPCMVolume(vol); }
#endif
}

void ADPCM_SetPan(int n)
{
	if ( (ADPCM_Pan&0x0c)!=(n&0x0c) )
   {
#ifdef ESP_PLATFORM
        if (g_x68p4_adpcm_lazy_pending) { (void)0; ADPCM_R1A20Materialize(); }
#endif
		ADPCM_Count     = 0;
		ADPCM_Clock     = (ADPCM_Clock&4)|((n>>2)&3);
		ADPCM_ClockRate = ADPCM_Clocks[ADPCM_Clock];
#ifdef ESP_PLATFORM
        ADPCM_R127_PreStepCurrent = ADPCM_PreStep[ADPCM_Clock & 7u];
        ADPCM_R1A20Rearm();
#endif
	}
	ADPCM_Pan = n;
#ifdef ESP_PLATFORM
   if (ADPCM_CPU0_Active()) { if (!DSound_AbsTimelineActive()) ADPCM_CPU0_SetPan((uint8_t)n); else (void)DSound_AbsTimelineADPCMPan((uint8_t)n); }
#endif
}

void ADPCM_SetClock(int n)
{
	if ( (ADPCM_Clock&4)!=n )
   {
#ifdef ESP_PLATFORM
        if (g_x68p4_adpcm_lazy_pending) { (void)0; ADPCM_R1A20Materialize(); }
#endif
		ADPCM_Count     = 0;
		ADPCM_Clock     = n | ((ADPCM_Pan>>2)&3);
		ADPCM_ClockRate = ADPCM_Clocks[ADPCM_Clock];
#ifdef ESP_PLATFORM
        ADPCM_R127_PreStepCurrent = ADPCM_PreStep[ADPCM_Clock & 7u];
        ADPCM_R1A20Rearm();
#endif
	}
#ifdef ESP_PLATFORM
   if (ADPCM_CPU0_Active()) { if (!DSound_AbsTimelineActive()) ADPCM_CPU0_SetClock((uint8_t)n); else (void)DSound_AbsTimelineADPCMClock((uint8_t)n); }
#endif
}

void ADPCM_Init(void)
{
	ADPCM_WrPtr      = 0;
	ADPCM_RdPtr      = 0;
	ADPCM_Out        = 0;
	ADPCM_Step       = 0;
	ADPCM_Playing    = 0;
	ADPCM_SampleRate = ADPCM_SAMPLE_RATE_X12;
	ADPCM_PreCounter = 0;
#ifdef ESP_PLATFORM
    g_x68p4_adpcm_lazy_pending = 0u;
    s_r1a20_adpcm_materialize = 0u;
    s_r1a20_adpcm_deadline_hits = 0u;
    s_r1a20_adpcm_clock_flush = 0u;
    s_r1a20_adpcm_due_loops = 0u;
    s_adpcm_buf_highwater = 0;
    s_adpcm_buf_overflows = 0;
#endif
	memset(Outs, 0, sizeof(Outs));
	OutsIp[0]  = OutsIp[1]  = OutsIp[2]  = OutsIp[3]  = -1;
	OutsIpR[0] = OutsIpR[1] = OutsIpR[2] = OutsIpR[3] = 0;
	OutsIpL[0] = OutsIpL[1] = OutsIpL[2] = OutsIpL[3] = 0;
	OldL = OldR = 0;

	ADPCM_SetPan(0x0b);
#ifdef ESP_PLATFORM
    ADPCM_R1A20Rearm();
#endif
	ADPCM_InitTable();
#ifdef ESP_PLATFORM
    printf("PX68K_ADSRAM_R24: Core1 ADPCM decode staging INTERNAL bytes=%u frames=%u (~%ums) highwater=0 overflow=0; R23 measured max=24, capacity reduced 4096->256\n",
           (unsigned)(ADPCM_BufSize * 2u * sizeof(int16_t)),
           (unsigned)ADPCM_BufSize,
           (unsigned)((ADPCM_BufSize * 1000u) / 44100u));
#endif
#ifdef ESP_PLATFORM
   /* CPU0 owns waveform generation only. CPU1 retains DMA/IRQ/register time. */
   if (ADPCM_CPU0_Init())
   {
      if (!DSound_AbsTimelineActive())
      {
         ADPCM_CPU0_SetPan((uint8_t)ADPCM_Pan);
         ADPCM_CPU0_SetClock((uint8_t)(ADPCM_Clock & 4u));
         ADPCM_CPU0_SetVolume((uint8_t)Config.PCM_VOL);
      }
      else
      {
         (void)DSound_AbsTimelineADPCMPan((uint8_t)ADPCM_Pan);
         (void)DSound_AbsTimelineADPCMClock((uint8_t)(ADPCM_Clock & 4u));
         (void)DSound_AbsTimelineADPCMVolume((uint8_t)Config.PCM_VOL);
      }
   }
#endif
}
