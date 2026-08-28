/*
 *  TVRAM.C - Text VRAM
 *  TODO: Transparent color processing and more
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Track text-VRAM generations so unchanged text rows do not trigger redundant host composition.
 * Layer8 Aug/17/2026
 */
#include	"common.h"
#include	"winx68k.h"
#include	"windraw.h"
#include	"bg.h"
#include	"crtc.h"
#include	"palette.h"
#include	"m68000.h"
#include	"tvram.h"
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
extern void tab5_guest_bus_post_write(uint32_t domain, uint32_t address, uint8_t value);
extern void tab5_guest_bus_post_tvram_reset(void);
#define TAB5_R57_POST(a,v) tab5_guest_bus_post_write(1u,(uint32_t)(a),(uint8_t)(v))
#else
#define TAB5_R57_POST(a,v) ((void)0)
#endif

uint8_t	TVRAM[0x80000];
#ifdef ESP_PLATFORM
/* R57E5: the old 1MiB fully-expanded TextDrawWork cache remains retired.
 * This 512KiB storage is owned by CPU0's ordered journal and now holds
 * render-ready 4-bit packed TEXT pixels (2 pixels/byte), not a raw TVRAM copy.
 * TextLineScratch remains only for the bounded pre-certification validator. */
static uint8_t R57TVRAMShadowStorage[0x80000];
static uint8_t TextLineScratch[1024];
static uint8_t TextExpandedDirty[1024];
static uint32_t TextLineScratchY=0xffffffffu;
#else
static uint8_t TextDrawWork[1024*1024];
#endif
extern uint8_t TextDirtyLine[1024];
extern uint8_t Text_TrFlag[1024];

/* pattern table */
static uint8_t TextDrawPattern[2048*4] __attribute__((aligned(4)));


#ifdef ESP_PLATFORM
/* BAT177NW10/R57E40: direct visible-span TEXT decode.  The old ESP path
 * expanded all 1024 physical pixels for every dirty scanline and then walked
 * the visible span a second time.  The common aligned/no-wrap path below
 * decodes only the visible 8-pixel blocks straight from the four TVRAM
 * bitplanes, using the already-existing TextDrawPattern table. */
static uint32_t s_tab5_text_fast_lines;
static uint32_t s_tab5_text_fallback_lines;
static uint32_t s_tab5_text_fast_blocks;
static uint8_t s_tab5_text_fast_ok;
/* R57E65: cheap CPU1-visible activity epoch.  This is not a correctness
 * signal; it merely lets the frame budget favor TEXT after real TVRAM writes. */
static uint32_t s_tab5_text_write_epoch;

uint32_t TVRAM_Tab5TextWriteEpoch(void)
{
    return __atomic_load_n(&s_tab5_text_write_epoch, __ATOMIC_RELAXED);
}

void TVRAM_Tab5TextFastStatsTake(uint32_t *fast_lines, uint32_t *fallback_lines,
                                 uint32_t *fast_blocks)
{
    if (fast_lines) *fast_lines = s_tab5_text_fast_lines;
    if (fallback_lines) *fallback_lines = s_tab5_text_fallback_lines;
    if (fast_blocks) *fast_blocks = s_tab5_text_fast_blocks;
    s_tab5_text_fast_lines = 0u;
    s_tab5_text_fallback_lines = 0u;
    s_tab5_text_fast_blocks = 0u;
}

static int TVRAM_Tab5TextFastSelfCheck(void)
{
#if defined(MSB_FIRST)
    return 0;
#else
    const uint32_t *lut = (const uint32_t *)(const void *)TextDrawPattern;
    for (uint32_t plane = 0u; plane < 4u; ++plane) {
        const uint32_t word_off = plane * 512u;
        const uint8_t bit = (uint8_t)(1u << plane);
        for (uint32_t pat = 0u; pat < 256u; ++pat) {
            union { uint32_t w[2]; uint8_t b[8]; } u;
            u.w[0] = lut[word_off + pat * 2u + 0u];
            u.w[1] = lut[word_off + pat * 2u + 1u];
            for (uint32_t k = 0u; k < 8u; ++k) {
                const uint8_t exp = (pat & (0x80u >> k)) ? bit : 0u;
                if (u.b[k] != exp) return 0;
            }
        }
    }
    return 1;
#endif
}

static INLINE void TVRAM_Tab5TextEmit8(uint32_t src_addr, uint32_t dst_off, int opaq)
{
    const uint32_t *lut = (const uint32_t *)(const void *)TextDrawPattern;
    const uint8_t p0 = TVRAM[src_addr];
    const uint8_t p1 = TVRAM[src_addr + 0x20000u];
    const uint8_t p2 = TVRAM[src_addr + 0x40000u];
    const uint8_t p3 = TVRAM[src_addr + 0x60000u];
    union { uint32_t w[2]; uint8_t b[8]; } px;
    px.w[0] = lut[p0 * 2u + 0u] | lut[512u + p1 * 2u + 0u] |
              lut[1024u + p2 * 2u + 0u] | lut[1536u + p3 * 2u + 0u];
    px.w[1] = lut[p0 * 2u + 1u] | lut[512u + p1 * 2u + 1u] |
              lut[1024u + p2 * 2u + 1u] | lut[1536u + p3 * 2u + 1u];

    if (opaq) {
        for (uint32_t k = 0u; k < 8u; ++k) {
            const uint8_t t = px.b[k];
            Text_TrFlag[dst_off + k] = t ? 1u : 0u;
            BG_LineBuf[dst_off + k] = TextPal[t];
        }
    } else {
        for (uint32_t k = 0u; k < 8u; ++k) {
            const uint8_t t = px.b[k];
            if (t) {
                Text_TrFlag[dst_off + k] |= 1u;
                BG_LineBuf[dst_off + k] = TextPal[t];
            }
        }
    }
}

/* BAT177NW18/R57E48: compact common TEXT decoder for the final 8px pipeline.
 * This is the same validated LUT/address transform as R57E40, but it emits
 * only palette indices.  No RGB565 and no layer-flag buffer are touched. */
int __attribute__((hot, optimize("O3"))) TVRAM_Tab5DecodeVisibleIndexLine(uint8_t *out, uint32_t width)
{
#if defined(MSB_FIRST)
    (void)out; (void)width;
    return 0;
#else
    if (!out || width == 0u || width != (uint32_t)TextDotX || width > 800u ||
        !s_tab5_text_fast_ok)
        return 0;

    uint32_t y = TextScrollY + VLINE;
    if ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu) y += VLINE;
    const uint32_t phys_y = y & 0x3ffu;
    const uint32_t x = TextScrollX & 0x3ffu;
    if ((x & 7u) != 0u || (width & 7u) != 0u || x + width > 1024u)
        return 0;

    const uint32_t *lut = (const uint32_t *)(const void *)TextDrawPattern;
    const uint32_t base = phys_y << 7;
    const uint32_t first_byte = x >> 3;
    const uint32_t blocks = width >> 3;
    for (uint32_t b = 0u; b < blocks; ++b) {
        const uint32_t a = (base + first_byte + b) ^ 1u;
        const uint8_t p0 = TVRAM[a];
        const uint8_t p1 = TVRAM[a + 0x20000u];
        const uint8_t p2 = TVRAM[a + 0x40000u];
        const uint8_t p3 = TVRAM[a + 0x60000u];
        union { uint32_t w[2]; uint8_t b[8]; } px;
        px.w[0] = lut[p0 * 2u + 0u] | lut[512u + p1 * 2u + 0u] |
                  lut[1024u + p2 * 2u + 0u] | lut[1536u + p3 * 2u + 0u];
        px.w[1] = lut[p0 * 2u + 1u] | lut[512u + p1 * 2u + 1u] |
                  lut[1024u + p2 * 2u + 1u] | lut[1536u + p3 * 2u + 1u];
        __builtin_memcpy(out + (b << 3), px.b, 8u);
    }
    return 1;
#endif
}

#endif

/* BAT177NW5/R57E35: classify legacy full-screen invalidations.  The counters
 * are diagnostic only; the rendering contract is unchanged. */
#ifdef ESP_PLATFORM
static uint32_t s_tab5_dirty_all_reason[TAB5_DIRTY_ALL_N];
#endif

void TVRAM_SetAllDirtyReason(uint32_t reason)
{
#ifdef ESP_PLATFORM
    if (reason >= TAB5_DIRTY_ALL_N) reason = TAB5_DIRTY_ALL_OTHER;
    TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_dirty_all_reason[reason], 1u);
#else
    (void)reason;
#endif
    memset(TextDirtyLine, 1, 1024);
}

void TVRAM_DebugDirtyAllTake(uint32_t *out, uint32_t count)
{
    if (!out) return;
    const uint32_t n = count < TAB5_DIRTY_ALL_N ? count : TAB5_DIRTY_ALL_N;
    for (uint32_t i = 0; i < n; ++i) {
#ifdef ESP_PLATFORM
        out[i] = __atomic_exchange_n(&s_tab5_dirty_all_reason[i], 0u, __ATOMIC_RELAXED);
#else
        out[i] = 0u;
#endif
    }
}

int TVRAM_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFARRAYN(TVRAM, 524288, "MEM_TVRAM"),
#ifndef ESP_PLATFORM
		SFARRAY(TextDrawWork, (1024 * 1024)),
#endif
		SFARRAY(TextDirtyLine, 1024),
		SFARRAY(Text_TrFlag, 1024),

		SFEND
	};
	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_TVRAM", false);

	if (load) {
		TVRAM_SetAllDirty();
#ifdef ESP_PLATFORM
        memset(TextExpandedDirty,1,sizeof(TextExpandedDirty));
        TextLineScratchY=0xffffffffu;
        /* A save-state load replaces TVRAM wholesale without journal events.
         * Disable CPU0 shadow ownership for this boot rather than render stale
         * state. A normal guest reset re-establishes ownership. */
        extern void tab5_guest_bus_invalidate_shadow(void);
        tab5_guest_bus_invalidate_shadow();
#endif
    }

	return ret;
}

void TVRAM_SetAllDirty(void)
{
    TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_OTHER);
}
void TVRAM_Cleanup(void)     { }

#ifdef ESP_PLATFORM
static void TVRAM_ExpandPhysicalLine(uint32_t y)
{
    y &= 1023u;
    if (TextLineScratchY==y && !TextExpandedDirty[y]) return;
    uint8_t *dst=TextLineScratch;
    const uint32_t base=y<<7;
    for (uint32_t b=0;b<128u;++b) {
        const uint32_t a=(base+b)^1u;
        const uint8_t p0=TVRAM[a];
        const uint8_t p1=TVRAM[a+0x20000u];
        const uint8_t p2=TVRAM[a+0x40000u];
        const uint8_t p3=TVRAM[a+0x60000u];
        for (uint32_t k=0;k<8u;++k) {
            const uint8_t m=(uint8_t)(0x80u>>k);
            dst[(b<<3)+k]=(uint8_t)(((p0&m)?1u:0u)|((p1&m)?2u:0u)|
                                      ((p2&m)?4u:0u)|((p3&m)?8u:0u));
        }
    }
    TextExpandedDirty[y]=0u;
    TextLineScratchY=y;
}

const uint8_t *TVRAM_GetExpandedLine(uint32_t y,uint32_t x,uint32_t width)
{
    y&=1023u; x&=1023u;
    if (!width || x+width>1024u) return NULL;
    TVRAM_ExpandPhysicalLine(y);
    return &TextLineScratch[x];
}

/* R57e1 production no longer keeps a 1MiB expanded cache.  The old full-buffer
 * accessor is retained only as an ABI marker; current Tab5 production paths use
 * TVRAM_GetExpandedLine(). */
const uint8_t *TVRAM_GetExpandedPixels(void)
{
    return NULL;
}

uint8_t *TVRAM_GetR57ShadowStorage(void)
{
    return R57TVRAMShadowStorage;
}
#endif

void TVRAM_Init(void)
{
	int i, j, bit;
	memset(TVRAM, 0, 0x80000);
#ifdef ESP_PLATFORM
    tab5_guest_bus_post_tvram_reset();
    memset(TextLineScratch,0,sizeof(TextLineScratch));
    memset(TextExpandedDirty,1,sizeof(TextExpandedDirty));
    TextLineScratchY=0xffffffffu;
#else
	memset(TextDrawWork, 0, 1024*1024);
#endif
	TVRAM_SetAllDirty();

	memset(TextDrawPattern, 0, 2048*4);
	for (i=0; i<256; i++)
	{
		for (j=0, bit=0x80; j<8; j++, bit>>=1)
		{
			if (i&bit)
         {
				TextDrawPattern[i*8+j     ] = 1;
				TextDrawPattern[i*8+j+2048] = 2;
				TextDrawPattern[i*8+j+4096] = 4;
				TextDrawPattern[i*8+j+6144] = 8;
			}
		}
	}
#ifdef ESP_PLATFORM
    s_tab5_text_fast_lines = s_tab5_text_fallback_lines = s_tab5_text_fast_blocks = 0u;
    s_tab5_text_fast_ok = (uint8_t)(TVRAM_Tab5TextFastSelfCheck() ? 1u : 0u);
    printf("PX68K_TEXTFAST_R57E40: visible-span 8px LUT self-check %s; aligned/no-wrap direct decode armed\n",
           s_tab5_text_fast_ok ? "PASS" : "FAIL-FALLBACK");
#endif
}

uint8_t FASTCALL TVRAM_Read(uint32_t adr)
{
	adr &= 0x7ffff;
#ifndef MSB_FIRST
	adr ^= 1;
#endif
	return TVRAM[adr];
}


static INLINE void TVRAM_WriteByte(uint32_t adr, uint8_t data)
{
	if (TVRAM[adr]!=data)
	{
		TextDirtyLine[(((adr&0x1ffff)/128)-TextScrollY)&1023] = 1;
#ifdef ESP_PLATFORM
        TextExpandedDirty[(adr&0x1ffffu)>>7]=1u;
        __atomic_add_fetch(&s_tab5_text_write_epoch, 1u, __ATOMIC_RELAXED);
#endif
		TVRAM[adr] = data;
		TAB5_R57_POST(adr, data);
	}
}

static INLINE void TVRAM_WriteByteMask(uint32_t adr, uint8_t data)
{
	data = (TVRAM[adr] & CRTC_Regs[0x2e + ((adr^1) & 1)]) | (data & (~CRTC_Regs[0x2e + ((adr ^ 1) & 1)]));
	if (TVRAM[adr] != data)
	{
		TextDirtyLine[(((adr&0x1ffff)/128)-TextScrollY)&1023] = 1;
#ifdef ESP_PLATFORM
        TextExpandedDirty[(adr&0x1ffffu)>>7]=1u;
        __atomic_add_fetch(&s_tab5_text_write_epoch, 1u, __ATOMIC_RELAXED);
#endif
		TVRAM[adr] = data;
		TAB5_R57_POST(adr, data);
	}
}

void FASTCALL TVRAM_Write(uint32_t adr, uint8_t data)
{
	adr &= 0x7ffff;
#ifndef MSB_FIRST
	adr ^= 1;
#endif
	if (CRTC_Regs[0x2a]&1)			/* Concurrent access */
	{
		adr &= 0x1ffff;
		if (CRTC_Regs[0x2a]&2)		/* Text Mask */
		{
			if (CRTC_Regs[0x2b]&0x10) TVRAM_WriteByteMask(adr        , data);
			if (CRTC_Regs[0x2b]&0x20) TVRAM_WriteByteMask(adr+0x20000, data);
			if (CRTC_Regs[0x2b]&0x40) TVRAM_WriteByteMask(adr+0x40000, data);
			if (CRTC_Regs[0x2b]&0x80) TVRAM_WriteByteMask(adr+0x60000, data);
		}
		else
		{
			if (CRTC_Regs[0x2b]&0x10) TVRAM_WriteByte(adr        , data);
			if (CRTC_Regs[0x2b]&0x20) TVRAM_WriteByte(adr+0x20000, data);
			if (CRTC_Regs[0x2b]&0x40) TVRAM_WriteByte(adr+0x40000, data);
			if (CRTC_Regs[0x2b]&0x80) TVRAM_WriteByte(adr+0x60000, data);
		}
	}
	else							 /* single access */
	{
		if (CRTC_Regs[0x2a]&2)		/* Text Mask */
			TVRAM_WriteByteMask(adr, data);
		else
			TVRAM_WriteByte(adr, data);
	}
#ifndef ESP_PLATFORM
	{
		uint32_t *ptr = (uint32_t *)TextDrawPattern;
		uint32_t tvram_addr = adr & 0x1ffff;
		uint32_t workadr = ((adr & 0x1ff80) + ((adr ^ 1) & 0x7f)) << 3;
		uint8_t pat = TVRAM[tvram_addr + 0x60000];
		uint32_t t0    = ptr[(pat * 2) + 1536];
		uint32_t t1    = ptr[(pat * 2 + 1) + 1536];

		pat         = TVRAM[tvram_addr + 0x40000];
		t0 |= ptr[(pat * 2) + 1024];
		t1 |= ptr[(pat * 2 + 1) + 1024];

		pat = TVRAM[tvram_addr + 0x20000];
		t0 |= ptr[(pat * 2) + 512];
		t1 |= ptr[(pat * 2 + 1) + 512];

		pat = TVRAM[tvram_addr];
		t0 |= ptr[(pat * 2)];
		t1 |= ptr[(pat * 2 + 1)];

		*((uint32_t *)&TextDrawWork[workadr]) = t0;
		*(((uint32_t *)(&TextDrawWork[workadr])) + 1) = t1;
	}
#endif
}

void FASTCALL TVRAM_RCUpdate(void)
{
	uint32_t adr = ((uint32_t)CRTC_Regs[0x2d]<<9);
#ifdef ESP_PLATFORM
    const uint32_t row=(adr>>7)&1023u;
    for (uint32_t i=0;i<4u;++i) TextExpandedDirty[(row+i)&1023u]=1u;
    TextLineScratchY=0xffffffffu;
    return;
#else

	/* XXX: BUG */
	uint32_t *ptr = (uint32_t *)TextDrawPattern;
	uint32_t *wptr = (uint32_t *)(TextDrawWork + (adr << 3));
	uint32_t t0, t1;
	uint32_t tadr;
	uint8_t pat;
	int i;

	for (i = 0; i < 512; i++, adr++)
   {
		tadr = adr ^ 1;

		pat = TVRAM[tadr + 0x60000];
		t0 = ptr[(pat * 2) + 1536];
		t1 = ptr[(pat * 2 + 1) + 1536];

		pat = TVRAM[tadr + 0x40000];
		t0 |= ptr[(pat * 2) + 1024];
		t1 |= ptr[(pat * 2 + 1) + 1024];

		pat = TVRAM[tadr + 0x20000];
		t0 |= ptr[(pat * 2) + 512];
		t1 |= ptr[(pat * 2 + 1) + 512];

		pat = TVRAM[tadr];
		t0 |= ptr[(pat * 2)];
		t1 |= ptr[(pat * 2 + 1)];

		*wptr++ = t0;
		*wptr++ = t1;
	}
#endif
}

void FASTCALL Text_DrawLine(int opaq)
{
	uint32_t addr;
	uint32_t x;
	uint32_t off = 16;
	uint32_t i;
	uint8_t t;
	uint32_t y = TextScrollY + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
#ifdef ESP_PLATFORM
    const uint32_t phys_y = y & 0x3ffu;
    x = TextScrollX & 0x3ffu;
    /* Common Tab5/X68000 path: 8-pixel aligned scroll, an 8-pixel multiple
     * visible width, and no 1024-pixel wrap.  Decode only the visible span
     * directly from the four bitplanes.  Every other geometry falls back to
     * the frozen legacy scratch-expansion path below. */
    if (s_tab5_text_fast_ok && ((x & 7u) == 0u) &&
        (((uint32_t)TextDotX & 7u) == 0u) &&
        (x + (uint32_t)TextDotX <= 1024u)) {
        const uint32_t base = phys_y << 7;
        const uint32_t first_byte = x >> 3;
        const uint32_t blocks = (uint32_t)TextDotX >> 3;
        for (uint32_t b = 0u; b < blocks; ++b) {
            const uint32_t a = (base + first_byte + b) ^ 1u;
            TVRAM_Tab5TextEmit8(a, off + (b << 3), opaq);
        }
        TAB5_RELEASE_DIAG_INC(s_tab5_text_fast_lines);
        TAB5_RELEASE_DIAG_ADD(s_tab5_text_fast_blocks, blocks);
        return;
    }
    TAB5_RELEASE_DIAG_INC(s_tab5_text_fallback_lines);
    TVRAM_ExpandPhysicalLine(phys_y);
    y = 0u; /* TextLineScratch is exactly one 1024-pixel physical row. */
#else
	y = (y & 0x3ff) << 10;
	x = TextScrollX & 0x3ff;
#endif

#ifdef ESP_PLATFORM
    /* x was already captured above for the fast-path eligibility test. */
#else
	x = TextScrollX & 0x3ff;
#endif
	addr = x + y;
	x = (x ^ 0x3ff) + 1;

	if (opaq) {
		for (i = 0; (i < TextDotX) && (x > 0); i++, x--, off++) {
			#ifdef ESP_PLATFORM
            t = TextLineScratch[addr++] & 0xf;
#else
            t = TextDrawWork[addr++] & 0xf;
#endif
			Text_TrFlag[off] = t ? 1 : 0;
			BG_LineBuf[off] = TextPal[t];
		}
		if (i++ != TextDotX) {
			for (; i < TextDotX; i++, off++) {
				BG_LineBuf[off] = TextPal[0];
				Text_TrFlag[off] = 0;
			}
		}
	} else {
		for (i = 0; (i < TextDotX) && (x > 0); i++, x--, off++) {
			#ifdef ESP_PLATFORM
            t = TextLineScratch[addr++] & 0xf;
#else
            t = TextDrawWork[addr++] & 0xf;
#endif
			if (t) {
				Text_TrFlag[off] |= 1;
				BG_LineBuf[off] = TextPal[t];
			}
		}
	}
}
