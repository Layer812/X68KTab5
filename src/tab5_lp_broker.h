#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R139A6A3 final production contract: LP remains a TouchJoy latest-value
 * transport only. Historical R57 mirror/hash certification, DIRTY workset
 * observation, gate probes and telemetry APIs are retired from HP runtime. */
int tab5_lp_broker_init(void);
int tab5_lp_broker_touch_latest_publish(uint16_t joy_bits);
int tab5_lp_broker_touch_latest_take(uint16_t *joy_bits);
int tab5_lp_broker_touch_control_publish(uint16_t joy_bits);
typedef struct {
    uint32_t result_seq;
    uint32_t state;
    uint32_t window_cycles;
    uint32_t start_cycle;
    uint32_t end_cycle;
    uint32_t elapsed_cycles;
    uint32_t lp_loops;
    uint32_t result_magic;
} tab5_lp_broker_collector_result_t;

/* R140P4S4 measurement foundation. LP owns each clock window. HP only arms
 * and consumes immutable results. S4 audio audit re-arms this API automatically
 * for multiple windows; no user action or runtime reporter task is required. */
int tab5_lp_broker_collector_arm_us(uint32_t window_us, uint32_t *seq_out);
uint32_t tab5_lp_broker_collector_hz(void);
int tab5_lp_broker_collector_take(tab5_lp_broker_collector_result_t *out);


#ifdef __cplusplus
}
#endif
