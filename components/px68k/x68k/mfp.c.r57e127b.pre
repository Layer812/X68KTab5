/*
 *  MFP.C - MFP (Multi-Function Peripheral)
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Cache safe MFP timer calculations and support scheduler-bounded polling acceleration without changing guest timer/IRQ semantics.
 * Layer8 Aug/17/2026
 */
#include "mfp.h"
#include "irqh.h"
#include "crtc.h"
#include "m68000.h"
#include "winx68k.h"
#include "keyboard.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#ifdef TCM_DRAM_ATTR
#define PX68K_DEVHOT TCM_DRAM_ATTR
#define PX68K_DEVCACHE DRAM_ATTR
#define PX68K_DEVIRAM IRAM_ATTR
#else
#define PX68K_DEVHOT DRAM_ATTR
#define PX68K_DEVCACHE DRAM_ATTR
#define PX68K_DEVIRAM IRAM_ATTR
#endif
#else
#define PX68K_DEVHOT
#define PX68K_DEVCACHE
#define PX68K_DEVIRAM
#endif

PX68K_DEVHOT uint8_t LastKey = 0;

/* Tab5: observe the final guest-visible stage of keyboard delivery. */
static uint32_t s_dbg_udr_reads;
static uint32_t s_dbg_rsr_reads;
static uint32_t s_dbg_kbd_irq_calls;
static uint32_t s_dbg_kbd_irq_enabled;
static uint32_t s_dbg_kbd_irq_disabled;
static uint8_t  s_dbg_last_udr;

PX68K_DEVHOT uint8_t MFP[24];
static PX68K_DEVHOT uint8_t Timer_Reload[4] = {0, 0, 0, 0};
static PX68K_DEVHOT int32_t Timer_Tick[4]   = {0, 0, 0, 0};
static const int Timer_Prescaler[8] = {1, 10, 25, 40, 125, 160, 250, 500};

/* Build 5.68: timer control changes only on MFP control-register writes,
 * reset/init, or state load.  Cache the four prescaler selectors and an
 * active-channel mask instead of decoding all control nibbles every scheduler
 * slice.  Exactly as in 5.63, Timer A is excluded from this ordinary-timer
 * path whenever TACR bit 3 is set; TACR==8 remains handled by MFP_TimerA(). */
static PX68K_DEVCACHE uint8_t s_timer_prescale_sel[4] = {0, 0, 0, 0};
PX68K_DEVCACHE uint8_t MFP_TimerActiveMask = 0;
PX68K_DEVCACHE uint8_t MFP_TimerFastMode = 0;
#ifdef ESP_PLATFORM
static DRAM_ATTR uint32_t s_mfp_exact_bc_calls = 0;
static DRAM_ATTR uint32_t s_mfp_fallback_calls = 0;

/* R26: R25 measured the steady-state exact timer tuple.  Production keeps
 * only hit/fallback counters; no per-call trace bookkeeping remains. */
static DRAM_ATTR uint8_t s_mfp_exact_bc_logged = 0;
#endif

static inline void mfp_refresh_timer_cache(void)
{
   s_timer_prescale_sel[0] = (uint8_t)(MFP[MFP_TACR] & 7);
   s_timer_prescale_sel[1] = (uint8_t)(MFP[MFP_TBCR] & 7);
   s_timer_prescale_sel[2] = (uint8_t)((MFP[MFP_TCDCR] >> 4) & 7);
   s_timer_prescale_sel[3] = (uint8_t)(MFP[MFP_TCDCR] & 7);
   MFP_TimerActiveMask = 0;

   /* Preserve the exact 5.63 semantics.  Timer A is excluded from the
    * ordinary timer path whenever TACR bit 3 is set, not only when TACR==8.
    * This matters for values 9..15: 5.64 accidentally stripped bit 3 before
    * forming the active mask and therefore treated them as ordinary timers. */
   if (s_timer_prescale_sel[0] && !(MFP[MFP_TACR] & 8u))
      MFP_TimerActiveMask |= 1u << 0;
   for (int i = 1; i < 4; ++i)
      if (s_timer_prescale_sel[i]) MFP_TimerActiveMask |= (uint8_t)(1u << i);

#ifdef ESP_PLATFORM
   /* R57E71: this exact tuple dominates the MDX workload.  Compute the
    * classification only when the guest changes timer programming. */
   MFP_TimerFastMode = (uint8_t)(
      MFP_TimerActiveMask == 0x06u &&
      MFP[MFP_TACR] == 0x08u &&
      MFP[MFP_TBCR] == 0x01u &&
      MFP[MFP_TCDCR] == 0x70u &&
      Timer_Reload[1] == 13u &&
      Timer_Reload[2] == 200u);
#else
   MFP_TimerFastMode = 0;
#endif
}

/* Build 5.60: GetGPIP() used to redo two variable divisions on every read.
 * These values change only when the CRTC timing registers/HSYNC clock change.
 * Keep the cache in ordinary internal DRAM, not the scarce 8 KiB TCM. */
static PX68K_DEVCACHE int s_gpip_cache_hsync = -1;
static PX68K_DEVCACHE uint8_t s_gpip_cache_hdisp = 0xff;
static PX68K_DEVCACHE uint8_t s_gpip_cache_hs = 0xff;
static PX68K_DEVCACHE uint8_t s_gpip_cache_he = 0xff;
static PX68K_DEVCACHE int s_gpip_hstart = 0;
static PX68K_DEVCACHE int s_gpip_hend = 0;

static inline void mfp_refresh_gpip_geometry(void)
{
   const int hsync = HSYNC_CLK;
   const uint8_t hdisp = CRTC_Regs[0x01];
   const uint8_t hs = CRTC_Regs[0x05];
   const uint8_t he = CRTC_Regs[0x07];
   if (hsync == s_gpip_cache_hsync && hdisp == s_gpip_cache_hdisp &&
       hs == s_gpip_cache_hs && he == s_gpip_cache_he)
      return;

   s_gpip_cache_hsync = hsync;
   s_gpip_cache_hdisp = hdisp;
   s_gpip_cache_hs = hs;
   s_gpip_cache_he = he;
   if (hdisp) {
      s_gpip_hstart = (int)hs * hsync / (int)hdisp;
      s_gpip_hend   = (int)he * hsync / (int)hdisp;
   } else {
      /* Original valid X68000 modes do not program zero here.  Avoid an
       * accidental host divide-by-zero while keeping a benign sync window. */
      s_gpip_hstart = 0;
      s_gpip_hend = 0;
   }
}

/* Turn the seven runtime prescaler divisors into constant divisors.  GCC can
 * lower these to multiply/shift sequences instead of a variable DIV. */
static inline uint32_t mfp_prescale_quotient(uint32_t accum, uint8_t sel)
{
   switch (sel) {
      case 1: return accum / 10u;
      case 2: return accum / 25u;
      case 3: return accum / 40u;
      case 4: return accum / 125u;
      case 5: return accum / 160u;
      case 6: return accum / 250u;
      case 7: return accum / 500u;
      default: return 0u;
   }
}

int MFP_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFARRAY(MFP, 24),
	   SFVAR(LastKey),
	   SFARRAY(Timer_Reload, 4),
	   SFARRAY32(Timer_Tick, 4),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_MFP", false);
	if (load)
		mfp_refresh_timer_cache();

	return ret;
}


uint32_t FASTCALL MFP_IntCallback(uint8_t irq)
{
   uint8_t flag;
   uint32_t vect;
   int offset = 0;
   IRQH_IRQCallBack(irq);
   if (irq!=6) return (uint32_t)-1;
   for (flag=0x80, vect=15; flag; flag>>=1, vect--)
   {
      if ((MFP[MFP_IPRA]&flag)&&(MFP[MFP_IMRA]&flag)&&(!(MFP[MFP_ISRA]&flag)))
         break;
   }
   if (!flag)
   {
      offset = 1;
      for (flag=0x80, vect=7; flag; flag>>=1, vect--)
      {
         if ((MFP[MFP_IPRB]&flag)&&(MFP[MFP_IMRB]&flag)&&(!(MFP[MFP_ISRB]&flag)))
            break;
      }
   }
   if (!flag)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "[PX68K] Error: MFP Int w/o Request. Default Vector(-1) has been returned.\n");
      return (uint32_t)-1;
   }

   MFP[MFP_IPRA+offset] &= (~flag);
   if (MFP[MFP_VR]&8)
      MFP[MFP_ISRA+offset] |= flag;
   vect |= (MFP[MFP_VR]&0xf0);
   for (flag=0x80; flag; flag>>=1)
   {
      if ((MFP[MFP_IPRA]&flag)&&(MFP[MFP_IMRA]&flag)&&(!(MFP[MFP_ISRA]&flag)))
      {
         IRQH_Int(6, &MFP_IntCallback);
         break;
      }
      if ((MFP[MFP_IPRB]&flag)&&(MFP[MFP_IMRB]&flag)&&(!(MFP[MFP_ISRB]&flag)))
      {
         IRQH_Int(6, &MFP_IntCallback);
         break;
      }
   }
   return vect;
}

void MFP_RecheckInt(void)
{
   uint8_t flag;
   IRQH_IRQCallBack(6);
   for (flag=0x80; flag; flag>>=1)
   {
      if ((MFP[MFP_IPRA]&flag)&&(MFP[MFP_IMRA]&flag)&&(!(MFP[MFP_ISRA]&flag)))
      {
         IRQH_Int(6, &MFP_IntCallback);
         break;
      }
      if ((MFP[MFP_IPRB]&flag)&&(MFP[MFP_IMRB]&flag)&&(!(MFP[MFP_ISRB]&flag)))
      {
         IRQH_Int(6, &MFP_IntCallback);
         break;
      }
   }
}

void MFP_Int(int irq) 
{
	const int original_irq = irq;
	uint8_t flag = 0x80;
	if (original_irq == 3)
		++s_dbg_kbd_irq_calls;
	if (irq<8)
	{
		flag >>= irq;
		if (MFP[MFP_IERA]&flag)
		{
			if (original_irq == 3)
				++s_dbg_kbd_irq_enabled;
			MFP[MFP_IPRA] |= flag;
			if ((MFP[MFP_IMRA]&flag)&&(!(MFP[MFP_ISRA]&flag)))
			{
				IRQH_Int(6, &MFP_IntCallback);
			}
		}
		else if (original_irq == 3)
		{
			++s_dbg_kbd_irq_disabled;
		}
	}
	else
	{
		irq -= 8;
		flag >>= irq;
		if (MFP[MFP_IERB]&flag)
		{
			MFP[MFP_IPRB] |= flag;
			if ((MFP[MFP_IMRB]&flag)&&(!(MFP[MFP_ISRB]&flag)))
			{
				IRQH_Int(6, &MFP_IntCallback);
			}
		}
	}
}

void MFP_Init(void)
{
	int i;
	static const uint8_t initregs[24] = {
		0x7b, 0x06, 0x00, 0x18, 0x3e, 0x00, 0x00, 0x00,
		0x00, 0x18, 0x3e, 0x40, 0x08, 0x01, 0x77, 0x01,
		0x0d, 0xc8, 0x14, 0x00, 0x88, 0x01, 0x81, 0x00
	};
	memcpy(MFP, initregs, 24);
	s_dbg_udr_reads = 0;
	s_dbg_rsr_reads = 0;
	s_dbg_kbd_irq_calls = 0;
	s_dbg_kbd_irq_enabled = 0;
	s_dbg_kbd_irq_disabled = 0;
	s_dbg_last_udr = 0;
	for (i=0; i<4; i++)
      Timer_Tick[i] = 0;
	mfp_refresh_timer_cache();
}

static uint8_t GetGPIP(void)
{
	uint8_t ret = 0x20; /* bit 5 is always 1 */
	mfp_refresh_gpip_geometry();
	int hpos    = (int)(ICount % HSYNC_CLK);

	if ((vline >= CRTC_VSTART) && (vline < CRTC_VEND))
		ret     |= 0x13;
	else
		ret     |= 0x03;

	if ((hpos >= s_gpip_hstart) && (hpos < s_gpip_hend))
		ret     &= 0x7f;
	else
		ret     |= 0x80;

	if (vline != CRTC_IntLine)
		ret     |= 0x40;
	return ret;
}

uint8_t FASTCALL MFP_Read(uint32_t adr)
{
   if (adr > 0xe8802f)
      return 0;

   if (adr&1)
   {
      uint8_t reg = (uint8_t)((adr&0x3f)>>1);

      switch(reg)
      {
         case MFP_GPIP:
            return GetGPIP();
         case MFP_UDR:
               ++s_dbg_udr_reads;
               s_dbg_last_udr = LastKey;
               KeyIntFlag = 0;
               return LastKey;
         case MFP_RSR:
               ++s_dbg_rsr_reads;
               if (KeyBufRP != KeyBufWP)
                  return MFP[reg] & 0x7f;
               return MFP[reg] | 0x80;
         default:
               break;
      }
      return MFP[reg];
   }
   return 0xff;
}

uint32_t MFP_DebugUDRReads(void)             { return s_dbg_udr_reads; }
uint32_t MFP_DebugRSRReads(void)             { return s_dbg_rsr_reads; }
uint32_t MFP_DebugKeyboardIRQCalls(void)     { return s_dbg_kbd_irq_calls; }
uint32_t MFP_DebugKeyboardIRQEnabled(void)   { return s_dbg_kbd_irq_enabled; }
uint32_t MFP_DebugKeyboardIRQDisabled(void)  { return s_dbg_kbd_irq_disabled; }
uint8_t  MFP_DebugLastUDR(void)              { return s_dbg_last_udr; }

void FASTCALL MFP_Write(uint32_t adr, uint8_t data)
{
   if (adr>0xe8802f)
      return;

   if ((adr & 1) != 0)
   {
      uint8_t reg = (uint8_t)((adr&0x3f)>>1);

      switch(reg)
      {
         case MFP_IERA:
         case MFP_IERB:
            MFP[reg]    = data;
            MFP[reg+2] &= data;  /* Prohibited items drop IPRA/B */
            MFP_RecheckInt();
            break;
         case MFP_IPRA:
         case MFP_IPRB:
         case MFP_ISRA:
         case MFP_ISRB:
            MFP[reg] &= data;
            MFP_RecheckInt();
            break;
         case MFP_IMRA:
         case MFP_IMRB:
            MFP[reg] = data;
            MFP_RecheckInt();
            break;
         case MFP_TADR:
            Timer_Reload[0] = MFP[reg] = data;
            mfp_refresh_timer_cache();
            break;
         case MFP_TBDR:
            Timer_Reload[1] = MFP[reg] = data;
            mfp_refresh_timer_cache();
            break;
         case MFP_TCDR:
            Timer_Reload[2] = MFP[reg] = data;
            mfp_refresh_timer_cache();
            break;
         case MFP_TDDR:
            Timer_Reload[3] = MFP[reg] = data;
            mfp_refresh_timer_cache();
            break;
         case MFP_TSR:
            MFP[reg] = data | 0x80; /* Tx is always enabled */
            break;
         case MFP_UDR:
            break;
         case MFP_TACR:
         case MFP_TBCR:
         case MFP_TCDCR:
            MFP[reg] = data;
            mfp_refresh_timer_cache();
            break;
         default:
            MFP[reg] = data;
            break;
      }
   }
}

/* Build 5.38: advance an MFP countdown in O(1).
 *
 * The old code decremented the 8-bit timer once per prescaled tick.  Timer B
 * commonly runs with prescaler=1, so one 200-cycle CPU scheduler slice could
 * execute the loop roughly 200 times.  No guest CPU executes between those
 * decrements: MFP_Timer() itself is called only after m68k_execute() returns.
 * Therefore multiple underflows inside the same scheduler slice are
 * guest-equivalent to one pending interrupt plus the mathematically correct
 * final counter value.
 *
 * A register value of zero means 256 decrements until the next underflow,
 * matching uint8_t wraparound in the original loop. */
static inline uint8_t mfp_timer_advance_fast(uint8_t cur, uint8_t reload,
                                              uint32_t decs, int *fired)
{
   if (!decs)
      return cur;

   uint32_t until = cur ? (uint32_t)cur : 256U;
   if (decs < until)
      return (uint8_t)((uint32_t)cur - decs);

   decs -= until;
   *fired = 1;

   if (!decs)
      return reload;

   const uint32_t period = reload ? (uint32_t)reload : 256U;
   /* Most scheduler slices do not span a second complete timer period.
    * Avoid the variable remainder instruction in that common case. */
   if (decs < period)
      return (uint8_t)((uint32_t)reload - decs);

   const uint32_t rem = decs % period;
   if (!rem)
      return reload;

   return (uint8_t)((uint32_t)reload - rem);
}

/* R57E71 fixed IRQ paths for the measured MDX timer tuple.  These are exact
 * specializations of MFP_Int(7) and MFP_Int(10): no priority/enable semantics
 * are changed, only the runtime irq-number decode and shifts are removed. */
static inline __attribute__((always_inline)) void mfp_r71_timerb_irq(void)
{
   const uint8_t flag = 0x01u; /* IRQ7 -> IERA/IPRA/IMRA/ISRA bit0 */
   if (MFP[MFP_IERA] & flag) {
      MFP[MFP_IPRA] |= flag;
      if ((MFP[MFP_IMRA] & flag) && !(MFP[MFP_ISRA] & flag))
         IRQH_Int(6, &MFP_IntCallback);
   }
}

static inline __attribute__((always_inline)) void mfp_r71_timerc_irq(void)
{
   const uint8_t flag = 0x20u; /* IRQ10 -> IERB/IPRB/IMRB/ISRB bit5 */
   if (MFP[MFP_IERB] & flag) {
      MFP[MFP_IPRB] |= flag;
      if ((MFP[MFP_IMRB] & flag) && !(MFP[MFP_ISRB] & flag))
         IRQH_Int(6, &MFP_IntCallback);
   }
}

void PX68K_DEVIRAM __attribute__((hot,optimize("O3"))) FASTCALL MFP_TimerR71ExactBC(int32_t clock)
{
#ifdef ESP_PLATFORM
   ++s_mfp_exact_bc_calls;
   if (__builtin_expect(!s_mfp_exact_bc_logged, 0)) {
      s_mfp_exact_bc_logged = 1u;
      printf("PX68K_MFP_R57E71: cached exact B/C hot path ACTIVE B=/10 reload=13 C=/500 reload=200; fixed IRQ decode\n");
   }

   int32_t accum = Timer_Tick[1] + clock;
   if (accum >= 10) {
      const uint32_t decs = (uint32_t)accum / 10u;
      Timer_Tick[1] = accum - (int32_t)(decs * 10u);
      int fired = 0;
      MFP[MFP_TBDR] = mfp_timer_advance_fast(MFP[MFP_TBDR], 13u, decs, &fired);
      if (fired) mfp_r71_timerb_irq();
   } else {
      Timer_Tick[1] = accum;
   }

   accum = Timer_Tick[2] + clock;
   if (accum >= 500) {
      const uint32_t decs = (uint32_t)accum / 500u;
      Timer_Tick[2] = accum - (int32_t)(decs * 500u);
      int fired = 0;
      MFP[MFP_TCDR] = mfp_timer_advance_fast(MFP[MFP_TCDR], 200u, decs, &fired);
      if (fired) mfp_r71_timerc_irq();
   } else {
      Timer_Tick[2] = accum;
   }
#else
   MFP_TimerSlow(clock);
#endif
}

void PX68K_DEVIRAM FASTCALL MFP_TimerSlow(int32_t clock)
{
   static const uint8_t TimerInt[4] = { 2, 7, 10, 11 };
   const uint8_t active = MFP_TimerActiveMask;

#ifdef ESP_PLATFORM
   /* Direct callers are rare, but preserve the same cached exact dispatch. */
   if (__builtin_expect(MFP_TimerFastMode == 1u, 0)) {
      MFP_TimerR71ExactBC(clock);
      return;
   }
   ++s_mfp_fallback_calls;
#endif

   for (int chan = 0; chan < 4; ++chan)
   {
      if (!(active & (uint8_t)(1u << chan)))
         continue;
      const uint8_t prescale_sel = s_timer_prescale_sel[chan];

      const int32_t t = Timer_Prescaler[prescale_sel];
      int32_t accum = Timer_Tick[chan] + clock;
      if (accum < t)
      {
         Timer_Tick[chan] = accum;
         continue;
      }

      const uint32_t decs = mfp_prescale_quotient((uint32_t)accum, prescale_sel);
      Timer_Tick[chan] = accum - (int32_t)(decs * (uint32_t)t);

      int fired = 0;
      MFP[MFP_TADR + chan] = mfp_timer_advance_fast(
         MFP[MFP_TADR + chan], Timer_Reload[chan], decs, &fired);
      if (fired)
         MFP_Int(TimerInt[chan]);
   }
}

/* R26a: restore Timer-A event-count slow helper accidentally dropped while
 * removing R25 trace code.  Semantics are byte-for-byte equivalent to R25. */
void PX68K_DEVIRAM FASTCALL MFP_TimerASlow(void)
{
   if (MFP[MFP_AER] & 0x10)
   {
      if (vline == CRTC_VSTART)
         MFP[MFP_TADR]--;
   }
   else
   {
      if (CRTC_VEND >= VLINE_TOTAL)
      {
         if ((long)vline == (VLINE_TOTAL - 1))
            MFP[MFP_TADR]--;
      }
      else
      {
         if (vline == CRTC_VEND)
            MFP[MFP_TADR]--;
      }
   }
   if (!MFP[MFP_TADR])
   {
      MFP[MFP_TADR] = Timer_Reload[0];
      MFP_Int(2);
   }
}

void MFP_Tab5TimerFastStats(uint32_t *exact_bc, uint32_t *fallback)
{
#ifdef ESP_PLATFORM
   if (exact_bc) *exact_bc = s_mfp_exact_bc_calls;
   if (fallback) *fallback = s_mfp_fallback_calls;
#else
   if (exact_bc) *exact_bc = 0;
   if (fallback) *fallback = 0;
#endif
}
