/*
 *  BG.C - BG and sprites
 *  TODO: Check transparent color processing (especially with Text)
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Reduce sprite/background host work and export compact state needed by the CPU0 compositor without changing guest-visible BG/SP registers.
 * Layer8 Aug/17/2026
 */
#include <string.h>

#include "common.h"
#include "windraw.h"
#include "winx68k.h"
#include "palette.h"
#include "tvram.h"
#include "crtc.h"
#include "bg.h"

#include "m68000.h"

#ifdef ESP_PLATFORM
/* Build 5.53a: host BG/Sprite jobs read the shared internal-SRAM BG source
 * arrays directly. Before the guest mutates those sources, drain only the
 * outstanding jobs that depend on them. This is a write-side barrier, not a
 * per-scanline wait, so normal rendering remains fully asynchronous. */
extern void tab5_compose_guest_bg_barrier(void);
#define BG_HOST_SOURCE_BARRIER() tab5_compose_guest_bg_barrier()
#else
#define BG_HOST_SOURCE_BARRIER() ((void)0)
#endif

extern uint8_t BG[0x8000];
extern uint8_t Sprite_Regs[0x800];
/* Build 5.37: hot derived index cache lives in application internal SRAM. */
extern uint8_t Sprite_ActiveIdx[3][128];
extern uint8_t Sprite_ActiveCount[3];
extern uint8_t Sprite_ActiveDirty;
extern uint8_t BG_Regs[0x12];
static uint16_t BG_CHREND = 0;
static uint16_t	BG_BG0TOP = 0;
static uint16_t	BG_BG0END = 0;
static uint16_t	BG_BG1TOP = 0;
static uint16_t	BG_BG1END = 0;
static uint8_t	BG_CHRSIZE = 16;
static uint32_t BG_AdrMask = 511;
static uint32_t	BG0ScrollX = 0, BG0ScrollY = 0;
static uint32_t	BG1ScrollX = 0, BG1ScrollY = 0;

int32_t	BG_HAdjust = 0;
int32_t	BG_VLINE = 0;

/* Build 5.25: decoded character cache + scanline buffers live in internal SRAM. */
extern uint8_t BGCHR8[8*8*256];
extern uint8_t BGCHR16[16*16*256];
extern uint16_t BG_LineBuf[1600];
extern uint16_t BG_PriBuf[1600];

uint32_t	VLINEBG = 0;

int BG_StateAction(StateMem *sm, int load, int data_only)
{
    if (load) BG_HOST_SOURCE_BARRIER();
	SFORMAT StateRegs[] = 
	{
		SFARRAY(BG, 0x8000),
		SFARRAY(Sprite_Regs, 0x800),
		SFARRAY(BG_Regs, 0x12),

		SFVAR(BG_CHREND),
		SFVAR(BG_BG0TOP),
		SFVAR(BG_BG0END),
		SFVAR(BG_BG1TOP),
		SFVAR(BG_BG1END),
		SFVAR(BG_CHRSIZE),
		SFVAR(BG_AdrMask),
		SFVAR(BG0ScrollX),
		SFVAR(BG0ScrollY),
		SFVAR(BG1ScrollX),
		SFVAR(BG1ScrollY),

		SFARRAY(BGCHR8, (8 * 8 * 256)),
		SFARRAY(BGCHR16, (16 * 16 * 256)),
		SFARRAY16(BG_PriBuf, 1600),
		SFARRAY16(BG_LineBuf, 1600),

		SFVAR(BG_HAdjust),
		SFVAR(BG_VLINE),
		SFVAR(VLINEBG),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_BG", false);

	/* Derived cache is intentionally not serialized. */
	if (load) Sprite_ActiveDirty = 1;
	return ret;
}


void BG_Init(void)
{
	uint32_t i;
	memset(Sprite_Regs, 0, 0x800);
	Sprite_ActiveDirty = 1;
	memset(BG, 0, 0x8000);
	memset(BGCHR8, 0, 8*8*256);
	memset(BGCHR16, 0, 16*16*256);
	memset(BG_LineBuf, 0, 1600*2);
	for (i=0; i<0x12; i++)
		BG_Write(0xeb0800+i, 0);
	BG_CHREND = 0x8000;
}

uint8_t FASTCALL BG_Read(uint32_t adr)
{
	if ((adr>=0xeb0000)&&(adr<0xeb0400))
	{
		adr -= 0xeb0000;
#ifndef MSB_FIRST
		adr ^= 1;
#endif
		return Sprite_Regs[adr];
	}
	else if ((adr>=0xeb0800)&&(adr<0xeb0812))
		return BG_Regs[adr-0xeb0800];
	else if ((adr>=0xeb8000)&&(adr<0xec0000))
		return BG[adr-0xeb8000];
	return 0xff;
}

void FASTCALL BG_Write(uint32_t adr, uint8_t data)
{
	uint32_t bg16chr;
	int v  = 0;
	int s1 = (((BG_Regs[0x11]  &4)?2:1)-((BG_Regs[0x11]  &16)?1:0));
	int s2 = (((CRTC_Regs[0x29]&4)?2:1)-((CRTC_Regs[0x29]&16)?1:0));
	if ( !(BG_Regs[0x11]&16) ) v = ((BG_Regs[0x0f]>>s1)-(CRTC_Regs[0x0d]>>s2));
	if ((adr>=0xeb0000)&&(adr<0xeb0400))
	{
		adr &= 0x3ff;
#ifndef MSB_FIRST
		adr ^= 1;
#endif
		if (Sprite_Regs[adr] != data)
		{

			BG_HOST_SOURCE_BARRIER();
			uint16_t t0, t, *pw;
			Sprite_ActiveDirty = 1;

			v = BG_VLINE - 16 - v;
			/* get YPOS pointer (Sprite_Regs[] is little endian) */
			pw = (uint16_t *)(Sprite_Regs + (adr & 0x3f8) + 2);

#define UPDATE_TDL(t)				\
{						\
	int i;					\
	for (i = 0; i < 16; i++) {		\
		TextDirtyLine[(t)] = 1;		\
		(t) = ((t) + 1) & 0x3ff;	\
	}					\
}

			t = t0 = (*pw + v) & 0x3ff;
			UPDATE_TDL(t);

			Sprite_Regs[adr] = data;

			t = (*pw + v) & 0x3ff;
			if (t != t0) {
				UPDATE_TDL(t);
			}

		}
	}
	else if ((adr>=0xeb0800)&&(adr<0xeb0812))
	{
		adr -= 0xeb0800;
		if (BG_Regs[adr]==data) return;	/* return if no data is changed */
		BG_HOST_SOURCE_BARRIER();
		BG_Regs[adr] = data;
		switch(adr)
		{
		case 0x00:
		case 0x01:
			BG0ScrollX = (((uint32_t)BG_Regs[0x00]<<8)+BG_Regs[0x01])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;
		case 0x02:
		case 0x03:
			BG0ScrollY = (((uint32_t)BG_Regs[0x02]<<8)+BG_Regs[0x03])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;
		case 0x04:
		case 0x05:
			BG1ScrollX = (((uint32_t)BG_Regs[0x04]<<8)+BG_Regs[0x05])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;
		case 0x06:
		case 0x07:
			BG1ScrollY = (((uint32_t)BG_Regs[0x06]<<8)+BG_Regs[0x07])&BG_AdrMask;
			TVRAM_SetAllDirty();
			break;

		case 0x08:		/* BG On/Off Changed */
			TVRAM_SetAllDirty();
			break;

		case 0x0d:
			BG_HAdjust = ((int32_t)BG_Regs[0x0d] - (CRTC_HSTART + 4)) * 8; /* Isn't it necessary to divide the horizontal resolution by 1/2? (Tetris) */
			TVRAM_SetAllDirty();
			break;
		case 0x0f:
			BG_VLINE = ((int32_t)BG_Regs[0x0f] - CRTC_VSTART) / ((BG_Regs[0x11] & 4) ? 1 : 2); /* Difference when BG and other elements are misaligned */
			TVRAM_SetAllDirty();
			break;

		case 0x11:		/* BG ScreenRes Changed */
			if (data&3)
			{
				if ((BG_BG0TOP==0x4000)||(BG_BG1TOP==0x4000))
					BG_CHREND = 0x4000;
				else if ((BG_BG0TOP==0x6000)||(BG_BG1TOP==0x6000))
					BG_CHREND = 0x6000;
				else
					BG_CHREND = 0x8000;
			}
			else
				BG_CHREND = 0x2000;
			BG_CHRSIZE = ((data&3)?16:8);
			BG_AdrMask = ((data&3)?1023:511);
			BG_HAdjust = ((int32_t)BG_Regs[0x0d] - (CRTC_HSTART + 4)) * 8; /*Isn't it necessary to divide the horizontal resolution by 1/2? (Tetris) */
			BG_VLINE   = ((int32_t)BG_Regs[0x0f] - CRTC_VSTART) / ((BG_Regs[0x11] & 4) ? 1 : 2); /* Difference when BG and other elements are misaligned */
			break;
		case 0x09:		/* BG Plane Cfg Changed */
			TVRAM_SetAllDirty();
			if (data&0x08)
			{
				if (data&0x30)
				{
					BG_BG1TOP = 0x6000;
					BG_BG1END = 0x8000;
				}
				else
				{
					BG_BG1TOP = 0x4000;
					BG_BG1END = 0x6000;
				}
			}
			else
				BG_BG1TOP = BG_BG1END = 0;
			if (data&0x01)
			{
				if (data&0x06)
				{
					BG_BG0TOP = 0x6000;
					BG_BG0END = 0x8000;
				}
				else
				{
					BG_BG0TOP = 0x4000;
					BG_BG0END = 0x6000;
				}
			}
			else
				BG_BG0TOP = BG_BG0END = 0;
			if (BG_Regs[0x11]&3)
			{
				if ((BG_BG0TOP==0x4000)||(BG_BG1TOP==0x4000))
					BG_CHREND = 0x4000;
				else if ((BG_BG0TOP==0x6000)||(BG_BG1TOP==0x6000))
					BG_CHREND = 0x6000;
				else
					BG_CHREND = 0x8000;
			}
			break;
		case 0x0b:
			break;
		}
	}
	else if ((adr>=0xeb8000)&&(adr<0xec0000))
	{
		adr -= 0xeb8000;
		if (BG[adr]==data) return;			/* return if no data is changed */
		BG_HOST_SOURCE_BARRIER();
		BG[adr] = data;
		if (adr<0x2000)
		{
			BGCHR8[adr*2]   = data>>4;
			BGCHR8[adr*2+1] = data&15;
		}
		bg16chr = ((adr&3)*2)+((adr&0x3c)*4)+((adr&0x40)>>3)+((adr&0x7f80)*2);
		BGCHR16[bg16chr]   = data>>4;
		BGCHR16[bg16chr+1] = data&15;

		if (adr<BG_CHREND)						/* pattern area */
			TVRAM_SetAllDirty();
		if ((adr>=BG_BG1TOP)&&(adr<BG_BG1END))	/* BG1 MAP Area */
			TVRAM_SetAllDirty();
		if ((adr>=BG_BG0TOP)&&(adr<BG_BG0END))	/* BG0 MAP Area */
			TVRAM_SetAllDirty();
	}
}


struct SPRITECTRLTBL {
    uint16_t sprite_posx;
    uint16_t sprite_posy;
    uint16_t sprite_ctrl;
    uint8_t  sprite_ply;
    uint8_t  dummy;
} __attribute__ ((packed));
typedef struct SPRITECTRLTBL SPRITECTRLTBL_T;

/* Build 5.27:
 * Prepare visible sprites once per scanline instead of rescanning all 128
 * sprite registers independently for priorities 1/2/3.
 *
 * The prepared buckets preserve the exact original order (sprite #127 -> #0)
 * inside each priority group and cache all values required by the 16-pixel
 * inner draw loop.
 */
typedef struct SPRITE_LINE_ITEM {
    uint16_t t;
    uint16_t ctrl;
    uint16_t pri_key;
    uint8_t  y;
    uint8_t  pad;
} SPRITE_LINE_ITEM_T;

static INLINE void Sprite_RebuildActiveIndex(void)
{
    SPRITECTRLTBL_T *sct = (SPRITECTRLTBL_T *)Sprite_Regs;
    static uint8_t s_reported = 0;
    int n;

    Sprite_ActiveCount[0] = Sprite_ActiveCount[1] = Sprite_ActiveCount[2] = 0;
    for (n = 127; n >= 0; --n) {
        unsigned pri = sct[n].sprite_ply & 3u;
        if (pri >= 1 && pri <= 3)
            Sprite_ActiveIdx[pri - 1][Sprite_ActiveCount[pri - 1]++] = (uint8_t)n;
    }
    Sprite_ActiveDirty = 0;
    if (!s_reported) {
        s_reported = 1;
        printf("PX68K_BGSP: Build 5.37 active-sprite index cache ACTIVE pri=%u/%u/%u\n",
               (unsigned)Sprite_ActiveCount[0],
               (unsigned)Sprite_ActiveCount[1],
               (unsigned)Sprite_ActiveCount[2]);
    }
}

/* Build 5.53a: capture only the scanline-specific scalar state and the small
 * active-sprite index. Pattern/map/sprite bytes themselves stay in the shared
 * internal-SRAM hot set and are protected by BG_HOST_SOURCE_BARRIER() on
 * guest writes. */
int BG_CaptureHostLineState(BG_HOST_LINE_STATE *out, uint32_t vline_bg, int gd)
{
    if (!out)
        return 0;
    if (Sprite_ActiveDirty)
        Sprite_RebuildActiveIndex();

    out->bg0_top = BG_BG0TOP;
    out->bg1_top = BG_BG1TOP;
    out->bg0_scroll_x = BG0ScrollX;
    out->bg0_scroll_y = BG0ScrollY;
    out->bg1_scroll_x = BG1ScrollX;
    out->bg1_scroll_y = BG1ScrollY;
    out->h_adjust = BG_HAdjust;
    out->bg_vline = BG_VLINE;
    out->vline_bg = vline_bg;
    out->chr_size = BG_CHRSIZE;
    out->reg9 = BG_Regs[9];
    out->gd = (uint8_t)(gd ? 1 : 0);
    memcpy(out->sprite_count, Sprite_ActiveCount, sizeof(out->sprite_count));
    memcpy(out->sprite_idx, Sprite_ActiveIdx, sizeof(out->sprite_idx));
    return 1;
}

/* Build 5.37:
 * Sprite enable/priority changes far less often than scanline rendering.  Keep
 * three descending-order active-index lists and rebuild them only after the
 * guest writes Sprite_Regs.  A typical game then tests only the sprites that
 * are actually enabled instead of all 128 entries on every dirty line.
 */
static INLINE int Sprite_PrepareLine(SPRITE_LINE_ITEM_T buckets[3][128],
                                     uint8_t counts[3])
{
    SPRITECTRLTBL_T *sct = (SPRITECTRLTBL_T *)Sprite_Regs;
    int p;
    int total = 0;

    if (Sprite_ActiveDirty)
        Sprite_RebuildActiveIndex();

    counts[0] = counts[1] = counts[2] = 0;

    for (p = 0; p < 3; ++p) {
        uint32_t k;
        const uint32_t active = Sprite_ActiveCount[p];
        for (k = 0; k < active; ++k) {
            const unsigned n = Sprite_ActiveIdx[p][k];
            SPRITECTRLTBL_T *sctp = &sct[n];
            uint32_t t;
            uint32_t y;
            SPRITE_LINE_ITEM_T *it;

            t = (sctp->sprite_posx + BG_HAdjust) & 0x3ff;
            if (t >= (uint32_t)TextDotX + 16u)
                continue;

            y = sctp->sprite_posy & 0x3ff;
            y -= VLINEBG;
            y += BG_VLINE;
            y = -y;
            y += 16;

            if (y > 15)
                continue;

            it = &buckets[p][counts[p]++];
            it->t       = (uint16_t)t;
            it->ctrl    = sctp->sprite_ctrl;
            it->pri_key = (uint16_t)(n * 8);
            it->y       = (uint8_t)y;
            ++total;
        }
    }

    return total;
}

static INLINE void Sprite_DrawPrepared(const SPRITE_LINE_ITEM_T *items, int count)
{
    int k;

    for (k = 0; k < count; ++k) {
        const SPRITE_LINE_ITEM_T *it = &items[k];
        uint32_t t = it->t;
        uint16_t ctrl = it->ctrl;
        uint8_t *p;
        uint32_t pal_base;
        int i, d;

        if (ctrl < 0x4000) {
            p = &BGCHR16[((ctrl * 256) & 0xffff) + ((uint32_t)it->y * 16)];
            d = 1;
        } else if ((ctrl - 0x4000) & 0x8000) {
            p = &BGCHR16[((ctrl * 256) & 0xffff)
                       + ((((uint32_t)it->y * 16) & 0xff) ^ 0xf0) + 15];
            d = -1;
        } else if ((int16_t)ctrl >= 0x4000) {
            p = &BGCHR16[((ctrl * 256) & 0xffff) + ((uint32_t)it->y * 16) + 15];
            d = -1;
        } else {
            p = &BGCHR16[((ctrl << 8) & 0xffff)
                       + ((((uint32_t)it->y * 16) & 0xff) ^ 0xf0)];
            d = 1;
        }

        pal_base = (ctrl >> 4) & 0xf0;

        for (i = 0; i < 16; ++i, ++t, p += d) {
            uint32_t pal = *p & 0xf;
            if (pal) {
                pal |= pal_base;
                if (BG_PriBuf[t] >= it->pri_key) {
                    BG_LineBuf[t] = TextPal[pal];
                    Text_TrFlag[t] |= 2;
                    BG_PriBuf[t] = it->pri_key;
                }
            }
        }
    }
}

/* Build 5.27:
 * Pointer-only inner character loops.  The hot destination/flag buffers and
 * decoded character caches are internal SRAM since Build 5.25.
 */
#define BG_BLIT_GD(CNT, STEP) do {                                      \
    uint16_t *dst__ = &BG_LineBuf[1 + edi];                             \
    uint8_t  *tr__  = &Text_TrFlag[1 + edi];                            \
    int jj__;                                                           \
    for (jj__ = 0; jj__ < (CNT); ++jj__) {                              \
        uint8_t dat__ = (uint8_t)(*esi | pal_hi);                       \
        if (dat__ != 0 && ((dat__ & 0x0f) || !(*tr__ & 2))) {           \
            *dst__ = TextPal[dat__];                                    \
            *tr__ |= 2;                                                 \
        }                                                               \
        esi += (STEP);                                                   \
        ++dst__;                                                        \
        ++tr__;                                                         \
    }                                                                   \
    edi += (CNT);                                                       \
} while (0)

#define BG_BLIT_NG(CNT, STEP) do {                                      \
    uint16_t *dst__ = &BG_LineBuf[1 + edi];                             \
    uint8_t  *tr__  = &Text_TrFlag[1 + edi];                            \
    int jj__;                                                           \
    for (jj__ = 0; jj__ < (CNT); ++jj__) {                              \
        uint8_t pix__ = *esi & 0x0f;                                    \
        if (pix__) {                                                    \
            *dst__ = TextPal[(uint8_t)(pix__ | pal_hi)];                \
            *tr__ |= 2;                                                 \
        }                                                               \
        esi += (STEP);                                                   \
        ++dst__;                                                        \
        ++tr__;                                                         \
    }                                                                   \
    edi += (CNT);                                                       \
} while (0)

static void bg_drawline_loopx8(uint16_t BGTOP, uint32_t BGScrollX,
                               uint32_t BGScrollY, int32_t adjust, int ng)
{
    int i;
    uint32_t ebp = ((BGScrollY + VLINEBG - BG_VLINE) & 7) << 3;
    uint32_t edx = BGTOP + (((BGScrollY + VLINEBG - BG_VLINE) & 0x1f8) << 4);
    uint32_t edi = ((BGScrollX - adjust) & 7) ^ 15;
    uint32_t ecx = ((BGScrollX - adjust) & 0x1f8) >> 2;

    for (i = TextDotX >> 3; i >= 0; --i) {
        const uint8_t *mp = &BG[ecx + edx];
#if defined(ESP_PLATFORM) && !defined(MSB_FIRST)
        const uint16_t mw = *(const uint16_t *)mp; /* address is always even */
        uint8_t map = (uint8_t)mw;
        uint16_t si = (uint16_t)(mw >> 8) << 6;
#else
        uint8_t map = mp[0];
        uint16_t si = (uint16_t)mp[1] << 6;
#endif
        uint8_t pal_hi = (uint8_t)(map << 4);
        uint8_t *esi;
        int rev;

        if (map < 0x40) {
            esi = &BGCHR8[si + ebp];
            rev = 0;
        } else if ((map - 0x40) & 0x80) {
            esi = &BGCHR8[si + 0x3f - ebp];
            rev = 1;
        } else if ((int8_t)map >= 0x40) {
            esi = &BGCHR8[si + ebp + 7];
            rev = 1;
        } else {
            esi = &BGCHR8[si + 0x38 - ebp];
            rev = 0;
        }

        if (ng) {
            if (rev) BG_BLIT_NG(8, -1);
            else     BG_BLIT_NG(8, +1);
        } else {
            if (rev) BG_BLIT_GD(8, -1);
            else     BG_BLIT_GD(8, +1);
        }

        ecx = (ecx + 2) & 0x7f;
    }
}

static void bg_drawline_loopx16(uint16_t BGTOP, uint32_t BGScrollX,
                                uint32_t BGScrollY, int32_t adjust, int ng)
{
    int i;
    uint32_t ebp = ((BGScrollY + VLINEBG - BG_VLINE) & 15) << 4;
    uint32_t edx = BGTOP + (((BGScrollY + VLINEBG - BG_VLINE) & 0x3f0) << 3);
    uint32_t edi = ((BGScrollX - adjust) & 15) ^ 15;
    uint32_t ecx = ((BGScrollX - adjust) & 0x3f0) >> 3;

    for (i = TextDotX >> 4; i >= 0; --i) {
        const uint8_t *mp = &BG[ecx + edx];
#if defined(ESP_PLATFORM) && !defined(MSB_FIRST)
        const uint16_t mw = *(const uint16_t *)mp; /* address is always even */
        uint8_t map = (uint8_t)mw;
        uint16_t si = (uint16_t)(mw >> 8) << 8;
#else
        uint8_t map = mp[0];
        uint16_t si = (uint16_t)mp[1] << 8;
#endif
        uint8_t pal_hi = (uint8_t)(map << 4);
        uint8_t *esi;
        int rev;

        if (map < 0x40) {
            esi = &BGCHR16[si + ebp];
            rev = 0;
        } else if ((map - 0x40) & 0x80) {
            esi = &BGCHR16[si + 0xff - ebp];
            rev = 1;
        } else if ((int8_t)map >= 0x40) {
            esi = &BGCHR16[si + ebp + 15];
            rev = 1;
        } else {
            esi = &BGCHR16[si + 0xf0 - ebp];
            rev = 0;
        }

        if (ng) {
            if (rev) BG_BLIT_NG(16, -1);
            else     BG_BLIT_NG(16, +1);
        } else {
            if (rev) BG_BLIT_GD(16, -1);
            else     BG_BLIT_GD(16, +1);
        }

        ecx = (ecx + 2) & 0x7f;
    }
}

#undef BG_BLIT_GD
#undef BG_BLIT_NG

static INLINE void BG_DrawLineMcr8(uint16_t BGTOP, uint32_t BGScrollX,
                                   uint32_t BGScrollY)
{
    bg_drawline_loopx8(BGTOP, BGScrollX, BGScrollY, BG_HAdjust, 0);
}

static INLINE void BG_DrawLineMcr16(uint16_t BGTOP, uint32_t BGScrollX,
                                    uint32_t BGScrollY)
{
    bg_drawline_loopx16(BGTOP, BGScrollX, BGScrollY, BG_HAdjust, 0);
}

static INLINE void BG_DrawLineMcr8_ng(uint16_t BGTOP, uint32_t BGScrollX,
                                      uint32_t BGScrollY)
{
    bg_drawline_loopx8(BGTOP, BGScrollX, BGScrollY, BG_HAdjust, 1);
}

static INLINE void BG_DrawLineMcr16_ng(uint16_t BGTOP, uint32_t BGScrollX,
                                       uint32_t BGScrollY)
{
    bg_drawline_loopx16(BGTOP, BGScrollX, BGScrollY, 0, 1);
}

void FASTCALL BG_DrawLine(int opaq, int gd)
{
    int i;
    SPRITE_LINE_ITEM_T sprite_buckets[3][128];
    uint8_t sprite_counts[3];
    int visible_sprites;
    void (*func8)(uint16_t, uint32_t, uint32_t);
    void (*func16)(uint16_t, uint32_t, uint32_t);

    visible_sprites = Sprite_PrepareLine(sprite_buckets, sprite_counts);

    /* BG_PriBuf is only consulted by sprite pixels. */
    if (visible_sprites)
        memset(&BG_PriBuf[16], 0xff, (size_t)TextDotX * sizeof(BG_PriBuf[0]));

    if (opaq)
    {
        const uint16_t c = TextPal[0];
        if (c == 0) {
            memset(&BG_LineBuf[16], 0, (size_t)TextDotX * sizeof(BG_LineBuf[0]));
        } else {
            for (i = 16; i < TextDotX + 16; ++i)
                BG_LineBuf[i] = c;
        }
    }

    func8 = (gd) ? BG_DrawLineMcr8 : BG_DrawLineMcr8_ng;
    func16 = (gd) ? BG_DrawLineMcr16 : BG_DrawLineMcr16_ng;

    if (sprite_counts[0])
        Sprite_DrawPrepared(sprite_buckets[0], sprite_counts[0]);

    if ((BG_Regs[9] & 8) && (BG_CHRSIZE == 8))
        (*func8)(BG_BG1TOP, BG1ScrollX, BG1ScrollY);

    if (sprite_counts[1])
        Sprite_DrawPrepared(sprite_buckets[1], sprite_counts[1]);

    if (BG_Regs[9] & 1)
    {
        if (BG_CHRSIZE == 8)
            (*func8)(BG_BG0TOP, BG0ScrollX, BG0ScrollY);
        else
            (*func16)(BG_BG0TOP, BG0ScrollX, BG0ScrollY);
    }

    if (sprite_counts[2])
        Sprite_DrawPrepared(sprite_buckets[2], sprite_counts[2]);
}
