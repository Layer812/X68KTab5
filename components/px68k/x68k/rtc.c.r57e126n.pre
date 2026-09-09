/*
 *  RTC.C - RTC (Real Time Clock / RICOH RP5C15)
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Adapt RTC scheduling to the standalone Tab5 timing loop without duplicating guest clock state on CPU0.
 * Layer8 Aug/17/2026
 */
#include "common.h"
#include "mfp.h"

#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#ifdef TCM_DRAM_ATTR
#define PX68K_DEVHOT TCM_DRAM_ATTR
#define PX68K_RTCCACHE DRAM_ATTR
#else
#define PX68K_DEVHOT DRAM_ATTR
#define PX68K_RTCCACHE DRAM_ATTR
#endif
#else
#define PX68K_DEVHOT
#define PX68K_RTCCACHE
#endif

static PX68K_DEVHOT uint8_t RTC_Regs[2][16];
static PX68K_DEVHOT uint8_t RTC_Bank  = 0;
PX68K_DEVHOT int RTC_Timer1    = 0;
PX68K_DEVHOT int RTC_Timer16   = 0;

/* Build 5.60: RTC_Read() used to call time()+localtime() for every nibble.
 * Guest software normally reads many RTC registers back-to-back, so cache the
 * host calendar once per guest second and serve reads from tiny BCD fields. */
static PX68K_RTCCACHE uint8_t s_rtc_sec_ones, s_rtc_sec_tens;
static PX68K_RTCCACHE uint8_t s_rtc_min_ones, s_rtc_min_tens;
static PX68K_RTCCACHE uint8_t s_rtc_hour_ones, s_rtc_hour_tens;
static PX68K_RTCCACHE uint8_t s_rtc_wday;
static PX68K_RTCCACHE uint8_t s_rtc_mday_ones, s_rtc_mday_tens;
static PX68K_RTCCACHE uint8_t s_rtc_mon_ones, s_rtc_mon_tens;
static PX68K_RTCCACHE uint8_t s_rtc_year_ones, s_rtc_year_tens, s_rtc_year_mod4;

static void rtc_refresh_host_time(void)
{
   time_t t = time(NULL);
   struct tm tmv;
#ifdef ESP_PLATFORM
   localtime_r(&t, &tmv);
#else
   struct tm *ptm = localtime(&t);
   if (ptm) tmv = *ptm; else memset(&tmv, 0, sizeof(tmv));
#endif
   const unsigned sec = (unsigned)tmv.tm_sec;
   const unsigned min = (unsigned)tmv.tm_min;
   const unsigned hour = (unsigned)tmv.tm_hour;
   const unsigned mday = (unsigned)tmv.tm_mday;
   const unsigned mon = (unsigned)(tmv.tm_mon + 1);
   const unsigned year = (unsigned)(tmv.tm_year - 80);
   s_rtc_sec_ones = (uint8_t)(sec % 10u); s_rtc_sec_tens = (uint8_t)(sec / 10u);
   s_rtc_min_ones = (uint8_t)(min % 10u); s_rtc_min_tens = (uint8_t)(min / 10u);
   s_rtc_hour_ones = (uint8_t)(hour % 10u); s_rtc_hour_tens = (uint8_t)(hour / 10u);
   s_rtc_wday = (uint8_t)tmv.tm_wday;
   s_rtc_mday_ones = (uint8_t)(mday % 10u); s_rtc_mday_tens = (uint8_t)(mday / 10u);
   s_rtc_mon_ones = (uint8_t)(mon % 10u); s_rtc_mon_tens = (uint8_t)(mon / 10u);
   s_rtc_year_ones = (uint8_t)(year % 10u); s_rtc_year_tens = (uint8_t)((year / 10u) & 0x0fu);
   s_rtc_year_mod4 = (uint8_t)(year & 3u);
}


int RTC_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFARRAYN(RTC_Regs[0], 16, "RTCRegs0"),
      SFARRAYN(RTC_Regs[1], 16, "RTCRegs1"),
	   SFVAR(RTC_Bank), /* never changed since alarm is not implemented, but whatever */
	   SFVAR(RTC_Timer1),
	   SFVAR(RTC_Timer16),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_RTC", false);
	if (load) rtc_refresh_host_time();

	return ret;
}

void RTC_Init(void)
{
	rtc_refresh_host_time();
	memset(&RTC_Regs[1][0], 0, 16);
	RTC_Regs[0][13] = 0;
	RTC_Regs[0][14] = 0;
	RTC_Regs[0][15] = 0x0c;
}

uint8_t FASTCALL RTC_Read(uint32_t adr)
{
   adr &= 0x1f;
   if (!(adr & 1))
      return 0;

   if (RTC_Bank == 0)
   {
      switch(adr)
      {
         case 0x01: return s_rtc_sec_ones;
         case 0x03: return s_rtc_sec_tens;
         case 0x05: return s_rtc_min_ones;
         case 0x07: return s_rtc_min_tens;
         case 0x09: return s_rtc_hour_ones;
         case 0x0b: return s_rtc_hour_tens;
         case 0x0d: return s_rtc_wday;
         case 0x0f: return s_rtc_mday_ones;
         case 0x11: return s_rtc_mday_tens;
         case 0x13: return s_rtc_mon_ones;
         case 0x15: return s_rtc_mon_tens;
         case 0x17: return s_rtc_year_ones;
         case 0x19: return s_rtc_year_tens;
         case 0x1b: return RTC_Regs[0][13];
         case 0x1d: return RTC_Regs[0][14];
         case 0x1f: return RTC_Regs[0][15];
      }
      return 0;
   }
   if (adr == 0x1b)
      return (RTC_Regs[1][13]|1);
   else if (adr == 0x17)
      return s_rtc_year_mod4;
   return RTC_Regs[1][adr>>1];
}

void FASTCALL RTC_Write(uint32_t adr, uint8_t data)
{
	if ( adr==0xe8a001 )
          return;
	if ( adr==0xe8a01b )       /* Alarm/Timer Enable control */
		RTC_Regs[0][13] = RTC_Regs[1][13] = data & 0x0c;
	else if ( adr==0xe8a01f ) /* Alarm terminal output control */
		RTC_Regs[0][15] = RTC_Regs[1][15] = data & 0x0c;
}

/* Build 5.91 slow path: RTC_Timer() in rtc.h has already added the
 * current slice clocks.  This function is called only on a threshold-crossing
 * slice, preserving the exact old interrupt granularity and one-subtraction
 * semantics even for unusual restored state. */
void RTC_TimerSlow(void)
{
	if ( RTC_Timer1>=10000000 )
   {
		rtc_refresh_host_time();
		if ( !(RTC_Regs[0][15]&8) ) MFP_Int(15);
		RTC_Timer1 -= 10000000;
	}
	if ( RTC_Timer16>=625000 )
   {
		if ( !(RTC_Regs[0][15]&4) ) MFP_Int(15);
		RTC_Timer16 -= 625000;
	}
}
