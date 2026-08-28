/*
 *  PALETTE.C - Text/BG/Graphic Palette
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Track palette generations so the CPU0 compositor can skip frames/rows whose visible palette state did not change.
 * Layer8 Aug/17/2026
 */
#include	"common.h"
#include	"windraw.h"
#include	"tvram.h"
#include	"bg.h"
#include	"crtc.h"
#include	"x68kmemory.h"
#include	"m68000.h"
#include	"palette.h"

uint8_t		Pal_Regs[1024];
extern uint16_t TextPal[256];
extern uint16_t GrphPal[256];
uint16_t	Pal16[65536];
uint16_t	Ibit;

uint16_t	Pal_HalfMask, Pal_Ix2;
uint16_t	Pal_R, Pal_G, Pal_B;

/* Build 5.8: graphics/text palette activity counters. */
static uint32_t s_debug_grph_write_count = 0;
static uint32_t s_debug_text_write_count = 0;
static uint32_t s_debug_last_addr = 0;
static uint8_t  s_debug_last_data = 0;
/* Build 6.15e: generation/contrast snapshot for CPU0 65K render cache. */
static uint32_t s_visual_generation = 1u;
static uint8_t s_effective_contrast = 15u;
#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
#define TAB5_RELEASE_DIAG_INC(v) (++(v))
#define TAB5_RELEASE_DIAG_ADD(v,n) ((v) += (n))
#define TAB5_RELEASE_DIAG_ATOMIC_ADD(ptr,n) __atomic_add_fetch((ptr),(n),__ATOMIC_RELAXED)
#else
#define TAB5_RELEASE_DIAG_INC(v) ((void)0)
#define TAB5_RELEASE_DIAG_ADD(v,n) ((void)0)
#define TAB5_RELEASE_DIAG_ATOMIC_ADD(ptr,n) ((void)0)
#endif

#ifdef ESP_PLATFORM
static uint32_t s_tab5_dirty_grph;
static uint32_t s_tab5_dirty_text;
static uint32_t s_tab5_dirty_contrast;
/* BAT177NW7/R57E37: exact TextPal full-dirty pruning telemetry. */
static uint32_t s_tab5_text_effective_same;
static uint32_t s_tab5_text_unused_bank;
static uint32_t s_tab5_text_visual_dirty;
static uint32_t s_tab5_text_bank_write[16];
#endif

uint32_t Pal_DebugGrphWriteCount(void) { return s_debug_grph_write_count; }
uint32_t Pal_DebugTextWriteCount(void) { return s_debug_text_write_count; }
uint32_t Pal_DebugLastAddr(void)       { return s_debug_last_addr; }
uint8_t  Pal_DebugLastData(void)       { return s_debug_last_data; }
uint32_t Pal_DebugVisualGeneration(void) { return s_visual_generation; }
uint8_t  Pal_DebugEffectiveContrast(void) { return s_effective_contrast; }

void Pal_Tab5DirtyStatsTake(uint32_t *grph, uint32_t *text, uint32_t *contrast)
{
#ifdef ESP_PLATFORM
    if (grph) *grph = __atomic_exchange_n(&s_tab5_dirty_grph, 0u, __ATOMIC_RELAXED);
    if (text) *text = __atomic_exchange_n(&s_tab5_dirty_text, 0u, __ATOMIC_RELAXED);
    if (contrast) *contrast = __atomic_exchange_n(&s_tab5_dirty_contrast, 0u, __ATOMIC_RELAXED);
#else
    if (grph) *grph = 0u;
    if (text) *text = 0u;
    if (contrast) *contrast = 0u;
#endif
}

void Pal_Tab5PruneStatsTake(uint32_t *effective_same,
                            uint32_t *unused_bank,
                            uint32_t *visual_dirty,
                            uint32_t bank_write[16])
{
#ifdef ESP_PLATFORM
    if (effective_same) *effective_same = __atomic_exchange_n(&s_tab5_text_effective_same, 0u, __ATOMIC_RELAXED);
    if (unused_bank) *unused_bank = __atomic_exchange_n(&s_tab5_text_unused_bank, 0u, __ATOMIC_RELAXED);
    if (visual_dirty) *visual_dirty = __atomic_exchange_n(&s_tab5_text_visual_dirty, 0u, __ATOMIC_RELAXED);
    if (bank_write) {
        for (uint32_t i = 0u; i < 16u; ++i)
            bank_write[i] = __atomic_exchange_n(&s_tab5_text_bank_write[i], 0u, __ATOMIC_RELAXED);
    }
#else
    if (effective_same) *effective_same = 0u;
    if (unused_bank) *unused_bank = 0u;
    if (visual_dirty) *visual_dirty = 0u;
    if (bank_write) memset(bank_write, 0, 16u * sizeof(bank_write[0]));
#endif
}

static void pal_visual_generation_bump(void)
{
    ++s_visual_generation;
    if (!s_visual_generation) ++s_visual_generation; /* reserve 0 as never-valid */
}

/* Pal16 itself is serialized, while the host-only effective-contrast byte is
 * not.  Recover the 0..15 contrast after state load from one full-red entry.
 * This keeps the CPU0 65K cache exact without changing the savestate format. */
static void pal_debug_recover_effective_contrast(void)
{
    uint16_t hi_r = 0u;
    for (uint16_t bit = 0x8000u; bit; bit >>= 1) {
        if (Pal_R & bit) { hi_r = bit; break; }
    }
    uint16_t lo_b = (uint16_t)(Pal_B & (uint16_t)(0u - Pal_B));
    const uint16_t ref = Pal16[0x0400u];
    for (uint32_t n = 0; n < 16u; ++n) {
        uint16_t exp = (uint16_t)((((uint32_t)hi_r * n) / 15u) & Pal_R);
        if (hi_r && !exp) exp = lo_b;
        if (exp == ref) { s_effective_contrast = (uint8_t)n; return; }
    }
    /* Fail-safe: this should be unreachable with the Tab5 RGB565 layout.
     * The runtime Pal16 self-check will disable the fast path on disagreement. */
    s_effective_contrast = 15u;
}

int Pal_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] =
	{
		SFARRAY(Pal_Regs, 1024),
		SFARRAY16(TextPal, 256),
		SFARRAY16(GrphPal, 256),
		SFARRAY16(Pal16, 65536),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_Palette", false);

    if (load) {
        pal_debug_recover_effective_contrast();
        pal_visual_generation_bump();
    }
	return ret;
}

void Pal_SetColor(void)
{
	int i;
	uint16_t bit;
	uint16_t R[5]     = {0, 0, 0, 0, 0};
	uint16_t G[5]     = {0, 0, 0, 0, 0};
	uint16_t B[5]     = {0, 0, 0, 0, 0};
	int r         = 5;
	int g         = 5;
	int b         = 5;
	Pal_R         = Pal_G = Pal_B = 0;
	uint16_t TempMask = 0;
	for (bit=0x8000; bit; bit>>=1)
	{
		if ( (WinDraw_Pal16R&bit)&&(r) )
		{
			R[--r] = bit;
			TempMask |= bit;
			Pal_R |= bit;
		}
		if ( (WinDraw_Pal16G&bit)&&(g) )
		{
			G[--g] = bit;
			TempMask |= bit;
			Pal_G |= bit;
		}
		if ( (WinDraw_Pal16B&bit)&&(b) )
		{
			B[--b] = bit;
			TempMask |= bit;
			Pal_B |= bit;
		}
	}

	Ibit = 1;
	for (bit=1; bit; bit<<=1)
	{
		if (!(TempMask&bit))
		{
			Ibit = bit;
			break;
		}
	}

	Pal_HalfMask = ~(B[0] | R[0] | G[0] | Ibit);
	Pal_Ix2 = Ibit << 1;

	for (i=0; i<65536; i++)
	{
		bit = 0;
		if (i&0x8000) bit |= G[4];
		if (i&0x4000) bit |= G[3];
		if (i&0x2000) bit |= G[2];
		if (i&0x1000) bit |= G[1];
		if (i&0x0800) bit |= G[0];
		if (i&0x0400) bit |= R[4];
		if (i&0x0200) bit |= R[3];
		if (i&0x0100) bit |= R[2];
		if (i&0x0080) bit |= R[1];
		if (i&0x0040) bit |= R[0];
		if (i&0x0020) bit |= B[4];
		if (i&0x0010) bit |= B[3];
		if (i&0x0008) bit |= B[2];
		if (i&0x0004) bit |= B[1];
		if (i&0x0002) bit |= B[0];
		if (i&0x0001) bit |= Ibit;
		Pal16[i] = bit;
	}
}

void Pal_Init(void)
{
	s_debug_grph_write_count = 0;
	s_debug_text_write_count = 0;
	s_debug_last_addr = 0;
	s_debug_last_data = 0;
	pal_visual_generation_bump();
	s_effective_contrast = 15u;
#ifdef ESP_PLATFORM
    s_tab5_dirty_grph = s_tab5_dirty_text = s_tab5_dirty_contrast = 0u;
    s_tab5_text_effective_same = s_tab5_text_unused_bank = s_tab5_text_visual_dirty = 0u;
    memset(s_tab5_text_bank_write, 0, sizeof(s_tab5_text_bank_write));
#endif

	memset(Pal_Regs, 0, 1024);
	memset(TextPal,  0, 512);
	memset(GrphPal,  0, 512);
	Pal_SetColor();
}

uint8_t FASTCALL Pal_Read(uint32_t adr)
{
	if (adr<0xe82400)
		return Pal_Regs[adr-0xe82000];
	else return 0xff;
}

void FASTCALL Pal_Write(uint32_t adr, uint8_t data)
{
	uint16_t pal;

	if (adr>=0xe82400) return;

	adr -= 0xe82000;
	if (Pal_Regs[adr] == data) return;

	s_debug_last_addr = adr + 0xe82000;
	s_debug_last_data = data;
	if (adr < 0x200)
		s_debug_grph_write_count++;
	else if (adr < 0x400)
		s_debug_text_write_count++;

	if (adr<0x200)
	{
		Pal_Regs[adr] = data;
		pal_visual_generation_bump();
#ifdef ESP_PLATFORM
        TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_dirty_grph, 1u);
#endif
		TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_PALETTE);
		pal = Pal_Regs[adr&0xfffe];
		pal = (pal<<8)+Pal_Regs[adr|1];
		GrphPal[adr/2] = Pal16[pal];
	}
	else if (adr<0x400)
	{
        const uint32_t text_index = (adr - 0x200u) >> 1;
        const uint16_t old_rgb565 = TextPal[text_index];
		Pal_Regs[adr] = data;
#ifdef ESP_PLATFORM
        TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_dirty_text, 1u);
        const uint32_t text_bank = text_index >> 4;
        TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_text_bank_write[text_bank], 1u);
#endif
		pal = Pal_Regs[adr&0xfffe];
		pal = (pal<<8)+Pal_Regs[adr|1];
        const uint16_t new_rgb565 = Pal16[pal];
		TextPal[text_index] = new_rgb565;
#ifdef ESP_PLATFORM
        /* Text/BG/sprite rendering consumes this half of palette RAM only
         * through TextPal[].  Therefore a raw-register change that maps to the
         * same RGB565 value is visually exact-noop.  Likewise, banks 1..15
         * with no current BG-map or sprite reference cannot affect the current
         * image; future map/sprite/config writes carry their own dirty facts.
         * Bank 0 remains conservatively live because TEXT uses indices 0..15. */
        if (new_rgb565 == old_rgb565) {
            TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_text_effective_same, 1u);
        } else if (!BG_Tab5TextPalBankMayAffect(text_index >> 4)) {
            TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_text_unused_bank, 1u);
        } else {
            TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_text_visual_dirty, 1u);
            TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_PALETTE);
        }
#else
        TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_PALETTE);
#endif
	}
}

void Pal_ChangeContrast(int num)
{
	s_effective_contrast = (uint8_t)(num & 15);
	pal_visual_generation_bump();
	uint16_t bit;
	uint16_t R[5] = {0, 0, 0, 0, 0};
	uint16_t G[5] = {0, 0, 0, 0, 0};
	uint16_t B[5] = {0, 0, 0, 0, 0};
	int r, g, b, i;
	int palr, palg, palb;
	uint16_t pal;

#ifdef ESP_PLATFORM
    TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_dirty_contrast, 1u);
#endif
	TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_PALETTE);

	r = g = b = 5;

	for (bit=0x8000; bit; bit>>=1)
	{
		if ( (WinDraw_Pal16R&bit)&&(r) ) R[--r] = bit;
		if ( (WinDraw_Pal16G&bit)&&(g) ) G[--g] = bit;
		if ( (WinDraw_Pal16B&bit)&&(b) ) B[--b] = bit;
	}

	for (i=0; i<65536; i++)
	{
		palr = palg = palb = 0;
		if (i&0x8000) palg |= G[4];
		if (i&0x4000) palg |= G[3];
		if (i&0x2000) palg |= G[2];
		if (i&0x1000) palg |= G[1];
		if (i&0x0800) palg |= G[0];
		if (i&0x0400) palr |= R[4];
		if (i&0x0200) palr |= R[3];
		if (i&0x0100) palr |= R[2];
		if (i&0x0080) palr |= R[1];
		if (i&0x0040) palr |= R[0];
		if (i&0x0020) palb |= B[4];
		if (i&0x0010) palb |= B[3];
		if (i&0x0008) palb |= B[2];
		if (i&0x0004) palb |= B[1];
		if (i&0x0002) palb |= B[0];
		pal = palr | palb | palg;
		palg = (uint16_t)((palg * num)/15)&Pal_G;
		palr = (uint16_t)((palr * num)/15)&Pal_R;
		palb = (uint16_t)((palb * num)/15)&Pal_B;
		Pal16[i] = palr | palb | palg;
		if ((pal)&&(!Pal16[i])) Pal16[i] = B[0];
		if (i&0x0001) Pal16[i] |= Ibit;
	}

	for (i=0; i<256; i++)
	{
		pal = Pal_Regs[i * 2];
		pal = (pal<<8)+Pal_Regs[i * 2+1];
		GrphPal[i] = Pal16[pal];

		pal = Pal_Regs[i * 2+512];
		pal = (pal<<8)+Pal_Regs[i * 2+513];
		TextPal[i] = Pal16[pal];
	}
}
