/*	$Id: mem_wrap.c,v 1.2 2003/12/05 18:07:19 nonaka Exp $	*/

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Add fast ordinary-RAM/IPL access helpers for ESP32-P4 while keeping MMIO/device accesses on authoritative PX68K wrappers.
 * Layer8 Aug/17/2026
 */
#include "common.h"
#include <string.h>
#include "../m68000/m68000.h"
#include "winx68k.h"

#include "adpcm.h"
#include "bg.h"
#include "crtc.h"
#include "dmac.h"
#include "fdc.h"
#include "gvram.h"
#include "mercury.h"
#include "mfp.h"
#include "midi.h"
#include "ioc.h"
#include "pia.h"
#include "rtc.h"
#include "sasi.h"
#include "scc.h"
#include "scsi.h"
#include "sram.h"
#include "sysport.h"
#include "tvram.h"

#include "fmg_wrap.h"
#include "dswin.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#ifdef TCM_DRAM_ATTR
#define PX68K_MEMHOT TCM_DRAM_ATTR
#else
#define PX68K_MEMHOT DRAM_ATTR
#endif
#else
#define PX68K_MEMHOT
#endif

/* R57E62 production: keep only the 12-byte guest-memory roots in TCM/SPM.
 * Backing RAM/ROM/font storage remains in PSRAM. */

/* R57E139A6_X68P4_PRODUCTION_COHERENCY
 * Production-clean single coherency authority.  The common data-store path
 * performs only the 4 KiB resident-region byte gate.  Exact 128-byte, 2-way
 * code-page invalidation remains; all diagnostic counters are removed. */
#ifdef ESP_PLATFORM
extern uint32_t g_x68p4_pre139_page_tag[128];
extern uint32_t g_x68p4_pre139_page_version[128];
extern uint8_t g_x68p4_pre139_region_count[3072];
extern uint32_t g_x68p4_pre139_enabled;
static inline __attribute__((always_inline)) unsigned tab5_x68p4_a6_set(uint32_t page)
{
    uint32_t pn=page>>7; pn^=pn>>7; pn^=pn>>14; return (unsigned)(pn&63u);
}
static inline __attribute__((always_inline)) void tab5_x68p4_dma139_note_page(uint32_t address)
{
    const uint32_t a=address&0x00ffffffu;
    if (__builtin_expect(!g_x68p4_pre139_enabled || a>=0x00c00000u,0)) return;
    const unsigned region=(unsigned)(a>>12);
    if (__builtin_expect(g_x68p4_pre139_region_count[region]==0u,1)) return;
    const uint32_t page=a&~0x7fu;
    const unsigned first=tab5_x68p4_a6_set(page)<<1;
    for(unsigned way=0;way<2u;++way){
        const unsigned slot=first+way;
        if(__builtin_expect(g_x68p4_pre139_page_tag[slot]==page,0)){
            g_x68p4_pre139_page_tag[slot]=0xffffffffu;
            uint32_t v=g_x68p4_pre139_page_version[slot]+1u;
            g_x68p4_pre139_page_version[slot]=v?v:1u;
            m68k_tab5_x68p4_bump_epoch();
            uint8_t rc=g_x68p4_pre139_region_count[region];
            if(rc) g_x68p4_pre139_region_count[region]=(uint8_t)(rc-1u);
            break;
        }
    }
}
static inline __attribute__((always_inline)) void tab5_x68p4_dma139_note_write(uint32_t address,uint32_t bytes)
{
    if(!bytes)return;
    const uint32_t a0=address&0x00ffffffu; tab5_x68p4_dma139_note_page(a0);
    const uint32_t a1=(a0+bytes-1u)&0x00ffffffu;
    if(__builtin_expect((a0^a1)&~0x7fu,0)) tab5_x68p4_dma139_note_page(a1);
}
#else
static inline void tab5_x68p4_dma139_note_write(uint32_t address,uint32_t bytes){(void)address;(void)bytes;}
#endif

PX68K_MEMHOT uint8_t *IPL;
PX68K_MEMHOT uint8_t *MEM;
static uint8_t *OP_ROM;
PX68K_MEMHOT uint8_t *FONT;

PX68K_MEMHOT uint32_t BusErrFlag = 0;
PX68K_MEMHOT uint32_t BusErrHandling = 0;
static PX68K_MEMHOT uint32_t BusErrAdr = 0;

/* forward declarations */
static void wm_opm(uint32_t addr, uint8_t val);
static void wm_adpcm(uint32_t addr, uint8_t val);
static void wm_buserr(uint32_t addr, uint8_t val);
static uint8_t rm_opm(uint32_t addr);
static uint8_t rm_ipl(uint32_t addr);
static uint8_t rm_buserr(uint32_t addr);
static uint8_t rm_font(uint32_t addr);
static uint8_t rm_nop(uint32_t addr) { return 0; }
static void wm_nop(uint32_t addr, uint8_t val) { }

uint8_t (*MemReadTable[])(uint32_t) = {
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	CRTC_Read, VCtrl_Read, DMA_Read, rm_nop, MFP_Read, RTC_Read, rm_nop, SysPort_Read,
	rm_opm, ADPCM_Read, FDC_Read, SASI_Read, SCC_Read, PIA_Read, IOC_Read, SCSI_Read,
	SCSI_Read, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, MIDI_Read,
	BG_Read, BG_Read, BG_Read, BG_Read, BG_Read, BG_Read, BG_Read, BG_Read,
#ifndef	NO_MERCURY
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, Mcry_Read, rm_buserr,
#else
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr,
#endif
	SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read,
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr,
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	/* In the case of SCSI, will it be rm_buserr? */
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
};

void (*MemWriteTable[])(uint32_t, uint8_t) = {
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	CRTC_Write, VCtrl_Write, DMA_Write, wm_nop, MFP_Write, RTC_Write, wm_nop, SysPort_Write,
	wm_opm, wm_adpcm, FDC_Write, SASI_Write, SCC_Write, PIA_Write, IOC_Write, SCSI_Write,
	SCSI_Write, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, MIDI_Write,
	BG_Write, BG_Write, BG_Write, BG_Write, BG_Write, BG_Write, BG_Write, BG_Write,
#ifndef	NO_MERCURY
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, Mcry_Write, wm_buserr,
#else
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
#endif
	SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	/* Any write to the ROM area results in a bus error */
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
};


static void wm_buserr(uint32_t addr, uint8_t val)
{
	BusErrFlag = 2;
	BusErrAdr = addr;
}

static void wm_cnt(uint32_t addr, uint8_t val)
{
	addr &= 0x00ffffff;
	if (addr < 0x00c00000) /* Use RAM upto 12MB */
	{
#ifdef MSB_FIRST
		MEM[addr    ] = val;
#else
		MEM[addr ^ 1] = val;
#endif
#ifdef ESP_PLATFORM
        m68k_tab5_exec123_note_ram_write8(addr);
#endif
	}
	else if (addr < 0x00e00000)
		GVRAM_Write(addr, val);
	else if (addr < 0x00e80000)
		TVRAM_Write(addr, val);
	else {
        /* R140P1: only the measured production-hot device pages bypass the
         * generic function-pointer table.  Keep uncommon devices on the
         * original table rather than growing another generic decoder. */
        const unsigned slot = (unsigned)((addr >> 13) & 0xffu);
        if (__builtin_expect(slot == 0x48u, 0)) { wm_opm(addr, val); return; }
        if (__builtin_expect(slot == 0x44u, 0)) { MFP_Write(addr, val); return; }
        if (__builtin_expect(slot == 0x42u, 0)) { DMA_Write(addr, val); return; }
        if (__builtin_expect(slot == 0x49u, 0)) { wm_adpcm(addr, val); return; }
        if (__builtin_expect(slot == 0x57u, 0)) { MIDI_Write(addr, val); return; }
        if (__builtin_expect(slot >= 0x58u && slot <= 0x5fu, 0)) { BG_Write(addr, val); return; }
        MemWriteTable[slot](addr, val);
    }
}


static void wm_main(uint32_t addr, uint8_t val) 
{
	if ((BusErrFlag & 7) == 0)
		wm_cnt(addr, val);
}

static void wm_opm(uint32_t addr, uint8_t val)
{
	uint8_t t = addr & 3;
	if (t == 1 || t == 3)
	{
		/* Build 6.15b: OPM and ADPCM have independent audible state.  Dense
		 * MDX register traffic must not force CPU1 to run ADPCM_Update() for
		 * every YM2151 data write.  DSound_OPMWrite() timestamps the FM write
		 * against the pending guest-audio frames and sends render+write as one
		 * ordered CPU0 event. */
		DSound_OPMWrite((t == 1) ? 0 : 1, val);
	}
}

static void wm_adpcm(uint32_t addr, uint8_t val)
{
	/* MSM6258 writes only require the ADPCM timeline to reach this boundary. */
	DSound_FlushADPCMPending();
	ADPCM_Write(addr, val);
}

static uint8_t rm_main(uint32_t addr)
{
	addr &= 0x00ffffff;
	if (addr < 0x00c00000) /* Use RAM upto 12MB */
		return MEM[addr ^ 1];
	else if (addr < 0x00e00000)
		return GVRAM_Read(addr);
    else if (addr < 0x00e80000)
        return TVRAM_Read(addr);

    /* R140P1: MDX/XVI16 hot MMIO pages go directly to the same authoritative
     * device handlers.  Optional/uncommon pages retain the original table. */
    const unsigned slot = (unsigned)((addr >> 13) & 0xffu);
    if (__builtin_expect(slot == 0x44u, 0)) return MFP_Read(addr);
    if (__builtin_expect(slot == 0x48u, 0)) return rm_opm(addr);
    if (__builtin_expect(slot == 0x42u, 0)) return DMA_Read(addr);
    if (__builtin_expect(slot == 0x57u, 0)) return MIDI_Read(addr);
    if (__builtin_expect(slot == 0x49u, 0)) return ADPCM_Read(addr);
    if (__builtin_expect(slot >= 0x58u && slot <= 0x5fu, 0)) return BG_Read(addr);
    return MemReadTable[slot](addr);
}

static uint8_t rm_font(uint32_t addr)
{
	return FONT[addr & 0xfffff];
}

static uint8_t rm_ipl(uint32_t addr)
{
	return IPL[(addr & 0x3ffff) ^ 1];
}

static uint8_t rm_opm(uint32_t addr)
{
	if ((addr & 3) == 3)
		return OPM_Read();
	return 0;
}

static uint8_t rm_buserr(uint32_t addr)
{
	BusErrFlag = 1;
	BusErrAdr = addr;

	return 0;
}

static void cpu_setOPbase24(uint32_t addr)
{
	switch ((addr >> 20) & 0xf)
   {
      case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      case 8: case 9: case 0xa: case 0xb:
         OP_ROM = MEM;
         break;

      case 0xc:
      case 0xd:
         OP_ROM = GVRAM + (addr - 0x00c00000);
         break;

      case 0xe:
         if (addr < 0x00e80000) 
            OP_ROM = TVRAM + (addr - 0x00e00000);
         else if ((addr >= 0x00ea0000) && (addr < 0x00ea2000))
            OP_ROM = SCSIIPL + (addr - 0x00ea0000);
         else if ((addr >= 0x00ed0000) && (addr < 0x00ed4000))
            OP_ROM = SRAM + (addr - 0x00ed0000);
         else
         {
            BusErrFlag = 3;
            BusErrAdr = addr;
            BusErrHandling = 1;
         }
         break;

      case 0xf:
         if ((addr >= 0x00fc0000) && (addr < 0x01000000))
            OP_ROM = IPL + (addr - 0x00fc0000);
         else
         {
            BusErrFlag     = 3;
            BusErrAdr      = addr;
            BusErrHandling = 1;
         }
         break;
   }
}

/*
 * write function
 */
void dma_writemem24(uint32_t addr, uint8_t val)
{
#ifdef ESP_PLATFORM
    tab5_x68p4_dma139_note_write(addr, 1u);
#endif
	wm_main(addr, val);
}

void dma_writemem24_word(uint32_t addr, uint16_t val)
{
#ifdef ESP_PLATFORM
    tab5_x68p4_dma139_note_write(addr, 2u);
#endif
	if (addr & 1)
   {
		BusErrFlag |= 4;
		return;
	}

	wm_main(addr, (val >> 8) & 0xff);
	wm_main(addr + 1, val & 0xff);
}

void dma_writemem24_dword(uint32_t addr, uint32_t val)
{
#ifdef ESP_PLATFORM
    tab5_x68p4_dma139_note_write(addr, 4u);
#endif
	if (addr & 1)
   {
      BusErrFlag |= 4;
      return;
   }

	wm_main(addr, (val >> 24) & 0xff);
	wm_main(addr + 1, (val >> 16) & 0xff);
	wm_main(addr + 2, (val >> 8) & 0xff);
	wm_main(addr + 3, val & 0xff);
}

void cpu_writemem24(uint32_t addr, uint32_t val)
{
#ifdef ESP_PLATFORM
    tab5_x68p4_dma139_note_write(addr, 1u);
#endif
	BusErrFlag = 0;

	wm_cnt(addr, val & 0xff);
	if (BusErrFlag & 2)
		BusErrHandling = 1;
}

void cpu_writemem24_word(uint32_t addr, uint32_t val)
{
#ifdef ESP_PLATFORM
    tab5_x68p4_dma139_note_write(addr, 2u);
#endif

	if (addr & 1)
		return;

	BusErrFlag = 0;

	wm_cnt(addr, (val >> 8) & 0xff);
	wm_main(addr + 1, val & 0xff);

	if (BusErrFlag & 2)
		BusErrHandling = 1;
}

void cpu_writemem24_dword(uint32_t addr, uint32_t val)
{
#ifdef ESP_PLATFORM
    tab5_x68p4_dma139_note_write(addr, 4u);
#endif
	if (addr & 1)
		return;

	BusErrFlag = 0;

	wm_cnt(addr, (val >> 24) & 0xff);
	wm_main(addr + 1, (val >> 16) & 0xff);
	wm_main(addr + 2, (val >> 8) & 0xff);
	wm_main(addr + 3, val & 0xff);

	if (BusErrFlag & 2)
		BusErrHandling = 1;
}

/*
 * read function
 */
uint8_t dma_readmem24(uint32_t addr)
{
	return rm_main(addr);
}

uint16_t dma_readmem24_word(uint32_t addr)
{
	uint16_t v;

	if (addr & 1) {
		BusErrFlag = 3;
		return 0;
	}

	v = rm_main(addr++) << 8;
	v |= rm_main(addr);
	return v;
}

uint32_t 
dma_readmem24_dword(uint32_t addr)
{
	uint32_t v;

	if (addr & 1) {
		BusErrFlag = 3;
		return 0;
	}

	v = rm_main(addr++) << 24;
	v |= rm_main(addr++) << 16;
	v |= rm_main(addr++) << 8;
	v |= rm_main(addr);
	return v;
}

uint32_t 
cpu_readmem24(uint32_t addr)
{
    uint8_t v = rm_main(addr);
    /* Build 5.98: retire the 5.97 last-MMIO-read recorder.  It served its
     * purpose diagnosing the CZ-6BM1 wait and should not tax every device
     * byte read during normal games. */
    if (BusErrFlag & 1)
        BusErrHandling = 1;
    return (uint32_t)v;
}

uint32_t 
cpu_readmem24_word(uint32_t addr)
{
	uint16_t v;

	if (addr & 1)
		return 0;

	BusErrFlag = 0;

	v = rm_main(addr++) << 8;
	v |= rm_main(addr);
	if (BusErrFlag & 1)
		BusErrHandling = 1;
	return (uint32_t) v;
}

uint32_t 
cpu_readmem24_dword(uint32_t addr)
{
	uint32_t v;

	if (addr & 1)
   {
		BusErrFlag = 3;
		return 0;
	}

	BusErrFlag = 0;

	v = rm_main(addr++) << 24;
	v |= rm_main(addr++) << 16;
	v |= rm_main(addr++) << 8;
	v |= rm_main(addr);
	return v;
}

/*
 * Memory misc
 */
void Memory_Init(void)
{
#if defined (HAVE_CYCLONE)
	cpu_setOPbase24((uint32_t)m68000_get_reg(M68K_PC));
#elif defined (HAVE_C68K)
	cpu_setOPbase24((uint32_t)C68k_Get_PC(&C68K));
#elif defined (HAVE_MUSASHI)
	cpu_setOPbase24((uint32_t)m68k_get_reg(NULL, M68K_REG_PC));
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}

void 
Memory_SetSCSIMode(void)
{
	int i;
	for (i = 0xe0; i < 0xf0; i++)
		MemReadTable[i] = rm_buserr;
}
