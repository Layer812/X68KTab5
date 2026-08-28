/*
 *  GVRAM.C - Graphic VRAM
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Track graphics generations and provide fast snapshot helpers so unchanged GVRAM does not force redundant CPU0/LCD work.
 * Layer8 Aug/17/2026
 */
#include	"common.h"
#include	"windraw.h"
#include	"winx68k.h"
#include	"crtc.h"
#include	"palette.h"
#include	"tvram.h"
#include	"gvram.h"
#include	"m68000.h"
#include	<string.h>

#ifdef ESP_PLATFORM
/* Build 6.14b2: async raw-GVRAM scanline DMA must observe the exact raster
 * snapshot.  Fast zero-pending test stays in this hot component; only a live
 * DMA read pays the cross-component barrier call. */
extern volatile uint32_t tab5_compose_gvram_dma_pending;
extern volatile uint32_t tab5_compose_gvram_cache_pending;
extern void tab5_compose_guest_gvram_barrier(void);
/* BAT177NW0: CPU1 NO-WAIT. Shared-source CPU0 raster paths are production-
 * quarantined, so guest writes never wait for host readers. */
#define GVRAM_HOST_SOURCE_BARRIER() do { } while (0)
#else
#define GVRAM_HOST_SOURCE_BARRIER() do { } while (0)
#endif

uint8_t	GVRAM[0x80000] __attribute__((aligned(64)));
volatile uint32_t GVRAM_RowGeneration[512];

static inline void gvram_row_generation_bump(uint32_t row)
{
#ifdef ESP_PLATFORM
    __atomic_add_fetch(&GVRAM_RowGeneration[row & 511u], 1u, __ATOMIC_RELEASE);
#else
    ++GVRAM_RowGeneration[row & 511u];
#endif
}

uint32_t GVRAM_RowGenerationGet(uint32_t row)
{
#ifdef ESP_PLATFORM
    return __atomic_load_n(&GVRAM_RowGeneration[row & 511u], __ATOMIC_ACQUIRE);
#else
    return GVRAM_RowGeneration[row & 511u];
#endif
}

static void gvram_row_generation_reset_all(void)
{
    /* Host caches survive guest-only reset.  Never recycle generation 1 here:
     * bump every physical row so a pre-reset decoded row cannot compare equal
     * after GVRAM has been cleared/reinitialized.  First boot naturally moves
     * BSS zero -> generation 1. */
    for (uint32_t i = 0; i < 512u; ++i)
        gvram_row_generation_bump(i);
}
/* Build 5.25: hot scanline scratch is defined by the app component so it
 * remains in internal SRAM instead of libpx68k.a external-BSS/PSRAM. */
extern uint16_t Grp_LineBuf[1024];
extern uint16_t Grp_LineBufSP[1024];
extern uint16_t Grp_LineBufSP2[1024];
extern uint16_t Grp_LineBufSP_Tr[1024];
extern uint16_t Pal16Adr[256];

/* Build 5.8: low-noise graphics activity counters for Tab5 bring-up. */
static uint32_t s_debug_write_count = 0;
static uint32_t s_debug_fast_clear_count = 0;
static uint32_t s_debug_last_addr = 0;
static uint8_t  s_debug_last_data = 0;
static uint8_t  s_debug_last_mode = 0;

uint32_t GVRAM_DebugWriteCount(void)      { return s_debug_write_count; }
uint32_t GVRAM_DebugFastClearCount(void)  { return s_debug_fast_clear_count; }
uint32_t GVRAM_DebugLastAddr(void)        { return s_debug_last_addr; }
uint8_t  GVRAM_DebugLastData(void)        { return s_debug_last_data; }
uint8_t  GVRAM_DebugLastMode(void)        { return s_debug_last_mode; }

int GVRAM_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFARRAYN(GVRAM, 524288, "MEM_GVRAM"),
		SFARRAY16(Grp_LineBuf, 1024),
		SFARRAY16(Grp_LineBufSP, 1024),
		SFARRAY16(Grp_LineBufSP2, 1024),
		SFARRAY16(Grp_LineBufSP_Tr, 1024),
		SFARRAY16(Pal16Adr, 256),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_GVRAM", false);

    if (load) {
        /* Host-only row cache is not serialized. Force every cached row stale. */
        for (uint32_t i = 0; i < 512u; ++i) gvram_row_generation_bump(i);
    }
	return ret;
}

#ifdef MSB_FIRST
#define GET_WORD_W8(src) (*(uint16_t *)(src))
#else
#define GET_WORD_W8(src) (*(uint8_t *)(src) | *((uint8_t *)(src) + 1) << 8)
#endif

void GVRAM_Init(void)
{
	int i;

    /* Guest-only reset can occur while host services remain alive.  Drain any
     * CPU0 cache readers before clearing the shared GVRAM backing store. */
    GVRAM_HOST_SOURCE_BARRIER();
	s_debug_write_count = 0;
	s_debug_fast_clear_count = 0;
	s_debug_last_addr = 0;
	s_debug_last_data = 0;
	s_debug_last_mode = 0;
    gvram_row_generation_reset_all();

	memset(GVRAM, 0, 0x80000);
	for (i=0; i<128; i++) /* For 16bit color palette address calculation */
	{
		Pal16Adr[i*2] = i*4;
		Pal16Adr[i*2+1] = i*4+1;
	}
}

void FASTCALL GVRAM_FastClear(void)
{
	GVRAM_HOST_SOURCE_BARRIER();
	s_debug_fast_clear_count++;
	uint32_t v = ((CRTC_Regs[0x29]&4)?512:256);
	uint32_t h = ((CRTC_Regs[0x29]&3)?512:256);

	uint16_t *p;
	uint32_t x, y, offset;

	uint32_t w[2];

	w[0] = h;
	w[1] = 0;

	if (((GrphScrollX[0] & 0x1ff) + w[0]) > 512) {
		w[1] = (GrphScrollX[0] & 0x1ff) + w[0] - 512;
		w[0] = 512 - (GrphScrollX[0] & 0x1ff);
	}

	for (y = 0; y < v; y++) {
        const uint32_t phys_row = (y + GrphScrollY[0]) & 0x1ffu;
		offset = phys_row << 10;
		p = (uint16_t *)(GVRAM + offset + ((GrphScrollX[0] & 0x1ff) * 2));

		for (x = 0; x < w[0]; x++) {
			*p++ &= CRTC_FastClrMask;
		}

		if (w[1] > 0) {
			p = (uint16_t *)(GVRAM + offset);
			for (x = 0; x < w[1]; x++) {
				*p++ &= CRTC_FastClrMask;
			}
		}
        gvram_row_generation_bump(phys_row);
	}
}

uint8_t FASTCALL GVRAM_Read(uint32_t adr)
{
	int type;

	adr &= 0x1fffff;

	if (CRTC_Regs[0x28] & 8)
		type = 4;
	else if (CRTC_Regs[0x28] & 4)
		type = 0;
	else
		type = (CRTC_Regs[0x28] & 3) + 1;

	switch (type)
	{
	case 0: /* 1024 dot, 16 colors */
		if ((adr & 1) == 0)
			return 0;

		if (adr & 0x100000)
		{
			if (adr & 0x400)
			{
				/* page 3 */
				adr = ((adr >> 1) & 0x7fc00) | (adr & 0x3ff);
				return (GVRAM[adr] >> 4);
			}
			else
			{
				/* page 2 */
				adr = ((adr >> 1) & 0x7fc00) | (adr & 0x3ff);
				return (GVRAM[adr] & 0x0f);
			}
		}
		else
		{
			if (adr & 0x400)
			{
				/* page 1 */
				adr = ((adr >> 1) & 0x7fc00) | (adr & 0x3ff);
				return (GVRAM[adr ^ 1] >> 4);
			}
			else
			{
				/* page 0 */
				adr = ((adr >> 1) & 0x7fc00) | (adr & 0x3ff);
				return (GVRAM[adr ^ 1] & 0x0f);
			}
		}
		break;

	case 1: /* 512 dot, 16 colors */
		if ((adr & 1) == 0)
			return 0;

		if (adr < 0x80000)
		{
			/* page 0: Low byte of word b0-b3 */
			return (GVRAM[adr ^ 1] & 0x0f);
		}

		if (adr < 0x100000)
		{
			/* page 1: Low byte of word b4-b7 */
			adr &= 0x7ffff;
			return (GVRAM[adr ^ 1] >> 4);
		}

		if (adr < 0x180000)
		{
			/* page 2: High byte of word b0-b3 */
			adr &= 0x7ffff;
			return (GVRAM[adr] & 0x0f);
		}

		/* page 3: High byte of word b4-b7 */
		adr &= 0x7ffff;
		return (GVRAM[adr] >> 4);

	case 2: /* 512 dot, 256 colors */
	case 3: /* unknown */
	    /* page 0 */
		if (adr < 0x80000)
		{
			if (adr & 1)
			{
				/* Low byte of word */
				return GVRAM[adr ^ 1];
			}
			return 0;
		}

		/* page 1 */
		if (adr < 0x100000)
		{
			adr &= 0x7ffff;
			if (adr & 1)
			{
				/* High byte of word */
				return GVRAM[adr];
			}
			return 0;
		}
#if 0
		else
		{
			/* bus error */
			BusErrFlag = 1;
			return 0xff;
		}
#endif
		break;
		

	case 4: /* 65536 */
		if (adr < 0x80000) {
			return GVRAM[adr ^ 1];
		}
#if 0
		else
		{
			/* bus error */
			BusErrFlag = 1;
			return 0xff;
		}
#endif
		break;
		
	}

	return 0;
}

void FASTCALL GVRAM_Write(uint32_t adr, uint8_t data)
{
	GVRAM_HOST_SOURCE_BARRIER();
	int line = 1023, scr = 0;
	uint32_t temp;
	int type;

	/* Count guest GVRAM accesses without logging each write. */
	s_debug_write_count++;
	s_debug_last_addr = adr & 0x1fffff;
	s_debug_last_data = data;

	adr &= 0x1fffff;

	if (CRTC_Regs[0x28] & 8)
		type = 4;
	else if (CRTC_Regs[0x28] & 4)
		type = 0;
	else
		type = (CRTC_Regs[0x28] & 3) + 1;

	s_debug_last_mode = (uint8_t)type;

	switch (type)
	{
	case 0: /* 1024 dot, 16 colors */
		if ((adr & 1) == 0)
			break;

		line = ((adr / 2048) - GrphScrollY[0]) & 1023;

		if (adr & 0x100000)
		{
			if (adr & 0x400)
			{
				adr = ((adr & 0xff800) >> 1) + (adr & 0x3ff);
				temp = GVRAM[adr] & 0x0f;
				temp |= (data & 0x0f) << 4;
				GVRAM[adr] = (uint8_t)temp;
			}
			else
			{
				adr = ((adr & 0xff800) >> 1) + (adr & 0x3ff);
				temp = GVRAM[adr] & 0xf0;
				temp |= data & 0x0f;
				GVRAM[adr] = (uint8_t)temp;
			}
		}
		else
		{
			if (adr & 0x400)
			{
				adr = ((adr & 0xff800) >> 1) + (adr & 0x3ff);
				temp = GVRAM[adr ^ 1] & 0x0f;
				temp |= (data & 0x0f) << 4;
				GVRAM[adr ^ 1] = (uint8_t)temp;
			}
			else
			{
				adr = ((adr & 0xff800) >> 1) + (adr & 0x3ff);
				temp = GVRAM[adr ^ 1] & 0xf0;
				temp |= data & 0x0f;
				GVRAM[adr ^ 1] = (uint8_t)temp;
			}
		}
		break;

	case 1: /* 16 colors */
		if ((adr & 1) == 0)
			break;

		scr = GrphScrollY[(adr >> 19) & 3];
		line = (((adr & 0x7ffff) >> 10) - scr) & 511;

		if (adr < 0x80000)
		{
			/* page 0: low byte of word b0-b3 */
			temp = (GVRAM[adr ^ 1] & 0xf0);
			temp |= (data & 0x0f);
			GVRAM[adr ^ 1] = (uint8_t)temp;
		}
		else if (adr < 0x100000)
		{
			/* page 1: low byte of word b4-b7 */
			adr &= 0x7ffff;
			temp = (GVRAM[adr ^ 1] & 0x0f);
			temp |= (data << 4);
			GVRAM[adr ^ 1] = (uint8_t)temp;
		}
		else if (adr < 0x180000)
		{
			/* page 2: high byte of word b0-b3 */
			adr &= 0x7ffff;
			temp = (GVRAM[adr] & 0xf0);
			temp |= (data & 0x0f);
			GVRAM[adr] = (uint8_t)temp;
		}
		else
		{
			/* page 3: high byte of word b4-b7 */
			adr &= 0x7ffff;
			temp = (GVRAM[adr] & 0x0f);
			temp |= (data << 4);
			GVRAM[adr] = (uint8_t)temp;
		}
		break;

	case 2: /* 256 colors */
	case 3: /* unknown */
		if ((adr & 1) == 0)
			break;

        const uint32_t phys_row_256 = (adr & 0x7ffffu) >> 10;
		if (adr < 0x100000)
		{
			scr = GrphScrollY[(adr >> 18) & 2];
			line = (((adr & 0x7ffff) >> 10) - scr) & 511;

			TextDirtyLine[line] = 1; /* When used like 32 colors, 4 sides */

			scr = GrphScrollY[((adr >> 18) & 2) + 1];
			line = (((adr & 0x7ffff) >> 10) - scr) & 511;

			/* page 0 */
			if (adr < 0x80000)
			{
				/* low byte of word */
				GVRAM[adr ^ 1] = (uint8_t)data;
			}
			/* page 1 */
			else
			{
				/* high byte of word */
				adr &= 0x7ffff;
				GVRAM[adr] = (uint8_t)data;
			}
            gvram_row_generation_bump(phys_row_256);
		}
#if 0
		/* TODO: */
		else
		{
			BusErrFlag = 1;
			return;
		}
#endif
		break;

	case 4: /* 65536 */
		if (adr < 0x80000)
		{
			line = (((adr & 0x7ffff) >> 10) - GrphScrollY[0]) & 511;
			GVRAM[adr ^ 1] = (uint8_t)data;
		}
#if 0
		/* TODO: */
		else
		{
			BusErrFlag = 1;
			return;
		}
#endif
		break;
	}

	TextDirtyLine[line] = 1;
}


/* Build 5.76: bulk backends for consecutive cpu_writemem24_word() accesses in
 * the 256-color GVRAM layout (GVRAM_Write type 2/3).  A 68000 word write
 * calls GVRAM_Write twice: the even-address high byte is ignored by this
 * display mode but marks dirty line 1023, and the odd-address low byte is the
 * actual pixel byte.  Collapse that invariant work while preserving the
 * final debug counters, GVRAM bytes, and every affected dirty-line bit.
 *
 * This helper intentionally refuses other GVRAM modes.  The Musashi stream
 * engine then falls back to the authoritative per-word path. */
static inline int gvram_word_stream256_prepare(uint32_t adr, uint32_t count,
                                                uint32_t *rel_out, int *type_out)
{
    uint32_t rel;
    int type;

    if (!count || (adr & 1u)) return 0;

    if (CRTC_Regs[0x28] & 8)
        type = 4;
    else if (CRTC_Regs[0x28] & 4)
        type = 0;
    else
        type = (CRTC_Regs[0x28] & 3) + 1;
    if (type != 2 && type != 3) return 0;

    rel = adr & 0x1fffffu;
    if (rel >= 0x100000u) return 0;
    if (count > ((0x100000u - rel) >> 1)) return 0;

    *rel_out = rel;
    *type_out = type;
    return 1;
}

static inline void gvram_word_stream256_store_low(uint32_t odd, uint8_t lo)
{
    int scr, line;

    scr = GrphScrollY[(odd >> 18) & 2u];
    line = (((odd & 0x7ffffu) >> 10) - scr) & 511;
    TextDirtyLine[line] = 1;

    scr = GrphScrollY[((odd >> 18) & 2u) + 1u];
    line = (((odd & 0x7ffffu) >> 10) - scr) & 511;

    if (odd < 0x80000u)
        GVRAM[odd ^ 1u] = lo;
    else
        GVRAM[odd & 0x7ffffu] = lo;

    gvram_row_generation_bump((odd & 0x7ffffu) >> 10);
    TextDirtyLine[line] = 1;
}

/* Build 5.76: repeated-register source backend used by the generalized
 * scheduler-bounded MOVE stream engine. */
uint32_t GVRAM_WriteWordRepeat256(uint32_t adr, uint16_t data, uint32_t count)
{
    uint32_t rel, last, i;
    uint8_t lo;
    int type;

    if (!gvram_word_stream256_prepare(adr, count, &rel, &type)) return 0;
    GVRAM_HOST_SOURCE_BARRIER();
    lo = (uint8_t)data;

    /* The even-byte half of every normal word write reaches GVRAM_Write(),
     * takes the even-address early exit, and sets dirty line 1023. */
    TextDirtyLine[1023] = 1;

    for (i = 0; i < count; ++i)
        gvram_word_stream256_store_low(rel + (i << 1) + 1u, lo);

    s_debug_write_count += count << 1;
    last = rel + ((count - 1u) << 1) + 1u;
    s_debug_last_addr = last;
    s_debug_last_data = lo;
    s_debug_last_mode = (uint8_t)type;
    return count;
}

/* Build 5.76: ordinary-RAM (An)+ source backend.  PX68K stores RAM words in
 * a host-native 16-bit representation, so memcpy of each aligned guest word
 * exactly matches m68k_read_memory_16() without invoking the wrapper. */
uint32_t GVRAM_WriteWordCopy256(uint32_t adr, const uint8_t *src_native_words,
                                uint32_t count, uint16_t *last_word)
{
    uint32_t rel, last, i;
    uint16_t w = 0;
    uint8_t lo = 0;
    int type;

    if (!src_native_words || !last_word) return 0;
    if (!gvram_word_stream256_prepare(adr, count, &rel, &type)) return 0;
    GVRAM_HOST_SOURCE_BARRIER();

    TextDirtyLine[1023] = 1;

    for (i = 0; i < count; ++i)
    {
        __builtin_memcpy(&w, src_native_words + (i << 1), sizeof(w));
        lo = (uint8_t)w;
        gvram_word_stream256_store_low(rel + (i << 1) + 1u, lo);
    }

    s_debug_write_count += count << 1;
    last = rel + ((count - 1u) << 1) + 1u;
    s_debug_last_addr = last;
    s_debug_last_data = lo;
    s_debug_last_mode = (uint8_t)type;
    *last_word = w;
    return count;
}


/* P4 PIE/XespV backend for the same native-word stream.
 *
 * This remains backend-only: the 68000 stream detector,
 * scheduler boundary, PC/A-register updates and MOVE flags remain in Musashi.
 * The P4 helper replaces only the host memory merge inside an already-proven
 * GVRAM stream chunk.
 *
 * A 128-bit vector covers eight guest words.  In 256-colour mode only the low
 * byte of each source word is visible in GVRAM; the companion byte belongs to
 * another page and must be preserved.  The assembly helper therefore performs
 * a 128-bit read/modify/write with an alternating-byte mask rather than a raw
 * memcpy.  Scalar prefix/tail handle natural alignment and keep the vector
 * helper strictly on aligned 16-byte accesses.
 */
#ifdef ESP_PLATFORM
extern void tab5_xespv_merge_wordblocks(uint8_t *dst, const uint8_t *src,
                                        uint32_t blocks, uint32_t odd_lane,
                                        const uint8_t *even_mask);
extern void tab5_xespv_fill_wordblocks(uint8_t *dst, const uint8_t *value_ptr,
                                       uint32_t blocks, uint32_t odd_lane,
                                       const uint8_t *even_mask);
#endif

static uint8_t s_tab5_xespv_even_mask[16] __attribute__((aligned(16))) = {
    0xff,0x00,0xff,0x00,0xff,0x00,0xff,0x00,
    0xff,0x00,0xff,0x00,0xff,0x00,0xff,0x00
};

static inline void gvram_word_stream256_mark_low(uint32_t odd)
{
    int scr, line;
    scr = GrphScrollY[(odd >> 18) & 2u];
    line = (((odd & 0x7ffffu) >> 10) - scr) & 511;
    TextDirtyLine[line] = 1;
    scr = GrphScrollY[((odd >> 18) & 2u) + 1u];
    line = (((odd & 0x7ffffu) >> 10) - scr) & 511;
    TextDirtyLine[line] = 1;
}

/* A stream chunk is <=24 words, therefore repeated words normally touch one
 * dirty line and at most one boundary.  Mark each unique page/line segment
 * once instead of repeating the same two stores for every vectorized word. */
static void gvram_word_stream256_mark_span(uint32_t rel, uint32_t count)
{
    uint32_t i = 0;
    while (i < count)
    {
        const uint32_t odd = rel + (i << 1) + 1u;
        const uint32_t key_line = (odd & 0x7ffffu) >> 10;
        const uint32_t key_page = (odd >> 18) & 2u;
        gvram_word_stream256_mark_low(odd);
        gvram_row_generation_bump(key_line);
        ++i;
        while (i < count)
        {
            const uint32_t next = rel + (i << 1) + 1u;
            if (((next & 0x7ffffu) >> 10) != key_line ||
                (((next >> 18) & 2u) != key_page))
                break;
            ++i;
        }
    }
}

/* Return the number of vector blocks and the scalar prefix required to align
 * the host destination.  For copy streams src and dst advance together, so
 * they can both become 16-byte aligned only when their modulo-16 offsets
 * match.  Never vectorize across the 0x80000 host-page alias boundary. */
static uint32_t gvram_word_stream256_p4_plan(uint32_t rel,
                                             const uint8_t *src,
                                             uint32_t count,
                                             int require_src_align,
                                             uint32_t *prefix_out,
                                             uint32_t *odd_lane_out)
{
    uint8_t *base;
    uint32_t prefix, remaining, blocks;
    const uint32_t end = rel + (count << 1);

    if (count < 8u) return 0;
    if ((rel < 0x80000u && end > 0x80000u) ||
        (rel >= 0x80000u && end > 0x100000u))
        return 0;

    base = GVRAM + (rel & 0x7ffffu);
    for (prefix = 0; prefix < count && prefix < 8u; ++prefix)
    {
        if ((((uintptr_t)(base + (prefix << 1))) & 15u) == 0u)
        {
            if (!require_src_align ||
                ((((uintptr_t)(src + (prefix << 1))) & 15u) == 0u))
                break;
        }
    }
    if (prefix >= count || prefix >= 8u) return 0;
    remaining = count - prefix;
    blocks = remaining >> 3;
    if (!blocks) return 0;

    *prefix_out = prefix;
    *odd_lane_out = (rel >= 0x80000u) ? 1u : 0u;
    return blocks;
}

int GVRAM_WriteWordRepeat256P4Eligible(uint32_t adr, uint32_t count)
{
#ifdef ESP_PLATFORM
    uint32_t rel, prefix, odd_lane;
    int type;
    if (!gvram_word_stream256_prepare(adr, count, &rel, &type)) return 0;
    return gvram_word_stream256_p4_plan(rel, NULL, count, 0,
                                        &prefix, &odd_lane) != 0u;
#else
    (void)adr; (void)count;
    return 0;
#endif
}

int GVRAM_WriteWordCopy256P4Eligible(uint32_t adr, const uint8_t *src_native_words,
                                     uint32_t count)
{
#ifdef ESP_PLATFORM
    uint32_t rel, prefix, odd_lane;
    int type;
    uint32_t blocks;
    if (!src_native_words) return 0;
    if (!gvram_word_stream256_prepare(adr, count, &rel, &type)) return 0;
    blocks = gvram_word_stream256_p4_plan(rel, src_native_words, count, 1,
                                          &prefix, &odd_lane);
    /* Build 5.80a: GCC/binutils 14.2.0+20241119 rejects the PIE SAR operand
     * forms needed by 5.80 to shift even source bytes into odd destination
     * lanes.  Keep copy on the shift-free even lane only; odd-lane copy
     * transparently remains on the proven scalar stream backend. */
    return blocks != 0u && odd_lane == 0u;
#else
    (void)adr; (void)src_native_words; (void)count;
    return 0;
#endif
}

uint32_t GVRAM_WriteWordRepeat256P4(uint32_t adr, uint16_t data, uint32_t count,
                                    uint32_t *pie_words)
{
#ifdef ESP_PLATFORM
    uint32_t rel, prefix, odd_lane, blocks, vec_words, i, tail_start, last;
    uint8_t lo = (uint8_t)data;
    uint8_t *vec_dst;
    int type;

    if (pie_words) *pie_words = 0;
    if (!gvram_word_stream256_prepare(adr, count, &rel, &type)) return 0;
    blocks = gvram_word_stream256_p4_plan(rel, NULL, count, 0,
                                          &prefix, &odd_lane);
    if (!blocks) return 0;
    GVRAM_HOST_SOURCE_BARRIER();
    vec_words = blocks << 3;

    TextDirtyLine[1023] = 1;
    for (i = 0; i < prefix; ++i)
        gvram_word_stream256_store_low(rel + (i << 1) + 1u, lo);

    vec_dst = GVRAM + ((rel & 0x7ffffu) + (prefix << 1));
    tab5_xespv_fill_wordblocks(vec_dst, &lo, blocks, odd_lane,
                               s_tab5_xespv_even_mask);
    gvram_word_stream256_mark_span(rel + (prefix << 1), vec_words);

    tail_start = prefix + vec_words;
    for (i = tail_start; i < count; ++i)
        gvram_word_stream256_store_low(rel + (i << 1) + 1u, lo);

    s_debug_write_count += count << 1;
    last = rel + ((count - 1u) << 1) + 1u;
    s_debug_last_addr = last;
    s_debug_last_data = lo;
    s_debug_last_mode = (uint8_t)type;
    if (pie_words) *pie_words = vec_words;
    return count;
#else
    (void)adr; (void)data; (void)count;
    if (pie_words) *pie_words = 0;
    return 0;
#endif
}

uint32_t GVRAM_WriteWordCopy256P4(uint32_t adr, const uint8_t *src_native_words,
                                  uint32_t count, uint16_t *last_word,
                                  uint32_t *pie_words)
{
#ifdef ESP_PLATFORM
    uint32_t rel, prefix, odd_lane, blocks, vec_words, i, tail_start, last;
    uint16_t w = 0;
    uint8_t lo = 0;
    uint8_t *vec_dst;
    const uint8_t *vec_src;
    int type;

    if (pie_words) *pie_words = 0;
    if (!src_native_words || !last_word) return 0;
    if (!gvram_word_stream256_prepare(adr, count, &rel, &type)) return 0;
    blocks = gvram_word_stream256_p4_plan(rel, src_native_words, count, 1,
                                          &prefix, &odd_lane);
    if (!blocks || odd_lane != 0u) return 0;
    GVRAM_HOST_SOURCE_BARRIER();
    vec_words = blocks << 3;

    TextDirtyLine[1023] = 1;
    for (i = 0; i < prefix; ++i)
    {
        __builtin_memcpy(&w, src_native_words + (i << 1), sizeof(w));
        lo = (uint8_t)w;
        gvram_word_stream256_store_low(rel + (i << 1) + 1u, lo);
    }

    vec_dst = GVRAM + ((rel & 0x7ffffu) + (prefix << 1));
    vec_src = src_native_words + (prefix << 1);
    tab5_xespv_merge_wordblocks(vec_dst, vec_src, blocks, odd_lane,
                                s_tab5_xespv_even_mask);
    gvram_word_stream256_mark_span(rel + (prefix << 1), vec_words);

    tail_start = prefix + vec_words;
    for (i = tail_start; i < count; ++i)
    {
        __builtin_memcpy(&w, src_native_words + (i << 1), sizeof(w));
        lo = (uint8_t)w;
        gvram_word_stream256_store_low(rel + (i << 1) + 1u, lo);
    }

    /* If the vector run ended exactly at count, recover the architectural
     * final source word for MOVE.W N/Z and debug-last-data. */
    __builtin_memcpy(&w, src_native_words + ((count - 1u) << 1), sizeof(w));
    lo = (uint8_t)w;

    s_debug_write_count += count << 1;
    last = rel + ((count - 1u) << 1) + 1u;
    s_debug_last_addr = last;
    s_debug_last_data = lo;
    s_debug_last_mode = (uint8_t)type;
    *last_word = w;
    if (pie_words) *pie_words = vec_words;
    return count;
#else
    (void)adr; (void)src_native_words; (void)count; (void)last_word;
    if (pie_words) *pie_words = 0;
    return 0;
#endif
}

int GVRAM_P4StreamSelfcheck(void)
{
#ifdef ESP_PLATFORM
    static uint8_t src[32] __attribute__((aligned(16)));
    static uint8_t got[32] __attribute__((aligned(16)));
    static uint8_t ref[32] __attribute__((aligned(16)));
    uint8_t value = 0xa6u;
    unsigned i, odd;

    for (i = 0; i < sizeof(src); ++i) src[i] = (uint8_t)(0x21u + 13u * i);
    for (odd = 0; odd < 2; ++odd)
    {
        for (i = 0; i < sizeof(got); ++i)
            got[i] = ref[i] = (uint8_t)(0xe3u ^ (7u * i));
        if (odd == 0u)
        {
            for (i = 0; i < 8; ++i)
                ref[i << 1] = src[i << 1];
            tab5_xespv_merge_wordblocks(got, src, 1, 0,
                                        s_tab5_xespv_even_mask);
            if (memcmp(got, ref, 16) != 0) return 0;
        }

        for (i = 0; i < sizeof(got); ++i)
            got[i] = ref[i] = (uint8_t)(0x59u + 5u * i);
        for (i = 0; i < 8; ++i)
            ref[(i << 1) + odd] = value;
        tab5_xespv_fill_wordblocks(got, &value, 1, odd,
                                   s_tab5_xespv_even_mask);
        if (memcmp(got, ref, 16) != 0) return 0;
    }
    return 1;
#else
    return 0;
#endif
}


/* Build 5.78: GVRAM (An)+ -> GVRAM (Am)+ backend for the generalized
 * MOVE.W stream engine.  This is deliberately a forward sequential loop,
 * not memcpy/memmove: source and destination can alias through the X68000
 * page mapping, and normal 68000 semantics read each source word before the
 * corresponding destination word is written.  Keeping that order makes
 * overlapping copies exact while still removing Musashi dispatch, memory
 * wrappers, address decoding and per-word GVRAM_Write() calls.
 *
 * In 256-color mode a word read at an even GVRAM guest address returns a
 * zero high byte and one mapped pixel byte as the low byte.  Relative source
 * addresses >= 1 MiB read as zero in the authoritative GVRAM_Read() path. */
uint32_t GVRAM_CopyWordStream256(uint32_t src_adr, uint32_t dst_adr,
                                 uint32_t count, uint16_t *last_word)
{
    uint32_t src_rel, dst_rel, last, i;
    uint16_t w = 0;
    uint8_t lo = 0;
    int type;

    if (!last_word || !count || (src_adr & 1u)) return 0;
    if (!gvram_word_stream256_prepare(dst_adr, count, &dst_rel, &type)) return 0;
    GVRAM_HOST_SOURCE_BARRIER();

    src_rel = src_adr & 0x1fffffu;
    if (count > ((0x200000u - src_rel) >> 1)) return 0;

    TextDirtyLine[1023] = 1;

    for (i = 0; i < count; ++i)
    {
        const uint32_t se = src_rel + (i << 1);
        const uint32_t so = se + 1u;

        if (se < 0x80000u)
            lo = GVRAM[so ^ 1u];
        else if (se < 0x100000u)
            lo = GVRAM[so & 0x7ffffu];
        else
            lo = 0;

        w = (uint16_t)lo;
        gvram_word_stream256_store_low(dst_rel + (i << 1) + 1u, lo);
    }

    s_debug_write_count += count << 1;
    last = dst_rel + ((count - 1u) << 1) + 1u;
    s_debug_last_addr = last;
    s_debug_last_data = lo;
    s_debug_last_mode = (uint8_t)type;
    *last_word = w;
    return count;
}


/*
 *   From here on, the screen will be expanded line by line.
 */
/* BAT177NW14/R57E44: exact 65K GRP decode + GBT selector fusion.
 *
 * The R57E43 profile shows GRP65K and the final GBT selector each consume
 * ~36-38 us/dirty line. The historical common path first materializes the
 * entire 512-pixel Grp_LineBuf, then immediately walks it again in the GBT
 * selector. This path consumes the raw 65K GVRAM word once and resolves the
 * final RGB565 pixel in the same loop.
 *
 * Palette semantics stay exact. The two Pal_Regs components selected by
 * Pal16Adr are cached in two 256-entry internal tables and rebuilt whenever
 * Pal_DebugVisualGeneration() changes (graphics palette, contrast, state
 * restore). R57E48 further replaces the final Pal16[] random lookup with an
 * exhaustively validated 32x3 RGB565 channel cache. */
static uint16_t s_grp16_gbt_lo[256] __attribute__((aligned(16)));
static uint16_t s_grp16_gbt_hi[256] __attribute__((aligned(16)));
/* BAT177NW18/R57E48 FINAL: Pal16[65536] lives in the component's large BSS
 * and may be external.  RGB565 conversion is exactly separable into the
 * 5-bit R/G/B fields plus the X68000 intensity bit.  Cache only 32 entries
 * per channel in hot internal BSS and remove the random 128KiB Pal16 lookup
 * from every 65K graphics pixel. */
static uint16_t s_grp16_rgb_r[32] __attribute__((aligned(16)));
static uint16_t s_grp16_rgb_g[32] __attribute__((aligned(16)));
static uint16_t s_grp16_rgb_b[32] __attribute__((aligned(16)));
static uint16_t s_grp16_rgb_fallback;
static uint16_t s_grp16_rgb_ibit;
static uint8_t s_grp16_rgb_contrast = 0xffu;
static uint8_t s_grp16_rgb_reported = 0u;
static uint32_t s_grp16_gbt_generation = 0xffffffffu;
static uint32_t s_grp16_gbt_cache_rebuilds;
static uint32_t s_grp16_gbt_cache_failures;
static int s_grp16_gbt_ok;

static inline uint16_t grp16_code_to_rgb(uint16_t code)
{
    uint16_t rgb = (uint16_t)(
        s_grp16_rgb_r[(code >> 6) & 31u] |
        s_grp16_rgb_g[(code >> 11) & 31u] |
        s_grp16_rgb_b[(code >> 1) & 31u]);
    /* Pal_ChangeContrast preserves the dimmest non-black color by forcing
     * B[0] when a nonzero RGB code scales entirely to zero. */
    if ((code & 0xfffeu) != 0u && rgb == 0u)
        rgb = s_grp16_rgb_fallback;
    if (code & 1u)
        rgb |= s_grp16_rgb_ibit;
    return rgb;
}

static int grp16_rgb_cache_rebuild(uint8_t contrast)
{
    /* At contrast zero every channel contribution is zero; only the legacy
     * non-black fallback and intensity bit survive. */
    if (contrast == 0u) {
        memset(s_grp16_rgb_r, 0, sizeof(s_grp16_rgb_r));
        memset(s_grp16_rgb_g, 0, sizeof(s_grp16_rgb_g));
        memset(s_grp16_rgb_b, 0, sizeof(s_grp16_rgb_b));
    } else {
        /* A max-blue or max-red anchor guarantees the historical global
         * "nonblack became zero" fallback cannot contaminate the channel
         * being extracted.  RGB565 masks are fixed by WinDraw_Init(). */
        for (uint32_t x = 0u; x < 32u; ++x) {
            s_grp16_rgb_r[x] = (uint16_t)(
                Pal16[(x << 6) | (31u << 1)] & WinDraw_Pal16R);
            s_grp16_rgb_g[x] = (uint16_t)(
                Pal16[(x << 11) | (31u << 1)] & WinDraw_Pal16G);
            s_grp16_rgb_b[x] = (uint16_t)(
                Pal16[(31u << 6) | (x << 1)] & WinDraw_Pal16B);
        }
    }
    s_grp16_rgb_fallback = Pal16[2u];
    s_grp16_rgb_ibit = Pal16[1u];

    /* Full sequential validation is paid only when effective contrast
     * changes (normally once at startup).  It proves the 96-entry reduction
     * against the authoritative Pal16 table for every possible color code. */
    for (uint32_t code = 0u; code < 65536u; ++code) {
        if (grp16_code_to_rgb((uint16_t)code) != Pal16[code]) {
            ++s_grp16_gbt_cache_failures;
            printf("PX68K_RGB5CACHE_R57E48: exhaustive code->RGB565 validation FAIL code=%04lX contrast=%u; fallback\n",
                   (unsigned long)code, (unsigned)contrast);
            return 0;
        }
    }
    s_grp16_rgb_contrast = contrast;
    if (!s_grp16_rgb_reported) {
        printf("PX68K_RGB5CACHE_R57E48: exhaustive 65536 code->RGB565 validation PASS contrast=%u; 32x3 channel cache armed\n",
               (unsigned)contrast);
        s_grp16_rgb_reported = 1u;
    }
    return 1;
}

static int grp16_gbt_cache_rebuild(void)
{
    const uint32_t gen = Pal_DebugVisualGeneration();
    const uint8_t contrast = Pal_DebugEffectiveContrast();
    for (uint32_t i = 0; i < 256u; ++i) {
        s_grp16_gbt_lo[i] = (uint16_t)Pal_Regs[Pal16Adr[i]];
        s_grp16_gbt_hi[i] = (uint16_t)((uint16_t)Pal_Regs[Pal16Adr[i] + 2u] << 8);
    }
    if (s_grp16_rgb_contrast != contrast && !grp16_rgb_cache_rebuild(contrast)) {
        s_grp16_gbt_ok = 0;
        return 0;
    }
    /* Component-wise validation is sufficient for raw-word -> 16-bit color
     * code: the legacy decoder forms exactly lo|hi.  code -> RGB565 was
     * exhaustively certified above for the active contrast. */
    for (uint32_t i = 0; i < 256u; ++i) {
        if (s_grp16_gbt_lo[i] != (uint16_t)Pal_Regs[Pal16Adr[i]] ||
            s_grp16_gbt_hi[i] != (uint16_t)((uint16_t)Pal_Regs[Pal16Adr[i] + 2u] << 8)) {
            ++s_grp16_gbt_cache_failures;
            s_grp16_gbt_ok = 0;
            return 0;
        }
    }
    s_grp16_gbt_generation = gen;
    ++s_grp16_gbt_cache_rebuilds;
    return 1;
}

static inline uint16_t grp16_gbt_decode(uint16_t raw)
{
    if (raw == 0u) return 0u;
    const uint16_t code = (uint16_t)(s_grp16_gbt_lo[raw & 0xffu] |
                                     s_grp16_gbt_hi[(raw >> 8) & 0xffu]);
    return grp16_code_to_rgb(code);
}

static inline uint16_t grp16_gbt_select(uint16_t g, uint16_t bt, uint8_t f,
                                         const uint8_t mode[4])
{
    f &= 3u;
    if (f == 0u) return g;
    if (mode[f] == 1u) return bt ? bt : g; /* BG/TEXT candidate above GRP */
    return g ? g : bt;                    /* GRP above BG/TEXT candidate */
}

int Grp_DrawLine16GBT_SelfCheck(void)
{
    /* Exhaustively validate the per-line priority reduction against the
     * historical R57E34 structural rule for all G/B/T priorities, flags and
     * zero/nonzero source combinations. */
    for (uint32_t gp = 0; gp < 4u; ++gp)
      for (uint32_t bp = 0; bp < 4u; ++bp)
        for (uint32_t tp = 0; tp < 4u; ++tp) {
            uint8_t mode[4] = {0u, 0u, 0u, 0u};
            mode[1] = (uint8_t)(tp <= gp);
            mode[2] = (uint8_t)(bp <= gp);
            mode[3] = (uint8_t)(((bp < tp) ? bp : tp) <= gp);
            for (uint32_t f = 0; f < 4u; ++f)
              for (uint32_t gz = 0; gz < 2u; ++gz)
                for (uint32_t bz = 0; bz < 2u; ++bz) {
                    const uint16_t g = gz ? 0u : 0x1234u;
                    const uint16_t bt = bz ? 0u : 0x5678u;
                    uint8_t p = 4u;
                    if (f & 2u) p = (uint8_t)bp;
                    if ((f & 1u) && tp < p) p = (uint8_t)tp;
                    const uint16_t ref = !f ? g :
                        ((p <= gp) ? (bt ? bt : g) : (g ? g : bt));
                    const uint16_t got = grp16_gbt_select(g, bt, (uint8_t)f, mode);
                    if (got != ref) {
                        s_grp16_gbt_ok = 0;
                        return 0;
                    }
                }
        }
    s_grp16_gbt_ok = 1;
    s_grp16_gbt_generation = 0xffffffffu;
    return 1;
}

void Grp_DrawLine16GBT_DebugGet(uint32_t *cache_rebuilds, uint32_t *cache_failures)
{
    if (cache_rebuilds) *cache_rebuilds = s_grp16_gbt_cache_rebuilds;
    if (cache_failures) *cache_failures = s_grp16_gbt_cache_failures;
}

int Grp_DrawLine16GBT(uint16_t *dst, const uint16_t *bt, const uint8_t *flags,
                      uint32_t width, uint8_t grp_pri, uint8_t bg_pri,
                      uint8_t text_pri)
{
    if (!s_grp16_gbt_ok || !dst || !bt || !flags || width == 0u || width > 800u)
        return 0;

    const uint32_t gen = Pal_DebugVisualGeneration();
    if (s_grp16_gbt_generation != gen && !grp16_gbt_cache_rebuild())
        return 0;

    grp_pri &= 3u; bg_pri &= 3u; text_pri &= 3u;
    uint8_t mode[4] = {0u, 0u, 0u, 0u};
    mode[1] = (uint8_t)(text_pri <= grp_pri);
    mode[2] = (uint8_t)(bg_pri <= grp_pri);
    mode[3] = (uint8_t)(((bg_pri < text_pri) ? bg_pri : text_pri) <= grp_pri);

    uint32_t y = GrphScrollY[0] + VLINE;
    if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) y += VLINE;
    y = (y & 0x1ffu) << 10;
    uint32_t sx = GrphScrollX[0] & 0x1ffu;
    const uint16_t *src = (const uint16_t *)(GVRAM + y + sx * 2u);
    uint32_t run = 0x200u - sx;
    if (run > width) run = width;

    uint32_t i = 0u;
    while (i < width) {
        uint32_t n = run;
        if (n > width - i) n = width - i;
        uint32_t j = 0u;
        /* Four-pixel unroll keeps raw GVRAM/palette/BT/flag accesses adjacent
         * while preserving exact per-pixel transparency semantics. */
        for (; j + 4u <= n; j += 4u) {
            const uint16_t g0 = grp16_gbt_decode(src[j + 0u]);
            const uint16_t g1 = grp16_gbt_decode(src[j + 1u]);
            const uint16_t g2 = grp16_gbt_decode(src[j + 2u]);
            const uint16_t g3 = grp16_gbt_decode(src[j + 3u]);
            dst[i+j+0u] = grp16_gbt_select(g0, bt[i+j+0u], flags[i+j+0u], mode);
            dst[i+j+1u] = grp16_gbt_select(g1, bt[i+j+1u], flags[i+j+1u], mode);
            dst[i+j+2u] = grp16_gbt_select(g2, bt[i+j+2u], flags[i+j+2u], mode);
            dst[i+j+3u] = grp16_gbt_select(g3, bt[i+j+3u], flags[i+j+3u], mode);
        }
        for (; j < n; ++j) {
            const uint16_t g = grp16_gbt_decode(src[j]);
            dst[i+j] = grp16_gbt_select(g, bt[i+j], flags[i+j], mode);
        }
        i += n;
        if (i >= width) break;
        src = (const uint16_t *)(GVRAM + y);
        run = width - i;
    }
    return 1;
}


/* BAT177NW18/R57E48: final common compositor in the compact index domain.
 * TEXT and BG have already been decoded to palette indices, but neither has
 * materialized RGB565 nor Text_TrFlag.  Resolve their exact historical draw
 * order, decode raw 65K GVRAM once, then apply the frozen R57E34 GRP-vs-B/T
 * priority/key-zero rule. */
static int s_grp16_tbgi_ok;

static inline uint8_t grp16_tbgi_pick_idx(uint8_t ti, uint8_t bi,
                                          uint8_t bg_pri, uint8_t text_pri)
{
    if (ti && bi)
        return (bg_pri < text_pri) ? bi : ti; /* TEXT wins equal priority */
    return ti ? ti : bi;
}

int Grp_DrawLine16TBGI_SelfCheck(void)
{
    /* Validate candidate-presence, BG/TEXT tie order, and final GRP relation
     * for every priority tuple.  RGB zero remains a key-zero value exactly as
     * in the materialized R57E44 selector. */
    for (uint32_t gp = 0; gp < 4u; ++gp)
      for (uint32_t bp = 0; bp < 4u; ++bp)
        for (uint32_t tp = 0; tp < 4u; ++tp)
          for (uint32_t tv = 0; tv < 2u; ++tv)
            for (uint32_t bv = 0; bv < 2u; ++bv)
              for (uint32_t gz = 0; gz < 2u; ++gz)
                for (uint32_t wz = 0; wz < 2u; ++wz) {
                    const uint8_t f = (uint8_t)((tv ? 1u : 0u) | (bv ? 2u : 0u));
                    const uint8_t ti = tv ? 0x57u : 0u;
                    const uint8_t bi = bv ? 0x34u : 0u;
                    const uint8_t exp_idx = !f ? 0u :
                        (tv && bv ? ((bp < tp) ? bi : ti) : (tv ? ti : bi));
                    if (grp16_tbgi_pick_idx(ti, bi, (uint8_t)bp, (uint8_t)tp) != exp_idx) {
                        s_grp16_tbgi_ok = 0;
                        return 0;
                    }
                    const uint16_t g = gz ? 0u : 0x1234u;
                    uint16_t bt = 0u;
                    if (f) bt = wz ? 0u : (tv && bv ? ((bp < tp) ? 0x3456u : 0x5678u)
                                                        : (tv ? 0x5678u : 0x3456u));
                    uint8_t p = 4u;
                    if (f & 2u) p = (uint8_t)bp;
                    if ((f & 1u) && tp < p) p = (uint8_t)tp;
                    const uint16_t ref = !f ? g :
                        ((p <= gp) ? (bt ? bt : g) : (g ? g : bt));

                    uint8_t mode[4] = {0u,0u,0u,0u};
                    mode[1] = (uint8_t)(tp <= gp);
                    mode[2] = (uint8_t)(bp <= gp);
                    mode[3] = (uint8_t)(((bp < tp) ? bp : tp) <= gp);
                    const uint16_t got = !f ? g :
                        (mode[f] ? (bt ? bt : g) : (g ? g : bt));
                    if (got != ref) { s_grp16_tbgi_ok = 0; return 0; }
                }
    s_grp16_tbgi_ok = 1;
    return 1;
}

int __attribute__((hot, optimize("O3"))) Grp_DrawLine16TBGI(uint16_t *dst, const uint8_t *text_idx,
                       const uint8_t *bg_idx, uint32_t width,
                       uint8_t grp_pri, uint8_t bg_pri, uint8_t text_pri)
{
    if (!s_grp16_tbgi_ok || !s_grp16_gbt_ok || !dst || !text_idx || !bg_idx ||
        width == 0u || width > 800u)
        return 0;
    const uint32_t gen = Pal_DebugVisualGeneration();
    if (s_grp16_gbt_generation != gen && !grp16_gbt_cache_rebuild())
        return 0;

    grp_pri &= 3u; bg_pri &= 3u; text_pri &= 3u;
    uint8_t mode[4] = {0u,0u,0u,0u};
    mode[1] = (uint8_t)(text_pri <= grp_pri);
    mode[2] = (uint8_t)(bg_pri <= grp_pri);
    mode[3] = (uint8_t)(((bg_pri < text_pri) ? bg_pri : text_pri) <= grp_pri);

    uint32_t y = GrphScrollY[0] + VLINE;
    if ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu) y += VLINE;
    y = (y & 0x1ffu) << 10;
    uint32_t sx = GrphScrollX[0] & 0x1ffu;
    const uint16_t *src = (const uint16_t *)(GVRAM + y + sx * 2u);
    uint32_t run = 0x200u - sx;
    if (run > width) run = width;

    uint32_t i = 0u;
    while (i < width) {
        uint32_t n = run;
        if (n > width - i) n = width - i;
        uint32_t j = 0u;
        for (; j + 4u <= n; j += 4u) {
            for (uint32_t k = 0u; k < 4u; ++k) {
                const uint32_t q = i + j + k;
                const uint8_t ti = text_idx[q];
                const uint8_t bi = bg_idx[q];
                const uint8_t f = (uint8_t)((ti ? 1u : 0u) | (bi ? 2u : 0u));
                const uint16_t g = grp16_gbt_decode(src[j + k]);
                if (!f) {
                    dst[q] = g;
                } else {
                    const uint16_t bt = TextPal[grp16_tbgi_pick_idx(ti, bi, bg_pri, text_pri)];
                    dst[q] = mode[f] ? (bt ? bt : g) : (g ? g : bt);
                }
            }
        }
        for (; j < n; ++j) {
            const uint32_t q = i + j;
            const uint8_t ti = text_idx[q];
            const uint8_t bi = bg_idx[q];
            const uint8_t f = (uint8_t)((ti ? 1u : 0u) | (bi ? 2u : 0u));
            const uint16_t g = grp16_gbt_decode(src[j]);
            if (!f) dst[q] = g;
            else {
                const uint16_t bt = TextPal[grp16_tbgi_pick_idx(ti, bi, bg_pri, text_pri)];
                dst[q] = mode[f] ? (bt ? bt : g) : (g ? g : bt);
            }
        }
        i += n;
        if (i >= width) break;
        src = (const uint16_t *)(GVRAM + y);
        run = width - i;
    }
    return 1;
}

void Grp_DrawLine16(void)
{
	uint16_t *srcp, *destp;
	uint32_t x;
	uint32_t i;
	uint16_t v, v0;
	uint32_t y = GrphScrollY[0] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y = (y & 0x1ff) << 10;

	x = GrphScrollX[0] & 0x1ff;
	srcp = (uint16_t *)(GVRAM + y + x * 2);
	destp = (uint16_t *)Grp_LineBuf;

	x = (x ^ 0x1ff) + 1;

	v = v0 = 0;
	i = 0;
	if (x < TextDotX) {
		for (; i < x; ++i) {
			v = *srcp++;
			if (v != 0) {
				v0 = (v >> 8) & 0xff;
				v &= 0x00ff;

				v = Pal_Regs[Pal16Adr[v]];
				v |= Pal_Regs[Pal16Adr[v0] + 2] << 8;
				v = Pal16[v];
			}
			*destp++ = v;
		}
		srcp -= 0x200;
	}

	for (; i < TextDotX; ++i) {
		v = *srcp++;
		if (v != 0) {
			v0 = (v >> 8) & 0xff;
			v &= 0x00ff;

			v = Pal_Regs[Pal16Adr[v]];
			v |= Pal_Regs[Pal16Adr[v0] + 2] << 8;
			v = Pal16[v];
		}
		*destp++ = v;
	}
}

void FASTCALL Grp_DrawLine8(int page, int opaq)
{
    /* Build 5.26 P4 fast path.
     *
     * The 256-colour renderer combines two interleaved 4-bit GVRAM pages.
     * Build 5.25 reduced one source from a 16-bit fetch to a byte fetch, but
     * the inner loop still updated/wrapped the second X coordinate for every
     * pixel.  On ESP32-P4 that arithmetic/branch is paid hundreds of times per
     * dirty scanline.
     *
     * Split the 512-dot ring into contiguous runs before entering the pixel
     * loop.  The hot loop then consists only of two byte loads, palette lookup
     * and a 16-bit store (plus the transparent test when opaq == 0).
     */
    uint16_t *destp = Grp_LineBuf;
    const uint16_t *pal = GrphPal;
    uint32_t y_lo, y_hi;
    uint32_t x_lo, x_hi;
    uint32_t remaining = TextDotX;

    page &= 1;

    y_lo = GrphScrollY[page * 2] + VLINE;
    y_hi = GrphScrollY[page * 2 + 1] + VLINE;
    if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) {
        y_lo += VLINE;
        y_hi += VLINE;
    }
    y_lo = ((y_lo & 0x1ff) << 10) + (uint32_t)page;
    y_hi = ((y_hi & 0x1ff) << 10) + (uint32_t)page;

    x_lo = GrphScrollX[page * 2] & 0x1ff;
    x_hi = GrphScrollX[page * 2 + 1] & 0x1ff;

    while (remaining) {
        uint32_t run_lo = 0x200u - x_lo;
        uint32_t run_hi = 0x200u - x_hi;
        uint32_t run = remaining;
        const uint8_t *src_lo;
        const uint8_t *src_hi;
        uint32_t i;

        if (run > run_lo) run = run_lo;
        if (run > run_hi) run = run_hi;

        src_lo = GVRAM + y_lo + (x_lo << 1);
        src_hi = GVRAM + y_hi + (x_hi << 1);

        if (opaq) {
            /* Four-pixel unroll keeps the PSRAM/cache streams simple while
             * preserving the original interleaved byte layout. */
            for (i = 0; i + 4 <= run; i += 4) {
                uint16_t v0 = (uint16_t)((src_hi[0] & 0xf0) | (src_lo[0] & 0x0f));
                uint16_t v1 = (uint16_t)((src_hi[2] & 0xf0) | (src_lo[2] & 0x0f));
                uint16_t v2 = (uint16_t)((src_hi[4] & 0xf0) | (src_lo[4] & 0x0f));
                uint16_t v3 = (uint16_t)((src_hi[6] & 0xf0) | (src_lo[6] & 0x0f));
                destp[0] = pal[v0];
                destp[1] = pal[v1];
                destp[2] = pal[v2];
                destp[3] = pal[v3];
                src_lo += 8;
                src_hi += 8;
                destp += 4;
            }
            for (; i < run; ++i) {
                uint16_t v = (uint16_t)((*src_hi & 0xf0) | (*src_lo & 0x0f));
                *destp++ = pal[v];
                src_lo += 2;
                src_hi += 2;
            }
        } else {
            for (i = 0; i + 4 <= run; i += 4) {
                uint16_t v0 = (uint16_t)((src_hi[0] & 0xf0) | (src_lo[0] & 0x0f));
                uint16_t v1 = (uint16_t)((src_hi[2] & 0xf0) | (src_lo[2] & 0x0f));
                uint16_t v2 = (uint16_t)((src_hi[4] & 0xf0) | (src_lo[4] & 0x0f));
                uint16_t v3 = (uint16_t)((src_hi[6] & 0xf0) | (src_lo[6] & 0x0f));
                if (v0) destp[0] = pal[v0];
                if (v1) destp[1] = pal[v1];
                if (v2) destp[2] = pal[v2];
                if (v3) destp[3] = pal[v3];
                src_lo += 8;
                src_hi += 8;
                destp += 4;
            }
            for (; i < run; ++i) {
                uint16_t v = (uint16_t)((*src_hi & 0xf0) | (*src_lo & 0x0f));
                if (v) *destp = pal[v];
                ++destp;
                src_lo += 2;
                src_hi += 2;
            }
        }

        remaining -= run;
        x_lo += run;
        x_hi += run;
        if (x_lo == 0x200u) x_lo = 0;
        if (x_hi == 0x200u) x_hi = 0;
    }
}

/* Manhattan Requiem Opening 7.0ｿｿ7.5MHz */

/* Build 5.37 ESP32-P4 shared-scroll paired-page fetch.
 *
 * In 256-colour mode logical page 0 and page 1 are adjacent bytes for each
 * physical GVRAM pixel.  Most games keep the two logical pages at the same
 * scroll position.  Build 5.36 still fetched those bytes through four
 * independent byte streams (bottom lo/hi + top lo/hi).  When the scrolls are
 * shared, fetch page0+page1 together as one 16-bit lane.  If both source
 * pointers are 32-bit aligned, fetch two pixels at once with two 32-bit PSRAM
 * loads (one low-nibble plane and one high-nibble plane).
 *
 * This changes only how the same bytes are fetched.  The existing one-line
 * legacy SELF-CHECK in windraw.c validates the resulting pixels before the
 * fast path is trusted for the frame.
 */
static void Grp_DrawLine8PairSharedScroll(int bottom_page, int top_page,
                                          uint32_t y_lo_base, uint32_t y_hi_base,
                                          uint32_t x_lo, uint32_t x_hi)
{
    uint16_t *destp = Grp_LineBuf;
    const uint16_t *pal = GrphPal;
    uint32_t remaining = TextDotX;
    static uint8_t s_reported = 0;

    if (!s_reported) {
        s_reported = 1;
        printf("PX68K_GRP8PAIR: Build 5.37 shared-scroll paired-page PSRAM fetch ACTIVE\n");
    }

    while (remaining) {
        uint32_t run = remaining;
        uint32_t r = 0x200u - x_lo;
        const uint8_t *lp8;
        const uint8_t *hp8;
        uint32_t i = 0;

        if (run > r) run = r;
        r = 0x200u - x_hi;
        if (run > r) run = r;

        lp8 = GVRAM + y_lo_base + (x_lo << 1);
        hp8 = GVRAM + y_hi_base + (x_hi << 1);

        /* The physical pair address is even.  Use a 16-bit lane per pixel;
         * page0 is the low byte and page1 is the high byte on ESP32-P4. */
#if !defined(MSB_FIRST)
        if ((((uintptr_t)lp8 | (uintptr_t)hp8) & 1u) == 0u) {
            /* If both streams have 32-bit alignment, consume two pixels per
             * PSRAM load.  Otherwise the 16-bit lane path still halves the
             * number of GVRAM load instructions versus Build 5.36. */
            if ((((uintptr_t)lp8 | (uintptr_t)hp8) & 3u) == 0u) {
                const uint32_t *lp32 = (const uint32_t *)lp8;
                const uint32_t *hp32 = (const uint32_t *)hp8;
                for (; i + 2 <= run; i += 2) {
                    uint32_t lw32 = *lp32++;
                    uint32_t hw32 = *hp32++;
                    uint16_t lw0 = (uint16_t)lw32;
                    uint16_t lw1 = (uint16_t)(lw32 >> 16);
                    uint16_t hw0 = (uint16_t)hw32;
                    uint16_t hw1 = (uint16_t)(hw32 >> 16);
                    uint16_t p00 = (uint16_t)((hw0 & 0x00f0u) | (lw0 & 0x000fu));
                    uint16_t p01 = (uint16_t)(((hw0 >> 8) & 0x00f0u) | ((lw0 >> 8) & 0x000fu));
                    uint16_t p10 = (uint16_t)((hw1 & 0x00f0u) | (lw1 & 0x000fu));
                    uint16_t p11 = (uint16_t)(((hw1 >> 8) & 0x00f0u) | ((lw1 >> 8) & 0x000fu));
                    uint16_t b0 = bottom_page ? p01 : p00;
                    uint16_t t0 = top_page    ? p01 : p00;
                    uint16_t b1 = bottom_page ? p11 : p10;
                    uint16_t t1 = top_page    ? p11 : p10;
                    destp[0] = pal[t0 ? t0 : b0];
                    destp[1] = pal[t1 ? t1 : b1];
                    destp += 2;
                }
                lp8 = (const uint8_t *)lp32;
                hp8 = (const uint8_t *)hp32;
            }

            for (; i < run; ++i) {
                uint16_t lw = *(const uint16_t *)lp8;
                uint16_t hw = *(const uint16_t *)hp8;
                uint16_t p0 = (uint16_t)((hw & 0x00f0u) | (lw & 0x000fu));
                uint16_t p1 = (uint16_t)(((hw >> 8) & 0x00f0u) | ((lw >> 8) & 0x000fu));
                uint16_t b = bottom_page ? p1 : p0;
                uint16_t t = top_page    ? p1 : p0;
                *destp++ = pal[t ? t : b];
                lp8 += 2;
                hp8 += 2;
            }
        } else
#endif
        {
            /* Portable/alignment fallback; normally not taken on Tab5. */
            const uint8_t *blo = lp8 + bottom_page;
            const uint8_t *tlo = lp8 + top_page;
            const uint8_t *bhi = hp8 + bottom_page;
            const uint8_t *thi = hp8 + top_page;
            for (i = 0; i < run; ++i) {
                uint16_t b = (uint16_t)((*bhi & 0xf0) | (*blo & 0x0f));
                uint16_t t = (uint16_t)((*thi & 0xf0) | (*tlo & 0x0f));
                *destp++ = pal[t ? t : b];
                blo += 2; tlo += 2; bhi += 2; thi += 2;
            }
        }

        remaining -= run;
        x_lo += run;
        x_hi += run;
        if (x_lo == 0x200u) x_lo = 0;
        if (x_hi == 0x200u) x_hi = 0;
    }
}

/* Build 5.36 ESP32-P4 common 256-colour fast path.
 *
 * The normal two-page 256-colour compositor used to call Grp_DrawLine8()
 * twice per dirty scanline: first an opaque lower-priority page, then a
 * transparent upper-priority page.  Each call converted every source pixel
 * through GrphPal and wrote Grp_LineBuf, so a two-page line paid two palette
 * lookups and (usually) two line-buffer stores per output pixel.
 *
 * Here we read the same four interleaved GVRAM byte streams in one pass,
 * choose the winning 8-bit palette index first (upper index 0 is transparent),
 * and perform only one palette lookup/store.  GVRAM read semantics, scroll
 * wrapping and priority are unchanged.  Special-priority / half-transparent
 * modes still use the legacy routines in windraw.c.
 */
void FASTCALL Grp_DrawLine8Pair(int bottom_page, int top_page)
{
    uint16_t *destp = Grp_LineBuf;
    const uint16_t *pal = GrphPal;
    uint32_t by_lo, by_hi, ty_lo, ty_hi;
    uint32_t bx_lo, bx_hi, tx_lo, tx_hi;
    uint32_t remaining = TextDotX;

    bottom_page &= 1;
    top_page &= 1;

    by_lo = GrphScrollY[bottom_page * 2] + VLINE;
    by_hi = GrphScrollY[bottom_page * 2 + 1] + VLINE;
    ty_lo = GrphScrollY[top_page * 2] + VLINE;
    ty_hi = GrphScrollY[top_page * 2 + 1] + VLINE;
    if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) {
        by_lo += VLINE;
        by_hi += VLINE;
        ty_lo += VLINE;
        ty_hi += VLINE;
    }

    by_lo = ((by_lo & 0x1ff) << 10) + (uint32_t)bottom_page;
    by_hi = ((by_hi & 0x1ff) << 10) + (uint32_t)bottom_page;
    ty_lo = ((ty_lo & 0x1ff) << 10) + (uint32_t)top_page;
    ty_hi = ((ty_hi & 0x1ff) << 10) + (uint32_t)top_page;

    bx_lo = GrphScrollX[bottom_page * 2] & 0x1ff;
    bx_hi = GrphScrollX[bottom_page * 2 + 1] & 0x1ff;
    tx_lo = GrphScrollX[top_page * 2] & 0x1ff;
    tx_hi = GrphScrollX[top_page * 2 + 1] & 0x1ff;

#if defined(ESP_PLATFORM) && !defined(MSB_FIRST)
    /* The two logical pages occupy adjacent bytes.  If their corresponding
     * low/high planes use identical scrolls, fetch those page bytes together. */
    if (bottom_page != top_page &&
        (by_lo - (uint32_t)bottom_page) == (ty_lo - (uint32_t)top_page) &&
        (by_hi - (uint32_t)bottom_page) == (ty_hi - (uint32_t)top_page) &&
        bx_lo == tx_lo && bx_hi == tx_hi) {
        Grp_DrawLine8PairSharedScroll(bottom_page, top_page,
                                      by_lo - (uint32_t)bottom_page,
                                      by_hi - (uint32_t)bottom_page,
                                      bx_lo, bx_hi);
        return;
    }
#endif

    while (remaining) {
        uint32_t run = remaining;
        uint32_t r;
        const uint8_t *blo, *bhi, *tlo, *thi;
        uint32_t i;

        r = 0x200u - bx_lo; if (run > r) run = r;
        r = 0x200u - bx_hi; if (run > r) run = r;
        r = 0x200u - tx_lo; if (run > r) run = r;
        r = 0x200u - tx_hi; if (run > r) run = r;

        blo = GVRAM + by_lo + (bx_lo << 1);
        bhi = GVRAM + by_hi + (bx_hi << 1);
        tlo = GVRAM + ty_lo + (tx_lo << 1);
        thi = GVRAM + ty_hi + (tx_hi << 1);

        for (i = 0; i + 4 <= run; i += 4) {
            uint16_t b0 = (uint16_t)((bhi[0] & 0xf0) | (blo[0] & 0x0f));
            uint16_t b1 = (uint16_t)((bhi[2] & 0xf0) | (blo[2] & 0x0f));
            uint16_t b2 = (uint16_t)((bhi[4] & 0xf0) | (blo[4] & 0x0f));
            uint16_t b3 = (uint16_t)((bhi[6] & 0xf0) | (blo[6] & 0x0f));
            uint16_t t0 = (uint16_t)((thi[0] & 0xf0) | (tlo[0] & 0x0f));
            uint16_t t1 = (uint16_t)((thi[2] & 0xf0) | (tlo[2] & 0x0f));
            uint16_t t2 = (uint16_t)((thi[4] & 0xf0) | (tlo[4] & 0x0f));
            uint16_t t3 = (uint16_t)((thi[6] & 0xf0) | (tlo[6] & 0x0f));
            destp[0] = pal[t0 ? t0 : b0];
            destp[1] = pal[t1 ? t1 : b1];
            destp[2] = pal[t2 ? t2 : b2];
            destp[3] = pal[t3 ? t3 : b3];
            blo += 8; bhi += 8; tlo += 8; thi += 8;
            destp += 4;
        }
        for (; i < run; ++i) {
            uint16_t b = (uint16_t)((*bhi & 0xf0) | (*blo & 0x0f));
            uint16_t t = (uint16_t)((*thi & 0xf0) | (*tlo & 0x0f));
            *destp++ = pal[t ? t : b];
            blo += 2; bhi += 2; tlo += 2; thi += 2;
        }

        remaining -= run;
        bx_lo += run; bx_hi += run; tx_lo += run; tx_hi += run;
        if (bx_lo == 0x200u) bx_lo = 0;
        if (bx_hi == 0x200u) bx_hi = 0;
        if (tx_lo == 0x200u) tx_lo = 0;
        if (tx_hi == 0x200u) tx_hi = 0;
    }
}

void FASTCALL Grp_DrawLine4(uint32_t page, int opaq)
{
	uint16_t *srcp, *destp;	/* XXX: ALIGN */
	uint32_t x, y;
	uint32_t off;
	uint32_t i;
	uint16_t v;

	page &= 3;

	y = GrphScrollY[page] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y = (y & 0x1ff) << 10;

	x = GrphScrollX[page] & 0x1ff;
	off = y + x * 2;

	x ^= 0x1ff;

	srcp = (uint16_t *)(GVRAM + off + (page >> 1));
	destp = (uint16_t *)Grp_LineBuf;

	v = 0;
	i = 0;

	if (page & 1) {
		if (opaq) {
			if (x < TextDotX) {
				for (; i < x; ++i) {
					v = GET_WORD_W8(srcp);
					srcp++;
					v = GrphPal[(v >> 4) & 0xf];
					*destp++ = v;
				}
				srcp -= 0x200;
			}
			for (; i < TextDotX; ++i) {
				v = GET_WORD_W8(srcp);
				srcp++;
				v = GrphPal[(v >> 4) & 0xf];
				*destp++ = v;
			}
		} else {
			if (x < TextDotX) {
				for (; i < x; ++i) {
					v = GET_WORD_W8(srcp);
					srcp++;
					v = (v >> 4) & 0x0f;
					if (v != 0x00)
						*destp = GrphPal[v];
					destp++;
				}
				srcp -= 0x200;
			}
			for (; i < TextDotX; ++i) {
				v = GET_WORD_W8(srcp);
				srcp++;
				v = (v >> 4) & 0x0f;
				if (v != 0x00)
					*destp = GrphPal[v];
				destp++;
			}
		}
	} else {
		if (opaq) {
			if (x < TextDotX) {
				for (; i < x; ++i) {
					v = GET_WORD_W8(srcp);
					srcp++;
					v = GrphPal[v & 0x0f];
					*destp++ = v;
				}
				srcp -= 0x200;
			}
			for (; i < TextDotX; ++i) {
				v = GET_WORD_W8(srcp);
				srcp++;
				v = GrphPal[v & 0x0f];
				*destp++ = v;
			}
		} else {
			if (x < TextDotX) {
				for (; i < x; ++i) {
					v = GET_WORD_W8(srcp);
					srcp++;
					v &= 0x0f;
					if (v != 0x00)
						*destp = GrphPal[v];
					destp++;
				}
				srcp -= 0x200;
			}
			for (; i < TextDotX; ++i) {
				v = GET_WORD_W8(srcp);
				srcp++;
				v &= 0x0f;
				if (v != 0x00)
					*destp = GrphPal[v];
				destp++;
			}
		}
	}
}

void FASTCALL Grp_DrawLine4h(void)
{
	uint16_t *srcp, *destp;
	uint32_t x, y;
	uint32_t i;
	uint16_t v;
	int bits;

	y = GrphScrollY[0] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y &= 0x3ff;

	if ((y & 0x200) == 0x000) {
		y <<= 10;
		bits = (GrphScrollX[0] & 0x200) ? 4 : 0;
	} else {
		y = (y & 0x1ff) << 10;
		bits = (GrphScrollX[0] & 0x200) ? 12 : 8;
	}

	x = GrphScrollX[0] & 0x1ff;
	srcp = (uint16_t *)(GVRAM + y + x * 2);
	destp = (uint16_t *)Grp_LineBuf;

	x = ((x & 0x1ff) ^ 0x1ff) + 1;

	for (i = 0; i < TextDotX; ++i) {
		v = *srcp++;
		*destp++ = GrphPal[(v >> bits) & 0x0f];

		if (--x == 0) {
			srcp -= 0x200;
			bits ^= 4;
			x = 512;
		}
	}
}


/*
 * --- 半透明／特殊Priのベースとなるページの描画 ---
 */
void FASTCALL Grp_DrawLine16SP(void)
{
	uint32_t x, y;
	uint32_t off;
	uint32_t i;
	uint16_t v;

	y = GrphScrollY[0] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y = (y & 0x1ff) << 10;

	x = GrphScrollX[0] & 0x1ff;
	off = y + x * 2;
	x = (x ^ 0x1ff) + 1;

	for (i = 0; i < TextDotX; ++i) {
		v = (Pal_Regs[GVRAM[off+1]*2] << 8) | Pal_Regs[GVRAM[off]*2+1];
		if ((GVRAM[off] & 1) == 0) {
			Grp_LineBufSP[i] = 0;
			Grp_LineBufSP2[i] = Pal16[v & 0xfffe];
		} else {
			Grp_LineBufSP[i] = Pal16[v & 0xfffe];
			Grp_LineBufSP2[i] = 0;
		}

		off += 2;
		if (--x == 0)
			off -= 0x400;
	}
}


void FASTCALL Grp_DrawLine8SP(int page)
{
	uint32_t x, x0;
	uint32_t y, y0;
	uint32_t off, off0;
	uint32_t i;
	uint16_t v;

	page &= 1;

	y = GrphScrollY[page * 2] + VLINE;
	y0 = GrphScrollY[page * 2 + 1] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c) {
		y += VLINE;
		y0 += VLINE;
	}
	y = (y & 0x1ff) << 10;
	y0 = (y0 & 0x1ff) << 10;

	x = GrphScrollX[page * 2] & 0x1ff;
	x0 = GrphScrollX[page * 2 + 1] & 0x1ff;

	off = y + x * 2 + page;
	off0 = y0 + x0 * 2 + page;

	x = (x ^ 0x1ff) + 1;

	for (i = 0; i < TextDotX; ++i) {
		v = (GVRAM[off] & 0x0f) | (GVRAM[off0] & 0xf0);
		Grp_LineBufSP_Tr[i] = 0;

		if ((v & 1) == 0) {
			v &= 0xfe;
			if (v != 0x00) {
 				v = GrphPal[v];
				if (!v)
					Grp_LineBufSP_Tr[i] = 0x1234;
			}

			Grp_LineBufSP[i] = 0;
			Grp_LineBufSP2[i] = v;
		} else {
			v &= 0xfe;
			if (v != 0x00)
				v = GrphPal[v] | Ibit;
			Grp_LineBufSP[i] = v;
			Grp_LineBufSP2[i] = 0;
		}

		off += 2;
		off0 += 2;
		if ((off0 & 0x3fe) == 0)
			off0 -= 0x400;
		if (--x == 0)
			off -= 0x400;
	}
}

void FASTCALL Grp_DrawLine4SP(uint32_t page/*, int opaq*/)
{
	uint32_t x, y;
	uint32_t off;
	uint32_t i;
	uint16_t v;
	uint32_t scrx = 0, scry = 0;
	page &= 3;
	switch(page)
   {
      case 0:
         scrx = GrphScrollX[0];
         scry = GrphScrollY[0];
         break;
      case 1:
         scrx = GrphScrollX[1];
         scry = GrphScrollY[1];
         break;
      case 2:
         scrx = GrphScrollX[2];
         scry = GrphScrollY[2];
         break;
      case 3:
         scrx = GrphScrollX[3];
         scry = GrphScrollY[3];
         break;
   }

	if (page & 1)
   {
      y = scry + VLINE;
      if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
         y += VLINE;
      y = (y & 0x1ff) << 10;

      x = scrx & 0x1ff;
      off = y + x * 2;
      if (page & 2)
         off++;
      x = (x ^ 0x1ff) + 1;

      for (i = 0; i < TextDotX; ++i) {
         v = GVRAM[off] >> 4;
         if ((v & 1) == 0) {
            v &= 0x0e;
            Grp_LineBufSP[i] = 0;
            Grp_LineBufSP2[i] = GrphPal[v];
         } else {
            v &= 0x0e;
            Grp_LineBufSP[i] = GrphPal[v];
            Grp_LineBufSP2[i] = 0;
         }

         off += 2;
         if (--x == 0)
            off -= 0x400;
      }
   }
   else
   {
      y = scry + VLINE;
      if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
         y += VLINE;
      y = (y & 0x1ff) << 10;

      x = scrx & 0x1ff;
      off = y + x * 2;
      if (page & 2)
         off++;
      x = (x ^ 0x1ff) + 1;

      for (i = 0; i < TextDotX; ++i) {
         v = GVRAM[off];
         if ((v & 1) == 0) {
            v &= 0x0e;
            Grp_LineBufSP[i] = 0;
            Grp_LineBufSP2[i] = GrphPal[v];
         } else {
            v &= 0x0e;
            Grp_LineBufSP[i] = GrphPal[v];
            Grp_LineBufSP2[i] = 0;
         }

         off += 2;
         if (--x == 0)
            off -= 0x400;
      }
   }
}


void FASTCALL Grp_DrawLine4hSP(void)
{
	uint16_t *srcp;
	uint32_t x;
	uint32_t i;
	int bits;
	uint16_t v;
	uint32_t y = GrphScrollY[0] + VLINE;
	if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
		y += VLINE;
	y &= 0x3ff;

	if ((y & 0x200) == 0x000)
   {
		y <<= 10;
		bits = (GrphScrollX[0] & 0x200) ? 4 : 0;
	}
   else
   {
		y    = (y & 0x1ff) << 10;
		bits = (GrphScrollX[0] & 0x200) ? 12 : 8;
	}

	x    = GrphScrollX[0] & 0x1ff;
	srcp = (uint16_t *)(GVRAM + y + x * 2);
	x    = ((x & 0x1ff) ^ 0x1ff) + 1;

	for (i = 0; i < TextDotX; ++i)
   {
      v = *srcp++ >> bits;
      if ((v & 1) == 0)
      {
         Grp_LineBufSP[i]  = 0;
         Grp_LineBufSP2[i] = GrphPal[v & 0x0e];
      }
      else
      {
         Grp_LineBufSP[i]  = GrphPal[v & 0x0e];
         Grp_LineBufSP2[i] = 0;
      }

      if (--x == 0)
         srcp -= 0x400;
   }
}

void FASTCALL Grp_DrawLine8TR(int page, int opaq)
{
	if (opaq)
   {
      uint32_t x, y;
      uint32_t v, v0;
      uint32_t i;

      page &= 1;

      y = GrphScrollY[page * 2] + VLINE;
      if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
         y += VLINE;
      y = ((y & 0x1ff) << 10) + page;
      x = GrphScrollX[page * 2] & 0x1ff;

      for (i = 0; i < TextDotX; ++i, x = (x + 1) & 0x1ff) {
         v0 = Grp_LineBufSP[i];
         v = GVRAM[y + x * 2];

         if (v0 != 0) {
            if (v != 0) {
               v = GrphPal[v];
               if (v != 0) {
                  v0 &= Pal_HalfMask;
                  if (v & Ibit)
                     v0 |= Pal_Ix2;
                  v &= Pal_HalfMask;
                  v += v0;
                  v >>= 1;
               }
            }
         } else
            v = GrphPal[v];
         Grp_LineBuf[i] = (uint16_t)v;
      }
   }
}

void FASTCALL Grp_DrawLine8TR_GT(int page, int opaq)
{
	if (opaq)
   {
      uint32_t x, y;
      uint32_t i;

      page &= 1;

      y = GrphScrollY[page * 2] + VLINE;
      if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
         y += VLINE;
      y = ((y & 0x1ff) << 10) + page;
      x = GrphScrollX[page * 2] & 0x1ff;

      for (i = 0; i < TextDotX; ++i, x = (x + 1) & 0x1ff)
      {
         Grp_LineBuf[i]      = (Grp_LineBufSP[i] || Grp_LineBufSP_Tr[i]) ? 0 : GrphPal[GVRAM[y + x * 2]];
         Grp_LineBufSP_Tr[i] = 0;
      }
   }
}

void FASTCALL Grp_DrawLine4TR(uint32_t page, int opaq)
{
   uint32_t x, y;
   uint32_t v, v0;
   uint32_t i;

   page &= 3;

   y = GrphScrollY[page] + VLINE;
   if ((CRTC_Regs[0x29] & 0x1c) == 0x1c)
      y += VLINE;
   y = (y & 0x1ff) << 10;
   x = GrphScrollX[page] & 0x1ff;

   if (page & 1) {
      page >>= 1;
      y += page;

      if (opaq) {
         for (i = 0; i < TextDotX; ++i, x = (x + 1) & 0x1ff) {
            v0 = Grp_LineBufSP[i];
            v = GVRAM[y + x * 2] >> 4;

            if (v0 != 0) {
               if (v != 0) {
                  v = GrphPal[v];
                  if (v != 0) {
                     v0 &= Pal_HalfMask;
                     if (v & Ibit)
                        v0 |= Pal_Ix2;
                     v &= Pal_HalfMask;
                     v += v0;
                     v >>= 1;
                  }
               }
            } else
               v = GrphPal[v];
            Grp_LineBuf[i] = (uint16_t)v;
         }
      } else {
         for (i = 0; i < TextDotX; ++i, x = (x + 1) & 0x1ff) {
            v0 = Grp_LineBufSP[i];

            if (v0 == 0)
            {
               v = GVRAM[y + x * 2] >> 4;
               if (v != 0)
                  Grp_LineBuf[i] = GrphPal[v];
            }
            else
            {
               v = GVRAM[y + x * 2] >> 4;
               if (v != 0)
               {
                  v = GrphPal[v];
                  if (v != 0)
                  {
                     v0 &= Pal_HalfMask;
                     if (v & Ibit)
                        v0 |= Pal_Ix2;
                     v &= Pal_HalfMask;
                     v += v0;
                     v = GrphPal[v >> 1];
                     Grp_LineBuf[i]=(uint16_t)v;
                  }
               } else
                  Grp_LineBuf[i] = (uint16_t)v;
            }
         }
      }
   } else {
      page >>= 1;
      y += page;

      if (opaq)
      {
         for (i = 0; i < TextDotX; ++i, x = (x + 1) & 0x1ff)
         {
            v  = GVRAM[y + x * 2] & 0x0f;
            v0 = Grp_LineBufSP[i];

            if (v0 != 0)
            {
               if (v != 0)
               {
                  v = GrphPal[v];
                  if (v != 0)
                  {
                     v0 &= Pal_HalfMask;
                     if (v & Ibit)
                        v0 |= Pal_Ix2;
                     v &= Pal_HalfMask;
                     v += v0;
                     v >>= 1;
                  }
               }
            } else
               v = GrphPal[v];
            Grp_LineBuf[i] = (uint16_t)v;
         }
      }
      else
      {
         for (i = 0; i < TextDotX; ++i, x = (x + 1) & 0x1ff)
         {
            v  = GVRAM[y + x * 2] & 0x0f;
            v0 = Grp_LineBufSP[i];

            if (v0 != 0)
            {
               if (v != 0)
               {
                  v = GrphPal[v];
                  if (v != 0)
                  {
                     v0 &= Pal_HalfMask;
                     if (v & Ibit)
                        v0 |= Pal_Ix2;
                     v &= Pal_HalfMask;
                     v += v0;
                     v >>= 1;
                     Grp_LineBuf[i]=(uint16_t)v;
                  }
               } else
                  Grp_LineBuf[i] = (uint16_t)v;
            } else if (v != 0)
               Grp_LineBuf[i] = GrphPal[v];
         }
      }
   }
}
