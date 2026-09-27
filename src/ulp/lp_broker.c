#include <stdint.h>

#define LPBR_READY_MAGIC 0x4C504236u /* "LPB6" */

/* Production LP core: TouchJoy latest-value/control transport only. */
volatile uint32_t lpbr_ready;
volatile uint32_t lpbr_hp_probe, lpbr_lp_echo;
volatile uint32_t lpbr_touch_in_epoch, lpbr_touch_in_value;
volatile uint32_t lpbr_touch_ctl_epoch, lpbr_touch_ctl_value;
volatile uint32_t lpbr_touch_out_serial, lpbr_touch_out_value;

static inline void fence_rw(void)
{
    __asm__ __volatile__("fence rw,rw" ::: "memory");
}

int main(void)
{
    lpbr_ready = 0u;
    lpbr_hp_probe = lpbr_lp_echo = 0u;
    lpbr_touch_in_epoch = lpbr_touch_in_value = 0u;
    lpbr_touch_ctl_epoch = lpbr_touch_ctl_value = 0u;
    lpbr_touch_out_serial = lpbr_touch_out_value = 0u;

    uint32_t touch_seen_epoch = 0u;
    uint32_t touch_ctl_seen_epoch = 0u;

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
