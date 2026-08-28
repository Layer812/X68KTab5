#include "tab5_lp_broker.h"

#ifndef PX68K_TAB5_RELEASE_DIAGNOSTICS
#define PX68K_TAB5_RELEASE_DIAGNOSTICS 0
#endif

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "ulp_lp_core.h"
#include "ulp_px68k_lpbr.h"

#define LPBR_READY_MAGIC 0x4C504236u /* "LPB6" */
#define LPBR_RING_SLOTS 256u
#define LPBR_RING_MASK (LPBR_RING_SLOTS - 1u)
#define LPBR_WORDS 3u

static const char *TAG = "TAB5_LPBR";
static volatile uint32_t s_active;
static volatile uint32_t s_hp_pub;
static volatile uint32_t s_hp_overflow;
static volatile uint32_t s_touch_hp_pub;
static volatile uint32_t s_touch_hp_take;
static volatile uint32_t s_touch_control_pub;
static volatile uint32_t s_touch_dup_suppressed;
static volatile uint32_t s_touch_control_generation;
static uint16_t s_touch_last_pub_value;
static uint8_t s_touch_last_pub_valid;
static uint32_t s_touch_pub_seen_control_generation;
static uint32_t s_touch_pub_epoch;
static uint32_t s_touch_take_serial;
static uint32_t s_touch_control_epoch;

/* R3 CPU0 consumer telemetry.  The LP owns classification/union semantics;
 * these words only record what the Screen Manager actually consumed. */
static volatile uint32_t s_dirty_hp_take;
static volatile uint32_t s_dirty_hp_last_frontier;
static volatile uint32_t s_dirty_hp_last_rows;
static volatile uint32_t s_dirty_hp_last_flags;
static volatile uint32_t s_dirty_hp_raw_total;
static volatile uint32_t s_dirty_hp_coalesced_total;
static volatile uint32_t s_dirty_unstable_reads;
static uint32_t s_dirty_take_serial;
static volatile uint32_t s_exact_cp_hp_pub;
static volatile uint32_t s_frontier_cpu0_seq;
static volatile uint32_t s_frontier_cpu0_hash;
static volatile int32_t s_frontier_last_delta;
static volatile uint32_t s_frontier_equal_pass;
static volatile uint32_t s_frontier_equal_fail;
static volatile uint32_t s_frontier_ahead_samples;
static volatile uint32_t s_frontier_behind_samples;
static volatile uint32_t s_frontier_equal_samples;
static volatile uint32_t s_frontier_max_abs_delta;
static volatile uint32_t s_gate_checks;
static volatile uint32_t s_gate_pass;
static volatile uint32_t s_gate_would_block;
static volatile uint32_t s_gate_max_lag;
static volatile uint32_t s_gate_block_streak;
static volatile uint32_t s_gate_max_block_streak;
static volatile uint32_t s_gate_release_regress;
static volatile uint32_t s_gate_last_release;
static volatile uint32_t s_gate_armed;
static volatile uint32_t s_gate_arm_seq;
static volatile uint32_t s_gate_prearm_checks;
static volatile uint32_t s_gate_prearm_would;

extern const uint8_t bin_start[] asm("_binary_ulp_px68k_lpbr_bin_start");
extern const uint8_t bin_end[] asm("_binary_ulp_px68k_lpbr_bin_end");

static inline void fence_rw(void)
{
    __asm__ __volatile__("fence rw,rw" ::: "memory");
}

int tab5_lp_broker_init(void)
{
    s_active = 0u;
    s_hp_pub = 0u;
    s_hp_overflow = 0u;
    s_touch_hp_pub = 0u;
    s_touch_hp_take = 0u;
    s_touch_control_pub = 0u;
    s_touch_dup_suppressed = 0u;
    __atomic_store_n(&s_touch_control_generation, 0u, __ATOMIC_RELAXED);
    s_touch_last_pub_value = 0u;
    s_touch_last_pub_valid = 0u;
    s_touch_pub_seen_control_generation = 0u;
    s_touch_pub_epoch = 0u;
    s_touch_take_serial = 0u;
    s_touch_control_epoch = 0u;
    s_dirty_hp_take = 0u;
    s_dirty_hp_last_frontier = 0u;
    s_dirty_hp_last_rows = 0u;
    s_dirty_hp_last_flags = 0u;
    s_dirty_hp_raw_total = 0u;
    s_dirty_hp_coalesced_total = 0u;
    s_dirty_unstable_reads = 0u;
    s_dirty_take_serial = 0u;
    s_exact_cp_hp_pub = 0u;
    s_frontier_cpu0_seq = 0u;
    s_frontier_cpu0_hash = 0u;
    s_frontier_last_delta = 0;
    s_frontier_equal_pass = 0u;
    s_frontier_equal_fail = 0u;
    s_frontier_ahead_samples = 0u;
    s_frontier_behind_samples = 0u;
    s_frontier_equal_samples = 0u;
    s_frontier_max_abs_delta = 0u;
    s_gate_checks = 0u;
    s_gate_pass = 0u;
    s_gate_would_block = 0u;
    s_gate_max_lag = 0u;
    s_gate_block_streak = 0u;
    s_gate_max_block_streak = 0u;
    s_gate_release_regress = 0u;
    s_gate_last_release = 0u;
    s_gate_armed = 0u;
    s_gate_arm_seq = 0u;
    s_gate_prearm_checks = 0u;
    s_gate_prearm_would = 0u;

    const size_t n = (size_t)(bin_end - bin_start);
    esp_err_t rc = ulp_lp_core_load_binary(bin_start, n);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG,
                 "LPFAB_R10 load failed rc=%d bytes=%u; R57 + direct touch retained",
                 (int)rc,
                 (unsigned)n);
        return 0;
    }

    ulp_lp_core_cfg_t cfg = {0};
    cfg.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU;
#if ESP_ROM_HAS_LP_ROM
    cfg.skip_lp_rom_boot = true;
#endif

    rc = ulp_lp_core_run(&cfg);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG,
                 "LPFAB_R10 run failed rc=%d; R57 + direct touch retained",
                 (int)rc);
        return 0;
    }

    /* LP continuously polls shared RTC RAM.  No HP->LP software interrupt is
     * needed: all SPSC/latest visibility is volatile storage + fence rw,rw. */
    int64_t deadline = esp_timer_get_time() + 100000;
    while (ulp_lpbr_ready != LPBR_READY_MAGIC &&
           esp_timer_get_time() < deadline) {
    }
    if (ulp_lpbr_ready != LPBR_READY_MAGIC) {
        ESP_LOGW(TAG,
                 "LPFAB_R10 READY timeout got=%08lX",
                 (unsigned long)ulp_lpbr_ready);
        return 0;
    }

    const uint32_t probe = 0xC3A55A3Cu;
    ulp_lpbr_hp_probe = probe;
    fence_rw();

    deadline = esp_timer_get_time() + 50000;
    while (ulp_lpbr_lp_echo != probe && esp_timer_get_time() < deadline) {
    }
    if (ulp_lpbr_lp_echo != probe) {
        ESP_LOGW(TAG,
                 "LPFAB_R10 echo timeout got=%08lX",
                 (unsigned long)ulp_lpbr_lp_echo);
        return 0;
    }

    s_active = 1u;
    ESP_LOGI(TAG,
             "LPFAB_R10 ACTIVE bytes=%u ring=%u READY+probe/echo PASS; "
             "R57=SHADOW EXACT + 1/1024 HASH CERT + PUBLISH-ORDER; PER-EVENT GATE RETIRED, "
             "TouchJoy=PRODUCTION LATEST, DIRTY=LP WORKSET OBSERVE/ACK",
             (unsigned)n,
             (unsigned)LPBR_RING_SLOTS);
    return 1;
}

void tab5_lp_broker_mirror(uint32_t seq, uint32_t address, uint32_t meta)
{
    if (__builtin_expect(!s_active, 0)) {
        return;
    }

    const uint32_t h = ulp_lpbr_head;
    fence_rw();
    const uint32_t t = ulp_lpbr_tail;

    if (__builtin_expect((uint32_t)(h - t) >= LPBR_RING_SLOTS, 0)) {
        ++s_hp_overflow;
        return;
    }

    const uint32_t i = (h & LPBR_RING_MASK) * LPBR_WORDS;
    ulp_lpbr_ring[i] = seq;
    ulp_lpbr_ring[i + 1u] = address;
    ulp_lpbr_ring[i + 2u] = meta;
    fence_rw();
    ulp_lpbr_head = h + 1u;
    fence_rw();
    ++s_hp_pub;
}

void tab5_lp_broker_exact_checkpoint(uint32_t seq, uint32_t producer_hash)
{
    if (__builtin_expect(!s_active, 0) || seq == 0u || (seq & 1023u) != 0u) {
        return;
    }
    const uint32_t slot = (seq >> 10) & 3u;
    ulp_lpbr_exact_cp_hash[slot] = producer_hash;
    fence_rw();
    /* Commit sequence last. LP can never observe a new sequence with an old hash. */
    ulp_lpbr_exact_cp_seq[slot] = seq;
    fence_rw();
    ++s_exact_cp_hp_pub;
}

void tab5_lp_broker_exact_frontier_observe(uint32_t cpu0_seq, uint32_t cpu0_hash)
{
    if (__builtin_expect(!s_active, 0)) {
        return;
    }
    /* BAT152/R10: per-event hard gate retired; frontier certification only. Before sync-arm these counters measure
     * startup catch-up; the first exact frontier equality snapshots them and
     * resets the same fields for steady-state-only measurement. */
    fence_rw();
    const uint32_t release_seq = ulp_lpbr_exact_release_seq;
    const uint32_t release_hash = ulp_lpbr_exact_release_hash;
    const int32_t delta = (int32_t)(release_seq - cpu0_seq);
    const uint32_t abs_delta = (delta < 0) ? (uint32_t)(-(int64_t)delta) : (uint32_t)delta;

    s_frontier_cpu0_seq = cpu0_seq;
    s_frontier_cpu0_hash = cpu0_hash;
    s_frontier_last_delta = delta;
    if (abs_delta > s_frontier_max_abs_delta) {
        s_frontier_max_abs_delta = abs_delta;
    }
    if (delta > 0) {
        ++s_frontier_ahead_samples;
    } else if (delta < 0) {
        ++s_frontier_behind_samples;
    } else {
        ++s_frontier_equal_samples;
        if (release_hash == cpu0_hash) {
            ++s_frontier_equal_pass;
            /* BAT151/R9: the first exact seq+hash equality is the only safe
             * point to separate startup catch-up from steady-state admission.
             * Preserve the pre-arm totals, then reset the dry-run counters so
             * subsequent telemetry describes only the synchronized regime. */
            if (!s_gate_armed && cpu0_seq != 0u) {
                s_gate_prearm_checks = s_gate_checks;
                s_gate_prearm_would = s_gate_would_block;
                s_gate_checks = 0u;
                s_gate_pass = 0u;
                s_gate_would_block = 0u;
                s_gate_max_lag = 0u;
                s_gate_block_streak = 0u;
                s_gate_max_block_streak = 0u;
                s_gate_release_regress = 0u;
                s_gate_last_release = release_seq;
                s_gate_arm_seq = cpu0_seq;
                __atomic_store_n(&s_gate_armed, 1u, __ATOMIC_RELEASE);
            }
        } else {
            ++s_frontier_equal_fail;
        }
    }
}


int tab5_lp_broker_exact_gate_probe(uint32_t next_seq)
{
    if (__builtin_expect(!s_active, 0) || next_seq == 0u) {
        return 1;
    }

    fence_rw();
    const uint32_t release_seq = ulp_lpbr_exact_release_seq;
    const uint32_t prev_release = s_gate_last_release;
    if (prev_release != 0u && (int32_t)(release_seq - prev_release) < 0) {
        ++s_gate_release_regress;
    }
    s_gate_last_release = release_seq;
    ++s_gate_checks;

    if ((int32_t)(release_seq - next_seq) >= 0) {
        ++s_gate_pass;
        s_gate_block_streak = 0u;
        return 1;
    }

    ++s_gate_would_block;
    const uint32_t lag = next_seq - release_seq;
    if (lag > s_gate_max_lag) {
        s_gate_max_lag = lag;
    }
    const uint32_t streak = s_gate_block_streak + 1u;
    s_gate_block_streak = streak;
    if (streak > s_gate_max_block_streak) {
        s_gate_max_block_streak = streak;
    }
    return 0; /* DRY RUN ONLY: caller deliberately continues consuming. */
}

int tab5_lp_broker_touch_latest_publish(uint16_t joy_bits)
{
    if (__builtin_expect(!s_active, 0)) {
        return 0;
    }

    const uint32_t control_generation =
        __atomic_load_n(&s_touch_control_generation, __ATOMIC_ACQUIRE);
    if (control_generation != s_touch_pub_seen_control_generation) {
        s_touch_pub_seen_control_generation = control_generation;
        s_touch_last_pub_valid = 0u;
    }
    if (s_touch_last_pub_valid && s_touch_last_pub_value == joy_bits) {
        ++s_touch_dup_suppressed;
        return 1;
    }
    s_touch_last_pub_value = joy_bits;
    s_touch_last_pub_valid = 1u;

    const uint32_t epoch = ++s_touch_pub_epoch;
    ulp_lpbr_touch_in_value = (uint32_t)joy_bits;
    fence_rw();
    ulp_lpbr_touch_in_epoch = epoch;
    fence_rw();
    ++s_touch_hp_pub;
    return 1;
}

int tab5_lp_broker_touch_latest_take(uint16_t *joy_bits)
{
    if (!joy_bits || __builtin_expect(!s_active, 0)) {
        return 0;
    }

    const uint32_t serial = ulp_lpbr_touch_out_serial;
    fence_rw();
    if (serial == 0u || serial == s_touch_take_serial) {
        return 0;
    }

    const uint32_t value = ulp_lpbr_touch_out_value;
    fence_rw();
    s_touch_take_serial = serial;
    ++s_touch_hp_take;
    *joy_bits = (uint16_t)value;
    return 1;
}

int tab5_lp_broker_touch_control_publish(uint16_t joy_bits)
{
    if (__builtin_expect(!s_active, 0)) {
        return 0;
    }

    const uint32_t epoch = ++s_touch_control_epoch;
    ulp_lpbr_touch_ctl_value = (uint32_t)joy_bits;
    fence_rw();
    ulp_lpbr_touch_ctl_epoch = epoch;
    fence_rw();
    ++s_touch_control_pub;
    __atomic_add_fetch(&s_touch_control_generation, 1u, __ATOMIC_RELEASE);
    return 1;
}

int tab5_lp_broker_dirty_take(tab5_lp_dirty_workset_t *out)
{
    if (!out || __builtin_expect(!s_active, 0)) {
        return 0;
    }

    const uint32_t serial1 = ulp_lpbr_dirty_out_serial;
    if (serial1 == 0u || serial1 == s_dirty_take_serial) {
        return 0;
    }
    fence_rw();

    tab5_lp_dirty_workset_t w;
    w.serial = serial1;
    w.frontier_seq = ulp_lpbr_dirty_frontier_seq;
    w.row_buckets = ulp_lpbr_dirty_rows;
    w.flags = ulp_lpbr_dirty_flags;
    w.raw_events = ulp_lpbr_dirty_raw;
    w.coalesced_events = ulp_lpbr_dirty_coalesced;
    fence_rw();

    /* Nonblocking snapshot validation.  If LP published a newer frontier
     * while CPU0 copied this one, retry on the next Screen Manager pass. */
    const uint32_t serial2 = ulp_lpbr_dirty_out_serial;
    if (__builtin_expect(serial1 != serial2, 0)) {
        ++s_dirty_unstable_reads;
        return 0;
    }

    s_dirty_take_serial = serial1;
    s_dirty_hp_last_frontier = w.frontier_seq;
    s_dirty_hp_last_rows = w.row_buckets;
    s_dirty_hp_last_flags = w.flags;
    s_dirty_hp_raw_total += w.raw_events;
    s_dirty_hp_coalesced_total += w.coalesced_events;
    ++s_dirty_hp_take;
    *out = w;

    /* Ack payload only after the local snapshot is complete.  LP clears that
     * published union iff this serial is still current; newer work survives. */
    ulp_lpbr_dirty_ack_serial = serial1;
    fence_rw();
    return 1;
}

void tab5_lp_broker_video_frontier_publish(uint32_t kind, uint64_t seq)
{
    if (!s_active) return;
    const uint32_t v = (uint32_t)seq;
    switch (kind) {
        case TAB5_LP_VIDEO_FRONTIER_GUEST:   ulp_lpbr_video_guest_frontier = v; break;
        case TAB5_LP_VIDEO_FRONTIER_COMPOSE: ulp_lpbr_video_compose_frontier = v; break;
        case TAB5_LP_VIDEO_FRONTIER_SCREEN:  ulp_lpbr_video_screen_frontier = v; break;
        case TAB5_LP_VIDEO_FRONTIER_VISIBLE: ulp_lpbr_video_visible_frontier = v; break;
        default: return;
    }
    fence_rw();
}

void tab5_lp_broker_get_stats(tab5_lp_broker_stats_t *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->hp_pub = s_hp_pub;
    out->hp_overflow = s_hp_overflow;
    out->touch_hp_pub = s_touch_hp_pub;
    out->touch_hp_take = s_touch_hp_take;
    out->touch_control_pub = s_touch_control_pub;
    out->touch_dup_suppressed = s_touch_dup_suppressed;
    out->dirty_hp_take = s_dirty_hp_take;
    out->dirty_hp_last_frontier = s_dirty_hp_last_frontier;
    out->dirty_hp_last_rows = s_dirty_hp_last_rows;
    out->dirty_hp_last_flags = s_dirty_hp_last_flags;
    out->dirty_hp_raw_total = s_dirty_hp_raw_total;
    out->dirty_hp_coalesced_total = s_dirty_hp_coalesced_total;
    out->dirty_unstable_reads = s_dirty_unstable_reads;
    out->exact_cp_hp_pub = s_exact_cp_hp_pub;
    out->frontier_cpu0_seq = s_frontier_cpu0_seq;
    out->frontier_cpu0_hash = s_frontier_cpu0_hash;
    out->frontier_last_delta = s_frontier_last_delta;
    out->frontier_equal_pass = s_frontier_equal_pass;
    out->frontier_equal_fail = s_frontier_equal_fail;
    out->frontier_ahead_samples = s_frontier_ahead_samples;
    out->frontier_behind_samples = s_frontier_behind_samples;
    out->frontier_equal_samples = s_frontier_equal_samples;
    out->frontier_max_abs_delta = s_frontier_max_abs_delta;
    out->gate_checks = s_gate_checks;
    out->gate_pass = s_gate_pass;
    out->gate_would_block = s_gate_would_block;
    out->gate_max_lag = s_gate_max_lag;
    out->gate_block_streak = s_gate_block_streak;
    out->gate_max_block_streak = s_gate_max_block_streak;
    out->gate_release_regress = s_gate_release_regress;
    out->gate_last_release = s_gate_last_release;
    out->gate_armed = __atomic_load_n(&s_gate_armed, __ATOMIC_ACQUIRE);
    out->gate_arm_seq = s_gate_arm_seq;
    out->gate_prearm_checks = s_gate_prearm_checks;
    out->gate_prearm_would = s_gate_prearm_would;
    if (!s_active) {
        return;
    }

    fence_rw();
    out->lp_cons = ulp_lpbr_cons;
    out->lp_seq = ulp_lpbr_seq;
    out->lp_gap = ulp_lpbr_gap;
    out->lp_hash = ulp_lpbr_hash;
    out->heartbeat = ulp_lpbr_heartbeat;
    out->probe = ulp_lpbr_hp_probe;
    out->echo = ulp_lpbr_lp_echo;
    out->trap_mcause = ulp_lpbr_trap_mcause;
    out->trap_mepc = ulp_lpbr_trap_mepc;
    out->trap_mtval = ulp_lpbr_trap_mtval;
    out->touch_lp_out_serial = ulp_lpbr_touch_out_serial;
    out->touch_lp_coalesced = ulp_lpbr_touch_coalesced;
    out->touch_value = ulp_lpbr_touch_out_value;
    out->dirty_lp_serial = ulp_lpbr_dirty_out_serial;
    out->dirty_lp_ack_serial = ulp_lpbr_dirty_ack_serial;
    out->dirty_lp_frontier_seq = ulp_lpbr_dirty_frontier_seq;
    out->exact_cp_lp_pass = ulp_lpbr_exact_cp_pass;
    out->exact_cp_lp_fail = ulp_lpbr_exact_cp_fail;
    out->exact_cp_lp_miss = ulp_lpbr_exact_cp_miss;
    out->exact_cp_last_seq = ulp_lpbr_exact_last_seq;
    out->exact_cp_last_hp_hash = ulp_lpbr_exact_last_hp_hash;
    out->exact_cp_last_lp_hash = ulp_lpbr_exact_last_lp_hash;
    out->exact_release_seq = ulp_lpbr_exact_release_seq;
    out->exact_release_hash = ulp_lpbr_exact_release_hash;
    out->exact_release_count = ulp_lpbr_exact_release_count;
    out->video_guest_frontier = ulp_lpbr_video_guest_frontier;
    out->video_compose_frontier = ulp_lpbr_video_compose_frontier;
    out->video_screen_frontier = ulp_lpbr_video_screen_frontier;
    out->video_visible_frontier = ulp_lpbr_video_visible_frontier;
}

void tab5_lp_broker_report(void)
{
#if PX68K_TAB5_RELEASE_DIAGNOSTICS
    tab5_lp_broker_stats_t s;
    tab5_lp_broker_get_stats(&s);
    ESP_LOGI(TAG,
             "LPFAB_R10 active=%lu shadow{hpPub=%lu lpCons=%lu lpSeq=%lu overflow=%lu "
             "gap=%lu hash=%08lX} touchLatest{pub=%lu outSerial=%lu take=%lu "
             "coal=%lu dupSup=%lu val=%04lX ctl=%lu} "
             "dirty{lpSerial=%lu ack=%lu frontier=%lu cpu0Take=%lu lastRows=%08lX "
             "lastFlags=%02lX rawTotal=%lu coalTotal=%lu unstable=%lu} "
             "exact{cpPub=%lu pass=%lu fail=%lu miss=%lu lastSeq=%lu hp=%08lX lp=%08lX} "
             "frontier{release=%lu relHash=%08lX count=%lu cpu0=%lu c0Hash=%08lX delta=%ld "
             "eq=%lu/%lu samples=%lu/%lu/%lu maxAbs=%lu} "
             "video{g=%lu c=%lu s=%lu v=%lu lag=%lu/%lu/%lu} "
             "gateRetired{armed=%lu armSeq=%lu pre=%lu/%lu checks=%lu pass=%lu would=%lu maxLag=%lu streak=%lu maxStreak=%lu regress=%lu lastRel=%lu} "
             "hb=%lu probe/echo=%08lX/%08lX trap=%08lX/%08lX/%08lX",
             (unsigned long)s.active,
             (unsigned long)s.hp_pub,
             (unsigned long)s.lp_cons,
             (unsigned long)s.lp_seq,
             (unsigned long)s.hp_overflow,
             (unsigned long)s.lp_gap,
             (unsigned long)s.lp_hash,
             (unsigned long)s.touch_hp_pub,
             (unsigned long)s.touch_lp_out_serial,
             (unsigned long)s.touch_hp_take,
             (unsigned long)s.touch_lp_coalesced,
             (unsigned long)s.touch_dup_suppressed,
             (unsigned long)(s.touch_value & 0xffffu),
             (unsigned long)s.touch_control_pub,
             (unsigned long)s.dirty_lp_serial,
             (unsigned long)s.dirty_lp_ack_serial,
             (unsigned long)s.dirty_lp_frontier_seq,
             (unsigned long)s.dirty_hp_take,
             (unsigned long)s.dirty_hp_last_rows,
             (unsigned long)(s.dirty_hp_last_flags & 0xffu),
             (unsigned long)s.dirty_hp_raw_total,
             (unsigned long)s.dirty_hp_coalesced_total,
             (unsigned long)s.dirty_unstable_reads,
             (unsigned long)s.exact_cp_hp_pub,
             (unsigned long)s.exact_cp_lp_pass,
             (unsigned long)s.exact_cp_lp_fail,
             (unsigned long)s.exact_cp_lp_miss,
             (unsigned long)s.exact_cp_last_seq,
             (unsigned long)s.exact_cp_last_hp_hash,
             (unsigned long)s.exact_cp_last_lp_hash,
             (unsigned long)s.exact_release_seq,
             (unsigned long)s.exact_release_hash,
             (unsigned long)s.exact_release_count,
             (unsigned long)s.frontier_cpu0_seq,
             (unsigned long)s.frontier_cpu0_hash,
             (long)s.frontier_last_delta,
             (unsigned long)s.frontier_equal_pass,
             (unsigned long)s.frontier_equal_fail,
             (unsigned long)s.frontier_ahead_samples,
             (unsigned long)s.frontier_behind_samples,
             (unsigned long)s.frontier_equal_samples,
             (unsigned long)s.frontier_max_abs_delta,
             (unsigned long)s.video_guest_frontier,
             (unsigned long)s.video_compose_frontier,
             (unsigned long)s.video_screen_frontier,
             (unsigned long)s.video_visible_frontier,
             (unsigned long)(s.video_guest_frontier - s.video_compose_frontier),
             (unsigned long)(s.video_guest_frontier - s.video_screen_frontier),
             (unsigned long)(s.video_guest_frontier - s.video_visible_frontier),
             (unsigned long)s.gate_armed,
             (unsigned long)s.gate_arm_seq,
             (unsigned long)s.gate_prearm_checks,
             (unsigned long)s.gate_prearm_would,
             (unsigned long)s.gate_checks,
             (unsigned long)s.gate_pass,
             (unsigned long)s.gate_would_block,
             (unsigned long)s.gate_max_lag,
             (unsigned long)s.gate_block_streak,
             (unsigned long)s.gate_max_block_streak,
             (unsigned long)s.gate_release_regress,
             (unsigned long)s.gate_last_release,
             (unsigned long)s.heartbeat,
             (unsigned long)s.probe,
             (unsigned long)s.echo,
             (unsigned long)s.trap_mcause,
             (unsigned long)s.trap_mepc,
             (unsigned long)s.trap_mtval);
#else
    /* Release: periodic broker telemetry is intentionally compiled out. */
#endif
}
