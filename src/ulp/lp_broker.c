#include <stdint.h>

#define LPBR_READY_MAGIC 0x4C504236u /* "LPB6" */
#define LPBR_COLLECT_MAGIC 0x4C504334u /* "LPC4" */
#define LPBR_COLLECT_CMD_ARM 1u
#define LPBR_COLLECT_STATE_IDLE 0u
#define LPBR_COLLECT_STATE_ARMED 1u
#define LPBR_COLLECT_STATE_CAPTURED 2u
#define LPBR_COLLECT_STATE_FAULT 3u

/* R139A6A3 FINAL PRODUCTION CLEAN
 *
 * LP is now only the proven TouchJoy latest-value/control transport.
 * Historical mirrored-R57 hash/checkpoint/DIRTY/frontier/heartbeat work was
 * observation-only and is physically retired.  Authoritative guest ordering,
 * shadowing and dirty ownership remain on the HP R57/Screen paths. */
volatile uint32_t lpbr_ready;
volatile uint32_t lpbr_hp_probe, lpbr_lp_echo;
volatile uint32_t lpbr_touch_in_epoch, lpbr_touch_in_value;
volatile uint32_t lpbr_touch_ctl_epoch, lpbr_touch_ctl_value;
volatile uint32_t lpbr_touch_out_serial, lpbr_touch_out_value;

/* R140P4S4: LP-owned repeatable measurement-window foundation.
 * HP publishes only a command sequence and window length.  LP owns the
 * window clock (mcycle), state transition and result publication.  No HP
 * timer/task/printf participates while a future measurement window is live. */
volatile uint32_t lpbr_collect_cmd_seq, lpbr_collect_cmd;
volatile uint32_t lpbr_collect_window_cycles;
volatile uint32_t lpbr_collect_state, lpbr_collect_seen_cmd_seq;
volatile uint32_t lpbr_collect_start_cycle, lpbr_collect_end_cycle;
volatile uint32_t lpbr_collect_elapsed_cycles, lpbr_collect_lp_loops;
volatile uint32_t lpbr_collect_result_seq, lpbr_collect_result_magic;

static inline void fence_rw(void)
{
    __asm__ __volatile__("fence rw,rw" ::: "memory");
}

static inline uint32_t lp_cycle32(void)
{
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(v));
    return v;
}

int main(void)
{
    lpbr_ready = 0u;
    lpbr_hp_probe = lpbr_lp_echo = 0u;
    lpbr_touch_in_epoch = lpbr_touch_in_value = 0u;
    lpbr_touch_ctl_epoch = lpbr_touch_ctl_value = 0u;
    lpbr_touch_out_serial = lpbr_touch_out_value = 0u;
    lpbr_collect_cmd_seq = lpbr_collect_cmd = 0u;
    lpbr_collect_window_cycles = 0u;
    lpbr_collect_state = LPBR_COLLECT_STATE_IDLE;
    lpbr_collect_seen_cmd_seq = 0u;
    lpbr_collect_start_cycle = lpbr_collect_end_cycle = 0u;
    lpbr_collect_elapsed_cycles = lpbr_collect_lp_loops = 0u;
    lpbr_collect_result_seq = lpbr_collect_result_magic = 0u;

    uint32_t touch_seen_epoch = 0u;
    uint32_t touch_ctl_seen_epoch = 0u;

    fence_rw();
    lpbr_ready = LPBR_READY_MAGIC;
    fence_rw();

    for (;;) {
        /* Cold HP<->LP bring-up/self-check echo. */
        const uint32_t probe = lpbr_hp_probe;
        if (lpbr_lp_echo != probe) {
            fence_rw();
            lpbr_lp_echo = probe;
            fence_rw();
        }

        /* R140P4S4 collector command.  result_seq is published last, after all
         * result fields, so HP can treat it as the completion frontier. */
        const uint32_t collect_seq = lpbr_collect_cmd_seq;
        if (collect_seq != lpbr_collect_seen_cmd_seq) {
            fence_rw();
            const uint32_t cmd = lpbr_collect_cmd;
            const uint32_t window = lpbr_collect_window_cycles;
            lpbr_collect_seen_cmd_seq = collect_seq;
            lpbr_collect_result_magic = 0u;
            lpbr_collect_result_seq = 0u;
            lpbr_collect_elapsed_cycles = 0u;
            lpbr_collect_lp_loops = 0u;
            if (cmd == LPBR_COLLECT_CMD_ARM && window != 0u) {
                lpbr_collect_start_cycle = lp_cycle32();
                lpbr_collect_end_cycle = lpbr_collect_start_cycle;
                lpbr_collect_state = LPBR_COLLECT_STATE_ARMED;
            } else {
                lpbr_collect_state = LPBR_COLLECT_STATE_FAULT;
            }
            fence_rw();
        }

        if (lpbr_collect_state == LPBR_COLLECT_STATE_ARMED) {
            const uint32_t now = lp_cycle32();
            ++lpbr_collect_lp_loops;
            if ((uint32_t)(now - lpbr_collect_start_cycle) >= lpbr_collect_window_cycles) {
                lpbr_collect_end_cycle = now;
                lpbr_collect_elapsed_cycles = (uint32_t)(now - lpbr_collect_start_cycle);
                lpbr_collect_result_magic = LPBR_COLLECT_MAGIC;
                lpbr_collect_state = LPBR_COLLECT_STATE_CAPTURED;
                fence_rw();
                lpbr_collect_result_seq = lpbr_collect_seen_cmd_seq;
                fence_rw();
            }
        }

        /* CPU1 ownership/control has priority.  A force-clear also consumes
         * any older CPU0 latest-value epoch so it cannot reappear afterward. */
        const uint32_t ctl_epoch = lpbr_touch_ctl_epoch;
        if (ctl_epoch != touch_ctl_seen_epoch) {
            fence_rw();
            const uint32_t ctl_value = lpbr_touch_ctl_value;
            touch_seen_epoch = lpbr_touch_in_epoch;
            lpbr_touch_out_value = ctl_value;
            fence_rw();
            ++lpbr_touch_out_serial;
            touch_ctl_seen_epoch = ctl_epoch;
            fence_rw();
        }

        /* CPU0 latest-value channel.  Newer epochs supersede unseen older ones. */
        const uint32_t touch_epoch = lpbr_touch_in_epoch;
        if (touch_epoch != touch_seen_epoch) {
            fence_rw();
            lpbr_touch_out_value = lpbr_touch_in_value;
            fence_rw();
            ++lpbr_touch_out_serial;
            touch_seen_epoch = touch_epoch;
            fence_rw();
        }
    }

    return 0;
}
