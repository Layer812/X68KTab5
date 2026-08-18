/*
 * PX68K source modified for the Tab5 port.
 * Intent: Route the standalone Tab5 IOCS/SCSI hooks needed for direct HDS boot while retaining normal guest IOC behavior elsewhere.
 * Layer8 Aug/17/2026
 */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif
/*
 *  IOC.C - I/O Controller
 */

#include "common.h"
#include "ioc.h"

uint8_t	IOC_IntStat = 0;
uint8_t	IOC_IntVect = 0;

int IOC_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFVAR(IOC_IntStat),
		SFVAR(IOC_IntVect),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_IOC", false);

	return ret;
}

void IOC_Init(void)
{
	IOC_IntStat = 0;
	IOC_IntVect = 0;
}

uint8_t FASTCALL IOC_Read(uint32_t adr)
{
	if (adr==0xe9c001)
		return IOC_IntStat;
	return 0xff;
}

void FASTCALL IOC_Write(uint32_t adr, uint8_t data)
{
#if defined(ESP_PLATFORM) && PX68K_TAB5_DIAG_VERBOSE
    /* Legacy IOC trace; disabled by default in Build 5.8. */
    {
        static unsigned s_ioc_trace = 0;

        if (s_ioc_trace < 64)
        {
            ESP_LOGI("PX68K_IOC",
                     "W #%u @%06lX <- %02X",
                     s_ioc_trace,
                     (unsigned long)adr,
                     (unsigned)data);
        }

        s_ioc_trace++;
    }
#endif
	if (adr==0xe9c001)
	{
		IOC_IntStat &= 0xf0;
		IOC_IntStat |= data&0x0f;
	}
	else if (adr==0xe9c003)
		IOC_IntVect = (data&0xfc);
}
