/*
 *  BG.C - BG and sprites
 *  TODO: Check transparent color processing (especially with Text)
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
#define TAB5_R57_BGPOST(d,a,v) tab5_guest_bus_post_write((uint32_t)(d),(uint32_t)(a),(uint8_t)(v))
#else
#define TAB5_R57_BGPOST(d,a,v) ((void)0)
#endif

#ifdef ESP_PLATFORM
/* Build 5.50a: host BG/Sprite jobs read the shared internal-SRAM BG source
 * arrays directly. Before the guest mutates those sources, drain only the
 * outstanding jobs that depend on them. This is a write-side barrier, not a
 * per-scanline wait, so normal rendering remains fully asynchronous. */
extern void tab5_compose_guest_bg_barrier(void);
/* BAT177NW0: shared-source host paths quarantined; CPU1 never waits. */
#define BG_HOST_SOURCE_BARRIER() ((void)0)
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

#ifdef ESP_PLATFORM
/* BAT177NW6/R57E36: a BG map byte can only change scanlines whose source Y
 * falls in that tile row.  Keep pattern writes conservative because the same
 * pattern data may be referenced by either BG plane or by sprites.
 *
 * This deliberately ignores X visibility: a map cell write dirties the whole
 * corresponding tile-row of output lines.  That is conservative (never misses
 * a visible pixel) while replacing the old 1024-line invalidation with a small
 * line set. */
static uint32_t s_tab5_bg_pattern_full;
static uint32_t s_tab5_bg_map0_local;
static uint32_t s_tab5_bg_map1_local;
static uint32_t s_tab5_bg_local_lines;
static uint32_t s_tab5_bg_inactive_skip;

/* BAT177NW7/R57E37: exact palette-bank presence for the two physical BG map
 * regions (0x4000-0x5fff and 0x6000-0x7fff).  Counts are maintained for the
 * raw map-byte positions even when a region is temporarily interpreted as
 * character pattern RAM.  If that region later becomes a map, the counts are
 * already correct.  This lets text/BG/sprite palette writes prove that a
 * 16-colour bank has no current consumer without scanning the maps. */
static uint16_t s_tab5_bg_map_bank_count[2][16];
static uint16_t s_tab5_bg_map_bank_mask[2];

/* BAT177NW8/R57E38: inverse map-row cache.
 * NW6 localized a BG map write by scanning all 1024 output lines. That was
 * exact, but with ~20k map writes / 600 guest frames the classifier itself
 * became a measurable CPU1 tax. Keep the same y->source transform, but rebuild
 * a 64-row inverse list only when its geometry/scroll signature changes.
 * A steady map write then walks only the output lines that can sample that
 * source tile row (typically ~16 lines), with no loss of exactness. */
typedef struct {
    uint32_t scroll_y;
    int32_t bg_vline;
    int32_t vdiff;
    uint8_t s1;
    uint8_t s2;
    uint8_t chr16;
    uint8_t valid;
} TAB5_BG_INV_SIG;

static TAB5_BG_INV_SIG s_tab5_bg_inv_sig[2];
static uint16_t s_tab5_bg_inv_head[2][64];
static uint16_t s_tab5_bg_inv_next[2][1024];
static uint32_t s_tab5_bg_inv_rebuilds;
static uint32_t s_tab5_bg_inv_visits;

/* Cache the conservative sprite palette-bank union until sprite RAM changes. */
static uint16_t s_tab5_sprite_bank_mask;
static uint8_t s_tab5_sprite_bank_mask_dirty = 1u;
static uint32_t s_tab5_sprite_bank_rebuilds;
static uint32_t s_tab5_sprite_bank_queries;

/* BAT177NW12/R57E42: exact first-writer BG acceleration + sprite-Y index.
 * The sprite cache changes only candidate enumeration: legacy visibility is
 * represented by the exact 16 possible Y buckets.  Empty priority groups are
 * skipped entirely.
 *
 * BG1 is the first BG writer.  When no priority-1 sprite is visible, nothing
 * can have set Text_TrFlag bit2 before BG1, so the old per-pixel bit2 read is
 * redundant.  BG0 keeps the stricter NW11 proof (no BG1 and no priority-1/2
 * sprite).  Every other case uses the legacy renderer. */
static uint32_t s_tab5_bg1_first_hits;
static uint32_t s_tab5_bg1_first_fallback;
static uint32_t s_tab5_bg0_first_hits;
static uint32_t s_tab5_bg0_first_fallback;
static uint32_t s_tab5_sprite_y_rebuilds;
static uint32_t s_tab5_sprite_y_probes;
static uint32_t s_tab5_sprite_y_items;
static uint8_t s_tab5_sprite_y_head[3][1024];
static uint8_t s_tab5_sprite_y_next[128];

/* BAT177NW13/R57E43: exact fused BG1+BG0 8px path.
 * In the steady 8px/GD case with no visible priority-1/2 sprites, BG1 and
 * BG0 are adjacent writers.  Decode BG1 to compact palette indices, then let
 * the BG0 tile pass resolve the exact two-plane result and perform only one
 * RGB565/Text_TrFlag write per output pixel.  The scratch is renderer-private
 * internal SRAM and only the visible 16..16+TextDotX span is consumed. */
static uint8_t s_tab5_bg1_idx[1600] __attribute__((aligned(4)));
static uint32_t s_tab5_bg_fuse_lines;
static uint32_t s_tab5_bg_fuse_fallback;
static uint32_t s_tab5_bg_fuse_blocks;
static uint8_t s_tab5_bg_fuse_ok;
static uint8_t s_tab5_bg_fuse_reported;

static void BG_Tab5FuseSelfCheck(void)
{
    uint32_t i1, i0;
    s_tab5_bg_fuse_ok = 0u;
    for (i1 = 0u; i1 < 256u; ++i1) {
        for (i0 = 0u; i0 < 256u; ++i0) {
            uint8_t old_idx = 0u, old_written = 0u;
            uint8_t new_idx;
            if (i1 != 0u) {
                old_idx = (uint8_t)i1;
                old_written = 1u;
            }
            if (i0 != 0u && (((i0 & 15u) != 0u) || !old_written)) {
                old_idx = (uint8_t)i0;
                old_written = 1u;
            }
            if ((i0 & 15u) != 0u)
                new_idx = (uint8_t)i0;
            else if (i1 != 0u)
                new_idx = (uint8_t)i1;
            else
                new_idx = (uint8_t)(i0 & 0xf0u);
            if (old_written != (uint8_t)(new_idx != 0u) ||
                (old_written && old_idx != new_idx)) {
                if (!s_tab5_bg_fuse_reported) {
                    printf("PX68K_BGFUSE_R57E43: exhaustive palette-index self-check FAIL i1=%lu i0=%lu\n",
                           (unsigned long)i1, (unsigned long)i0);
                    s_tab5_bg_fuse_reported = 1u;
                }
                return;
            }
        }
    }
    s_tab5_bg_fuse_ok = 1u;
    if (!s_tab5_bg_fuse_reported) {
        printf("PX68K_BGFUSE_R57E43: exhaustive 256x256 two-pass equivalence PASS; fused BG1+BG0 armed\n");
        s_tab5_bg_fuse_reported = 1u;
    }
}

static void BG_Tab5RebuildMapBankCounts(void)
{
    memset(s_tab5_bg_map_bank_count, 0, sizeof(s_tab5_bg_map_bank_count));
    s_tab5_bg_map_bank_mask[0] = 0u;
    s_tab5_bg_map_bank_mask[1] = 0u;
    for (uint32_t region = 0; region < 2u; ++region) {
        const uint32_t base = 0x4000u + region * 0x2000u;
        for (uint32_t off = 0u; off < 0x2000u; off += 2u) {
            const uint32_t bank = (uint32_t)(BG[base + off] >> 4);
            ++s_tab5_bg_map_bank_count[region][bank];
        }
        for (uint32_t bank = 0u; bank < 16u; ++bank)
            if (s_tab5_bg_map_bank_count[region][bank])
                s_tab5_bg_map_bank_mask[region] |= (uint16_t)(1u << bank);
    }
}

static INLINE void BG_Tab5MapBankByteChange(uint32_t adr, uint8_t oldv, uint8_t newv)
{
    if (adr < 0x4000u || adr >= 0x8000u || (adr & 1u))
        return;
    const uint32_t region = (adr >= 0x6000u) ? 1u : 0u;
    const uint32_t oldb = (uint32_t)(oldv >> 4);
    const uint32_t newb = (uint32_t)(newv >> 4);
    if (oldb == newb)
        return;
    if (s_tab5_bg_map_bank_count[region][oldb] &&
        --s_tab5_bg_map_bank_count[region][oldb] == 0u)
        s_tab5_bg_map_bank_mask[region] &= (uint16_t)~(1u << oldb);
    if (s_tab5_bg_map_bank_count[region][newb]++ == 0u)
        s_tab5_bg_map_bank_mask[region] |= (uint16_t)(1u << newb);
}

static INLINE uint16_t BG_Tab5SpriteBankMask(void)
{
    TAB5_RELEASE_DIAG_INC(s_tab5_sprite_bank_queries);
    if (s_tab5_sprite_bank_mask_dirty) {
        uint16_t mask = 0u;
        for (uint32_t n = 0u; n < 128u; ++n) {
            const uint32_t off = n * 8u + 4u;
            const uint16_t ctrl = (uint16_t)Sprite_Regs[off] |
                                  (uint16_t)((uint16_t)Sprite_Regs[off + 1u] << 8);
            mask |= (uint16_t)(1u << ((ctrl >> 8) & 0x0fu));
        }
        s_tab5_sprite_bank_mask = mask;
        s_tab5_sprite_bank_mask_dirty = 0u;
        TAB5_RELEASE_DIAG_INC(s_tab5_sprite_bank_rebuilds);
    }
    return s_tab5_sprite_bank_mask;
}

uint32_t BG_Tab5TextPalBankMayAffect(uint32_t bank)
{
    if (bank >= 16u) return 1u;
    uint16_t mask = 1u; /* Text pixels directly use palette indices 0..15. */

    if (BG_BG0TOP == 0x4000u) mask |= s_tab5_bg_map_bank_mask[0];
    else if (BG_BG0TOP == 0x6000u) mask |= s_tab5_bg_map_bank_mask[1];

    if (BG_BG1TOP == 0x4000u) mask |= s_tab5_bg_map_bank_mask[0];
    else if (BG_BG1TOP == 0x6000u) mask |= s_tab5_bg_map_bank_mask[1];

    mask |= BG_Tab5SpriteBankMask();
    return (mask & (uint16_t)(1u << bank)) ? 1u : 0u;
}

static INLINE void BG_Tab5InvSignature(uint32_t scroll_y, TAB5_BG_INV_SIG *sig)
{
    const int s1 = (((BG_Regs[0x11] & 4) ? 2 : 1) -
                    ((BG_Regs[0x11] & 16) ? 1 : 0));
    const int s2 = (((CRTC_Regs[0x29] & 4) ? 2 : 1) -
                    ((CRTC_Regs[0x29] & 16) ? 1 : 0));
    sig->scroll_y = scroll_y;
    sig->bg_vline = BG_VLINE;
    sig->vdiff = (BG_Regs[0x11] & 16)
        ? 0
        : ((BG_Regs[0x0f] >> s1) - (CRTC_Regs[0x0d] >> s2));
    sig->s1 = (uint8_t)s1;
    sig->s2 = (uint8_t)s2;
    sig->chr16 = (uint8_t)(BG_CHRSIZE == 16);
    sig->valid = 1u;
}

static INLINE int BG_Tab5InvSigEqual(const TAB5_BG_INV_SIG *a,
                                     const TAB5_BG_INV_SIG *b)
{
    return a->valid &&
           a->scroll_y == b->scroll_y &&
           a->bg_vline == b->bg_vline &&
           a->vdiff == b->vdiff &&
           a->s1 == b->s1 &&
           a->s2 == b->s2 &&
           a->chr16 == b->chr16;
}

static void BG_Tab5RebuildInv(int plane, const TAB5_BG_INV_SIG *sig)
{
    uint16_t *head = s_tab5_bg_inv_head[plane];
    uint16_t *next = s_tab5_bg_inv_next[plane];
    for (uint32_t r = 0u; r < 64u; ++r)
        head[r] = 0xffffu;

    for (uint32_t y = 0u; y < 1024u; ++y) {
        uint32_t vbg = y;
        vbg <<= sig->s1;
        vbg >>= sig->s2;
        if (!(BG_Regs[0x11] & 16))
            vbg -= (uint32_t)sig->vdiff;
        const uint32_t src_y = sig->scroll_y + vbg - (uint32_t)sig->bg_vline;
        const uint32_t row = (src_y >> (sig->chr16 ? 4u : 3u)) & 63u;
        next[y] = head[row];
        head[row] = (uint16_t)y;
    }
    s_tab5_bg_inv_sig[plane] = *sig;
    TAB5_RELEASE_DIAG_INC(s_tab5_bg_inv_rebuilds);
}

static void BG_Tab5MarkMapRowDirty(uint32_t adr, uint16_t top,
                                   uint32_t scroll_y, int plane)
{
    TAB5_BG_INV_SIG sig;
    BG_Tab5InvSignature(scroll_y, &sig);
    if (!BG_Tab5InvSigEqual(&s_tab5_bg_inv_sig[plane], &sig))
        BG_Tab5RebuildInv(plane, &sig);

    const uint32_t row = ((adr - (uint32_t)top) >> 7) & 63u;
    uint32_t lines = 0u;
    uint32_t visits = 0u;
    for (uint16_t y = s_tab5_bg_inv_head[plane][row];
         y != 0xffffu;
         y = s_tab5_bg_inv_next[plane][y]) {
        ++visits;
        if (!TextDirtyLine[y]) ++lines;
        TextDirtyLine[y] = 1u;
    }

    TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_bg_inv_visits, visits);
    TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_bg_local_lines, lines);
    if (plane == 0)
        TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_bg_map0_local, 1u);
    else
        TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_bg_map1_local, 1u);
}

void BG_Tab5DirtyPruneStatsTake(uint32_t *pattern_full,
                                uint32_t *map0_local,
                                uint32_t *map1_local,
                                uint32_t *local_lines,
                                uint32_t *inactive_skip,
                                uint32_t *inv_rebuilds,
                                uint32_t *inv_visits,
                                uint32_t *sprite_mask_rebuilds,
                                uint32_t *sprite_mask_queries)
{
    if (pattern_full) *pattern_full = __atomic_exchange_n(&s_tab5_bg_pattern_full, 0u, __ATOMIC_RELAXED);
    if (map0_local) *map0_local = __atomic_exchange_n(&s_tab5_bg_map0_local, 0u, __ATOMIC_RELAXED);
    if (map1_local) *map1_local = __atomic_exchange_n(&s_tab5_bg_map1_local, 0u, __ATOMIC_RELAXED);
    if (local_lines) *local_lines = __atomic_exchange_n(&s_tab5_bg_local_lines, 0u, __ATOMIC_RELAXED);
    if (inactive_skip) *inactive_skip = __atomic_exchange_n(&s_tab5_bg_inactive_skip, 0u, __ATOMIC_RELAXED);
    if (inv_rebuilds) *inv_rebuilds = __atomic_exchange_n(&s_tab5_bg_inv_rebuilds, 0u, __ATOMIC_RELAXED);
    if (inv_visits) *inv_visits = __atomic_exchange_n(&s_tab5_bg_inv_visits, 0u, __ATOMIC_RELAXED);
    if (sprite_mask_rebuilds) *sprite_mask_rebuilds = __atomic_exchange_n(&s_tab5_sprite_bank_rebuilds, 0u, __ATOMIC_RELAXED);
    if (sprite_mask_queries) *sprite_mask_queries = __atomic_exchange_n(&s_tab5_sprite_bank_queries, 0u, __ATOMIC_RELAXED);
}

void BG_Tab5RenderFastStatsTake(uint32_t *bg1_first_hits,
                                uint32_t *bg1_first_fallback,
                                uint32_t *bg0_first_hits,
                                uint32_t *bg0_first_fallback,
                                uint32_t *sprite_y_rebuilds,
                                uint32_t *sprite_y_probes,
                                uint32_t *sprite_y_items)
{
    if (bg1_first_hits) *bg1_first_hits = __atomic_exchange_n(&s_tab5_bg1_first_hits, 0u, __ATOMIC_RELAXED);
    if (bg1_first_fallback) *bg1_first_fallback = __atomic_exchange_n(&s_tab5_bg1_first_fallback, 0u, __ATOMIC_RELAXED);
    if (bg0_first_hits) *bg0_first_hits = __atomic_exchange_n(&s_tab5_bg0_first_hits, 0u, __ATOMIC_RELAXED);
    if (bg0_first_fallback) *bg0_first_fallback = __atomic_exchange_n(&s_tab5_bg0_first_fallback, 0u, __ATOMIC_RELAXED);
    if (sprite_y_rebuilds) *sprite_y_rebuilds = __atomic_exchange_n(&s_tab5_sprite_y_rebuilds, 0u, __ATOMIC_RELAXED);
    if (sprite_y_probes) *sprite_y_probes = __atomic_exchange_n(&s_tab5_sprite_y_probes, 0u, __ATOMIC_RELAXED);
    if (sprite_y_items) *sprite_y_items = __atomic_exchange_n(&s_tab5_sprite_y_items, 0u, __ATOMIC_RELAXED);
}
void BG_Tab5FuseStatsTake(uint32_t *lines, uint32_t *fallback, uint32_t *blocks)
{
    if (lines) *lines = __atomic_exchange_n(&s_tab5_bg_fuse_lines, 0u, __ATOMIC_RELAXED);
    if (fallback) *fallback = __atomic_exchange_n(&s_tab5_bg_fuse_fallback, 0u, __ATOMIC_RELAXED);
    if (blocks) *blocks = __atomic_exchange_n(&s_tab5_bg_fuse_blocks, 0u, __ATOMIC_RELAXED);
}
#else
void BG_Tab5DirtyPruneStatsTake(uint32_t *pattern_full,
                                uint32_t *map0_local,
                                uint32_t *map1_local,
                                uint32_t *local_lines,
                                uint32_t *inactive_skip,
                                uint32_t *inv_rebuilds,
                                uint32_t *inv_visits,
                                uint32_t *sprite_mask_rebuilds,
                                uint32_t *sprite_mask_queries)
{
    if (pattern_full) *pattern_full = 0u;
    if (map0_local) *map0_local = 0u;
    if (map1_local) *map1_local = 0u;
    if (local_lines) *local_lines = 0u;
    if (inactive_skip) *inactive_skip = 0u;
    if (inv_rebuilds) *inv_rebuilds = 0u;
    if (inv_visits) *inv_visits = 0u;
    if (sprite_mask_rebuilds) *sprite_mask_rebuilds = 0u;
    if (sprite_mask_queries) *sprite_mask_queries = 0u;
}
void BG_Tab5RenderFastStatsTake(uint32_t *bg1_first_hits,
                                uint32_t *bg1_first_fallback,
                                uint32_t *bg0_first_hits,
                                uint32_t *bg0_first_fallback,
                                uint32_t *sprite_y_rebuilds,
                                uint32_t *sprite_y_probes,
                                uint32_t *sprite_y_items)
{
    if (bg1_first_hits) *bg1_first_hits = 0u;
    if (bg1_first_fallback) *bg1_first_fallback = 0u;
    if (bg0_first_hits) *bg0_first_hits = 0u;
    if (bg0_first_fallback) *bg0_first_fallback = 0u;
    if (sprite_y_rebuilds) *sprite_y_rebuilds = 0u;
    if (sprite_y_probes) *sprite_y_probes = 0u;
    if (sprite_y_items) *sprite_y_items = 0u;
}
void BG_Tab5FuseStatsTake(uint32_t *lines, uint32_t *fallback, uint32_t *blocks)
{
    if (lines) *lines = 0u;
    if (fallback) *fallback = 0u;
    if (blocks) *blocks = 0u;
}
#endif

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

	/* Derived caches are intentionally not serialized. */
	if (load) {
        Sprite_ActiveDirty = 1;
#ifdef ESP_PLATFORM
        BG_Tab5RebuildMapBankCounts();
        s_tab5_bg_inv_sig[0].valid = s_tab5_bg_inv_sig[1].valid = 0u;
        s_tab5_sprite_bank_mask_dirty = 1u;
#endif
    }
	return ret;
}


void BG_Init(void)
{
	uint32_t i;
	memset(Sprite_Regs, 0, 0x800);
	Sprite_ActiveDirty = 1;
	memset(BG, 0, 0x8000);
#ifdef ESP_PLATFORM
    BG_Tab5RebuildMapBankCounts();
    s_tab5_bg_inv_sig[0].valid = s_tab5_bg_inv_sig[1].valid = 0u;
    s_tab5_sprite_bank_mask_dirty = 1u;
    BG_Tab5FuseSelfCheck();
#endif
	memset(BGCHR8, 0, 8*8*256);
	memset(BGCHR16, 0, 16*16*256);
	memset(BG_LineBuf, 0, 1600*2);
	for (i=0; i<0x12; i++)
		BG_Write(0xeb0800+i, 0);
	BG_CHREND = 0x8000;
	TAB5_R57_BGPOST(5u, 0u, 0u); /* R57b ordered CPU0 shadow reset */
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
#ifdef ESP_PLATFORM
            s_tab5_sprite_bank_mask_dirty = 1u;
#endif

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
			TAB5_R57_BGPOST(2u, adr, data);

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
		TAB5_R57_BGPOST(3u, adr, data);
		switch(adr)
		{
		case 0x00:
		case 0x01:
			BG0ScrollX = (((uint32_t)BG_Regs[0x00]<<8)+BG_Regs[0x01])&BG_AdrMask;
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
			break;
		case 0x02:
		case 0x03:
			BG0ScrollY = (((uint32_t)BG_Regs[0x02]<<8)+BG_Regs[0x03])&BG_AdrMask;
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
			break;
		case 0x04:
		case 0x05:
			BG1ScrollX = (((uint32_t)BG_Regs[0x04]<<8)+BG_Regs[0x05])&BG_AdrMask;
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
			break;
		case 0x06:
		case 0x07:
			BG1ScrollY = (((uint32_t)BG_Regs[0x06]<<8)+BG_Regs[0x07])&BG_AdrMask;
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
			break;

		case 0x08:		/* BG On/Off Changed */
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
			break;

		case 0x0d:
			BG_HAdjust = ((int32_t)BG_Regs[0x0d] - (CRTC_HSTART + 4)) * 8; /* Isn't it necessary to divide the horizontal resolution by 1/2? (Tetris) */
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
			break;
		case 0x0f:
			BG_VLINE = ((int32_t)BG_Regs[0x0f] - CRTC_VSTART) / ((BG_Regs[0x11] & 4) ? 1 : 2); /* Difference when BG and other elements are misaligned */
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
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
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_REG);
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
#ifdef ESP_PLATFORM
        const uint8_t tab5_old_bg = BG[adr];
#endif
		BG_HOST_SOURCE_BARRIER();
#ifdef ESP_PLATFORM
        BG_Tab5MapBankByteChange(adr, tab5_old_bg, data);
#endif
		BG[adr] = data;
		TAB5_R57_BGPOST(4u, adr, data);
		if (adr<0x2000)
		{
			BGCHR8[adr*2]   = data>>4;
			BGCHR8[adr*2+1] = data&15;
		}
		bg16chr = ((adr&3)*2)+((adr&0x3c)*4)+((adr&0x40)>>3)+((adr&0x7f80)*2);
		BGCHR16[bg16chr]   = data>>4;
		BGCHR16[bg16chr+1] = data&15;

#ifdef ESP_PLATFORM
        /* BAT177NW6: pattern data is shared by BG and sprites, so keep the
         * conservative full dirty there.  Active map writes are localized to
         * the output lines that can sample that tile row.  If a plane is
         * disabled, its map write cannot affect the current image; enabling
         * the plane later is a BG-register full dirty. */
        if (adr < BG_CHREND) {                         /* pattern area */
            TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_bg_pattern_full, 1u);
            TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_MEM);
        } else {
            if ((adr >= BG_BG1TOP) && (adr < BG_BG1END)) { /* BG1 MAP */
                if ((BG_Regs[9] & 8u) && BG_CHRSIZE == 8u)
                    BG_Tab5MarkMapRowDirty(adr, BG_BG1TOP, BG1ScrollY, 1);
                else
                    TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_bg_inactive_skip, 1u);
            }
            if ((adr >= BG_BG0TOP) && (adr < BG_BG0END)) { /* BG0 MAP */
                if (BG_Regs[9] & 1u)
                    BG_Tab5MarkMapRowDirty(adr, BG_BG0TOP, BG0ScrollY, 0);
                else
                    TAB5_RELEASE_DIAG_ATOMIC_ADD(&s_tab5_bg_inactive_skip, 1u);
            }
        }
#else
		if (adr<BG_CHREND)
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_MEM);
		if ((adr>=BG_BG1TOP)&&(adr<BG_BG1END))
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_MEM);
		if ((adr>=BG_BG0TOP)&&(adr<BG_BG0END))
			TVRAM_SetAllDirtyReason(TAB5_DIRTY_ALL_BG_MEM);
#endif
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
#ifdef ESP_PLATFORM
    memset(s_tab5_sprite_y_head, 0xff, sizeof(s_tab5_sprite_y_head));
    memset(s_tab5_sprite_y_next, 0xff, sizeof(s_tab5_sprite_y_next));
#endif
    for (n = 127; n >= 0; --n) {
        unsigned pri = sct[n].sprite_ply & 3u;
        if (pri >= 1 && pri <= 3) {
            Sprite_ActiveIdx[pri - 1][Sprite_ActiveCount[pri - 1]++] = (uint8_t)n;
#ifdef ESP_PLATFORM
            {
                const uint32_t sy = (uint32_t)sct[n].sprite_posy & 0x3ffu;
                s_tab5_sprite_y_next[n] = s_tab5_sprite_y_head[pri - 1][sy];
                s_tab5_sprite_y_head[pri - 1][sy] = (uint8_t)n;
            }
#endif
        }
    }
#ifdef ESP_PLATFORM
    TAB5_RELEASE_DIAG_INC(s_tab5_sprite_y_rebuilds);
#endif
    Sprite_ActiveDirty = 0;
    if (!s_reported) {
        s_reported = 1;
        printf("PX68K_BGSP: Build 5.37 active-sprite index cache ACTIVE pri=%u/%u/%u\n",
               (unsigned)Sprite_ActiveCount[0],
               (unsigned)Sprite_ActiveCount[1],
               (unsigned)Sprite_ActiveCount[2]);
    }
}

/* Build 5.50a: capture only the scanline-specific scalar state and the small
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

#ifdef ESP_PLATFORM
    /* R57E42: exact inverse-Y enumeration.  Legacy visibility is true iff
     * d=(posy-VLINEBG+BG_VLINE) in [1,16].  Therefore the only possible
     * posy buckets are (VLINEBG-BG_VLINE+d) modulo 2^32 for d=1..16. */
    {
        const uint32_t base = VLINEBG - (uint32_t)BG_VLINE;
        uint32_t probes = 0u, items = 0u;
        for (p = 0; p < 3; ++p) {
            uint32_t d;
            if (Sprite_ActiveCount[p] == 0u)
                continue;
            for (d = 1u; d <= 16u; ++d) {
                const uint32_t sy = base + d;
                uint8_t n;
                ++probes;
                if (sy > 0x3ffu)
                    continue;
                for (n = s_tab5_sprite_y_head[p][sy]; n != 0xffu;
                     n = s_tab5_sprite_y_next[n]) {
                    SPRITECTRLTBL_T *sctp = &sct[n];
                    const uint32_t t = (sctp->sprite_posx + BG_HAdjust) & 0x3ffu;
                    SPRITE_LINE_ITEM_T *it;
                    ++items;
                    if (t >= (uint32_t)TextDotX + 16u)
                        continue;
                    it = &buckets[p][counts[p]++];
                    it->t       = (uint16_t)t;
                    it->ctrl    = sctp->sprite_ctrl;
                    it->pri_key = (uint16_t)((uint32_t)n * 8u);
                    it->y       = (uint8_t)(16u - d);
                    ++total;
                }
            }
        }
        TAB5_RELEASE_DIAG_ADD(s_tab5_sprite_y_probes, probes);
        TAB5_RELEASE_DIAG_ADD(s_tab5_sprite_y_items, items);
    }
#else
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
#endif

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

#define BG_BLIT_GD_FIRST(CNT, STEP) do {                                \
    uint16_t *dst__ = &BG_LineBuf[1 + edi];                             \
    uint8_t  *tr__  = &Text_TrFlag[1 + edi];                            \
    int jj__;                                                           \
    if (pal_hi) {                                                       \
        for (jj__ = 0; jj__ < (CNT); ++jj__) {                          \
            const uint8_t dat__ = (uint8_t)((*esi & 0x0f) | pal_hi);    \
            *dst__ = TextPal[dat__];                                    \
            *tr__ |= 2;                                                  \
            esi += (STEP); ++dst__; ++tr__;                             \
        }                                                               \
    } else {                                                            \
        for (jj__ = 0; jj__ < (CNT); ++jj__) {                          \
            const uint8_t pix__ = (uint8_t)(*esi & 0x0f);               \
            if (pix__) { *dst__ = TextPal[pix__]; *tr__ |= 2; }         \
            esi += (STEP); ++dst__; ++tr__;                             \
        }                                                               \
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
                               uint32_t BGScrollY, int32_t adjust, int ng, int first)
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
            if (first) {
                if (rev) BG_BLIT_GD_FIRST(8, -1);
                else     BG_BLIT_GD_FIRST(8, +1);
            } else {
                if (rev) BG_BLIT_GD(8, -1);
                else     BG_BLIT_GD(8, +1);
            }
        }

        ecx = (ecx + 2) & 0x7f;
    }
}

static void bg_drawline_loopx16(uint16_t BGTOP, uint32_t BGScrollX,
                                uint32_t BGScrollY, int32_t adjust, int ng, int first)
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
            if (first) {
                if (rev) BG_BLIT_GD_FIRST(16, -1);
                else     BG_BLIT_GD_FIRST(16, +1);
            } else {
                if (rev) BG_BLIT_GD(16, -1);
                else     BG_BLIT_GD(16, +1);
            }
        }

        ecx = (ecx + 2) & 0x7f;
    }
}

#undef BG_BLIT_GD
#undef BG_BLIT_GD_FIRST
#undef BG_BLIT_NG

#ifdef ESP_PLATFORM
/* R57E43: compact first-plane decode.  This preserves the exact map/pattern
 * address transform from bg_drawline_loopx8(), but emits the 8-bit TextPal
 * index rather than touching RGB565/Text_TrFlag.  All visible destination
 * pixels [16,16+TextDotX) are covered regardless of the sub-tile scroll. */
static INLINE void bg_idx_store8(uint8_t *dst, const uint8_t *src, int rev, uint8_t pal_hi)
{
    if (!rev) {
        if (pal_hi == 0u) {
            memcpy(dst, src, 8u);
        } else {
            dst[0] = (uint8_t)(src[0] | pal_hi);
            dst[1] = (uint8_t)(src[1] | pal_hi);
            dst[2] = (uint8_t)(src[2] | pal_hi);
            dst[3] = (uint8_t)(src[3] | pal_hi);
            dst[4] = (uint8_t)(src[4] | pal_hi);
            dst[5] = (uint8_t)(src[5] | pal_hi);
            dst[6] = (uint8_t)(src[6] | pal_hi);
            dst[7] = (uint8_t)(src[7] | pal_hi);
        }
    } else {
        dst[0] = (uint8_t)(src[ 0] | pal_hi);
        dst[1] = (uint8_t)(src[-1] | pal_hi);
        dst[2] = (uint8_t)(src[-2] | pal_hi);
        dst[3] = (uint8_t)(src[-3] | pal_hi);
        dst[4] = (uint8_t)(src[-4] | pal_hi);
        dst[5] = (uint8_t)(src[-5] | pal_hi);
        dst[6] = (uint8_t)(src[-6] | pal_hi);
        dst[7] = (uint8_t)(src[-7] | pal_hi);
    }
}

static void bg_decode_index_line8(uint16_t BGTOP, uint32_t BGScrollX,
                                  uint32_t BGScrollY, int32_t adjust,
                                  uint8_t *out)
{
    int i;
    uint32_t ebp = ((BGScrollY + VLINEBG - BG_VLINE) & 7u) << 3;
    uint32_t edx = BGTOP + (((BGScrollY + VLINEBG - BG_VLINE) & 0x1f8u) << 4);
    uint32_t edi = ((BGScrollX - adjust) & 7u) ^ 15u;
    uint32_t ecx = ((BGScrollX - adjust) & 0x1f8u) >> 2;

    for (i = TextDotX >> 3; i >= 0; --i) {
        const uint8_t *mp = &BG[ecx + edx];
#if !defined(MSB_FIRST)
        const uint16_t mw = *(const uint16_t *)mp;
        uint8_t map = (uint8_t)mw;
        uint16_t si = (uint16_t)(mw >> 8) << 6;
#else
        uint8_t map = mp[0];
        uint16_t si = (uint16_t)mp[1] << 6;
#endif
        const uint8_t pal_hi = (uint8_t)(map << 4);
        const uint8_t *src;
        int rev;

        if (map < 0x40) {
            src = &BGCHR8[si + ebp];
            rev = 0;
        } else if ((map - 0x40) & 0x80) {
            src = &BGCHR8[si + 0x3f - ebp];
            rev = 1;
        } else if ((int8_t)map >= 0x40) {
            src = &BGCHR8[si + ebp + 7];
            rev = 1;
        } else {
            src = &BGCHR8[si + 0x38 - ebp];
            rev = 0;
        }

        bg_idx_store8(&out[1u + edi], src, rev, pal_hi);
        edi += 8u;
        ecx = (ecx + 2u) & 0x7fu;
    }
}

static INLINE void bg_fuse_pixel(uint32_t pos, uint8_t pix0, uint8_t pal0_hi)
{
    const uint8_t idx1 = s_tab5_bg1_idx[pos];
    uint8_t idx;

    /* Exact reduction of the two legacy GD passes with no priority-1/2
     * sprite writer between them:
     *   BG0 low-nibble != 0  -> BG0 always wins;
     *   BG0 color-0          -> BG1 wins when BG1 wrote;
     *   otherwise            -> BG0 palette color-0 if its bank is nonzero. */
    if (pix0 != 0u)
        idx = (uint8_t)(pix0 | pal0_hi);
    else if (idx1 != 0u)
        idx = idx1;
    else
        idx = pal0_hi;

    if (idx != 0u) {
        BG_LineBuf[pos] = TextPal[idx];
        Text_TrFlag[pos] |= 2u;
    }
}

static INLINE void bg_fuse_full8(uint32_t pos, const uint8_t *src, int rev, uint8_t pal0_hi)
{
    if (!rev) {
        bg_fuse_pixel(pos + 0u, src[0], pal0_hi);
        bg_fuse_pixel(pos + 1u, src[1], pal0_hi);
        bg_fuse_pixel(pos + 2u, src[2], pal0_hi);
        bg_fuse_pixel(pos + 3u, src[3], pal0_hi);
        bg_fuse_pixel(pos + 4u, src[4], pal0_hi);
        bg_fuse_pixel(pos + 5u, src[5], pal0_hi);
        bg_fuse_pixel(pos + 6u, src[6], pal0_hi);
        bg_fuse_pixel(pos + 7u, src[7], pal0_hi);
    } else {
        bg_fuse_pixel(pos + 0u, src[ 0], pal0_hi);
        bg_fuse_pixel(pos + 1u, src[-1], pal0_hi);
        bg_fuse_pixel(pos + 2u, src[-2], pal0_hi);
        bg_fuse_pixel(pos + 3u, src[-3], pal0_hi);
        bg_fuse_pixel(pos + 4u, src[-4], pal0_hi);
        bg_fuse_pixel(pos + 5u, src[-5], pal0_hi);
        bg_fuse_pixel(pos + 6u, src[-6], pal0_hi);
        bg_fuse_pixel(pos + 7u, src[-7], pal0_hi);
    }
}

static void bg_drawline_fused8(uint16_t BG1TOP, uint32_t BG1SX, uint32_t BG1SY,
                               uint16_t BG0TOP, uint32_t BG0SX, uint32_t BG0SY,
                               int32_t adjust)
{
    int i;
    const uint32_t vis_lo = 16u;
    const uint32_t vis_hi = 16u + (uint32_t)TextDotX;
    uint32_t ebp = ((BG0SY + VLINEBG - BG_VLINE) & 7u) << 3;
    uint32_t edx = BG0TOP + (((BG0SY + VLINEBG - BG_VLINE) & 0x1f8u) << 4);
    uint32_t edi = ((BG0SX - adjust) & 7u) ^ 15u;
    uint32_t ecx = ((BG0SX - adjust) & 0x1f8u) >> 2;

    bg_decode_index_line8(BG1TOP, BG1SX, BG1SY, adjust, s_tab5_bg1_idx);

    for (i = TextDotX >> 3; i >= 0; --i) {
        const uint8_t *mp = &BG[ecx + edx];
#if !defined(MSB_FIRST)
        const uint16_t mw = *(const uint16_t *)mp;
        uint8_t map = (uint8_t)mw;
        uint16_t si = (uint16_t)(mw >> 8) << 6;
#else
        uint8_t map = mp[0];
        uint16_t si = (uint16_t)mp[1] << 6;
#endif
        const uint8_t pal_hi = (uint8_t)(map << 4);
        const uint8_t *src;
        int rev;
        const uint32_t pos = 1u + edi;

        if (map < 0x40) {
            src = &BGCHR8[si + ebp];
            rev = 0;
        } else if ((map - 0x40) & 0x80) {
            src = &BGCHR8[si + 0x3f - ebp];
            rev = 1;
        } else if ((int8_t)map >= 0x40) {
            src = &BGCHR8[si + ebp + 7];
            rev = 1;
        } else {
            src = &BGCHR8[si + 0x38 - ebp];
            rev = 0;
        }

        if (pos >= vis_lo && pos + 8u <= vis_hi) {
            bg_fuse_full8(pos, src, rev, pal_hi);
        } else {
            uint32_t j0 = pos < vis_lo ? vis_lo - pos : 0u;
            uint32_t j1 = pos + 8u > vis_hi ? vis_hi - pos : 8u;
            uint32_t j;
            if (j0 < 8u && j1 <= 8u && j0 < j1) {
                for (j = j0; j < j1; ++j) {
                    const uint8_t pix0 = rev ? src[-(int)j] : src[j];
                    bg_fuse_pixel(pos + j, pix0, pal_hi);
                }
            }
        }

        edi += 8u;
        ecx = (ecx + 2u) & 0x7fu;
    }

}

/* BAT177NW18/R57E48: index-domain version of the already-proven R57E43
 * BG1+BG0 fusion.  Eligibility deliberately matches the R57E43 GD proof:
 * 8px BG1+BG0, no priority-1/2 sprite between the planes.  Priority-3
 * sprites are then applied in the same order using the same sprite-number
 * tie-break buffer, but only compact TextPal indices are written. */
static INLINE void Sprite_DrawPreparedIndex(const SPRITE_LINE_ITEM_T *items,
                                             int count, uint8_t *out)
{
    for (int k = 0; k < count; ++k) {
        const SPRITE_LINE_ITEM_T *it = &items[k];
        uint32_t t = it->t;
        uint16_t ctrl = it->ctrl;
        uint8_t *cp;
        uint32_t pal_base;
        int d;

        if (ctrl < 0x4000) {
            cp = &BGCHR16[((ctrl * 256) & 0xffff) + ((uint32_t)it->y * 16)]; d = 1;
        } else if ((ctrl - 0x4000) & 0x8000) {
            cp = &BGCHR16[((ctrl * 256) & 0xffff) + ((((uint32_t)it->y * 16) & 0xff) ^ 0xf0) + 15]; d = -1;
        } else if ((int16_t)ctrl >= 0x4000) {
            cp = &BGCHR16[((ctrl * 256) & 0xffff) + ((uint32_t)it->y * 16) + 15]; d = -1;
        } else {
            cp = &BGCHR16[((ctrl << 8) & 0xffff) + ((((uint32_t)it->y * 16) & 0xff) ^ 0xf0)]; d = 1;
        }
        pal_base = (ctrl >> 4) & 0xf0;
        for (int i = 0; i < 16; ++i, ++t, cp += d) {
            uint32_t pal = *cp & 0xf;
            if (pal) {
                pal |= pal_base;
                if (BG_PriBuf[t] >= it->pri_key) {
                    out[t] = (uint8_t)pal;
                    BG_PriBuf[t] = it->pri_key;
                }
            }
        }
    }
}

static INLINE void bg_fuse_index_pixel(uint8_t *out, uint32_t pos,
                                        uint8_t pix0, uint8_t pal0_hi)
{
    const uint8_t idx1 = s_tab5_bg1_idx[pos];
    uint8_t idx;
    if (pix0 != 0u) idx = (uint8_t)(pix0 | pal0_hi);
    else if (idx1 != 0u) idx = idx1;
    else idx = pal0_hi;
    out[pos] = idx;
}

static __attribute__((hot, optimize("O3"))) void bg_drawline_fused_index8(uint16_t BG1TOP, uint32_t BG1SX, uint32_t BG1SY,
                                      uint16_t BG0TOP, uint32_t BG0SX, uint32_t BG0SY,
                                      int32_t adjust, uint8_t *out)
{
    const uint32_t vis_lo = 16u;
    const uint32_t vis_hi = 16u + (uint32_t)TextDotX;
    uint32_t ebp = ((BG0SY + VLINEBG - BG_VLINE) & 7u) << 3;
    uint32_t edx = BG0TOP + (((BG0SY + VLINEBG - BG_VLINE) & 0x1f8u) << 4);
    uint32_t edi = ((BG0SX - adjust) & 7u) ^ 15u;
    uint32_t ecx = ((BG0SX - adjust) & 0x1f8u) >> 2;

    bg_decode_index_line8(BG1TOP, BG1SX, BG1SY, adjust, s_tab5_bg1_idx);
    for (int i = TextDotX >> 3; i >= 0; --i) {
        const uint8_t *mp = &BG[ecx + edx];
#if !defined(MSB_FIRST)
        const uint16_t mw = *(const uint16_t *)mp;
        uint8_t map = (uint8_t)mw;
        uint16_t si = (uint16_t)(mw >> 8) << 6;
#else
        uint8_t map = mp[0];
        uint16_t si = (uint16_t)mp[1] << 6;
#endif
        const uint8_t pal_hi = (uint8_t)(map << 4);
        const uint8_t *src;
        int rev;
        const uint32_t pos = 1u + edi;
        if (map < 0x40) { src = &BGCHR8[si + ebp]; rev = 0; }
        else if ((map - 0x40) & 0x80) { src = &BGCHR8[si + 0x3f - ebp]; rev = 1; }
        else if ((int8_t)map >= 0x40) { src = &BGCHR8[si + ebp + 7]; rev = 1; }
        else { src = &BGCHR8[si + 0x38 - ebp]; rev = 0; }

        uint32_t j0 = pos < vis_lo ? vis_lo - pos : 0u;
        uint32_t j1 = pos + 8u > vis_hi ? vis_hi - pos : 8u;
        if (j0 < 8u && j1 <= 8u && j0 < j1) {
            for (uint32_t j = j0; j < j1; ++j) {
                const uint8_t pix0 = rev ? src[-(int)j] : src[j];
                bg_fuse_index_pixel(out, pos + j, pix0, pal_hi);
            }
        }
        edi += 8u;
        ecx = (ecx + 2u) & 0x7fu;
    }
}

int __attribute__((hot, optimize("O3"))) BG_Tab5DecodeIndexLine(uint8_t *out_full, uint32_t capacity, int gd)
{
    SPRITE_LINE_ITEM_T sprite_buckets[3][128];
    uint8_t sprite_counts[3];
    int visible_sprites;
    if (!out_full || capacity < 1600u || !gd || !s_tab5_bg_fuse_ok ||
        BG_CHRSIZE != 8u || !(BG_Regs[9] & 8u) || !(BG_Regs[9] & 1u) ||
        TextDotX <= 0 || (uint32_t)TextDotX + 24u > capacity)
        return 0;

    visible_sprites = Sprite_PrepareLine(sprite_buckets, sprite_counts);
    if (sprite_counts[0] != 0u || sprite_counts[1] != 0u)
        return 0;
    if (visible_sprites)
        memset(&BG_PriBuf[16], 0xff, (size_t)TextDotX * sizeof(BG_PriBuf[0]));

    TAB5_RELEASE_DIAG_INC(s_tab5_bg_fuse_lines);
    TAB5_RELEASE_DIAG_ADD(s_tab5_bg_fuse_blocks, ((uint32_t)TextDotX + 7u) >> 3);
    TAB5_RELEASE_DIAG_INC(s_tab5_bg1_first_hits);
    bg_drawline_fused_index8(BG_BG1TOP, BG1ScrollX, BG1ScrollY,
                             BG_BG0TOP, BG0ScrollX, BG0ScrollY, BG_HAdjust,
                             out_full);
    if (sprite_counts[2])
        Sprite_DrawPreparedIndex(sprite_buckets[2], sprite_counts[2], out_full);
    return 1;
}
#endif /* ESP_PLATFORM */

static INLINE void BG_DrawLineMcr8(uint16_t BGTOP, uint32_t BGScrollX,
                                   uint32_t BGScrollY)
{
    bg_drawline_loopx8(BGTOP, BGScrollX, BGScrollY, BG_HAdjust, 0, 0);
}

static INLINE void BG_DrawLineMcr16(uint16_t BGTOP, uint32_t BGScrollX,
                                    uint32_t BGScrollY)
{
    bg_drawline_loopx16(BGTOP, BGScrollX, BGScrollY, BG_HAdjust, 0, 0);
}

static INLINE void BG_DrawLineMcr8_ng(uint16_t BGTOP, uint32_t BGScrollX,
                                      uint32_t BGScrollY)
{
    bg_drawline_loopx8(BGTOP, BGScrollX, BGScrollY, BG_HAdjust, 1, 0);
}

static INLINE void BG_DrawLineMcr16_ng(uint16_t BGTOP, uint32_t BGScrollX,
                                       uint32_t BGScrollY)
{
    bg_drawline_loopx16(BGTOP, BGScrollX, BGScrollY, 0, 1, 0);
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

#ifdef ESP_PLATFORM
    /* BAT177NW13/R57E43: fuse the two adjacent 8px GD background passes when
     * no priority-1/2 sprite can intervene.  This condition is stronger than
     * either first-writer shortcut: it proves that the exact legacy BG1 then
     * BG0 result can be resolved before a single RGB565 write. */
    if (s_tab5_bg_fuse_ok && gd && BG_CHRSIZE == 8u &&
        (BG_Regs[9] & 8u) && (BG_Regs[9] & 1u) &&
        sprite_counts[0] == 0u && sprite_counts[1] == 0u &&
        TextDotX > 0 && (uint32_t)TextDotX + 24u <= (uint32_t)sizeof(s_tab5_bg1_idx)) {
        TAB5_RELEASE_DIAG_INC(s_tab5_bg_fuse_lines);
        TAB5_RELEASE_DIAG_ADD(s_tab5_bg_fuse_blocks, ((uint32_t)TextDotX + 7u) >> 3);
        TAB5_RELEASE_DIAG_INC(s_tab5_bg1_first_hits);
        bg_drawline_fused8(BG_BG1TOP, BG1ScrollX, BG1ScrollY,
                           BG_BG0TOP, BG0ScrollX, BG0ScrollY, BG_HAdjust);
        if (sprite_counts[2])
            Sprite_DrawPrepared(sprite_buckets[2], sprite_counts[2]);
        return;
    }
    TAB5_RELEASE_DIAG_INC(s_tab5_bg_fuse_fallback);
#endif

    if (sprite_counts[0])
        Sprite_DrawPrepared(sprite_buckets[0], sprite_counts[0]);

    if ((BG_Regs[9] & 8) && (BG_CHRSIZE == 8)) {
#ifdef ESP_PLATFORM
        /* R57E42: priority-1 sprites are the only bit2 writer before BG1.
         * If none are visible, BG1 is provably first and may avoid reading
         * Text_TrFlag in the GD inner loop.  NG already has no such read. */
        if (gd && sprite_counts[0] == 0) {
            TAB5_RELEASE_DIAG_INC(s_tab5_bg1_first_hits);
            bg_drawline_loopx8(BG_BG1TOP, BG1ScrollX, BG1ScrollY,
                               BG_HAdjust, 0, 1);
        } else {
            TAB5_RELEASE_DIAG_INC(s_tab5_bg1_first_fallback);
            (*func8)(BG_BG1TOP, BG1ScrollX, BG1ScrollY);
        }
#else
        (*func8)(BG_BG1TOP, BG1ScrollX, BG1ScrollY);
#endif
    }

    if (sprite_counts[1])
        Sprite_DrawPrepared(sprite_buckets[1], sprite_counts[1]);

    if (BG_Regs[9] & 1)
    {
#ifdef ESP_PLATFORM
        const int bg1_active = ((BG_Regs[9] & 8) && (BG_CHRSIZE == 8));
        const int bg0_first = (!bg1_active && sprite_counts[0] == 0 && sprite_counts[1] == 0);
        if (bg0_first) {
            TAB5_RELEASE_DIAG_INC(s_tab5_bg0_first_hits);
            if (BG_CHRSIZE == 8)
                bg_drawline_loopx8(BG_BG0TOP, BG0ScrollX, BG0ScrollY, BG_HAdjust, gd ? 0 : 1, 1);
            else
                bg_drawline_loopx16(BG_BG0TOP, BG0ScrollX, BG0ScrollY, gd ? BG_HAdjust : 0, gd ? 0 : 1, 1);
        } else {
            TAB5_RELEASE_DIAG_INC(s_tab5_bg0_first_fallback);
            if (BG_CHRSIZE == 8)
                (*func8)(BG_BG0TOP, BG0ScrollX, BG0ScrollY);
            else
                (*func16)(BG_BG0TOP, BG0ScrollX, BG0ScrollY);
        }
#else
        if (BG_CHRSIZE == 8)
            (*func8)(BG_BG0TOP, BG0ScrollX, BG0ScrollY);
        else
            (*func16)(BG_BG0TOP, BG0ScrollX, BG0ScrollY);
#endif
    }

    if (sprite_counts[2])
        Sprite_DrawPrepared(sprite_buckets[2], sprite_counts[2]);
}
