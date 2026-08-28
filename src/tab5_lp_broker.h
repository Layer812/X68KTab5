#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TAB5_LP_DIRTY_TEXT       = 1u << 0,
    TAB5_LP_DIRTY_BG         = 1u << 1,
    TAB5_LP_DIRTY_SPRITE     = 1u << 2,
    TAB5_LP_DIRTY_BGREG      = 1u << 3,
    TAB5_LP_DIRTY_TEXT_COPY  = 1u << 4,
    TAB5_LP_DIRTY_TEXT_RESET = 1u << 5,
    TAB5_LP_DIRTY_BG_RESET   = 1u << 6,
};


enum {
    TAB5_LP_VIDEO_FRONTIER_GUEST = 0,
    TAB5_LP_VIDEO_FRONTIER_COMPOSE = 1,
    TAB5_LP_VIDEO_FRONTIER_SCREEN = 2,
    TAB5_LP_VIDEO_FRONTIER_VISIBLE = 3,
};
typedef struct {
    uint32_t serial;
    uint32_t frontier_seq;
    uint32_t row_buckets;      /* 32 buckets x 32 TVRAM rows = 1024 rows */
    uint32_t flags;
    uint32_t raw_events;
    uint32_t coalesced_events;
} tab5_lp_dirty_workset_t;

typedef struct {
    uint32_t active;
    uint32_t hp_pub;
    uint32_t hp_overflow;
    uint32_t lp_cons;
    uint32_t lp_seq;
    uint32_t lp_gap;
    uint32_t lp_hash;
    uint32_t heartbeat;
    uint32_t probe;
    uint32_t echo;
    uint32_t trap_mcause;
    uint32_t trap_mepc;
    uint32_t trap_mtval;
    uint32_t touch_hp_pub;
    uint32_t touch_lp_out_serial;
    uint32_t touch_hp_take;
    uint32_t touch_lp_coalesced;
    uint32_t touch_value;
    uint32_t touch_control_pub;
    uint32_t touch_dup_suppressed;
    uint32_t dirty_lp_serial;
    uint32_t dirty_lp_ack_serial;
    uint32_t dirty_lp_frontier_seq;
    uint32_t dirty_hp_take;
    uint32_t dirty_hp_last_frontier;
    uint32_t dirty_hp_last_rows;
    uint32_t dirty_hp_last_flags;
    uint32_t dirty_hp_raw_total;
    uint32_t dirty_hp_coalesced_total;
    uint32_t dirty_unstable_reads;
    uint32_t exact_cp_hp_pub;
    uint32_t exact_cp_lp_pass;
    uint32_t exact_cp_lp_fail;
    uint32_t exact_cp_lp_miss;
    uint32_t exact_cp_last_seq;
    uint32_t exact_cp_last_hp_hash;
    uint32_t exact_cp_last_lp_hash;
    uint32_t exact_release_seq;
    uint32_t exact_release_hash;
    uint32_t exact_release_count;
    uint32_t frontier_cpu0_seq;
    uint32_t frontier_cpu0_hash;
    int32_t frontier_last_delta;
    uint32_t frontier_equal_pass;
    uint32_t frontier_equal_fail;
    uint32_t frontier_ahead_samples;
    uint32_t frontier_behind_samples;
    uint32_t frontier_equal_samples;
    uint32_t frontier_max_abs_delta;
    uint32_t video_guest_frontier;
    uint32_t video_compose_frontier;
    uint32_t video_screen_frontier;
    uint32_t video_visible_frontier;
    uint32_t gate_checks;
    uint32_t gate_pass;
    uint32_t gate_would_block;
    uint32_t gate_max_lag;
    uint32_t gate_block_streak;
    uint32_t gate_max_block_streak;
    uint32_t gate_release_regress;
    uint32_t gate_last_release;
    uint32_t gate_armed;
    uint32_t gate_arm_seq;
    uint32_t gate_prearm_checks;
    uint32_t gate_prearm_would;
} tab5_lp_broker_stats_t;

int tab5_lp_broker_init(void);

/* R1/R2 shadow: CPU1 R57 events are mirrored to LP while exact production
 * ownership remains CPU1 -> R57 journal -> CPU0.  R3 classifies these events
 * into a lossless DIRTY union at raster frontiers. */
void tab5_lp_broker_mirror(uint32_t seq, uint32_t address, uint32_t meta);

/* R4 exact checkpoint certification. CPU1 publishes the already-computed
 * authoritative R57 cumulative hash BEFORE mirroring each 1024th event.
 * LP compares its independently reconstructed hash at the same sequence. */
void tab5_lp_broker_exact_checkpoint(uint32_t seq, uint32_t producer_hash);

/* R6/R7 control-plane frontier certification. CPU0 reports its
 * authoritative R57 consumer seq/hash at coherent report points. No gating in
 * BAT148; this measures whether LP can safely become the release authority. */
void tab5_lp_broker_exact_frontier_observe(uint32_t cpu0_seq, uint32_t cpu0_hash);

/* R7/R10 dry-run admission gate. Called immediately before CPU0 would
 * consume each authoritative R57 tuple. It records whether LP release_seq has
 * already certified that tuple, but NEVER blocks or changes production. BAT152 resets telemetry at first exact seq+hash equality so steady-state is isolated. */
int tab5_lp_broker_exact_gate_probe(uint32_t next_seq);

/* R2 production LATEST path.
 * Producer: CPU0 touch presenter. Broker: LP latest-wins slot.
 * Consumer: CPU1 emulation task before tab5_guest_input_tick(). */
int tab5_lp_broker_touch_latest_publish(uint16_t joy_bits);
int tab5_lp_broker_touch_latest_take(uint16_t *joy_bits);
int tab5_lp_broker_touch_control_publish(uint16_t joy_bits);

/* R3 DIRTY workset transport.
 * Producer/classifier: LP from mirrored ordered R57 events.
 * Consumer: CPU0 Screen Manager.  The workset is observational in BAT145:
 * CPU0 consumes/acks it, but it does not yet gate rendering or correctness.
 * R4 additionally certifies the mirrored EXACT stream by cumulative hash. */
int tab5_lp_broker_dirty_take(tab5_lp_dirty_workset_t *out);

/* BAT177NW0: observational video progress board in LP shared RAM.
 * Writers never wait; LP never gates production on these values. */
void tab5_lp_broker_video_frontier_publish(uint32_t kind, uint64_t seq);

void tab5_lp_broker_get_stats(tab5_lp_broker_stats_t *out);
void tab5_lp_broker_report(void);
#ifdef __cplusplus
}
#endif
