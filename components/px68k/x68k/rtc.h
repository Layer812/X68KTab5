/*
 * PX68K source modified for the Tab5 port.
 * Intent: RTC hooks used by the Tab5 standalone scheduler.
 * Layer8 Aug/17/2026
 */
#ifndef _X68K_RTC_H
#define _X68K_RTC_H

#include <stdint.h>

void RTC_Init(void);
uint8_t FASTCALL RTC_Read(uint32_t adr);
void FASTCALL RTC_Write(uint32_t adr, uint8_t data);
/* Build 5.91: keep the common RTC no-event path in the caller.
 * RP5C15 events can only occur when one of these exact guest-clock
 * accumulators crosses its threshold.  Updating/checking them inline avoids
 * a cross-TU call on almost every CPU scheduler slice while RTC_TimerSlow()
 * still runs on the exact same slice that crosses an event boundary. */
extern int RTC_Timer1;
extern int RTC_Timer16;
void RTC_TimerSlow(void);
static inline __attribute__((always_inline)) void RTC_Timer(int clock)
{
    const int t1 = RTC_Timer1 + clock;
    const int t16 = RTC_Timer16 + clock;
    RTC_Timer1 = t1;
    RTC_Timer16 = t16;
    if (__builtin_expect(t1 >= 10000000 || t16 >= 625000, 0))
        RTC_TimerSlow();
}
int RTC_StateAction(StateMem *sm, int load, int data_only);

#endif /* _X68K_RTC_H */
