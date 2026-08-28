/* Build 5.25 - ESP32-P4 internal-SRAM graphics working set.
 *
 * libpx68k.a deliberately maps its .bss to PSRAM because the emulator has
 * several megabytes of state. These particular buffers are different:
 * they are touched for almost every composed pixel. Defining them in the
 * application component keeps them in normal internal DRAM while leaving
 * GVRAM/TVRAM/large tables in PSRAM.
 */
/*
 * Tab5 port-specific implementation.
 * Intent: Place selected PX68K hot data in internal memory so CPU1 avoids repeated PSRAM latency on high-frequency emulator state.
 * Layer8 Aug/17/2026
 */
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_memory_utils.h"
#if defined(SPM_DRAM_ATTR)
#define TAB5_SPM_HOT SPM_DRAM_ATTR
#elif defined(TCM_DRAM_ATTR)
#define TAB5_SPM_HOT TCM_DRAM_ATTR
#else
#define TAB5_SPM_HOT
#endif
#else
#define TAB5_SPM_HOT
#endif

#define HOT_ALIGN __attribute__((aligned(64)))

uint16_t Grp_LineBuf[1024] HOT_ALIGN;
uint16_t Grp_LineBufSP[1024] HOT_ALIGN;
uint16_t Grp_LineBufSP2[1024] HOT_ALIGN;
uint16_t Grp_LineBufSP_Tr[1024] HOT_ALIGN;
/* R23: Pal16Adr and the two 256-entry RGB565 palettes are among the
 * highest-frequency graphics lookups and fit comfortably in SPM after the
 * 3.1 KiB ADPCM nibble table moved back to normal Internal DRAM. */
TAB5_SPM_HOT uint16_t Pal16Adr[256] HOT_ALIGN;

uint8_t  Sprite_Regs[0x800] HOT_ALIGN;

/* Build 5.37: derived sprite active-index cache.  This is not guest state;
 * it is rebuilt from Sprite_Regs whenever the guest changes sprite registers.
 * Keep it internal because BG_DrawLine consults it for every dirty scanline. */
uint8_t  Sprite_ActiveIdx[3][128] HOT_ALIGN;
uint8_t  Sprite_ActiveCount[3] HOT_ALIGN;
uint8_t  Sprite_ActiveDirty HOT_ALIGN;
uint8_t  BG_Regs[0x12] HOT_ALIGN;
uint8_t  BG[0x8000] HOT_ALIGN;
uint8_t  BGCHR8[8 * 8 * 256] HOT_ALIGN;
uint8_t  BGCHR16[16 * 16 * 256] HOT_ALIGN;
uint16_t BG_LineBuf[1600] HOT_ALIGN;
uint16_t BG_PriBuf[1600] HOT_ALIGN;

uint8_t TextDirtyLine[1024] HOT_ALIGN;
uint8_t Text_TrFlag[1024] HOT_ALIGN;

TAB5_SPM_HOT uint16_t TextPal[256] HOT_ALIGN;
TAB5_SPM_HOT uint16_t GrphPal[256] HOT_ALIGN;
/* Build 6.00: retired GRP8 live A/B reference line removed (1600B internal DRAM returned). */

uint32_t tab5_px68k_hotmem_bytes(void)
{
    return (uint32_t)(
        sizeof(Grp_LineBuf) + sizeof(Grp_LineBufSP) + sizeof(Grp_LineBufSP2) +
        sizeof(Grp_LineBufSP_Tr) + sizeof(Pal16Adr) + sizeof(Sprite_Regs) +
        sizeof(Sprite_ActiveIdx) + sizeof(Sprite_ActiveCount) + sizeof(Sprite_ActiveDirty) +
        sizeof(BG_Regs) + sizeof(BG) + sizeof(BGCHR8) + sizeof(BGCHR16) +
        sizeof(BG_LineBuf) + sizeof(BG_PriBuf) + sizeof(TextDirtyLine) +
        sizeof(Text_TrFlag) + sizeof(TextPal) + sizeof(GrphPal));
}

#ifdef ESP_PLATFORM
int tab5_px68k_spm_palette_ok(void)
{
#if defined(SPM_DRAM_ATTR) || defined(TCM_DRAM_ATTR)
    return esp_ptr_in_tcm(Pal16Adr) && esp_ptr_in_tcm(TextPal) && esp_ptr_in_tcm(GrphPal);
#else
    return 0;
#endif
}
#else
int tab5_px68k_spm_palette_ok(void) { return 0; }
#endif

uint32_t tab5_px68k_spm_palette_bytes(void)
{
    return (uint32_t)(sizeof(Pal16Adr) + sizeof(TextPal) + sizeof(GrphPal));
}
