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

uint32_t Pal_DebugGrphWriteCount(void) { return s_debug_grph_write_count; }
uint32_t Pal_DebugTextWriteCount(void) { return s_debug_text_write_count; }
uint32_t Pal_DebugLastAddr(void)       { return s_debug_last_addr; }
uint8_t  Pal_DebugLastData(void)       { return s_debug_last_data; }

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
		TVRAM_SetAllDirty();
		pal = Pal_Regs[adr&0xfffe];
		pal = (pal<<8)+Pal_Regs[adr|1];
		GrphPal[adr/2] = Pal16[pal];
	}
	else if (adr<0x400)
	{
		Pal_Regs[adr] = data;
		TVRAM_SetAllDirty();
		pal = Pal_Regs[adr&0xfffe];
		pal = (pal<<8)+Pal_Regs[adr|1];
		TextPal[(adr-0x200)/2] = Pal16[pal];
	}
}

void Pal_ChangeContrast(int num)
{
	uint16_t bit;
	uint16_t R[5] = {0, 0, 0, 0, 0};
	uint16_t G[5] = {0, 0, 0, 0, 0};
	uint16_t B[5] = {0, 0, 0, 0, 0};
	int r, g, b, i;
	int palr, palg, palb;
	uint16_t pal;

	TVRAM_SetAllDirty();

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
