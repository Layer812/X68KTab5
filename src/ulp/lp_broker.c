#include <stdint.h>

#define LPBR_READY_MAGIC 0x4C504236u /* "LPB6" */
#define LPBR_RING_SLOTS 256u
#define LPBR_RING_MASK (LPBR_RING_SLOTS - 1u)
#define LPBR_WORDS 3u

#define R57_DOMAIN_TVRAM       1u
#define R57_DOMAIN_SPRITE      2u
#define R57_DOMAIN_BGREG       3u
#define R57_DOMAIN_BGMEM       4u
#define R57_DOMAIN_BG_RESET    5u
#define R57_DOMAIN_RASTER      6u
#define R57_DOMAIN_TVRAM_COPY  7u
#define R57_DOMAIN_TVRAM_RESET 8u

#define DIRTY_TEXT       (1u << 0)
#define DIRTY_BG         (1u << 1)
#define DIRTY_SPRITE     (1u << 2)
#define DIRTY_BGREG      (1u << 3)
#define DIRTY_TEXT_COPY  (1u << 4)
#define DIRTY_TEXT_RESET (1u << 5)
#define DIRTY_BG_RESET   (1u << 6)

volatile uint32_t lpbr_ready, lpbr_head, lpbr_tail, lpbr_cons, lpbr_seq, lpbr_gap, lpbr_hash, lpbr_heartbeat;
volatile uint32_t lpbr_hp_probe, lpbr_lp_echo, lpbr_diag_stage, lpbr_trap_mcause, lpbr_trap_mepc, lpbr_trap_mtval;
volatile uint32_t lpbr_ring[LPBR_RING_SLOTS * LPBR_WORDS];

/* R2 production channel.
 * CPU0 -> LP: LATEST touch joypad state.
 * CPU1 -> LP: rare ownership/control state (separate SPSC slot).
 * LP -> CPU1: one ordered latest output serial/value. */
volatile uint32_t lpbr_touch_in_epoch, lpbr_touch_in_value;
volatile uint32_t lpbr_touch_ctl_epoch, lpbr_touch_ctl_value;
volatile uint32_t lpbr_touch_out_serial, lpbr_touch_out_value;
volatile uint32_t lpbr_touch_coalesced;

/* R3 DIRTY workset channel.
 * LP classifies the ordered mirrored R57 stream.  Dirty facts accumulate in a
 * build set until a raster frontier, then merge into the published union.
 * CPU0 acks only the serial it actually read.  Newer serials are therefore
 * never cleared by a stale ack, and events arriving after a frontier remain in
 * the build set until the next frontier. */
volatile uint32_t lpbr_dirty_out_serial;
volatile uint32_t lpbr_dirty_ack_serial;
volatile uint32_t lpbr_dirty_frontier_seq;
volatile uint32_t lpbr_dirty_rows;
volatile uint32_t lpbr_dirty_flags;
volatile uint32_t lpbr_dirty_raw;
volatile uint32_t lpbr_dirty_coalesced;

/* R4 independent EXACT checkpoint certification. The HP authoritative R57
 * producer commits hash first and sequence last into one of four rotating
 * slots every 1024 events. With the mirror ring only 256 deep, a lossless LP
 * consumer cannot legitimately miss four checkpoint generations. */
volatile uint32_t lpbr_exact_cp_seq[4];
volatile uint32_t lpbr_exact_cp_hash[4];
volatile uint32_t lpbr_exact_cp_pass;
volatile uint32_t lpbr_exact_cp_fail;
volatile uint32_t lpbr_exact_cp_miss;
volatile uint32_t lpbr_exact_last_seq;
volatile uint32_t lpbr_exact_last_hp_hash;
volatile uint32_t lpbr_exact_last_lp_hash;
/* R6 control-plane frontier: committed only after LP has fully hash-checked
 * and classified the exact tuple. Payload remains in the HP PSRAM journal. */
volatile uint32_t lpbr_exact_release_seq;
volatile uint32_t lpbr_exact_release_hash;
volatile uint32_t lpbr_exact_release_count;

/* BAT177NW0 four-frontier video progress board. HP writers publish low-32
 * monotonic sequence values; LP is observational only and never gates them. */
volatile uint32_t lpbr_video_guest_frontier;
volatile uint32_t lpbr_video_compose_frontier;
volatile uint32_t lpbr_video_screen_frontier;
volatile uint32_t lpbr_video_visible_frontier;

static inline void fence_rw(void)
{
    __asm__ __volatile__("fence rw,rw" ::: "memory");
}

static inline uint32_t mix32(uint32_t h, uint32_t v)
{
    /* Byte-for-byte identical to the authoritative R57 producer hash. */
    h ^= v;
    h *= 16777619u;
    h ^= v >> 16;
    h *= 16777619u;
    return h;
}

static inline uint32_t tvram_row_bucket(uint32_t address)
{
    const uint32_t phys = (address & 0x1ffffu) ^ 1u;
    const uint32_t y = (phys >> 7) & 1023u;
    return 1u << (y >> 5);
}

static inline uint32_t tvram_copy_row_buckets(uint32_t address)
{
    const uint32_t dst_line = (address >> 8) & 0xffu;
    const uint32_t y0 = (dst_line << 2) & 1023u;
    uint32_t rows = 0u;
    rows |= 1u << ((y0 + 0u) >> 5);
    rows |= 1u << ((y0 + 1u) >> 5);
    rows |= 1u << ((y0 + 2u) >> 5);
    rows |= 1u << ((y0 + 3u) >> 5);
    return rows;
}

int main(void)
{
    lpbr_head = lpbr_tail = lpbr_cons = lpbr_seq = lpbr_gap = 0u;
    lpbr_hash = 2166136261u;
    lpbr_heartbeat = 1u;
    lpbr_hp_probe = lpbr_lp_echo = 0u;
    lpbr_diag_stage = 1u;
    lpbr_trap_mcause = lpbr_trap_mepc = lpbr_trap_mtval = 0u;
    lpbr_touch_in_epoch = lpbr_touch_in_value = 0u;
    lpbr_touch_ctl_epoch = lpbr_touch_ctl_value = 0u;
    lpbr_touch_out_serial = lpbr_touch_out_value = 0u;
    lpbr_touch_coalesced = 0u;
    lpbr_dirty_out_serial = 0u;
    lpbr_dirty_ack_serial = 0u;
    lpbr_dirty_frontier_seq = 0u;
    lpbr_dirty_rows = 0u;
    lpbr_dirty_flags = 0u;
    lpbr_dirty_raw = 0u;
    lpbr_dirty_coalesced = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        lpbr_exact_cp_seq[i] = 0u;
        lpbr_exact_cp_hash[i] = 0u;
    }
    lpbr_exact_cp_pass = lpbr_exact_cp_fail = lpbr_exact_cp_miss = 0u;
    lpbr_exact_last_seq = lpbr_exact_last_hp_hash = lpbr_exact_last_lp_hash = 0u;
    lpbr_exact_release_seq = lpbr_exact_release_hash = lpbr_exact_release_count = 0u;
    lpbr_video_guest_frontier = 0u;
    lpbr_video_compose_frontier = 0u;
    lpbr_video_screen_frontier = 0u;
    lpbr_video_visible_frontier = 0u;

    uint32_t touch_seen_epoch = 0u;
    uint32_t touch_ctl_seen_epoch = 0u;
    uint32_t dirty_cleared_serial = 0u;
    uint32_t dirty_build_rows = 0u;
    uint32_t dirty_build_flags = 0u;
    uint32_t dirty_build_raw = 0u;
    uint32_t dirty_build_coalesced = 0u;

    fence_rw();
    lpbr_ready = LPBR_READY_MAGIC;
    fence_rw();

    for (;;) {
        const uint32_t probe = lpbr_hp_probe;
        if (lpbr_lp_echo != probe) {
            fence_rw();
            lpbr_lp_echo = probe;
            fence_rw();
        }

        /* CPU0 acknowledged the exact published serial it consumed.  Clearing
         * the published union never touches the next-frontier build set. */
        const uint32_t dirty_ack = lpbr_dirty_ack_serial;
        const uint32_t dirty_serial = lpbr_dirty_out_serial;
        if (dirty_serial != 0u && dirty_ack == dirty_serial &&
            dirty_cleared_serial != dirty_serial) {
            lpbr_dirty_rows = 0u;
            lpbr_dirty_flags = 0u;
            lpbr_dirty_raw = 0u;
            lpbr_dirty_coalesced = 0u;
            dirty_cleared_serial = dirty_serial;
            fence_rw();
        }

        /* CPU1 ownership/control has priority.  On a force-clear, discard every
         * CPU0 LATEST state already published before this control epoch. */
        const uint32_t ctl_epoch = lpbr_touch_ctl_epoch;
        if (ctl_epoch != touch_ctl_seen_epoch) {
            fence_rw();
            const uint32_t ctl_value = lpbr_touch_ctl_value;
            touch_seen_epoch = lpbr_touch_in_epoch;
            lpbr_touch_out_value = ctl_value;
            fence_rw();
            ++lpbr_touch_out_serial;
            touch_ctl_seen_epoch = ctl_epoch;
            ++lpbr_heartbeat;
            fence_rw();
        }

        /* LATEST: a newer CPU0 epoch supersedes every unseen older value. */
        const uint32_t touch_epoch = lpbr_touch_in_epoch;
        if (touch_epoch != touch_seen_epoch) {
            fence_rw();
            const uint32_t touch_value = lpbr_touch_in_value;
            const uint32_t delta = touch_epoch - touch_seen_epoch;
            if (touch_seen_epoch != 0u && delta > 1u) {
                lpbr_touch_coalesced += delta - 1u;
            }
            lpbr_touch_out_value = touch_value;
            fence_rw();
            ++lpbr_touch_out_serial;
            touch_seen_epoch = touch_epoch;
            ++lpbr_heartbeat;
            fence_rw();
        }

        const uint32_t t = lpbr_tail;
        const uint32_t h = lpbr_head;
        if (t != h) {
            const uint32_t i = (t & LPBR_RING_MASK) * LPBR_WORDS;
            const uint32_t seq = lpbr_ring[i];
            const uint32_t addr = lpbr_ring[i + 1u];
            const uint32_t meta = lpbr_ring[i + 2u];
            fence_rw();

            const uint32_t prev = lpbr_seq;
            if (prev && seq != prev + 1u) {
                ++lpbr_gap;
            }
            uint32_t x = lpbr_hash;
            x = mix32(x, seq);
            x = mix32(x, addr);
            x = mix32(x, meta);
            lpbr_hash = x;

            if ((seq & 1023u) == 0u) {
                const uint32_t slot = (seq >> 10) & 3u;
                fence_rw();
                const uint32_t cp_seq = lpbr_exact_cp_seq[slot];
                const uint32_t cp_hash = lpbr_exact_cp_hash[slot];
                lpbr_exact_last_seq = seq;
                lpbr_exact_last_hp_hash = cp_hash;
                lpbr_exact_last_lp_hash = x;
                if (cp_seq != seq) {
                    ++lpbr_exact_cp_miss;
                } else if (cp_hash == x) {
                    ++lpbr_exact_cp_pass;
                } else {
                    ++lpbr_exact_cp_fail;
                }
                fence_rw();
            }

            const uint32_t domain = meta & 0xffu;
            if (domain == R57_DOMAIN_RASTER) {
                if (dirty_build_raw != 0u) {
                    /* Preserve any older unacked output by union/sum.  A newer
                     * serial names the enlarged lossless workset. */
                    lpbr_dirty_rows |= dirty_build_rows;
                    lpbr_dirty_flags |= dirty_build_flags;
                    lpbr_dirty_raw += dirty_build_raw;
                    lpbr_dirty_coalesced += dirty_build_coalesced;
                    lpbr_dirty_frontier_seq = seq;
                    fence_rw();
                    ++lpbr_dirty_out_serial;
                    fence_rw();
                    dirty_build_rows = 0u;
                    dirty_build_flags = 0u;
                    dirty_build_raw = 0u;
                    dirty_build_coalesced = 0u;
                }
            } else {
                uint32_t add_rows = 0u;
                uint32_t add_flags = 0u;
                if (domain == R57_DOMAIN_TVRAM) {
                    add_rows = tvram_row_bucket(addr);
                    add_flags = DIRTY_TEXT;
                } else if (domain == R57_DOMAIN_SPRITE) {
                    add_flags = DIRTY_SPRITE;
                } else if (domain == R57_DOMAIN_BGREG) {
                    add_flags = DIRTY_BGREG;
                } else if (domain == R57_DOMAIN_BGMEM) {
                    add_flags = DIRTY_BG;
                } else if (domain == R57_DOMAIN_BG_RESET) {
                    add_flags = DIRTY_BG | DIRTY_BG_RESET;
                } else if (domain == R57_DOMAIN_TVRAM_COPY) {
                    add_rows = tvram_copy_row_buckets(addr);
                    add_flags = DIRTY_TEXT | DIRTY_TEXT_COPY;
                } else if (domain == R57_DOMAIN_TVRAM_RESET) {
                    add_rows = 0xffffffffu;
                    add_flags = DIRTY_TEXT | DIRTY_TEXT_RESET;
                }

                if (add_rows || add_flags) {
                    const uint32_t before_rows = lpbr_dirty_rows | dirty_build_rows;
                    const uint32_t before_flags = lpbr_dirty_flags | dirty_build_flags;
                    const uint32_t after_rows = before_rows | add_rows;
                    const uint32_t after_flags = before_flags | add_flags;
                    dirty_build_rows |= add_rows;
                    dirty_build_flags |= add_flags;
                    ++dirty_build_raw;
                    if (after_rows == before_rows && after_flags == before_flags) {
                        ++dirty_build_coalesced;
                    }
                }
            }

            /* R6: publish a control-plane release frontier only after every
             * semantic action above is complete. Commit hash first, sequence last.
             * CPU0 still consumes the authoritative PSRAM R57 payload in BAT148. */
            lpbr_exact_release_hash = x;
            fence_rw();
            lpbr_exact_release_seq = seq;
            ++lpbr_exact_release_count;
            fence_rw();

            lpbr_seq = seq;
            ++lpbr_cons;
            lpbr_tail = t + 1u;
            fence_rw();
            ++lpbr_heartbeat;
        }
    }
    return 0;
}
