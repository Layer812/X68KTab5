#include "tab5_video_flow.h"

#include <stddef.h>
#include <string.h>

/* App-side LP broker ABI. Kept as a narrow extern to avoid a component
 * dependency cycle; publication is a single observational RTC-RAM store. */
extern void tab5_lp_broker_video_frontier_publish(uint32_t kind, uint64_t seq);
enum { FLOW_LP_GUEST=0u, FLOW_LP_COMPOSE=1u, FLOW_LP_SCREEN=2u, FLOW_LP_VISIBLE=3u };

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

typedef struct {
    volatile uint32_t write_guard; /* odd while producer mutates pixels */
    uint64_t render_seq;
    uint32_t video_epoch;
    uint32_t visual_seq;
    uint16_t width;
    uint16_t reserved;
} flow_source_meta_t;

static uint16_t *s_cpu1_fb;
static uint32_t s_cpu1_pitch;
static uint32_t s_cpu1_height;
static flow_source_meta_t s_cpu1_meta[TAB5_VIDEO_FLOW_FB_LINES];
static volatile uint32_t s_dirty_bits[(TAB5_VIDEO_FLOW_MAX_LINES + 31u) / 32u];
static volatile uint64_t s_guest_frontier;
static volatile uint64_t s_compose_frontier;
static volatile uint64_t s_screen_commit_frontier;
static volatile uint64_t s_visible_frontier;
static volatile uint64_t s_cpu1_exact_commits;
static volatile uint64_t s_cpu0_final_commits;
static volatile uint64_t s_dirty_posts;
static volatile uint64_t s_snapshot_races;
static volatile uint64_t s_stale_skips;
static volatile uint64_t s_requeues;
static volatile uint32_t s_dirty_lines;
static volatile uint32_t s_dirty_max;
static int s_ready;
static volatile uint64_t s_lp_guest_mirror;
static volatile uint64_t s_lp_compose_mirror;

static inline void flow_lp_mirror_batched(volatile uint64_t *last, uint32_t kind,
                                          uint64_t seq, uint64_t quantum)
{
    uint64_t old = __atomic_load_n(last, __ATOMIC_RELAXED);
    if (seq - old < quantum) return;
    if (__atomic_compare_exchange_n(last, &old, seq, 0,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        tab5_lp_broker_video_frontier_publish(kind, seq);
}

static inline void flow_mark_dirty(uint32_t y)
{
    const uint32_t wi = y >> 5;
    const uint32_t bit = 1u << (y & 31u);
    const uint32_t old = __atomic_fetch_or(&s_dirty_bits[wi], bit, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_dirty_posts, 1u, __ATOMIC_RELAXED);
    if (!(old & bit)) {
        const uint32_t n = __atomic_add_fetch(&s_dirty_lines, 1u, __ATOMIC_RELAXED);
        uint32_t m = __atomic_load_n(&s_dirty_max, __ATOMIC_RELAXED);
        while (n > m && !__atomic_compare_exchange_n(&s_dirty_max, &m, n, 0,
                                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        /* Only the empty->nonempty transition needs a scheduler kick.  This
         * bounds CPU1 cross-core RTOS traffic while all further lines remain
         * visible through the atomic dirty bitset. */
        if (n == 1u)
            tab5_screen_no_wait_kick();
    }
}

int tab5_video_flow_init(void)
{
    if (s_ready)
        return 1;
    /* BAT177NW1: only the cross-core CPU1 authoritative framebuffer is
     * published through video_flow. CPU0 final-compositor results stay on CPU0
     * and use Screen Manager's already-existing bounded result slots, avoiding
     * a duplicate 800x600 PSRAM framebuffer. */
    memset(s_cpu1_meta, 0, sizeof(s_cpu1_meta));
    memset((void *)s_dirty_bits, 0, sizeof(s_dirty_bits));
    s_ready = 1;
    return 1;
}

void tab5_video_flow_register_cpu1_fb(uint16_t *cpu1_fb,
                                      uint32_t pitch_pixels,
                                      uint32_t height_lines)
{
    s_cpu1_fb = cpu1_fb;
    s_cpu1_pitch = pitch_pixels;
    s_cpu1_height = height_lines;
}

uint64_t tab5_video_flow_cpu1_begin(uint32_t y)
{
    uint64_t seq = __atomic_add_fetch(&s_guest_frontier, 1u, __ATOMIC_RELAXED);
    if (!seq)
        seq = __atomic_add_fetch(&s_guest_frontier, 1u, __ATOMIC_RELAXED);
    flow_lp_mirror_batched(&s_lp_guest_mirror, FLOW_LP_GUEST, seq, 64u);
    if (y < TAB5_VIDEO_FLOW_FB_LINES) {
        uint32_t g = __atomic_load_n(&s_cpu1_meta[y].write_guard, __ATOMIC_RELAXED);
        if (g & 1u) ++g;
        __atomic_store_n(&s_cpu1_meta[y].write_guard, g + 1u, __ATOMIC_RELEASE);
    }
    return seq;
}

void tab5_video_flow_cpu1_abort(uint32_t y)
{
    if (y >= TAB5_VIDEO_FLOW_FB_LINES)
        return;
    uint32_t g = __atomic_load_n(&s_cpu1_meta[y].write_guard, __ATOMIC_RELAXED);
    if (g & 1u)
        __atomic_store_n(&s_cpu1_meta[y].write_guard, g + 1u, __ATOMIC_RELEASE);
}

void tab5_video_flow_cpu1_commit(uint32_t y, uint32_t width,
                                 uint64_t render_seq,
                                 uint32_t video_epoch,
                                 uint32_t visual_seq)
{
    if (!s_ready || !s_cpu1_fb || y >= s_cpu1_height ||
        y >= TAB5_VIDEO_FLOW_FB_LINES || !width || width > TAB5_VIDEO_FLOW_MAX_WIDTH) {
        tab5_video_flow_cpu1_abort(y);
        return;
    }
    flow_source_meta_t *m = &s_cpu1_meta[y];
    m->render_seq = render_seq;
    m->video_epoch = video_epoch;
    m->visual_seq = visual_seq;
    m->width = (uint16_t)width;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t g = __atomic_load_n(&m->write_guard, __ATOMIC_RELAXED);
    if (!(g & 1u)) ++g;
    __atomic_store_n(&m->write_guard, g + 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_cpu1_exact_commits, 1u, __ATOMIC_RELAXED);
    /* Final pixels already exist on CPU1, so the renderer/compose frontier is
     * complete at this sequence even though Screen/LCD remain downstream. */
    uint64_t cf = __atomic_load_n(&s_compose_frontier, __ATOMIC_RELAXED);
    while (render_seq > cf && !__atomic_compare_exchange_n(&s_compose_frontier, &cf,
                                                            render_seq, 0,
                                                            __ATOMIC_RELEASE,
                                                            __ATOMIC_RELAXED)) {}
    flow_lp_mirror_batched(&s_lp_compose_mirror, FLOW_LP_COMPOSE, render_seq, 64u);
    flow_mark_dirty(y);
}

void tab5_video_flow_cpu0_complete(uint64_t render_seq)
{
    uint64_t cf = __atomic_load_n(&s_compose_frontier, __ATOMIC_RELAXED);
    while (render_seq > cf && !__atomic_compare_exchange_n(&s_compose_frontier, &cf,
                                                            render_seq, 0,
                                                            __ATOMIC_RELEASE,
                                                            __ATOMIC_RELAXED)) {}
    flow_lp_mirror_batched(&s_lp_compose_mirror, FLOW_LP_COMPOSE, render_seq, 64u);
    __atomic_add_fetch(&s_cpu0_final_commits, 1u, __ATOMIC_RELAXED);
}

uint64_t tab5_video_flow_cpu1_line_seq(uint32_t y)
{
    if (y >= TAB5_VIDEO_FLOW_FB_LINES)
        return 0u;
    return __atomic_load_n(&s_cpu1_meta[y].render_seq, __ATOMIC_ACQUIRE);
}

uint32_t tab5_video_flow_take_dirty_word(uint32_t word_index)
{
    if (word_index >= (TAB5_VIDEO_FLOW_MAX_LINES + 31u) / 32u)
        return 0u;
    const uint32_t bits = __atomic_exchange_n(&s_dirty_bits[word_index], 0u, __ATOMIC_ACQ_REL);
    if (bits) {
        const uint32_t n = (uint32_t)__builtin_popcount(bits);
        uint32_t cur = __atomic_load_n(&s_dirty_lines, __ATOMIC_RELAXED);
        while (1) {
            const uint32_t next = (cur > n) ? (cur - n) : 0u;
            if (__atomic_compare_exchange_n(&s_dirty_lines, &cur, next, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                break;
        }
    }
    return bits;
}

void tab5_video_flow_requeue_line(uint32_t y)
{
    if (y >= TAB5_VIDEO_FLOW_MAX_LINES)
        return;
    __atomic_add_fetch(&s_requeues, 1u, __ATOMIC_RELAXED);
    flow_mark_dirty(y);
}

static int snapshot_source(const flow_source_meta_t *m, const uint16_t *src,
                           uint32_t y, uint16_t *dst, uint32_t cap,
                           tab5_video_flow_line_t *out, uint8_t source)
{
    const uint32_t g0 = __atomic_load_n(&m->write_guard, __ATOMIC_ACQUIRE);
    if (g0 & 1u)
        return 0;
    const uint64_t seq = __atomic_load_n(&m->render_seq, __ATOMIC_RELAXED);
    const uint32_t epoch = __atomic_load_n(&m->video_epoch, __ATOMIC_RELAXED);
    const uint32_t visual = __atomic_load_n(&m->visual_seq, __ATOMIC_RELAXED);
    const uint32_t width = __atomic_load_n(&m->width, __ATOMIC_RELAXED);
    if (!seq || !width || width > cap || width > TAB5_VIDEO_FLOW_MAX_WIDTH)
        return -1;
    memcpy(dst, src, (size_t)width * sizeof(uint16_t));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint32_t g1 = __atomic_load_n(&m->write_guard, __ATOMIC_ACQUIRE);
    if (g0 != g1 || (g1 & 1u))
        return 0;
    out->render_seq = seq;
    out->video_epoch = epoch;
    out->visual_seq = visual;
    out->y = (uint16_t)y;
    out->width = (uint16_t)width;
    out->source = source;
    return 1;
}

int tab5_video_flow_snapshot_latest(uint32_t y,
                                    uint16_t *dst,
                                    uint32_t dst_capacity_pixels,
                                    tab5_video_flow_line_t *meta)
{
    if (!s_ready || !dst || !meta || y >= TAB5_VIDEO_FLOW_FB_LINES)
        return -1;

    const uint64_t q1 = __atomic_load_n(&s_cpu1_meta[y].render_seq, __ATOMIC_ACQUIRE);
    int rc;
    if (q1) {
        if (!s_cpu1_fb || y >= s_cpu1_height)
            return -1;
        rc = snapshot_source(&s_cpu1_meta[y],
                             s_cpu1_fb + (size_t)y * s_cpu1_pitch,
                             y, dst, dst_capacity_pixels, meta,
                             TAB5_VIDEO_FLOW_SOURCE_CPU1);
        if (rc == 0) __atomic_add_fetch(&s_snapshot_races, 1u, __ATOMIC_RELAXED);
        return rc;
    }
    return -1;
}

uint64_t tab5_video_flow_guest_frontier(void)
{
    return __atomic_load_n(&s_guest_frontier, __ATOMIC_ACQUIRE);
}

void tab5_video_flow_set_screen_commit_frontier(uint64_t seq)
{
    uint64_t cur = __atomic_load_n(&s_screen_commit_frontier, __ATOMIC_RELAXED);
    while (seq > cur && !__atomic_compare_exchange_n(&s_screen_commit_frontier, &cur,
                                                      seq, 0, __ATOMIC_RELEASE,
                                                      __ATOMIC_RELAXED)) {}
    tab5_lp_broker_video_frontier_publish(FLOW_LP_SCREEN, seq);
}

void tab5_video_flow_set_visible_frontier(uint64_t seq)
{
    uint64_t cur = __atomic_load_n(&s_visible_frontier, __ATOMIC_RELAXED);
    while (seq > cur && !__atomic_compare_exchange_n(&s_visible_frontier, &cur,
                                                      seq, 0, __ATOMIC_RELEASE,
                                                      __ATOMIC_RELAXED)) {}
    tab5_lp_broker_video_frontier_publish(FLOW_LP_VISIBLE, seq);
}

void tab5_video_flow_get_stats(tab5_video_flow_stats_t *out)
{
    if (!out) return;
    out->guest_frontier = __atomic_load_n(&s_guest_frontier, __ATOMIC_ACQUIRE);
    out->compose_frontier = __atomic_load_n(&s_compose_frontier, __ATOMIC_ACQUIRE);
    out->screen_commit_frontier = __atomic_load_n(&s_screen_commit_frontier, __ATOMIC_ACQUIRE);
    out->visible_frontier = __atomic_load_n(&s_visible_frontier, __ATOMIC_ACQUIRE);
    out->cpu1_exact_commits = __atomic_load_n(&s_cpu1_exact_commits, __ATOMIC_RELAXED);
    out->cpu0_final_commits = __atomic_load_n(&s_cpu0_final_commits, __ATOMIC_RELAXED);
    out->dirty_posts = __atomic_load_n(&s_dirty_posts, __ATOMIC_RELAXED);
    out->snapshot_races = __atomic_load_n(&s_snapshot_races, __ATOMIC_RELAXED);
    out->stale_skips = __atomic_load_n(&s_stale_skips, __ATOMIC_RELAXED);
    out->requeues = __atomic_load_n(&s_requeues, __ATOMIC_RELAXED);
    out->dirty_lines = __atomic_load_n(&s_dirty_lines, __ATOMIC_RELAXED);
    out->dirty_max = __atomic_load_n(&s_dirty_max, __ATOMIC_RELAXED);
}
