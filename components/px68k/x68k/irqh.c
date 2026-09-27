/*
 *  IRQH.C - IRQ Handler
 */

#include "common.h"
#include "../m68000/m68000.h"
#include "irqh.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#ifdef TCM_DRAM_ATTR
#define PX68K_IRQHOT TCM_DRAM_ATTR
#else
#define PX68K_IRQHOT DRAM_ATTR
#endif
#else
#define PX68K_IRQHOT
#endif

#if defined (HAVE_CYCLONE)
extern struct Cyclone m68k;
typedef int32_t  FASTCALL C68K_INT_CALLBACK(int32_t level);
#elif defined (HAVE_MUSASHI)
typedef int32_t  FASTCALL C68K_INT_CALLBACK(int32_t level);
#endif /* HAVE_CYCLONE */ /* HAVE_MUSASHI */

static PX68K_IRQHOT uint8_t	IRQH_IRQ[8];
static PX68K_IRQHOT void	*IRQH_CallBack[8];

/* X68KTAB_R1A13R2_IRQ_ONESHOT_GLOBAL
 * IRQ lines are level-sensitive. Reasserting an already-high line with the
 * same callback cannot create another guest-visible event; it only repeats
 * callback assignment, priority scan and m68k_set_irq(). Suppress that work
 * globally while preserving handler changes and the normal post-IACK path. */
static PX68K_IRQHOT volatile uint32_t IRQH_R1A13AssertCalls;
static PX68K_IRQHOT volatile uint32_t IRQH_R1A13AssertEffective;
static PX68K_IRQHOT volatile uint32_t IRQH_R1A13DupSuppressed;
static PX68K_IRQHOT volatile uint32_t IRQH_R1A13DupByLevel[8];

int IRQH_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFARRAY(IRQH_IRQ, 8),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_IRQH", false);

	return ret;
}

void IRQH_Init(void)
{
	memset(IRQH_IRQ, 0, 8);
#ifdef ESP_PLATFORM
    IRQH_R1A13AssertCalls = 0u;
    IRQH_R1A13AssertEffective = 0u;
    IRQH_R1A13DupSuppressed = 0u;
    memset((void *)IRQH_R1A13DupByLevel, 0, sizeof(IRQH_R1A13DupByLevel));
#endif
}

static uint32_t FASTCALL IRQH_DefaultVector(uint8_t irq)
{
	IRQH_IRQCallBack(irq);
	return -1;
}


void IRQH_IRQCallBack(uint8_t irq)
{
	IRQH_IRQ[irq&7] = 0;
	int i;

#if defined (HAVE_CYCLONE)
	m68k.irq =0;
#elif defined (HAVE_C68K)
	C68k_Set_IRQ(&C68K, 0);
#elif defined (HAVE_MUSASHI)
	m68k_set_irq(0);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */

	for (i=7; i>0; i--)
	{
		if (IRQH_IRQ[i])
		{
#if defined (HAVE_CYCLONE)
			m68k.irq = i;
#elif defined (HAVE_C68K)
			C68k_Set_IRQ(&C68K, i);
#elif defined (HAVE_MUSASHI)
			m68k_set_irq(i);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
			return;
		}
	}
}

void IRQH_Int(uint8_t irq, void* handler)
{
	int i;
    const uint8_t level = (uint8_t)(irq & 7u);
    void *new_handler;
    if (handler==NULL)
        new_handler = (void *)&IRQH_DefaultVector;
    else
        new_handler = handler;
#ifdef ESP_PLATFORM
    (void)0;
#endif
    if (IRQH_IRQ[level] && IRQH_CallBack[level] == new_handler)
    {
#ifdef ESP_PLATFORM
        (void)0;
        (void)0;
#endif
        return;
    }
	IRQH_IRQ[level] = 1;
    IRQH_CallBack[level] = new_handler;
#ifdef ESP_PLATFORM
    (void)0;
#endif
	for (i=7; i>0; i--)
	{
		if (IRQH_IRQ[i])
		{
#if defined (HAVE_CYCLONE)

			m68k.irq = i;
#elif defined (HAVE_C68K)
			C68k_Set_IRQ(&C68K, i);
#elif defined (HAVE_MUSASHI)
			m68k_set_irq(i);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
			return;
		}
	}
}

void IRQH_R1A13GetStats(uint32_t *calls, uint32_t *effective,
                        uint32_t *dup_total, uint32_t dup_by_level[8])
{
    if (calls) *calls = 0u;
    if (effective) *effective = 0u;
    if (dup_total) *dup_total = 0u;
    if (dup_by_level) memset(dup_by_level, 0, 8u * sizeof(uint32_t));
}

signed int my_irqh_callback(signed int level)
{
   int i;
   C68K_INT_CALLBACK *func = IRQH_CallBack[level&7];
   int vect = (func)(level&7);

   for (i=7; i>0; i--)
   {
      if (IRQH_IRQ[i])
      {
#if defined (HAVE_CYCLONE)
         m68k.irq = i;
#elif defined (HAVE_C68K)
         C68k_Set_IRQ(&C68K, i);
#elif defined (HAVE_MUSASHI)
         m68k_set_irq(i);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
         break;
      }
   }

   return (int32_t)vect;
}
