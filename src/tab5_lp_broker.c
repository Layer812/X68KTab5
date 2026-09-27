#include "tab5_lp_broker.h"
#include <stddef.h>
#include <stdbool.h>


#include "esp_log.h"
#include "esp_timer.h"
#include "ulp_lp_core.h"
#include "ulp_px68k_lpbr.h"

#define LPBR_READY_MAGIC 0x4C504236u /* "LPB6" */
static const char *TAG = "TAB5_LPBR";
static volatile uint32_t s_active;
static volatile uint32_t s_touch_control_generation;
static uint16_t s_touch_last_pub_value;
static uint8_t s_touch_last_pub_valid;
static uint32_t s_touch_pub_seen_control_generation;
static uint32_t s_touch_pub_epoch;
static uint32_t s_touch_take_serial;
static uint32_t s_touch_control_epoch;

extern const uint8_t bin_start[] asm("_binary_ulp_px68k_lpbr_bin_start");
extern const uint8_t bin_end[] asm("_binary_ulp_px68k_lpbr_bin_end");

static inline void fence_rw(void)
{
    __asm__ __volatile__("fence rw,rw" ::: "memory");
}

int tab5_lp_broker_init(void)
{
    s_active = 0u;
    __atomic_store_n(&s_touch_control_generation, 0u, __ATOMIC_RELAXED);
    s_touch_last_pub_value = 0u;
    s_touch_last_pub_valid = 0u;
    s_touch_pub_seen_control_generation = 0u;
    s_touch_pub_epoch = 0u;
    s_touch_take_serial = 0u;
    s_touch_control_epoch = 0u;

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
             "LPFAB_R139A6A3 ACTIVE bytes=%u READY+probe/echo PASS; TouchJoy=PRODUCTION LATEST; R57 mirror/hash + DIRTY observer RETIRED",
             (unsigned)n);
    return 1;
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
        return 1;
    }
    s_touch_last_pub_value = joy_bits;
    s_touch_last_pub_valid = 1u;

    const uint32_t epoch = ++s_touch_pub_epoch;
    ulp_lpbr_touch_in_value = (uint32_t)joy_bits;
    fence_rw();
    ulp_lpbr_touch_in_epoch = epoch;
    fence_rw();
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
    __atomic_add_fetch(&s_touch_control_generation, 1u, __ATOMIC_RELEASE);
    return 1;
}
