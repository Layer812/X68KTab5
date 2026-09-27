#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Production LP contract: TouchJoy latest-value/control transport only. */
int tab5_lp_broker_init(void);
int tab5_lp_broker_touch_latest_publish(uint16_t joy_bits);
int tab5_lp_broker_touch_latest_take(uint16_t *joy_bits);
int tab5_lp_broker_touch_control_publish(uint16_t joy_bits);

#ifdef __cplusplus
}
#endif
