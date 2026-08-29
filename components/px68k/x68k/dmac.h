#ifndef _WINX68K_DMAC_H
#define _WINX68K_DMAC_H

#include <stdint.h>
#include "common.h"

typedef struct
{
	uint8_t CSR;		/* 00 */
	uint8_t CER;
	uint8_t dmy0[2];
	uint8_t DCR;		/* 04 */
	uint8_t OCR;
	uint8_t SCR;
	uint8_t CCR;
	uint8_t dmy1[2];	/* 08 */
	uint16_t MTC;
	uint32_t MAR;		/* 0C */
	uint8_t dmy2[4];	/* 10 */
	uint32_t DAR;		/* 14 */
	uint8_t dmy3[2];	/* 18 */
	uint16_t BTC;
	uint32_t BAR;		/* 1C */
	uint8_t dmy4[5];	/* 20 */
	uint8_t NIV;
	uint8_t dmy5;
	uint8_t EIV;
	uint8_t dmy6;		/* 28 */
	uint8_t MFC;
	uint8_t dmy7[3];
	uint8_t CPR;
	uint8_t dmy8[3];
	uint8_t DFC;
	uint8_t dmy9[7];
	uint8_t BFC;
	uint8_t dmya[5];
	uint8_t GCR;

} dmac_ch;

extern dmac_ch	DMA[4];

uint8_t FASTCALL DMA_Read(uint32_t adr);
void FASTCALL DMA_Write(uint32_t adr, uint8_t data);

int FASTCALL DMA_Exec(int ch);
int FASTCALL DMA_ExecActive012(void);
/* R57E71: production CPU scheduler pays this boundary on every execute slice.
 * Check the three CSR active bits in the caller and enter the large DMA engine
 * only for channels that can actually transfer. Channel order is unchanged. */
static inline __attribute__((always_inline)) void DMA_ExecActive012Inline(void)
{
    if (__builtin_expect(DMA[0].CSR & 0x08u, 0)) DMA_Exec(0);
    if (__builtin_expect(DMA[1].CSR & 0x08u, 0)) DMA_Exec(1);
    if (__builtin_expect(DMA[2].CSR & 0x08u, 0)) DMA_Exec(2);
}
/* R26: guarded exact MDX channel-3 RAM -> MSM6258 path measured by R25.
 * Returns 1 when handled, 0 when authoritative generic DMA_Exec(3) is needed. */
int FASTCALL DMA_Exec3ADPCMFast(void);
void DMA_Tab5ADPCMFastStats(uint32_t *hits, uint32_t *fallbacks);
void DMA_Init(void);
int DMAC_StateAction(StateMem *sm, int load, int data_only);

#endif /* _WINX68K_DMAC_H */
