#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R57E69: Apply the PlatformIO-selected HP-core clock profile once, before
 * host/video/audio initialization. Returns the measured CPU clock in Hz. */
uint32_t tab5_cpu_clock_apply_profile(void);

/* Runtime value used by the guest phase-reservoir governor.  Keeping this
 * dynamic is important: if the experimental 400 MHz request is rejected,
 * the guest pacer must continue using the measured 360 MHz host clock. */
uint32_t tab5_host_cpu_hz_runtime(void);

#ifdef __cplusplus
}
#endif
