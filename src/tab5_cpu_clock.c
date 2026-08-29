/*
 * X68K Tab / ESP32-P4 host CPU clock profiles.
 * R57E69: PlatformIO-selectable 360 MHz Standard / 400 MHz Turbo build.
 *
 * The ESP32-P4 on the current Tab5 test unit is rev1.3 and the validated
 * ESP-IDF 5.5.4 sdkconfig boots it at 360 MHz.  The 400 MHz profile is
 * deliberately experimental and uses the IDF RTC-clock HAL at app start.
 * If IDF refuses the conversion or the measured clock is not ~400 MHz, the
 * code returns to the validated 360 MHz profile and the emulator continues.
 *
 * X68000 guest timing, PSRAM, flash, DSI and LP clocks are not changed here.
 */
#include "tab5_cpu_clock.h"

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifndef PX68K_TAB5_P4_TURBO400
#define PX68K_TAB5_P4_TURBO400 0
#endif

#ifndef PX68K_TAB5_HOST_CPU_MHZ
#define PX68K_TAB5_HOST_CPU_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#endif

#if PX68K_TAB5_P4_TURBO400
#include "esp_private/rtc_clk.h"
#endif

static const char *TAG = "TAB5_P4CLK";
static volatile uint32_t s_host_cpu_hz =
    (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;

/* Measure CPU-cycle counter slope against esp_timer, which stays in wall-time.
 * Busy-measure only at boot; never on the emulator hot path. */
static uint32_t measure_cpu_hz(void)
{
    const int64_t t0 = esp_timer_get_time();
    const uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
    int64_t now;
    do {
        now = esp_timer_get_time();
    } while ((now - t0) < 4000);
    const uint32_t c1 = (uint32_t)esp_cpu_get_cycle_count();
    const uint32_t dc = c1 - c0;
    const uint32_t us = (uint32_t)(now - t0);
    if (!us) return 0u;
    return (uint32_t)(((uint64_t)dc * 1000000ull + us / 2u) / us);
}

static uint32_t normalize_measured_hz(uint32_t hz)
{
    /* Cycle/timer sampling has a small amount of call/interrupt noise. Snap
     * only values already close to our two intended profiles. */
    if (hz >= 350000000u && hz <= 370000000u) return 360000000u;
    if (hz >= 390000000u && hz <= 410000000u) return 400000000u;
    return hz;
}

uint32_t tab5_host_cpu_hz_runtime(void)
{
    return s_host_cpu_hz;
}

uint32_t tab5_cpu_clock_apply_profile(void)
{
    uint32_t before = normalize_measured_hz(measure_cpu_hz());
    if (before) s_host_cpu_hz = before;

#if PX68K_TAB5_P4_TURBO400
    const uint32_t target_mhz = (uint32_t)PX68K_TAB5_HOST_CPU_MHZ;
    rtc_cpu_freq_config_t cfg;

    if (target_mhz != 400u) {
        ESP_LOGE(TAG, "R57E69 TURBO build has invalid target=%lu MHz; keeping measured %lu MHz",
                 (unsigned long)target_mhz,
                 (unsigned long)(s_host_cpu_hz / 1000000u));
        return s_host_cpu_hz;
    }

    if (!rtc_clk_cpu_freq_mhz_to_config(target_mhz, &cfg)) {
        ESP_LOGE(TAG,
                 "R57E69 TURBO400: IDF clock HAL rejected 400 MHz; SAFE FALLBACK=%lu MHz",
                 (unsigned long)(s_host_cpu_hz / 1000000u));
        return s_host_cpu_hz;
    }

    /* Full setter rather than the fast PM transition helper: this is a single
     * boot-time profile change, before Tab5 host peripherals/workers start. */
    rtc_clk_cpu_freq_set_config(&cfg);

    uint32_t after = normalize_measured_hz(measure_cpu_hz());
    if (after >= 390000000u && after <= 410000000u) {
        s_host_cpu_hz = 400000000u;
        ESP_LOGW(TAG,
                 "R57E69 P4 TURBO400 ACTIVE: boot=%lu MHz -> HP cores=400 MHz; guest=10 MHz; PSRAM/Flash unchanged; EXPERIMENTAL",
                 (unsigned long)(before / 1000000u));
        return s_host_cpu_hz;
    }

    ESP_LOGE(TAG,
             "R57E69 TURBO400 verify FAILED: measured=%lu Hz; restoring validated 360 MHz",
             (unsigned long)after);

    rtc_cpu_freq_config_t safe;
    if (rtc_clk_cpu_freq_mhz_to_config(360u, &safe)) {
        rtc_clk_cpu_freq_set_config(&safe);
    }
    after = normalize_measured_hz(measure_cpu_hz());
    s_host_cpu_hz = after ? after : 360000000u;
    ESP_LOGW(TAG, "R57E69 P4 clock SAFE FALLBACK active: measured=%lu MHz",
             (unsigned long)(s_host_cpu_hz / 1000000u));
    return s_host_cpu_hz;
#else
    ESP_LOGI(TAG,
             "R57E69 P4 STANDARD: HP cores=%lu MHz measured; guest=10 MHz; validated 360 MHz profile",
             (unsigned long)(s_host_cpu_hz / 1000000u));
    return s_host_cpu_hz;
#endif
}
