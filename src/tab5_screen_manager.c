#include "tab5_screen_manager.h"
#include "tab5_video_flow.h"

#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tab5_video.h"
#include "tab5_lp_broker.h"
#include "esp_rom_sys.h"

extern void *tab5_ppa_alloc_framebuffer(size_t bytes);
extern void tab5_ppa_free_framebuffer(void *ptr);

/* These generation hooks are transport metadata only.  Screen Manager remains
 * the sole pixel writer; the hooks let legacy diagnostics observe a coherent
 * committed line without granting them ownership. */
extern void tab5_video_fb_line_write_begin(uint32_t y);
extern void tab5_video_fb_line_write_end_changed_tiles32(uint32_t y, int changed,
                                                          uint32_t tile_mask,
                                                          uint32_t width_pixels);

#define SCREEN_MAX_WIDTH          800u
#define SCREEN_MAX_HEIGHT         600u
#define SCREEN_BACKING_PITCH      800u
#define SCREEN_RESULT_SLOTS        64u
#define SCREEN_MAX_INFLIGHT         32u /* queue capacity != allowed outstanding work */
#define SCREEN_TICKET_RECORDS    1024u
#define SCREEN_VERSION_RECORDS     16u
#define SCREEN_RESULT_QUEUE_DEPTH  80u
#define SCREEN_DISPLAY_QUEUE_DEPTH  8u
#define SCREEN_TILE_SHIFT            5u /* 32-pixel horizontal mailbox tile */
#define SCREEN_TILE_WIDTH           (1u << SCREEN_TILE_SHIFT)
#define SCREEN_TILE_COUNT_MAX       ((SCREEN_MAX_WIDTH + SCREEN_TILE_WIDTH - 1u) >> SCREEN_TILE_SHIFT)
#define SCREEN_MAILBOX_RESCUES_PER_REFRESH_MAX 15u /* plus physical cut = up to 16 ownership cuts */
#define SCREEN_MAILBOX_RESCUE_PENDING_SOFT_MAX 8u
#define SCREEN_MAILBOX_RESCUE_DRAIN_SOFT_MAX   4u

static const char *TAG = "TAB5_SCREEN";

typedef enum {
    SCREEN_REC_EMPTY = 0,
    SCREEN_REC_READY,
    SCREEN_REC_VISIBLE,
    SCREEN_REC_RETIRED,
    SCREEN_REC_DROPPED,
} screen_record_state_t;

typedef struct {
    uint64_t id;
    uint64_t present_token;
    uint64_t source_start_seq;
    uint64_t cutoff_seq;
    uint64_t guest_frame_cutoff;
    screen_record_state_t state;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t video_epoch;
    uint32_t video_epoch_min;
    uint32_t video_epoch_max;
    uint16_t epoch_mismatch_lines;
    uint16_t epoch_unknown_lines;
} screen_record_t;

typedef struct {
    uint64_t ticket;
    uint64_t guest_seq;
    uint64_t screen_id;
    uint32_t generation;
    uint32_t video_epoch;
    uint32_t visual_seq;
    uint16_t y;
    uint16_t width;
    uint8_t active;
} render_ticket_record_t;

typedef struct {
    uint64_t ticket;
    uint64_t render_seq;
    uint32_t video_epoch;
    uint32_t visual_seq;
    uint16_t y;
    uint16_t width;
    uint16_t pixels[SCREEN_MAX_WIDTH];
} screen_result_slot_t;

typedef enum {
    RESULT_MSG_PIXELS = 1,
    RESULT_MSG_CANCEL = 2,
    RESULT_MSG_FLOW_PIXELS = 3,
} result_msg_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t slot_index;
    uint16_t reserved;
    uint64_t ticket;
} result_msg_t;

typedef struct {
    uint64_t present_token;
    uint8_t success;
} display_msg_t;

static uint16_t *s_work;
static screen_result_slot_t *s_result_slots;
static QueueHandle_t s_result_free_q;
static QueueHandle_t s_result_q;
static QueueHandle_t s_display_q;
static TaskHandle_t s_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;

/* BAT177NW1: CPU1->CPU0 control is a latest-value atomic mailbox. CPU1 only
 * stores facts + sends a task notification; it never takes s_mux. */
static volatile uint32_t s_nw_reset_serial;
static volatile uint32_t s_nw_frame_serial;
static volatile uint32_t s_nw_frame_width;
static volatile uint32_t s_nw_frame_height;
static volatile uint32_t s_nw_frame_pitch;
static volatile uint32_t s_nw_present_serial;
static volatile uint64_t s_nw_present_cutoff;
static uint32_t s_nw_reset_seen;
static uint32_t s_nw_frame_seen;
static uint32_t s_nw_present_seen;
static uint16_t *s_flow_scratch;
static uint64_t *s_flow_last_seq;
static uint64_t s_flow_epoch_observation_seq;
static uint64_t s_flow_ingest_commits;
static uint64_t s_flow_ingest_stale;
static uint64_t s_flow_ingest_race;

/* Guest time axis.  Only VIDEO_* fact APIs advance these counters. */
static uint64_t s_guest_seq;
static uint64_t s_guest_frame;
static uint64_t s_ticket_seq;

/* Screen construction state.  No CPU/core affinity appears here. */
static uint64_t s_candidate_id = 1u;
static uint64_t s_candidate_start_seq;
static uint64_t s_candidate_cutoff_seq;
static uint64_t s_candidate_cutoff_guest_frame;
static uint64_t s_visible_id;
static uint64_t s_present_seq;
static uint32_t s_generation = 1u;
/* BAT172E0: diagnostic-only semantic epoch carried by render tickets. */
static uint32_t s_candidate_video_epoch_min;
static uint32_t s_candidate_video_epoch_max;
static uint32_t s_candidate_visual_seq_min;
static uint32_t s_candidate_visual_seq_max;
static uint32_t s_last_latched_video_epoch;
static uint16_t s_line_video_epoch_tag[SCREEN_MAX_HEIGHT];
static uint64_t s_epoch_transitions;
static uint64_t s_epoch_mixed_commits;
/* BAT174F0: hard VideoEpoch correctness gate.  CPU1 owns epoch semantics;
 * Screen Manager owns the invariant that an incoherent hard-epoch candidate
 * is never handed to the presenter.  Quarantine advances construction rather
 * than blocking CPU1 or physical refresh completion. */
static uint8_t s_epoch_transition_pending;
static uint32_t s_epoch_transition_target;
static uint64_t s_epoch_transition_seals;
static uint64_t s_epoch_transition_bad_seals;
static uint64_t s_epoch_transition_bad_visible;
static uint64_t s_epoch_quarantine_drops;
static uint64_t s_epoch_quarantine_coherent_seals;
static uint64_t s_epoch_last_transition_seal_id;
static uint32_t s_epoch_last_transition_seal_min;
static uint32_t s_epoch_last_transition_seal_max;
static uint32_t s_epoch_last_transition_seal_epoch;
static uint32_t s_epoch_last_transition_mismatch_lines;
static uint32_t s_epoch_last_transition_unknown_lines;
static uint64_t s_epoch_last_bad_visible_id;
static uint32_t s_epoch_last_bad_visible_epoch;
static uint32_t s_epoch_last_bad_visible_mismatch_lines;
static uint32_t s_epoch_last_bad_visible_unknown_lines;
static uint32_t s_pending_tickets;
static uint64_t s_line_ticket[SCREEN_MAX_HEIGHT];
static uint32_t s_line_commit_seq[SCREEN_MAX_HEIGHT]; /* even=stable, odd=Screen Manager writing */
static render_ticket_record_t *s_tickets;
static bool s_admission_open = true;
static bool s_seal_requested;
static bool s_geometry_seen;
static bool s_geometry_reset_pending;
static bool s_reset_pending;
static uint32_t s_width = SCREEN_MAX_WIDTH;
static uint32_t s_height = SCREEN_MAX_HEIGHT;
static uint32_t s_pitch = SCREEN_BACKING_PITCH;
static uint32_t s_pending_width;
static uint32_t s_pending_height;
static uint32_t s_pending_pitch;
static uint32_t s_requested_actions;
static uint64_t s_candidate_commits;
static uint8_t s_retry_line[SCREEN_MAX_HEIGHT];
static uint64_t s_retry_debt_seq[SCREEN_MAX_HEIGHT];
static uint32_t s_retry_debt_count;
static uint32_t s_retry_cursor;
static uint8_t s_deferred_line[SCREEN_MAX_HEIGHT];
static uint32_t s_deferred_count;

/* R57E8 lock-free double-buffer latest-wins dirty mailbox.
 *
 * Both tile-mask banks and their logical-seq banks live in PSRAM.  CPU1 is the
 * sole writer of the currently ACTIVE bank.  CPU0 changes ownership only at a
 * successful physical refresh by atomically toggling one small INTERNAL index;
 * CPU1 never waits for refresh and CPU0 never locks the Screen state merely to
 * rotate dirty ownership.
 *
 * PSRAM itself is intentionally NOT used as an atomic-RMW target.  The only
 * cross-core atomics are the INTERNAL active index and two writer-inflight
 * counters.  A producer pins the selected bank with inflight++, rechecks the
 * active index, writes mask+seq, then inflight-- with release ordering.  After
 * CPU0 toggles the active index, the old bank is harvested only when its
 * inflight count reaches zero.  If a producer raced the toggle it either
 * retries on the new bank or finishes the old bank before CPU0 reads it; no
 * dirty update can be lost and neither core waits on the other.
 *
 * Current WinDraw dirtiness remains scanline-granular, so this first producer
 * conservatively marks all 32-pixel tiles on a dirty line.  The ABI is already
 * a tile mask, allowing TVRAM/GVRAM write paths to narrow x coverage later.
 *
 * R57E9 separates render-admission dedupe from ScreenVersion generation.
 * Each successful ownership cut (physical refresh or opportunistic CPU0 rescue)
 * advances a small logical offer_epoch.  A line may be admitted once per epoch;
 * repeated writes inside that epoch remain latest-wins in the active PSRAM bank.
 * R57E11 raises the light-load ceiling to fifteen extra cuts while audio
 * reserve and Screen load remain healthy.  The dynamic ceiling collapses
 * 15->7->3->1->0 as audio reserve falls, so CPU0 never owes rescue work and
 * CPU1 remains unaware of refresh timing. */
static uint32_t *s_dirty_tile_mailbox[2];      /* 2 x 2400 bytes, PSRAM */
static uint64_t *s_dirty_tile_latest_seq[2];   /* 2 x 4800 bytes, PSRAM */
static uint32_t *s_line_offer_epoch;           /* 2400 bytes, PSRAM */
static uint32_t s_dirty_mailbox_active_idx;    /* INTERNAL atomic ownership */
static uint32_t s_dirty_mailbox_writer_inflight[2]; /* INTERNAL atomics */
static uint32_t s_dirty_mailbox_bank_has_data[2];   /* INTERNAL atomic edge flags */
/* R57E10: one-shot rescue cue.  A clean->dirty bank transition arms exactly
 * one CPU0 rescue attempt.  If that attempt finds CPU0 busy or out of rescue
 * budget, the cue is deliberately forfeited; the physical refresh path still
 * owns eventual delivery.  This prevents result/display traffic from turning
 * one dirty bank into thousands of repeated rescue-policy checks. */
static uint32_t s_dirty_mailbox_rescue_edge_pending; /* INTERNAL atomic bool */
static uint32_t s_dirty_mailbox_pending_mask;  /* INTERNAL atomic claim bits */
static uint64_t s_dirty_mailbox_pending_token[2];
static uint64_t s_mailbox_posts;
static uint64_t s_mailbox_duplicate_posts;
static uint64_t s_mailbox_offer_suppressed;
static uint64_t s_mailbox_refresh_claims;
static uint64_t s_mailbox_refresh_lines;
static uint64_t s_mailbox_refresh_tiles;
static uint64_t s_mailbox_last_claim_max_seq;
static uint64_t s_mailbox_last_claim_present_token;
static uint32_t s_mailbox_buffer_swaps;
static uint32_t s_mailbox_swap_deferred;
static uint32_t s_mailbox_writer_retries;
static uint32_t s_mailbox_offer_epoch = 1u;
static uint32_t s_mailbox_rescues_since_refresh;
static uint64_t s_mailbox_rescue_attempts;
static uint64_t s_mailbox_rescue_swaps;
static uint64_t s_mailbox_rescue_skip_busy;
static uint64_t s_mailbox_rescue_skip_budget;
static uint32_t s_mailbox_edge_wakes;
/* R57E11: CPU1 publishes audio reserve as facts only; Screen Manager turns it
 * into a dynamic optional-rescue ceiling.  No wall-clock or refresh timing is
 * exposed to CPU1 and no caller waits on CPU0. */
static uint32_t s_audio_reserve_frames;
static uint32_t s_audio_submitted_frames;
static uint32_t s_mailbox_rescue_limit = 3u;

/* R57E4: bounded-inflight rotating admission fairness.  CPU1 now outruns the
 * CPU0 renderer often enough that a fixed ascending VLINE scan can otherwise
 * let hot top-of-screen lines reacquire every newly freed one of the 32
 * tickets while lower dirty lines remain deferred forever.  This is the
 * Screen-only fairness mechanism proven synthetically in R56g, restored on
 * top of the later R56k6 stdio-fenced worker.  It changes neither pixels nor
 * the seal correctness gate. */
static uint32_t s_fair_cursor;
static uint64_t s_fair_frame_seen = UINT64_MAX;
static bool s_fair_capacity_hit;
static uint64_t s_admission_deferred_fair;
static uint64_t s_fair_wraps;

static screen_record_t s_records[SCREEN_VERSION_RECORDS];

/* Diagnostics. */
static uint64_t s_ready_count;
static uint64_t s_retired_count;
static uint64_t s_dropped_count;
static uint64_t s_line_latches;
static uint64_t s_line_results;
static uint64_t s_line_noops;
static uint64_t s_line_drops;
static uint64_t s_retry_requests;
static uint64_t s_retry_delivered;
static uint64_t s_seal_deferred_retry;
static uint64_t s_seal_aborts_retry;
static uint64_t s_stale_results;
static uint64_t s_present_submits;
static uint64_t s_present_completions;
static uint64_t s_present_backpressure;
static uint64_t s_admission_deferred_capacity;
static uint64_t s_admission_deferred_same_line;
static uint64_t s_geometry_duplicate_suppressed;
/* R56k6: task-context diagnostic events are counters only.  The Screen
 * Manager worker never enters ESP_LOG/newlib stdio; CPU1 samples these fields. */
static uint64_t s_tasklog_record_collisions;
static uint64_t s_tasklog_present_unknown;
static uint64_t s_tasklog_present_rejected;
static uint64_t s_tasklog_present_out_of_order;

static inline uint32_t next_generation(uint32_t g)
{
    ++g;
    return g ? g : 1u;
}

static inline uint16_t video_epoch_tag(uint32_t epoch)
{
    uint16_t tag = (uint16_t)(epoch & 0xffffu);
    return tag ? tag : 1u;
}

static inline void reset_candidate_epoch_diag_locked(void)
{
    s_candidate_video_epoch_min = 0u;
    s_candidate_video_epoch_max = 0u;
    s_candidate_visual_seq_min = 0u;
    s_candidate_visual_seq_max = 0u;
}

static inline uint64_t next_nonzero_u64(uint64_t v)
{
    ++v;
    return v ? v : 1u;
}

/* R57E9 render-admission epoch.  It is ordering metadata only; it never gates
 * guest execution or waits for a physical display event.  Wrap is practically
 * unreachable, but clear the PSRAM stamps if it ever occurs.  Caller holds s_mux. */
static void advance_mailbox_offer_epoch_locked(void)
{
    uint32_t next = s_mailbox_offer_epoch + 1u;
    if (!next) {
        if (s_line_offer_epoch)
            memset(s_line_offer_epoch, 0,
                   SCREEN_MAX_HEIGHT * sizeof(*s_line_offer_epoch));
        next = 1u;
    }
    s_mailbox_offer_epoch = next;
}

static inline uint32_t screen_full_tile_mask(uint32_t width)
{
    uint32_t tiles = (width + SCREEN_TILE_WIDTH - 1u) >> SCREEN_TILE_SHIFT;
    if (tiles >= 32u)
        return UINT32_MAX;
    return tiles ? ((1u << tiles) - 1u) : 0u;
}

static inline uint32_t popcount32(uint32_t v)
{
    return (uint32_t)__builtin_popcount(v);
}

static void queue_wake(void);

/* CPU1 latest-wins publisher.  The PSRAM bank is single-writer while active.
 * INTERNAL inflight atomics establish a lock-free ownership handoff with CPU0. */
static bool post_dirty_mailbox_latest(uint32_t y, uint32_t mask, uint64_t seq)
{
    if (y >= SCREEN_MAX_HEIGHT || !mask ||
        !s_dirty_tile_mailbox[0] || !s_dirty_tile_mailbox[1] ||
        !s_dirty_tile_latest_seq[0] || !s_dirty_tile_latest_seq[1])
        return false;

    for (;;) {
        const uint32_t idx = __atomic_load_n(&s_dirty_mailbox_active_idx,
                                              __ATOMIC_ACQUIRE) & 1u;
        __atomic_add_fetch(&s_dirty_mailbox_writer_inflight[idx], 1u,
                           __ATOMIC_ACQ_REL);

        /* CPU0 may have toggled between the index load and pin.  Do not touch
         * that now-inactive PSRAM bank; unpin and retry against the new owner. */
        if ((__atomic_load_n(&s_dirty_mailbox_active_idx, __ATOMIC_ACQUIRE) & 1u) != idx) {
            __atomic_sub_fetch(&s_dirty_mailbox_writer_inflight[idx], 1u,
                               __ATOMIC_RELEASE);
            (void)__atomic_add_fetch(&s_mailbox_writer_retries, 1u, __ATOMIC_RELAXED);
            continue;
        }

        uint32_t *const masks = s_dirty_tile_mailbox[idx];
        uint64_t *const seqs = s_dirty_tile_latest_seq[idx];
        const uint32_t old = masks[y];
        masks[y] = old | mask;
        if (seq > seqs[y])
            seqs[y] = seq;

        /* Publish all PSRAM stores before allowing CPU0 to observe inflight=0.
         * bank_has_data is an INTERNAL edge flag, not a PSRAM atomic.  Only the
         * first writer after a bank clear wakes CPU0, avoiding one notification
         * per guest write while still giving an idle Screen Manager a rescue cue. */
        const bool first_in_bank =
            (__atomic_exchange_n(&s_dirty_mailbox_bank_has_data[idx], 1u,
                                 __ATOMIC_ACQ_REL) == 0u);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_sub_fetch(&s_dirty_mailbox_writer_inflight[idx], 1u,
                           __ATOMIC_RELEASE);
        if (first_in_bank) {
            (void)__atomic_add_fetch(&s_mailbox_edge_wakes, 1u, __ATOMIC_RELAXED);
            /* R57E10: edge-latch before wake.  Screen Manager consumes this
             * exactly once; busy/budget rejection does not self-rearm. */
            __atomic_store_n(&s_dirty_mailbox_rescue_edge_pending, 1u,
                             __ATOMIC_RELEASE);
            queue_wake();
        }
        return ((old & mask) != mask);
    }
}

/* CPU0-only harvesting of an INACTIVE bank.  It never waits for CPU1: a bank
 * with an in-flight writer remains pending and is retried on the next Screen
 * Manager pass.  Once inflight==0, acquire ordering makes the final producer
 * PSRAM stores visible before CPU0 reads and clears the bank. */
static bool harvest_dirty_mailbox_bank(uint32_t idx)
{
    idx &= 1u;
    if (!(__atomic_load_n(&s_dirty_mailbox_pending_mask, __ATOMIC_ACQUIRE) & (1u << idx)))
        return true;
    if ((__atomic_load_n(&s_dirty_mailbox_active_idx, __ATOMIC_ACQUIRE) & 1u) == idx)
        return false;
    if (__atomic_load_n(&s_dirty_mailbox_writer_inflight[idx], __ATOMIC_ACQUIRE) != 0u)
        return false;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t max_seq = 0u;
    uint64_t lines = 0u;
    uint64_t tiles = 0u;
    uint32_t *const masks = s_dirty_tile_mailbox[idx];
    uint64_t *const seqs = s_dirty_tile_latest_seq[idx];
    if (!masks || !seqs)
        return false;

    for (uint32_t y = 0u; y < SCREEN_MAX_HEIGHT; ++y) {
        const uint32_t mask = masks[y];
        if (!mask)
            continue;
        ++lines;
        tiles += popcount32(mask);
        if (seqs[y] > max_seq)
            max_seq = seqs[y];
    }

    const uint64_t present_token = s_dirty_mailbox_pending_token[idx];
    memset(masks, 0, SCREEN_MAX_HEIGHT * sizeof(*masks));
    memset(seqs, 0, SCREEN_MAX_HEIGHT * sizeof(*seqs));
    __atomic_store_n(&s_dirty_mailbox_bank_has_data[idx], 0u, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    s_dirty_mailbox_pending_token[idx] = 0u;
    (void)__atomic_fetch_and(&s_dirty_mailbox_pending_mask, ~(1u << idx), __ATOMIC_ACQ_REL);

    portENTER_CRITICAL(&s_mux);
    ++s_mailbox_refresh_claims;
    s_mailbox_refresh_lines += lines;
    s_mailbox_refresh_tiles += tiles;
    s_mailbox_last_claim_max_seq = max_seq;
    s_mailbox_last_claim_present_token = present_token;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static void harvest_pending_dirty_mailboxes(void)
{
    (void)harvest_dirty_mailbox_bank(0u);
    (void)harvest_dirty_mailbox_bank(1u);
}

/* CPU0 lock-free ownership cut.  Both physical refresh and an opportunistic
 * rescue use the same one-word active_idx release-store.  CPU0 never waits for
 * a producer; if the destination bank is still pending, the cut is deferred. */
static bool rotate_dirty_mailbox(uint64_t present_token, bool rescue)
{
    harvest_pending_dirty_mailboxes();

    const uint32_t old_idx = __atomic_load_n(&s_dirty_mailbox_active_idx,
                                              __ATOMIC_ACQUIRE) & 1u;
    const uint32_t new_idx = old_idx ^ 1u;

    if (rescue &&
        __atomic_load_n(&s_dirty_mailbox_bank_has_data[old_idx], __ATOMIC_ACQUIRE) == 0u)
        return false;

    if (__atomic_load_n(&s_dirty_mailbox_pending_mask, __ATOMIC_ACQUIRE) & (1u << new_idx)) {
        (void)__atomic_add_fetch(&s_mailbox_swap_deferred, 1u, __ATOMIC_RELAXED);
        return false;
    }

    __atomic_store_n(&s_dirty_mailbox_active_idx, new_idx, __ATOMIC_RELEASE);
    (void)__atomic_add_fetch(&s_mailbox_buffer_swaps, 1u, __ATOMIC_RELAXED);
    s_dirty_mailbox_pending_token[old_idx] = present_token;
    (void)__atomic_fetch_or(&s_dirty_mailbox_pending_mask, (1u << old_idx), __ATOMIC_RELEASE);

    /* A successful ownership cut is also a render-admission sub-epoch.  This
     * is a tiny metadata update under the Screen mux; no PSRAM walk or refresh
     * wait occurs while the lock is held. */
    portENTER_CRITICAL(&s_mux);
    advance_mailbox_offer_epoch_locked();
    if (rescue) {
        ++s_mailbox_rescue_swaps;
        ++s_mailbox_rescues_since_refresh;
    }
    portEXIT_CRITICAL(&s_mux);

    (void)harvest_dirty_mailbox_bank(old_idx);
    return true;
}

/* Physical refresh is the base ownership cut.  R57E11 permits up to fifteen
 * additional ownership cuts only when the current audio-reserve hint and CPU0
 * Screen load allow them; with the refresh cut this is at most sixteen cuts. */
static void rotate_dirty_mailbox_after_refresh(uint64_t present_token)
{
    (void)rotate_dirty_mailbox(present_token, false);
    portENTER_CRITICAL(&s_mux);
    s_mailbox_rescues_since_refresh = 0u;
    portEXIT_CRITICAL(&s_mux);
}

/* CPU0 opportunistic rescue.  There is deliberately no timer and no CPU1
 * participation: a first write into a newly-active bank wakes Screen Manager.
 * R57E10 evaluates that edge once.  If CPU0 is busy, latest-wins state simply
 * waits in PSRAM for the guaranteed physical-refresh ownership cut. */
static void maybe_rescue_dirty_mailbox(uint32_t results_drained)
{
    /* R57E10 AUDIO-SAFE policy: no dirty edge, no policy work.  This exchange
     * is the entire hot no-op path.  A rejected edge is intentionally not
     * rearmed; physical refresh remains the guaranteed eventual ownership cut. */
    if (__atomic_exchange_n(&s_dirty_mailbox_rescue_edge_pending, 0u,
                            __ATOMIC_ACQ_REL) == 0u)
        return;

    const uint32_t active_idx = __atomic_load_n(&s_dirty_mailbox_active_idx,
                                                 __ATOMIC_ACQUIRE) & 1u;
    if (__atomic_load_n(&s_dirty_mailbox_bank_has_data[active_idx],
                        __ATOMIC_ACQUIRE) == 0u)
        return;

    bool busy = false;
    bool budget_full = false;
    const uint32_t rescue_limit =
        __atomic_load_n(&s_mailbox_rescue_limit, __ATOMIC_ACQUIRE);
    portENTER_CRITICAL(&s_mux);
    ++s_mailbox_rescue_attempts;
    budget_full = (s_mailbox_rescues_since_refresh >= rescue_limit);
    busy = s_reset_pending || s_geometry_reset_pending || !s_admission_open ||
           s_seal_requested || s_retry_debt_count != 0u ||
           s_pending_tickets > SCREEN_MAILBOX_RESCUE_PENDING_SOFT_MAX ||
           results_drained > SCREEN_MAILBOX_RESCUE_DRAIN_SOFT_MAX;
    if (budget_full)
        ++s_mailbox_rescue_skip_budget;
    else if (busy)
        ++s_mailbox_rescue_skip_busy;
    portEXIT_CRITICAL(&s_mux);

    /* Do NOT immediately retry a rejected edge.  New writes remain latest-wins
     * in the active PSRAM bank and the next physical cut guarantees progress. */
    if (budget_full || busy)
        return;

    (void)rotate_dirty_mailbox(0u, true);
}

static void reset_dirty_mailbox_locked(void)
{
    /* Reset render-admission dedupe immediately.  The double PSRAM mailbox is
     * not destructively cleared here because CPU0 may be harvesting an old bank
     * concurrently.  Existing bits are harmless latest-state diagnostics and
     * will be reclaimed by the normal refresh ownership cycle. */
    if (s_line_offer_epoch)
        memset(s_line_offer_epoch, 0,
               SCREEN_MAX_HEIGHT * sizeof(*s_line_offer_epoch));
    s_mailbox_offer_epoch = 1u;
    s_mailbox_rescues_since_refresh = 0u;
    __atomic_store_n(&s_dirty_mailbox_rescue_edge_pending, 0u, __ATOMIC_RELEASE);
}

static screen_record_t *record_for_screen(uint64_t id)
{
    if (!id)
        return NULL;
    screen_record_t *r = &s_records[id % SCREEN_VERSION_RECORDS];
    return (r->id == id) ? r : NULL;
}

static screen_record_t *record_for_present(uint64_t token)
{
    if (!token)
        return NULL;
    for (uint32_t i = 0u; i < SCREEN_VERSION_RECORDS; ++i) {
        if (s_records[i].id && s_records[i].present_token == token)
            return &s_records[i];
    }
    return NULL;
}

static screen_record_t *record_create(uint64_t id, uint64_t present_token,
                                      uint64_t source_start_seq,
                                      uint64_t cutoff_seq,
                                      uint64_t guest_frame_cutoff,
                                      uint32_t w, uint32_t h, uint32_t pitch,
                                      uint32_t video_epoch,
                                      uint32_t video_epoch_min,
                                      uint32_t video_epoch_max,
                                      uint32_t epoch_mismatch_lines,
                                      uint32_t epoch_unknown_lines)
{
    screen_record_t *r = &s_records[id % SCREEN_VERSION_RECORDS];
    if (r->id && r->state != SCREEN_REC_RETIRED && r->state != SCREEN_REC_DROPPED)
        ++s_tasklog_record_collisions;
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->present_token = present_token;
    r->source_start_seq = source_start_seq;
    r->cutoff_seq = cutoff_seq;
    r->guest_frame_cutoff = guest_frame_cutoff;
    r->state = SCREEN_REC_READY;
    r->width = w;
    r->height = h;
    r->pitch = pitch;
    r->video_epoch = video_epoch;
    r->video_epoch_min = video_epoch_min;
    r->video_epoch_max = video_epoch_max;
    r->epoch_mismatch_lines = (uint16_t)((epoch_mismatch_lines > 0xffffu) ? 0xffffu : epoch_mismatch_lines);
    r->epoch_unknown_lines = (uint16_t)((epoch_unknown_lines > 0xffffu) ? 0xffffu : epoch_unknown_lines);
    return r;
}

static render_ticket_record_t *ticket_lookup_locked(uint64_t ticket)
{
    if (!ticket || !s_tickets)
        return NULL;
    render_ticket_record_t *r = &s_tickets[ticket % SCREEN_TICKET_RECORDS];
    return (r->active && r->ticket == ticket) ? r : NULL;
}

static void ticket_release_locked(render_ticket_record_t *r)
{
    if (!r || !r->active)
        return;
    const uint32_t y = r->y;
    if (y < SCREEN_MAX_HEIGHT && s_line_ticket[y] == r->ticket)
        s_line_ticket[y] = 0u;
    r->active = 0u;
    if (s_pending_tickets)
        --s_pending_tickets;
}

static void queue_wake(void)
{
    TaskHandle_t task = s_task;
    if (task)
        xTaskNotifyGive(task);
}

void tab5_screen_no_wait_kick(void)
{
    /* One-way cross-core signal only: no mutex, queue space, ticket or reply. */
    queue_wake();
}

static void clear_retry_debt_locked(void)
{
    memset(s_retry_line, 0, sizeof(s_retry_line));
    memset(s_retry_debt_seq, 0, sizeof(s_retry_debt_seq));
    s_retry_debt_count = 0u;
    s_retry_cursor = 0u;
}

static void clear_deferred_locked(void)
{
    memset(s_deferred_line, 0, sizeof(s_deferred_line));
    s_deferred_count = 0u;
}

static inline void reset_fairness_locked(void)
{
    s_fair_cursor = 0u;
    s_fair_frame_seen = UINT64_MAX;
    s_fair_capacity_hit = false;
}

static inline void mark_deferred_locked(uint32_t y)
{
    if (y >= SCREEN_MAX_HEIGHT)
        return;
    if (!s_deferred_line[y]) {
        s_deferred_line[y] = 1u;
        ++s_deferred_count;
    }
}

static inline void clear_deferred_line_locked(uint32_t y)
{
    if (y >= SCREEN_MAX_HEIGHT)
        return;
    if (s_deferred_line[y]) {
        s_deferred_line[y] = 0u;
        if (s_deferred_count)
            --s_deferred_count;
    }
}

/* A render failure means the candidate is not sealable.  If host policy had
 * already requested a seal, abort that seal and re-open admission so the
 * emulator can retry on a later guest frame.  This is a Screen Manager
 * lifecycle decision; the renderer never decides to reuse old pixels. */
static void mark_retry_debt_locked(uint32_t y, uint64_t required_seq)
{
    if (y >= SCREEN_MAX_HEIGHT || s_geometry_reset_pending || s_reset_pending)
        return;

    if (!required_seq)
        required_seq = s_guest_seq ? s_guest_seq : 1u;
    if (!s_retry_debt_seq[y]) {
        s_retry_debt_seq[y] = required_seq;
        ++s_retry_debt_count;
    } else if (required_seq > s_retry_debt_seq[y]) {
        s_retry_debt_seq[y] = required_seq;
    }
    if (!s_retry_line[y]) {
        s_retry_line[y] = 1u;
        ++s_retry_requests;
    }
    /* A genuine renderer failure is the only reason a line may be offered
     * again inside the same Screen generation. */
    s_line_offer_epoch[y] = 0u;

    if (s_seal_requested) {
        s_seal_requested = false;
        s_admission_open = true;
        s_candidate_cutoff_seq = 0u;
        s_candidate_cutoff_guest_frame = 0u;
        ++s_seal_aborts_retry;
    }
}

static void cancel_ticket_now(uint64_t ticket, int stale)
{
    portENTER_CRITICAL(&s_mux);
    render_ticket_record_t *r = ticket_lookup_locked(ticket);
    if (r) {
        const uint32_t y = r->y;
        /* Renderer failure never mutates guest dirty state from the worker
         * CPU.  It creates unresolved Screen construction debt.  A candidate
         * with debt can never become READY; the guest/emulator side later
         * receives only a line-retry message. */
        mark_retry_debt_locked(y, r->guest_seq);
        ticket_release_locked(r);
        ++s_line_drops;
        if (stale)
            ++s_stale_results;
    } else if (stale) {
        ++s_stale_results;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void handle_cancel(uint64_t ticket)
{
    cancel_ticket_now(ticket, 0);
}

/* Coherent read of one manager-owned working line without handing out a
 * writable Screen pointer.  Screen Manager is the sole writer and publishes
 * an odd/even sequence around each commit; a guest-side message producer may
 * briefly retry only this line if it raced the commit itself. */
static void snapshot_work_line(uint32_t y, uint32_t width, uint16_t *dst)
{
    const uint16_t *src = s_work + (size_t)y * SCREEN_BACKING_PITCH;
    for (;;) {
        const uint32_t seq0 = __atomic_load_n(&s_line_commit_seq[y], __ATOMIC_ACQUIRE);
        if (seq0 & 1u) {
            taskYIELD();
            continue;
        }
        memcpy(dst, src, (size_t)width * sizeof(uint16_t));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t seq1 = __atomic_load_n(&s_line_commit_seq[y], __ATOMIC_ACQUIRE);
        if (seq0 == seq1 && !(seq1 & 1u))
            return;
        taskYIELD();
    }
}

static void commit_result(uint8_t idx, uint64_t ticket)
{
    if (idx >= SCREEN_RESULT_SLOTS || !s_result_slots || !s_work)
        return;

    screen_result_slot_t *slot = &s_result_slots[idx];
    uint32_t y = 0u;
    uint32_t expected_width = 0u;
    bool valid = false;
    bool discard_for_geometry = false;

    portENTER_CRITICAL(&s_mux);
    render_ticket_record_t *tr = ticket_lookup_locked(ticket);
    if (tr && tr->screen_id == s_candidate_id && tr->generation == s_generation &&
        (!s_candidate_cutoff_seq || tr->guest_seq <= s_candidate_cutoff_seq)) {
        y = tr->y;
        expected_width = tr->width;
        valid = (y < SCREEN_MAX_HEIGHT && expected_width == slot->width &&
                 expected_width > 0u && expected_width <= SCREEN_MAX_WIDTH);
        discard_for_geometry = s_geometry_reset_pending;
    }
    portEXIT_CRITICAL(&s_mux);

    if (!valid) {
        cancel_ticket_now(ticket, 1);
        return;
    }

    if (discard_for_geometry) {
        portENTER_CRITICAL(&s_mux);
        tr = ticket_lookup_locked(ticket);
        if (tr) {
            ticket_release_locked(tr);
            ++s_line_drops;
        }
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    uint16_t *dst = s_work + (size_t)y * SCREEN_BACKING_PITCH;
    uint32_t tile_mask = 0u;
    const uint32_t tiles = (expected_width + 31u) >> 5;

    for (uint32_t t = 0u; t < tiles && t < 32u; ++t) {
        const uint32_t x0 = t << 5;
        const uint32_t count = (expected_width - x0 > 32u) ? 32u : (expected_width - x0);
        const size_t bytes = (size_t)count * sizeof(uint16_t);
        if (memcmp(dst + x0, slot->pixels + x0, bytes) != 0)
            tile_mask |= 1u << t;
    }

    if (tile_mask) {
        __atomic_add_fetch(&s_line_commit_seq[y], 1u, __ATOMIC_ACQ_REL); /* odd */
        tab5_video_fb_line_write_begin(y);
        for (uint32_t t = 0u; t < tiles && t < 32u; ++t) {
            if (!(tile_mask & (1u << t)))
                continue;
            const uint32_t x0 = t << 5;
            const uint32_t count = (expected_width - x0 > 32u) ? 32u : (expected_width - x0);
            memcpy(dst + x0, slot->pixels + x0, (size_t)count * sizeof(uint16_t));
        }
        tab5_video_fb_line_write_end_changed_tiles32(y, 1, tile_mask, expected_width);
        __atomic_add_fetch(&s_line_commit_seq[y], 1u, __ATOMIC_RELEASE); /* even */
    }

    portENTER_CRITICAL(&s_mux);
    tr = ticket_lookup_locked(ticket);
    if (tr && tr->screen_id == s_candidate_id && tr->generation == s_generation) {
        if (!s_candidate_start_seq || tr->guest_seq < s_candidate_start_seq)
            s_candidate_start_seq = tr->guest_seq;
        const uint64_t committed_seq = tr->guest_seq;
        if (tr->video_epoch) {
            if (!s_candidate_video_epoch_min || tr->video_epoch < s_candidate_video_epoch_min)
                s_candidate_video_epoch_min = tr->video_epoch;
            if (tr->video_epoch > s_candidate_video_epoch_max)
                s_candidate_video_epoch_max = tr->video_epoch;
            if (s_candidate_video_epoch_min != s_candidate_video_epoch_max)
                ++s_epoch_mixed_commits;
            s_line_video_epoch_tag[y] = video_epoch_tag(tr->video_epoch);
        }
        if (tr->visual_seq) {
            if (!s_candidate_visual_seq_min || tr->visual_seq < s_candidate_visual_seq_min)
                s_candidate_visual_seq_min = tr->visual_seq;
            if (tr->visual_seq > s_candidate_visual_seq_max)
                s_candidate_visual_seq_max = tr->visual_seq;
        }
        ticket_release_locked(tr);
        if (y < SCREEN_MAX_HEIGHT && s_retry_debt_seq[y] &&
            committed_seq >= s_retry_debt_seq[y]) {
            s_retry_debt_seq[y] = 0u;
            if (s_retry_debt_count)
                --s_retry_debt_count;
        }
        ++s_line_results;
        if (tile_mask)
            ++s_candidate_commits;
        else
            ++s_line_noops;
    } else {
        ++s_stale_results;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void handle_display_complete(uint64_t present_token, int success)
{
    uint64_t screen_id = 0u;
    uint64_t retired_id = 0u;
    bool known = false;
    bool out_of_order = false;

    portENTER_CRITICAL(&s_mux);
    screen_record_t *r = record_for_present(present_token);
    if (r && r->state == SCREEN_REC_READY) {
        known = true;
        screen_id = r->id;
        ++s_present_completions;
        if (!success) {
            r->state = SCREEN_REC_DROPPED;
            ++s_dropped_count;
        } else if (s_visible_id && r->id <= s_visible_id) {
            /* A late completion must never move display time backwards. */
            r->state = SCREEN_REC_RETIRED;
            ++s_retired_count;
            out_of_order = true;
        } else {
            if (s_visible_id) {
                screen_record_t *old = record_for_screen(s_visible_id);
                if (old && old->state == SCREEN_REC_VISIBLE)
                    old->state = SCREEN_REC_RETIRED;
                retired_id = s_visible_id;
                ++s_retired_count;
            }
            r->state = SCREEN_REC_VISIBLE;
            s_visible_id = r->id;
            tab5_video_flow_set_visible_frontier(r->cutoff_seq);
            if ((r->video_epoch_min && r->video_epoch_max &&
                 r->video_epoch_min != r->video_epoch_max) ||
                r->epoch_mismatch_lines || r->epoch_unknown_lines) {
                ++s_epoch_transition_bad_visible;
                s_epoch_last_bad_visible_id = r->id;
                s_epoch_last_bad_visible_epoch = r->video_epoch;
                s_epoch_last_bad_visible_mismatch_lines = r->epoch_mismatch_lines;
                s_epoch_last_bad_visible_unknown_lines = r->epoch_unknown_lines;
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (!known) {
        ++s_tasklog_present_unknown;
        return;
    }

    if (!success) {
        ++s_tasklog_present_rejected;
        return;
    }
    if (out_of_order) {
        ++s_tasklog_present_out_of_order;
        return;
    }

    /* R57E6: CPU0 harvests the latest-wins dirty mailbox only at successful
     * physical refresh completion.  This clears notification bits, never guest
     * VRAM/TVRAM data and never blocks CPU1. */
    rotate_dirty_mailbox_after_refresh(present_token);
    (void)screen_id;
    (void)retired_id;
}

static void process_reset(void)
{
    uint64_t old_id = 0u;
    uint64_t old_commits = 0u;
    uint64_t seq = 0u;

    portENTER_CRITICAL(&s_mux);
    if (!s_reset_pending) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    old_id = s_candidate_id;
    old_commits = s_candidate_commits;
    seq = s_guest_seq;
    s_reset_pending = false;
    s_geometry_reset_pending = false;
    s_geometry_seen = false;
    ++s_candidate_id;
    s_candidate_start_seq = 0u;
    s_candidate_cutoff_seq = 0u;
    s_candidate_cutoff_guest_frame = 0u;
    s_candidate_commits = 0u;
    reset_candidate_epoch_diag_locked();
    ++s_dropped_count;
    portEXIT_CRITICAL(&s_mux);

    /* Old renderer jobs carry an invalidated generation/ticket and can finish
     * in the background, but can no longer commit.  Screen Manager resets only
     * its private construction surface; the current physical VISIBLE screen is
     * untouched until a post-reset ScreenVersion is actually displayed. */
    memset(s_work, 0, SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT * sizeof(uint16_t));
    memset(s_line_video_epoch_tag, 0, sizeof(s_line_video_epoch_tag));

    portENTER_CRITICAL(&s_mux);
    s_requested_actions |= TAB5_SCREEN_EMU_ACTION_MARK_ALL_DIRTY;
    s_admission_open = true;
    const uint64_t new_id = s_candidate_id;
    const uint32_t generation = s_generation;
    portEXIT_CRITICAL(&s_mux);
    (void)new_id;
    (void)generation;
    (void)old_id;
    (void)old_commits;
    (void)seq;

}

static void process_geometry_reset(void)
{
    uint64_t old_id = 0u;
    uint64_t old_commits = 0u;
    uint32_t new_w = 0u, new_h = 0u, new_pitch = 0u;

    portENTER_CRITICAL(&s_mux);
    if (!s_geometry_reset_pending) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    old_id = s_candidate_id;
    old_commits = s_candidate_commits;
    new_w = s_pending_width;
    new_h = s_pending_height;
    new_pitch = s_pending_pitch;

    s_admission_open = false;
    s_seal_requested = false;
    s_geometry_reset_pending = false;
    ++s_candidate_id;
    s_candidate_start_seq = 0u;
    s_candidate_cutoff_seq = 0u;
    s_candidate_cutoff_guest_frame = 0u;
    s_candidate_commits = 0u;
    reset_candidate_epoch_diag_locked();
    s_width = new_w;
    s_height = new_h;
    s_pitch = new_pitch;
    reset_fairness_locked();
    ++s_dropped_count;
    portEXIT_CRITICAL(&s_mux);

    /* No ScreenVersion owns this memory yet.  Screen Manager is the sole writer
     * and resets the new construction before re-opening admission. */
    memset(s_work, 0, SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT * sizeof(uint16_t));
    memset(s_line_video_epoch_tag, 0, sizeof(s_line_video_epoch_tag));

    portENTER_CRITICAL(&s_mux);
    s_requested_actions |= TAB5_SCREEN_EMU_ACTION_MARK_ALL_DIRTY;
    s_admission_open = true;
    const uint64_t new_id = s_candidate_id;
    const uint32_t generation = s_generation;
    portEXIT_CRITICAL(&s_mux);
    (void)new_id;
    (void)generation;
    (void)old_id;
    (void)old_commits;

}

static void advance_candidate_without_present(const char *reason)
{
    uint64_t old_id;
    uint64_t cutoff;

    portENTER_CRITICAL(&s_mux);
    old_id = s_candidate_id;
    cutoff = s_candidate_cutoff_seq;
    ++s_dropped_count;
    s_seal_requested = false;
    s_generation = next_generation(s_generation);
    advance_mailbox_offer_epoch_locked();
    ++s_candidate_id;
    s_candidate_start_seq = 0u;
    s_candidate_cutoff_seq = 0u;
    s_candidate_cutoff_guest_frame = 0u;
    s_candidate_commits = 0u;
    reset_candidate_epoch_diag_locked();
    memset(s_line_ticket, 0, sizeof(s_line_ticket));
    clear_deferred_locked();
    s_admission_open = true;
    const uint64_t new_id = s_candidate_id;
    portEXIT_CRITICAL(&s_mux);
    (void)old_id;
    (void)cutoff;
    (void)new_id;
    (void)reason;

}

static void try_seal_and_submit(void)
{
    uint64_t id, start_seq, cutoff_seq, cutoff_frame, present_token;
    uint64_t commits;
    uint32_t w, h;
    uint32_t seal_video_epoch;
    uint32_t seal_video_epoch_min;
    uint32_t seal_video_epoch_max;
    uint32_t seal_epoch_mismatch = 0u;
    uint32_t seal_epoch_unknown = 0u;
    uint32_t seal_transition_target = 0u;
    bool seal_transition_observed = false;
    bool seal_epoch_bad = false;

    portENTER_CRITICAL(&s_mux);
    if (!s_seal_requested || s_geometry_reset_pending || s_reset_pending ||
        s_pending_tickets != 0u || s_retry_debt_count != 0u) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    id = s_candidate_id;
    start_seq = s_candidate_start_seq;
    cutoff_seq = s_candidate_cutoff_seq;
    cutoff_frame = s_candidate_cutoff_guest_frame;
    commits = s_candidate_commits;
    w = s_width;
    h = s_height;
    seal_video_epoch = s_candidate_video_epoch_max;
    seal_video_epoch_min = s_candidate_video_epoch_min;
    seal_video_epoch_max = s_candidate_video_epoch_max;

    /* BAT174F0: inspect hard-epoch ownership BEFORE the ordinary no-change
     * shortcut.  A hard transition may need multiple construction candidates
     * to repaint every visible line.  The line tags and s_work are deliberately
     * retained across quarantined candidates so progress accumulates; only the
     * candidate/ticket lifecycle advances. */
    if (s_epoch_transition_pending && s_epoch_transition_target) {
        const uint16_t target_tag = video_epoch_tag(s_epoch_transition_target);
        const uint32_t hdiag = (h < SCREEN_MAX_HEIGHT) ? h : SCREEN_MAX_HEIGHT;
        seal_transition_observed = true;
        seal_transition_target = s_epoch_transition_target;
        for (uint32_t y = 0u; y < hdiag; ++y) {
            const uint16_t tag = s_line_video_epoch_tag[y];
            if (!tag) ++seal_epoch_unknown;
            else if (tag != target_tag) ++seal_epoch_mismatch;
        }
        ++s_epoch_transition_seals;
        s_epoch_last_transition_seal_id = id;
        s_epoch_last_transition_seal_min = seal_video_epoch_min;
        s_epoch_last_transition_seal_max = seal_video_epoch_max;
        s_epoch_last_transition_seal_epoch = seal_transition_target;
        s_epoch_last_transition_mismatch_lines = seal_epoch_mismatch;
        s_epoch_last_transition_unknown_lines = seal_epoch_unknown;
        seal_epoch_bad =
            (seal_video_epoch_min && seal_video_epoch_max &&
             seal_video_epoch_min != seal_video_epoch_max) ||
            seal_epoch_mismatch || seal_epoch_unknown;
        if (seal_epoch_bad) {
            ++s_epoch_transition_bad_seals;
            ++s_epoch_quarantine_drops;
        } else {
            ++s_epoch_quarantine_coherent_seals;
        }
    }

    if (seal_epoch_bad) {
        portEXIT_CRITICAL(&s_mux);
        /* Do not submit stale/mixed pixels.  This is NOT a wait/fence on CPU1:
         * advance_candidate_without_present() rotates the logical offer epoch
         * immediately, keeps already-correct target-epoch lines in s_work, and
         * reopens admission for remaining dirty lines. */
        advance_candidate_without_present("hard-video-epoch-incoherent");
        return;
    }

    /* Do not spend LCD bandwidth on a pixel-identical ScreenVersion once a
     * physical screen exists.  If this is the first coherent candidate after
     * a hard transition, the physical pixels are already identical; retire the
     * transition semantically without forcing a redundant LCD transaction. */
    const bool no_change = (commits == 0u && s_visible_id != 0u);
    if (no_change) {
        if (seal_transition_observed && s_epoch_transition_pending &&
            s_epoch_transition_target == seal_transition_target)
            s_epoch_transition_pending = 0u;
        portEXIT_CRITICAL(&s_mux);
        advance_candidate_without_present("pixel-identical");
        return;
    }

    s_present_seq = next_nonzero_u64(s_present_seq);
    present_token = s_present_seq;
    portEXIT_CRITICAL(&s_mux);

    /* admission is closed and pending==0, so s_work is immutable for the
     * duration of this presenter snapshot copy. */
    if (!tab5_video_present_px68k_managed(s_work, w, h,
                                          SCREEN_BACKING_PITCH,
                                          present_token)) {
        portENTER_CRITICAL(&s_mux);
        ++s_present_backpressure;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    portENTER_CRITICAL(&s_mux);
    (void)record_create(id, present_token,
                        start_seq ? start_seq : cutoff_seq,
                        cutoff_seq, cutoff_frame,
                        w, h, SCREEN_BACKING_PITCH,
                        seal_video_epoch, seal_video_epoch_min, seal_video_epoch_max,
                        seal_epoch_mismatch, seal_epoch_unknown);
    if (seal_transition_observed && s_epoch_transition_pending &&
        s_epoch_transition_target == seal_transition_target) {
        s_epoch_transition_pending = 0u;
    }
    ++s_ready_count;
    ++s_present_submits;

    /* The immutable pixels now live in presenter-owned storage.  The same
     * manager-owned work surface becomes the base for the next construction. */
    s_seal_requested = false;
    s_generation = next_generation(s_generation);
    advance_mailbox_offer_epoch_locked();
    ++s_candidate_id;
    s_candidate_start_seq = 0u;
    s_candidate_cutoff_seq = 0u;
    s_candidate_cutoff_guest_frame = 0u;
    s_candidate_commits = 0u;
    reset_candidate_epoch_diag_locked();
    memset(s_line_ticket, 0, sizeof(s_line_ticket));
    clear_deferred_locked();
    s_admission_open = true;
    const uint64_t next_id = s_candidate_id;
    const uint32_t next_gen = s_generation;
    portEXIT_CRITICAL(&s_mux);
    (void)next_id;
    (void)next_gen;

}


/* BAT177NW1 ---------------------------------------------------------------
 * CPU1 NO-WAIT ingress. CPU1 publishes stable logical lines into
 * tab5_video_flow. Only this CPU0 task converts them into ScreenVersion state.
 * No render ticket, admission query, retry mailbox or cross-core s_mux access
 * exists on the guest scanline path. */
static int commit_flow_line(const tab5_video_flow_line_t *m, const uint16_t *pixels, int requeue_on_defer)
{
    if (!m || !pixels || m->y >= SCREEN_MAX_HEIGHT || !m->width ||
        m->width > SCREEN_MAX_WIDTH)
        return -1;

    const uint32_t y = m->y;
    const uint32_t width = m->width;

    /* A sealed candidate has a logical cutoff. Newer lines belong to the next
     * ScreenVersion and stay latest-wins in the flow mailbox. */
    portENTER_CRITICAL(&s_mux);
    const bool geom_busy = s_geometry_reset_pending || s_reset_pending;
    const bool after_cutoff = s_seal_requested && s_candidate_cutoff_seq &&
                              m->render_seq > s_candidate_cutoff_seq;
    const uint64_t already = s_flow_last_seq ? s_flow_last_seq[y] : 0u;
    portEXIT_CRITICAL(&s_mux);
    if (geom_busy || after_cutoff) {
        if (requeue_on_defer)
            tab5_video_flow_requeue_line(y);
        return 0;
    }
    if (m->render_seq <= already) {
        ++s_flow_ingest_stale;
        return 1;
    }

    uint16_t *dst = s_work + (size_t)y * SCREEN_BACKING_PITCH;
    uint32_t tile_mask = 0u;
    const uint32_t tiles = (width + 31u) >> 5;
    for (uint32_t t = 0u; t < tiles && t < 32u; ++t) {
        const uint32_t x0 = t << 5;
        const uint32_t count = (width - x0 > 32u) ? 32u : (width - x0);
        if (memcmp(dst + x0, pixels + x0, (size_t)count * sizeof(uint16_t)) != 0)
            tile_mask |= 1u << t;
    }

    if (tile_mask) {
        __atomic_add_fetch(&s_line_commit_seq[y], 1u, __ATOMIC_ACQ_REL);
        tab5_video_fb_line_write_begin(y);
        for (uint32_t t = 0u; t < tiles && t < 32u; ++t) {
            if (!(tile_mask & (1u << t)))
                continue;
            const uint32_t x0 = t << 5;
            const uint32_t count = (width - x0 > 32u) ? 32u : (width - x0);
            memcpy(dst + x0, pixels + x0, (size_t)count * sizeof(uint16_t));
        }
        tab5_video_fb_line_write_end_changed_tiles32(y, 1, tile_mask, width);
        __atomic_add_fetch(&s_line_commit_seq[y], 1u, __ATOMIC_RELEASE);
    }

    portENTER_CRITICAL(&s_mux);
    if (s_flow_last_seq)
        s_flow_last_seq[y] = m->render_seq;
    if (m->render_seq > s_guest_seq)
        s_guest_seq = m->render_seq;
    if (!s_candidate_start_seq || m->render_seq < s_candidate_start_seq)
        s_candidate_start_seq = m->render_seq;

    if (m->video_epoch) {
        /* Only newer logical lines may move the observed hard-epoch frontier;
         * a late old CPU0 compose completion can never move it backwards. */
        if (m->render_seq > s_flow_epoch_observation_seq) {
            if (s_last_latched_video_epoch &&
                s_last_latched_video_epoch != m->video_epoch) {
                ++s_epoch_transitions;
                s_epoch_transition_pending = 1u;
                s_epoch_transition_target = m->video_epoch;
            }
            s_last_latched_video_epoch = m->video_epoch;
            s_flow_epoch_observation_seq = m->render_seq;
        }
        if (!s_candidate_video_epoch_min || m->video_epoch < s_candidate_video_epoch_min)
            s_candidate_video_epoch_min = m->video_epoch;
        if (m->video_epoch > s_candidate_video_epoch_max)
            s_candidate_video_epoch_max = m->video_epoch;
        if (s_candidate_video_epoch_min != s_candidate_video_epoch_max)
            ++s_epoch_mixed_commits;
        s_line_video_epoch_tag[y] = video_epoch_tag(m->video_epoch);
    }
    if (m->visual_seq) {
        if (!s_candidate_visual_seq_min || m->visual_seq < s_candidate_visual_seq_min)
            s_candidate_visual_seq_min = m->visual_seq;
        if (m->visual_seq > s_candidate_visual_seq_max)
            s_candidate_visual_seq_max = m->visual_seq;
    }
    ++s_line_results;
    if (tile_mask)
        ++s_candidate_commits;
    else
        ++s_line_noops;
    ++s_flow_ingest_commits;
    portEXIT_CRITICAL(&s_mux);

    tab5_video_flow_set_screen_commit_frontier(m->render_seq);
    return 1;
}

/* BAT177NW1: CPU0 final-compositor results never need a second full framebuffer.
 * They are rendered directly into one of Screen Manager's existing bounded
 * result slots and consumed on this CPU0 task. If an old seal cutoff is still
 * armed, abort that host-side seal and include the completed line in the next
 * construction; CPU1 is never consulted or delayed. */
static void commit_cpu0_flow_slot(uint8_t idx)
{
    if (idx >= SCREEN_RESULT_SLOTS || !s_result_slots)
        return;
    screen_result_slot_t *slot = &s_result_slots[idx];
    tab5_video_flow_line_t m = {0};
    m.render_seq = slot->render_seq;
    m.video_epoch = slot->video_epoch;
    m.visual_seq = slot->visual_seq;
    m.y = slot->y;
    m.width = slot->width;
    m.source = TAB5_VIDEO_FLOW_SOURCE_CPU0;

    if (!m.render_seq || m.y >= SCREEN_MAX_HEIGHT || !m.width ||
        m.width > SCREEN_MAX_WIDTH) {
        ++s_flow_ingest_stale;
        return;
    }

    /* If CPU1 has already published an equal/newer exact line, this async CPU0
     * completion is obsolete even if CPU0 Screen ingress has not copied it yet. */
    if (m.render_seq <= tab5_video_flow_cpu1_line_seq(m.y)) {
        ++s_flow_ingest_stale;
        return;
    }

    portENTER_CRITICAL(&s_mux);
    const bool geom_busy = s_geometry_reset_pending || s_reset_pending;
    const bool after_cutoff = s_seal_requested && s_candidate_cutoff_seq &&
                              m.render_seq > s_candidate_cutoff_seq;
    if (after_cutoff && !geom_busy) {
        s_seal_requested = false;
        s_admission_open = true;
        s_candidate_cutoff_seq = 0u;
        s_candidate_cutoff_guest_frame = 0u;
    }
    portEXIT_CRITICAL(&s_mux);

    if (geom_busy) {
        ++s_flow_ingest_stale;
        return;
    }
    (void)commit_flow_line(&m, slot->pixels, 0);
}

static uint32_t ingest_video_flow(uint32_t budget)
{
    if (!s_flow_scratch || !s_flow_last_seq || !budget)
        return 0u;
    uint32_t done = 0u;
    const uint32_t words = (SCREEN_MAX_HEIGHT + 31u) >> 5;
    for (uint32_t wi = 0u; wi < words; ++wi) {
        uint32_t bits = tab5_video_flow_take_dirty_word(wi);
        while (bits) {
            const uint32_t bit = (uint32_t)__builtin_ctz(bits);
            bits &= bits - 1u;
            const uint32_t y = (wi << 5) + bit;
            if (y >= SCREEN_MAX_HEIGHT)
                continue;
            if (done >= budget) {
                tab5_video_flow_requeue_line(y);
                while (bits) {
                    const uint32_t b2 = (uint32_t)__builtin_ctz(bits);
                    bits &= bits - 1u;
                    const uint32_t y2 = (wi << 5) + b2;
                    if (y2 < SCREEN_MAX_HEIGHT)
                        tab5_video_flow_requeue_line(y2);
                }
                return done;
            }
            tab5_video_flow_line_t meta = {0};
            const int rc = tab5_video_flow_snapshot_latest(
                y, s_flow_scratch, SCREEN_MAX_WIDTH, &meta);
            if (rc == 0) {
                ++s_flow_ingest_race;
                tab5_video_flow_requeue_line(y);
            } else if (rc > 0) {
                (void)commit_flow_line(&meta, s_flow_scratch, 1);
            }
            ++done;
        }
    }
    return done;
}

static void consume_nowait_reset_request(void)
{
    const uint32_t serial = __atomic_load_n(&s_nw_reset_serial, __ATOMIC_ACQUIRE);
    if (serial == s_nw_reset_seen)
        return;
    s_nw_reset_seen = serial;

    portENTER_CRITICAL(&s_mux);
    s_guest_seq = tab5_video_flow_guest_frontier();
    s_reset_pending = true;
    s_geometry_reset_pending = false;
    s_admission_open = false;
    s_seal_requested = false;
    s_generation = next_generation(s_generation);
    s_candidate_cutoff_seq = 0u;
    s_candidate_cutoff_guest_frame = 0u;
    memset(s_line_ticket, 0, sizeof(s_line_ticket));
    memset(s_tickets, 0, SCREEN_TICKET_RECORDS * sizeof(*s_tickets));
    s_pending_tickets = 0u;
    clear_retry_debt_locked();
    clear_deferred_locked();
    reset_fairness_locked();
    reset_dirty_mailbox_locked();
    portEXIT_CRITICAL(&s_mux);
}

static void consume_nowait_frame_request(void)
{
    const uint32_t serial = __atomic_load_n(&s_nw_frame_serial, __ATOMIC_ACQUIRE);
    if (serial == s_nw_frame_seen)
        return;
    const uint32_t delta = serial - s_nw_frame_seen;
    s_nw_frame_seen = serial;
    const uint32_t width = __atomic_load_n(&s_nw_frame_width, __ATOMIC_RELAXED);
    const uint32_t height = __atomic_load_n(&s_nw_frame_height, __ATOMIC_RELAXED);
    const uint32_t pitch = __atomic_load_n(&s_nw_frame_pitch, __ATOMIC_RELAXED);
    if (!width || width > SCREEN_MAX_WIDTH || !height || height > SCREEN_MAX_HEIGHT)
        return;

    bool changed = false;
    uint32_t old_w = 0u, old_h = 0u;
    uint64_t frame = 0u;
    portENTER_CRITICAL(&s_mux);
    s_guest_frame += delta ? delta : 1u;
    if (!s_guest_frame) s_guest_frame = 1u;
    frame = s_guest_frame;
    const uint64_t gf = tab5_video_flow_guest_frontier();
    if (gf > s_guest_seq) s_guest_seq = gf;

    if (!s_geometry_seen) {
        s_geometry_seen = true;
        s_width = width;
        s_height = height;
        s_pitch = pitch;
    } else if (width != s_width || height != s_height || pitch != s_pitch) {
        if (!(s_geometry_reset_pending && width == s_pending_width &&
              height == s_pending_height && pitch == s_pending_pitch)) {
            old_w = s_width;
            old_h = s_height;
            s_pending_width = width;
            s_pending_height = height;
            s_pending_pitch = pitch;
            s_geometry_reset_pending = true;
            s_admission_open = false;
            s_seal_requested = false;
            s_generation = next_generation(s_generation);
            advance_mailbox_offer_epoch_locked();
            s_candidate_cutoff_seq = 0u;
            s_candidate_cutoff_guest_frame = 0u;
            memset(s_line_ticket, 0, sizeof(s_line_ticket));
            memset(s_tickets, 0, SCREEN_TICKET_RECORDS * sizeof(*s_tickets));
            s_pending_tickets = 0u;
            clear_retry_debt_locked();
            clear_deferred_locked();
            changed = true;
        } else {
            ++s_geometry_duplicate_suppressed;
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (changed) {
        ESP_LOGI(TAG,
                 "CPU0 accepted NO-WAIT VIDEO_GEOMETRY frame=%llu %lux%lu -> %lux%lu",
                 (unsigned long long)frame,
                 (unsigned long)old_w, (unsigned long)old_h,
                 (unsigned long)width, (unsigned long)height);
    }
}

static void consume_nowait_present_request(void)
{
    const uint32_t serial = __atomic_load_n(&s_nw_present_serial, __ATOMIC_ACQUIRE);
    if (serial == s_nw_present_seen)
        return;
    s_nw_present_seen = serial;
    uint64_t cutoff = __atomic_load_n(&s_nw_present_cutoff, __ATOMIC_ACQUIRE);
    bool armed = false;

    portENTER_CRITICAL(&s_mux);
    if (!cutoff) cutoff = s_guest_seq;
    if (s_admission_open && !s_geometry_reset_pending && !s_reset_pending &&
        !s_seal_requested) {
        s_seal_requested = true;
        s_admission_open = false;
        s_candidate_cutoff_seq = cutoff;
        s_candidate_cutoff_guest_frame = s_guest_frame;
        armed = true;
    }
    portEXIT_CRITICAL(&s_mux);
    (void)armed;
}

static void screen_task(void *arg)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    {
        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);
        esp_rom_printf("R56K5_TASKSELF name=px68k_screen core=%d base=0x%08x top=0x%08x bytes=4096 hwm=%u\n",
                       (int)xPortGetCoreID(), (unsigned)r56k5_base,
                       (unsigned)(r56k5_base + 4096u),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#endif
    (void)arg;

    /* R56k host scheduler contract:
     * Screen Manager joins YM/audio/compositor/LCD in one CPU0 priority-3
     * work class.  It never sleeps for a fixed wall-clock interval.  One full
     * ownership/state-machine pass is the atomic scheduling unit; only after
     * that pass may an equal-priority host worker run.  If no producer has
     * posted more work, the notification wait blocks indefinitely. */
    for (;;) {
        /* LPFAB_R3: CPU0 consumes the LP-classified DIRTY union as transport
         * metadata only. BAT145 deliberately does not gate rendering, mutate
         * ScreenVersion state, or replace the proven R57 exact shadow path. */
        tab5_lp_dirty_workset_t lp_dirty_workset;
        (void)tab5_lp_broker_dirty_take(&lp_dirty_workset);

        /* BAT177NW1 P0: consume CPU1 facts from one-way atomic mailboxes.
         * These helpers run on CPU0, so all Screen ownership mutation stays
         * on this core. CPU1 never takes s_mux. */
        consume_nowait_reset_request();
        consume_nowait_frame_request();
        process_reset();
        process_geometry_reset();

        /* Latest-wins logical/final lines.  A bounded CPU0 copy budget keeps
         * audio/YM peers schedulable; CPU1 never sees or waits for this lag. */
        const uint32_t flow_drained = ingest_video_flow(64u);

        /* R57E8 legacy dirty metadata can remain for diagnostics/other host
         * producers, but CPU1 exact video no longer depends on it. */
        harvest_pending_dirty_mailboxes();

        /* P0b: physical display completion.  It changes ownership/lifecycle but
         * never guest ordering. */
        display_msg_t dm;
        while (xQueueReceive(s_display_q, &dm, 0) == pdTRUE)
            handle_display_complete(dm.present_token, dm.success ? 1 : 0);

        /* P1: render results/cancels.  Results may arrive in any host order;
         * opaque tickets map them back to guest-sequenced line latches. */
        result_msg_t rm;
        uint32_t drained = 0u;
        while (drained < SCREEN_RESULT_QUEUE_DEPTH &&
               xQueueReceive(s_result_q, &rm, 0) == pdTRUE) {
            if (rm.kind == RESULT_MSG_PIXELS) {
                commit_result(rm.slot_index, rm.ticket);
                uint8_t idx = rm.slot_index;
                (void)xQueueSend(s_result_free_q, &idx, portMAX_DELAY);
            } else if (rm.kind == RESULT_MSG_FLOW_PIXELS) {
                commit_cpu0_flow_slot(rm.slot_index);
                uint8_t idx = rm.slot_index;
                (void)xQueueSend(s_result_free_q, &idx, portMAX_DELAY);
            } else if (rm.kind == RESULT_MSG_CANCEL) {
                handle_cancel(rm.ticket);
            }
            ++drained;
        }

        /* R57E9 legacy rescue remains CPU0-only. */
        maybe_rescue_dirty_mailbox(drained + flow_drained);

        /* CPU1 present is only a one-way opportunity serial. Arm/consume it on
         * CPU0 after current line ingress, then seal without any guest reply. */
        consume_nowait_present_request();
        try_seal_and_submit();

        /* Complete management-pass boundary.  taskYIELD() rotates only among
         * READY tasks of this same priority; it injects no fixed 10-ms gap. */
        taskYIELD();
        /* BAT177NW1: if bounded latest-wins ingress left work behind, CPU0
         * immediately schedules another pass. No CPU1 retry/kick is needed. */
        tab5_video_flow_stats_t flow_after = {0};
        tab5_video_flow_get_stats(&flow_after);
        if (flow_after.dirty_lines != 0u)
            continue;
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

int tab5_screen_manager_init(void)
{
    if (s_ready)
        return 1;

    s_work = (uint16_t *)tab5_ppa_alloc_framebuffer(
        SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT * sizeof(uint16_t));
    if (!s_work)
        return 0;
    memset(s_work, 0, SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT * sizeof(uint16_t));

    if (!tab5_video_flow_init()) {
        tab5_ppa_free_framebuffer(s_work);
        s_work = NULL;
        return 0;
    }
    s_flow_scratch = (uint16_t *)heap_caps_aligned_calloc(
        64, SCREEN_MAX_WIDTH, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_flow_last_seq = (uint64_t *)heap_caps_calloc(
        SCREEN_MAX_HEIGHT, sizeof(uint64_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_flow_scratch || !s_flow_last_seq) {
        if (s_flow_scratch) heap_caps_free(s_flow_scratch);
        if (s_flow_last_seq) heap_caps_free(s_flow_last_seq);
        s_flow_scratch = NULL;
        s_flow_last_seq = NULL;
        tab5_ppa_free_framebuffer(s_work);
        s_work = NULL;
        return 0;
    }

    s_result_slots = (screen_result_slot_t *)heap_caps_aligned_calloc(
        64, SCREEN_RESULT_SLOTS, sizeof(screen_result_slot_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_result_slots) {
        tab5_ppa_free_framebuffer(s_work);
        s_work = NULL;
        return 0;
    }

    /* Ticket metadata is cold relative to rendering pixels and costs roughly
     * 32 KiB at 1024 records. Keep it in PSRAM so the state-machine redesign
     * does not consume scarce internal SRAM merely for bookkeeping. */
    s_tickets = (render_ticket_record_t *)heap_caps_calloc(
        SCREEN_TICKET_RECORDS, sizeof(*s_tickets),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tickets) {
        heap_caps_free(s_result_slots);
        s_result_slots = NULL;
        tab5_ppa_free_framebuffer(s_work);
        s_work = NULL;
        return 0;
    }

    /* R57E8: both latest-wins banks live wholly in PSRAM.  Only three tiny
     * ownership words (active index + two inflight counters) remain internal. */
    for (uint32_t bank = 0u; bank < 2u; ++bank) {
        s_dirty_tile_mailbox[bank] = (uint32_t *)heap_caps_calloc(
            SCREEN_MAX_HEIGHT, sizeof(*s_dirty_tile_mailbox[bank]),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_dirty_tile_latest_seq[bank] = (uint64_t *)heap_caps_calloc(
            SCREEN_MAX_HEIGHT, sizeof(*s_dirty_tile_latest_seq[bank]),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_line_offer_epoch = (uint32_t *)heap_caps_calloc(
        SCREEN_MAX_HEIGHT, sizeof(*s_line_offer_epoch),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dirty_tile_mailbox[0] || !s_dirty_tile_mailbox[1] ||
        !s_dirty_tile_latest_seq[0] || !s_dirty_tile_latest_seq[1] ||
        !s_line_offer_epoch) {
        for (uint32_t bank = 0u; bank < 2u; ++bank) {
            if (s_dirty_tile_mailbox[bank]) {
                heap_caps_free(s_dirty_tile_mailbox[bank]);
                s_dirty_tile_mailbox[bank] = NULL;
            }
            if (s_dirty_tile_latest_seq[bank]) {
                heap_caps_free(s_dirty_tile_latest_seq[bank]);
                s_dirty_tile_latest_seq[bank] = NULL;
            }
        }
        if (s_line_offer_epoch) {
            heap_caps_free(s_line_offer_epoch);
            s_line_offer_epoch = NULL;
        }
        heap_caps_free(s_tickets);
        s_tickets = NULL;
        heap_caps_free(s_result_slots);
        s_result_slots = NULL;
        tab5_ppa_free_framebuffer(s_work);
        s_work = NULL;
        return 0;
    }
    __atomic_store_n(&s_dirty_mailbox_active_idx, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_dirty_mailbox_writer_inflight[0], 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_dirty_mailbox_writer_inflight[1], 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_dirty_mailbox_bank_has_data[0], 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_dirty_mailbox_bank_has_data[1], 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_dirty_mailbox_rescue_edge_pending, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_dirty_mailbox_pending_mask, 0u, __ATOMIC_RELEASE);
    s_mailbox_offer_epoch = 1u;
    s_mailbox_rescues_since_refresh = 0u;
    __atomic_store_n(&s_audio_reserve_frames, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_audio_submitted_frames, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mailbox_rescue_limit, 3u, __ATOMIC_RELEASE);
    s_dirty_mailbox_pending_token[0] = 0u;
    s_dirty_mailbox_pending_token[1] = 0u;

    s_result_free_q = xQueueCreate(SCREEN_RESULT_SLOTS, sizeof(uint8_t));
    s_result_q = xQueueCreate(SCREEN_RESULT_QUEUE_DEPTH, sizeof(result_msg_t));
    s_display_q = xQueueCreate(SCREEN_DISPLAY_QUEUE_DEPTH, sizeof(display_msg_t));
    if (!s_result_free_q || !s_result_q || !s_display_q)
        return 0;

    for (uint32_t i = 0u; i < SCREEN_RESULT_SLOTS; ++i) {
        uint8_t idx = (uint8_t)i;
        (void)xQueueSend(s_result_free_q, &idx, 0);
    }

#if portNUM_PROCESSORS > 1
    BaseType_t ok = xTaskCreatePinnedToCore(screen_task, "px68k_screen", 4096,
                                            NULL, 3, &s_task, 0);
#else
    BaseType_t ok = xTaskCreate(screen_task, "px68k_screen", 4096,
                                NULL, 3, &s_task);
#endif
    if (ok != pdPASS)
        return 0;

    s_ready = true;
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R56E: Screen Manager ACTIVE CORRECTNESS-FROZEN + BOUNDED-INFLIGHT32; sole ScreenVersion writer; guest_seq/render_ticket/present_token timelines separated; work=%p 800x600 pitch=800 resultSlots=%u ticketsPSRAM=%p",
             (void *)s_work, (unsigned)SCREEN_RESULT_SLOTS, (void *)s_tickets);
    ESP_LOGI(TAG,
             "PX68K_HOST_R56K: Screen Manager prio=3 event-driven management-pass boundary-yield; fixed-delay WDT service removed");
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R56K6: worker stdio=FORBIDDEN; task-context diagnostics are counters sampled by CPU1");
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R57E31: CPU0 final-flow uses existing 64 bounded result slots; duplicate 800x600 CPU0 framebuffer=0 bytes; CPU1 cross-core flow remains latest-wins");
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R57E6: LATEST-WINS TILE MAILBOX base retained tile=32px; seal requires pending=retry=0; R57E9 supersedes offer cadence with rescue epochs");
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R57E8: LOCK-FREE DOUBLE PSRAM TILE MAILBOX ACTIVE; CPU1 never refresh-waits; active_idx switch=ONE atomic release-store; PSRAM is never atomic-RMW");
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R57E9: ADAPTIVE MAILBOX RESCUE base retained; one offer/line/rescue-epoch");
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R57E10: AUDIO-SAFE EDGE-LATCH RESCUE ACTIVE; exactly one rescue policy attempt per clean->dirty bank edge; rejected edge waits for physical cut; banks mask=%uB+seq=%uB each offer=%p",
             (unsigned)(SCREEN_MAX_HEIGHT * sizeof(*s_dirty_tile_mailbox[0])),
             (unsigned)(SCREEN_MAX_HEIGHT * sizeof(*s_dirty_tile_latest_seq[0])),
             (void *)s_line_offer_epoch);
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R57E11: AUDIO-RESERVE ADAPTIVE16 ACTIVE; optional rescue ceiling=0/1/3/7/15 plus physical cut, CPU0 busy gate retained, CPU1 refresh-blind");
    ESP_LOGI(TAG,
             "PX68K_SCREEN_R57E27: HARD VIDEO-EPOCH QUARANTINE ACTIVE; incoherent transition candidates never enter presenter; construction advances without CPU1 wait");
    ESP_LOGI(TAG, "SCREEN 1 CONSTRUCTING generation=1 geom=unresolved");
    return 1;
}

int tab5_screen_video_line_latch_state(uint32_t y, uint32_t width,
                                       uint32_t video_epoch,
                                       uint32_t visual_seq,
                                       uint16_t *private_seed_line,
                                       uint64_t *render_ticket)
{
    if (!s_ready || !render_ticket ||
        y >= SCREEN_MAX_HEIGHT || width == 0u || width > SCREEN_MAX_WIDTH)
        return 0;

    uint64_t ticket = 0u;
    bool accepted = false;
    const uint32_t dirty_mask = screen_full_tile_mask(width);

    portENTER_CRITICAL(&s_mux);

    /* R57E9 common logical order.  Every VIDEO_LINE_LATCH notification gets a
     * monotonically increasing guestSeq and is published to the active PSRAM
     * bank.  Ordering remains logical, never wall-clock.  Render admission is
     * one offer/line/offer_epoch; CPU0 advances offer_epoch on a physical cut
     * or on an optional light-load rescue cut. */
    const uint32_t observed_generation = s_generation;
    const uint32_t observed_offer_epoch = s_mailbox_offer_epoch;
    if (video_epoch) {
        if (s_last_latched_video_epoch && s_last_latched_video_epoch != video_epoch) {
            ++s_epoch_transitions;
            s_epoch_transition_pending = 1u;
            s_epoch_transition_target = video_epoch;
        }
        s_last_latched_video_epoch = video_epoch;
    }
    const bool offer_new = (s_line_offer_epoch[y] != observed_offer_epoch);
    s_guest_seq = next_nonzero_u64(s_guest_seq);
    ++s_line_latches;
    const uint64_t dirty_seq = s_guest_seq;
    portEXIT_CRITICAL(&s_mux);

    const bool mailbox_new = post_dirty_mailbox_latest(y, dirty_mask, dirty_seq);

    portENTER_CRITICAL(&s_mux);
    ++s_mailbox_posts;
    if (!mailbox_new)
        ++s_mailbox_duplicate_posts;

    /* Geometry/reset may have changed generation while CPU1 was publishing the
     * lock-free PSRAM notification.  The dirty stays safely recorded in the
     * latest bank, but an offer observed in the old generation is not admitted. */
    if (s_generation != observed_generation || s_reset_pending ||
        s_geometry_reset_pending) {
        ++s_mailbox_offer_suppressed;
        portEXIT_CRITICAL(&s_mux);
        return 0;
    }

    if (!offer_new) {
        /* Same rescue epoch: keep only the latest mailbox state/order.  CPU0
         * may open another offer epoch later if it has spare work capacity. */
        ++s_mailbox_offer_suppressed;
        portEXIT_CRITICAL(&s_mux);
        return 0;
    }
    s_line_offer_epoch[y] = observed_offer_epoch;

    /* R57E4 fair-admission cursor remains useful across Screen generations,
     * but R57E8 retains the R57E6 meaning of a rejection: capacity/fairness rejection
     * is carry-over work for the NEXT ScreenVersion, not correctness debt that
     * can prevent the current Screen from sealing forever. */
    if (s_fair_frame_seen != s_guest_frame) {
        if (!s_fair_capacity_hit && s_fair_cursor != 0u) {
            s_fair_cursor = 0u;
            ++s_fair_wraps;
        }
        if (s_fair_cursor >= s_height) {
            s_fair_cursor = 0u;
            ++s_fair_wraps;
        }
        s_fair_frame_seen = s_guest_frame;
        s_fair_capacity_hit = false;
    }

    if (s_admission_open && !s_seal_requested && !s_geometry_reset_pending &&
        !s_reset_pending) {
        if (s_fair_cursor != 0u && y < s_fair_cursor) {
            mark_deferred_locked(y); /* diagnostic carry-over only */
            ++s_admission_deferred_fair;
        } else if (s_line_ticket[y] != 0u) {
            mark_deferred_locked(y); /* normally prevented by offer_epoch */
            ++s_admission_deferred_same_line;
        } else if (s_pending_tickets >= SCREEN_MAX_INFLIGHT) {
            mark_deferred_locked(y); /* next ScreenVersion will retry latest state */
            ++s_admission_deferred_capacity;
            if (!s_fair_capacity_hit) {
                s_fair_cursor = y;
                s_fair_capacity_hit = true;
            }
        } else {
            s_ticket_seq = next_nonzero_u64(s_ticket_seq);
            ticket = s_ticket_seq;
            render_ticket_record_t *tr = &s_tickets[ticket % SCREEN_TICKET_RECORDS];
            if (!tr->active) {
                tr->ticket = ticket;
                tr->guest_seq = s_guest_seq;
                tr->screen_id = s_candidate_id;
                tr->generation = s_generation;
                tr->video_epoch = video_epoch;
                tr->visual_seq = visual_seq;
                tr->y = (uint16_t)y;
                tr->width = (uint16_t)width;
                tr->active = 1u;
                s_line_ticket[y] = ticket;
                ++s_pending_tickets;
                clear_deferred_line_locked(y);
                if (!s_candidate_start_seq)
                    s_candidate_start_seq = s_guest_seq;
                accepted = true;
            } else {
                mark_retry_debt_locked(y, s_guest_seq);
                ++s_line_drops;
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (!accepted)
        return 0;

    /* BAT175A0: NULL means post-render publication from the CPU1-owned
     * logical framebuffer.  Admission may fail, but CPU1 rendering has already
     * completed and is never rolled back or delayed by that host decision. */
    if (private_seed_line)
        snapshot_work_line(y, width, private_seed_line);
    *render_ticket = ticket;
    return 1;
}

int tab5_screen_video_line_latch(uint32_t y, uint32_t width,
                                 uint16_t *private_seed_line,
                                 uint64_t *render_ticket)
{
    return tab5_screen_video_line_latch_state(y, width, 0u, 0u,
                                              private_seed_line, render_ticket);
}

int tab5_screen_render_result(uint64_t render_ticket, uint32_t width,
                              const uint16_t *private_result_line)
{
    if (!s_ready || !render_ticket || !private_result_line ||
        width == 0u || width > SCREEN_MAX_WIDTH)
        return 0;

    uint32_t expected_width = 0u;
    portENTER_CRITICAL(&s_mux);
    render_ticket_record_t *tr = ticket_lookup_locked(render_ticket);
    if (tr)
        expected_width = tr->width;
    portEXIT_CRITICAL(&s_mux);
    if (!expected_width || expected_width != width) {
        cancel_ticket_now(render_ticket, 1);
        return 0;
    }

    uint8_t idx = 0xffu;
    if (xQueueReceive(s_result_free_q, &idx, 0) != pdTRUE) {
        cancel_ticket_now(render_ticket, 0);
        return 0;
    }

    screen_result_slot_t *slot = &s_result_slots[idx];
    slot->ticket = render_ticket;
    slot->width = (uint16_t)width;
    memcpy(slot->pixels, private_result_line, (size_t)width * sizeof(uint16_t));

    result_msg_t msg = {0};
    msg.kind = RESULT_MSG_PIXELS;
    msg.slot_index = idx;
    msg.ticket = render_ticket;
    if (xQueueSend(s_result_q, &msg, 0) != pdTRUE) {
        (void)xQueueSend(s_result_free_q, &idx, portMAX_DELAY);
        cancel_ticket_now(render_ticket, 0);
        return 0;
    }
    queue_wake();
    return 1;
}

uint16_t *tab5_screen_cpu0_flow_acquire(uint32_t *slot_token)
{
    if (!s_ready || !slot_token || !s_result_slots || !s_result_free_q)
        return NULL;
    uint8_t idx = 0xffu;
    /* R57E54: Screen pressure is visual-only. The compositor must never
     * sleep here because an accepted R57 line may otherwise retain downstream
     * pressure for an unbounded interval.  Caller re-dirties the latest line. */
    if (xQueueReceive(s_result_free_q, &idx, 0) != pdTRUE)
        return NULL;
    if (idx >= SCREEN_RESULT_SLOTS)
        return NULL;
    screen_result_slot_t *slot = &s_result_slots[idx];
    slot->ticket = 0u;
    slot->render_seq = 0u;
    slot->video_epoch = 0u;
    slot->visual_seq = 0u;
    slot->y = 0u;
    slot->width = 0u;
    *slot_token = idx;
    return slot->pixels;
}

int tab5_screen_cpu0_flow_submit(uint32_t slot_token,
                                 uint32_t y, uint32_t width,
                                 uint64_t render_seq,
                                 uint32_t video_epoch,
                                 uint32_t visual_seq)
{
    if (!s_ready || slot_token >= SCREEN_RESULT_SLOTS || !render_seq ||
        y >= SCREEN_MAX_HEIGHT || !width || width > SCREEN_MAX_WIDTH)
        return 0;
    const uint8_t idx = (uint8_t)slot_token;
    screen_result_slot_t *slot = &s_result_slots[idx];
    slot->ticket = 0u;
    slot->render_seq = render_seq;
    slot->video_epoch = video_epoch;
    slot->visual_seq = visual_seq;
    slot->y = (uint16_t)y;
    slot->width = (uint16_t)width;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    result_msg_t msg = {0};
    msg.kind = RESULT_MSG_FLOW_PIXELS;
    msg.slot_index = idx;
    /* R57E54: never wait behind Screen Manager. A failed submit is simply a
     * latest-wins visual drop; caller returns the slot and requests CPU1 retry. */
    if (xQueueSend(s_result_q, &msg, 0) != pdTRUE)
        return 0;
    queue_wake();
    return 1;
}

void tab5_screen_cpu0_flow_release(uint32_t slot_token)
{
    if (!s_ready || slot_token >= SCREEN_RESULT_SLOTS || !s_result_free_q)
        return;
    uint8_t idx = (uint8_t)slot_token;
    (void)xQueueSend(s_result_free_q, &idx, 0);
}

void tab5_screen_render_cancel(uint64_t render_ticket)
{
    if (!s_ready || !render_ticket)
        return;

    result_msg_t msg = {0};
    msg.kind = RESULT_MSG_CANCEL;
    msg.ticket = render_ticket;
    if (xQueueSend(s_result_q, &msg, 0) != pdTRUE)
        cancel_ticket_now(render_ticket, 0);
    else
        queue_wake();
}

void tab5_screen_video_reset(void)
{
    if (!s_ready)
        return;
    __atomic_add_fetch(&s_nw_reset_serial, 1u, __ATOMIC_RELEASE);
    queue_wake();
}

void tab5_screen_video_frame_boundary(uint32_t width, uint32_t height,
                                      uint32_t pitch)
{
    if (!s_ready || width == 0u || width > SCREEN_MAX_WIDTH ||
        height == 0u || height > SCREEN_MAX_HEIGHT)
        return;
    __atomic_store_n(&s_nw_frame_width, width, __ATOMIC_RELAXED);
    __atomic_store_n(&s_nw_frame_height, height, __ATOMIC_RELAXED);
    __atomic_store_n(&s_nw_frame_pitch, pitch, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_nw_frame_serial, 1u, __ATOMIC_RELEASE);
    queue_wake();
}

void tab5_screen_manager_audio_reserve_hint(uint32_t queued_frames,
                                           uint32_t speaker_queued_frames,
                                           uint32_t submitted_frames)
{
    uint32_t reserve = queued_frames;
    if (UINT32_MAX - reserve < speaker_queued_frames)
        reserve = UINT32_MAX;
    else
        reserve += speaker_queued_frames;

    uint32_t limit;
    if (submitted_frames < 4096u)
        limit = 3u;                 /* startup: responsive, but conservative */
    else if (reserve < 2048u)
        limit = 0u;                 /* audio deadline shield */
    else if (reserve < 4096u)
        limit = 1u;
    else if (reserve < 8192u)
        limit = 3u;
    else if (reserve < 16384u)
        limit = 7u;
    else
        limit = SCREEN_MAILBOX_RESCUES_PER_REFRESH_MAX;

    __atomic_store_n(&s_audio_reserve_frames, reserve, __ATOMIC_RELEASE);
    __atomic_store_n(&s_audio_submitted_frames, submitted_frames, __ATOMIC_RELEASE);
    __atomic_store_n(&s_mailbox_rescue_limit, limit, __ATOMIC_RELEASE);
}

void tab5_screen_present_opportunity(void)
{
    if (!s_ready)
        return;
    __atomic_store_n(&s_nw_present_cutoff,
                     tab5_video_flow_guest_frontier(), __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_nw_present_serial, 1u, __ATOMIC_RELEASE);
    queue_wake();
}

void tab5_screen_manager_present_complete(uint64_t present_token, int success)
{
    if (!s_ready || !present_token)
        return;

    display_msg_t msg = {0};
    msg.present_token = present_token;
    msg.success = success ? 1u : 0u;
    if (xQueueSend(s_display_q, &msg, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGE(TAG, "PRESENT p=%llu completion queue overflow",
                 (unsigned long long)present_token);
        return;
    }
    queue_wake();
}

uint32_t tab5_screen_take_emulator_actions(void)
{
    uint32_t actions;
    portENTER_CRITICAL(&s_mux);
    actions = s_requested_actions;
    s_requested_actions = 0u;
    portEXIT_CRITICAL(&s_mux);
    return actions;
}

int tab5_screen_take_emulator_retry_line(uint32_t *y)
{
    if (!s_ready || !y)
        return 0;

    int found = 0;
    portENTER_CRITICAL(&s_mux);
    for (uint32_t n = 0u; n < SCREEN_MAX_HEIGHT; ++n) {
        const uint32_t i = (s_retry_cursor + n) % SCREEN_MAX_HEIGHT;
        if (s_retry_line[i]) {
            s_retry_line[i] = 0u;
            s_retry_cursor = (i + 1u) % SCREEN_MAX_HEIGHT;
            *y = i;
            ++s_retry_delivered;
            found = 1;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

const uint16_t *tab5_screen_readonly_work_buffer(void)
{
    return s_work;
}

void tab5_screen_manager_get_stats(tab5_screen_manager_stats_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->guest_seq = s_guest_seq;
    out->guest_frame = s_guest_frame;
    out->candidate_id = s_candidate_id;
    out->visible_id = s_visible_id;
    out->ready_count = s_ready_count;
    out->retired_count = s_retired_count;
    out->dropped_count = s_dropped_count;
    out->line_latches = s_line_latches;
    out->line_results = s_line_results;
    out->line_noops = s_line_noops;
    out->line_drops = s_line_drops;
    out->retry_requests = s_retry_requests;
    out->retry_delivered = s_retry_delivered;
    out->seal_deferred_retry = s_seal_deferred_retry;
    out->seal_aborts_retry = s_seal_aborts_retry;
    out->stale_results = s_stale_results;
    out->present_submits = s_present_submits;
    out->present_completions = s_present_completions;
    out->present_backpressure = s_present_backpressure;
    out->admission_deferred_capacity = s_admission_deferred_capacity;
    out->admission_deferred_same_line = s_admission_deferred_same_line;
    out->geometry_duplicate_suppressed = s_geometry_duplicate_suppressed;
    out->tasklog_record_collisions = s_tasklog_record_collisions;
    out->tasklog_present_unknown = s_tasklog_present_unknown;
    out->tasklog_present_rejected = s_tasklog_present_rejected;
    out->tasklog_present_out_of_order = s_tasklog_present_out_of_order;
    out->admission_deferred_fair = s_admission_deferred_fair;
    out->fair_wraps = s_fair_wraps;
    out->mailbox_posts = s_mailbox_posts;
    out->mailbox_duplicate_posts = s_mailbox_duplicate_posts;
    out->mailbox_offer_suppressed = s_mailbox_offer_suppressed;
    out->mailbox_refresh_claims = s_mailbox_refresh_claims;
    out->mailbox_refresh_lines = s_mailbox_refresh_lines;
    out->mailbox_refresh_tiles = s_mailbox_refresh_tiles;
    out->mailbox_last_claim_max_seq = s_mailbox_last_claim_max_seq;
    out->mailbox_last_claim_present_token = s_mailbox_last_claim_present_token;
    out->mailbox_buffer_swaps = __atomic_load_n(&s_mailbox_buffer_swaps, __ATOMIC_RELAXED);
    out->mailbox_swap_deferred = __atomic_load_n(&s_mailbox_swap_deferred, __ATOMIC_RELAXED);
    out->mailbox_writer_retries = __atomic_load_n(&s_mailbox_writer_retries, __ATOMIC_RELAXED);
    out->mailbox_rescue_attempts = s_mailbox_rescue_attempts;
    out->mailbox_rescue_swaps = s_mailbox_rescue_swaps;
    out->mailbox_rescue_skip_busy = s_mailbox_rescue_skip_busy;
    out->mailbox_rescue_skip_budget = s_mailbox_rescue_skip_budget;
    out->mailbox_edge_wakes = __atomic_load_n(&s_mailbox_edge_wakes, __ATOMIC_RELAXED);
    out->mailbox_rescue_limit = __atomic_load_n(&s_mailbox_rescue_limit, __ATOMIC_ACQUIRE);
    out->audio_reserve_frames = __atomic_load_n(&s_audio_reserve_frames, __ATOMIC_ACQUIRE);
    out->mailbox_offer_epoch = s_mailbox_offer_epoch;
    out->mailbox_rescues_since_refresh = s_mailbox_rescues_since_refresh;
    out->mailbox_active_index = __atomic_load_n(&s_dirty_mailbox_active_idx, __ATOMIC_ACQUIRE) & 1u;
    out->mailbox_pending_mask = __atomic_load_n(&s_dirty_mailbox_pending_mask, __ATOMIC_ACQUIRE);
    out->mailbox_inflight0 = __atomic_load_n(&s_dirty_mailbox_writer_inflight[0], __ATOMIC_ACQUIRE);
    out->mailbox_inflight1 = __atomic_load_n(&s_dirty_mailbox_writer_inflight[1], __ATOMIC_ACQUIRE);
    out->render_generation = s_generation;
    out->pending_tickets = s_pending_tickets;
    out->retry_debt = s_retry_debt_count;
    out->deferred_lines = s_deferred_count;
    out->max_inflight = SCREEN_MAX_INFLIGHT;
    out->admission_open = s_admission_open ? 1u : 0u;
    out->seal_requested = s_seal_requested ? 1u : 0u;
    out->geometry_reset_pending = s_geometry_reset_pending ? 1u : 0u;
    out->candidate_video_epoch_min = s_candidate_video_epoch_min;
    out->candidate_video_epoch_max = s_candidate_video_epoch_max;
    out->candidate_visual_seq_min = s_candidate_visual_seq_min;
    out->candidate_visual_seq_max = s_candidate_visual_seq_max;

    /* BAT173E1: all expensive line-tag inspection moved to the actual seal
     * boundary after a hard transition.  Public stats only copy counters. */
    out->epoch_transitions = s_epoch_transitions;
    out->epoch_mixed_commits = s_epoch_mixed_commits;
    out->epoch_transition_pending = s_epoch_transition_pending ? 1u : 0u;
    out->epoch_transition_target = s_epoch_transition_target;
    out->epoch_transition_seals = s_epoch_transition_seals;
    out->epoch_transition_bad_seals = s_epoch_transition_bad_seals;
    out->epoch_transition_bad_visible = s_epoch_transition_bad_visible;
    out->epoch_quarantine_drops = s_epoch_quarantine_drops;
    out->epoch_quarantine_coherent_seals = s_epoch_quarantine_coherent_seals;
    out->epoch_last_transition_seal_id = s_epoch_last_transition_seal_id;
    out->epoch_last_transition_seal_min = s_epoch_last_transition_seal_min;
    out->epoch_last_transition_seal_max = s_epoch_last_transition_seal_max;
    out->epoch_last_transition_seal_epoch = s_epoch_last_transition_seal_epoch;
    out->epoch_last_transition_mismatch_lines = s_epoch_last_transition_mismatch_lines;
    out->epoch_last_transition_unknown_lines = s_epoch_last_transition_unknown_lines;
    out->epoch_last_bad_visible_id = s_epoch_last_bad_visible_id;
    out->epoch_last_bad_visible_epoch = s_epoch_last_bad_visible_epoch;
    out->epoch_last_bad_visible_mismatch_lines = s_epoch_last_bad_visible_mismatch_lines;
    out->epoch_last_bad_visible_unknown_lines = s_epoch_last_bad_visible_unknown_lines;

    /* BAT144: diagnostic only.  A full 32-ticket window should normally be
     * transient.  Scan the PSRAM ticket table only in that exceptional state
     * (the public stats call itself is only every 600 guest frames), so normal
     * runtime cost remains unchanged.  This tells us whether a renderer has
     * stopped returning tickets or the Screen Manager result queue is stuck. */
    if (s_pending_tickets >= SCREEN_MAX_INFLIGHT && s_tickets) {
        uint64_t oldest_ticket = 0u;
        uint64_t oldest_seq = 0u;
        uint32_t oldest_y = 0xffffffffu;
        for (uint32_t i = 0u; i < SCREEN_TICKET_RECORDS; ++i) {
            const render_ticket_record_t *tr = &s_tickets[i];
            if (!tr->active)
                continue;
            if (!oldest_seq || tr->guest_seq < oldest_seq) {
                oldest_ticket = tr->ticket;
                oldest_seq = tr->guest_seq;
                oldest_y = tr->y;
            }
        }
        out->oldest_pending_ticket = oldest_ticket;
        out->oldest_pending_guest_seq = oldest_seq;
        out->oldest_pending_age_seq =
            (oldest_seq && s_guest_seq >= oldest_seq) ? (s_guest_seq - oldest_seq) : 0u;
        out->oldest_pending_y = oldest_y;
    } else {
        out->oldest_pending_y = 0xffffffffu;
    }
    out->width = s_width;
    out->height = s_height;
    out->pitch = SCREEN_BACKING_PITCH;

    /* FreeRTOS queue-depth reads are diagnostic snapshots and do not alter
     * producer/consumer ownership. */
    out->result_q_depth = s_result_q ? (uint32_t)uxQueueMessagesWaiting(s_result_q) : 0u;
    out->result_free_depth = s_result_free_q ? (uint32_t)uxQueueMessagesWaiting(s_result_free_q) : 0u;
}

uint32_t tab5_screen_manager_stack_highwater(void)
{
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0u;
}
