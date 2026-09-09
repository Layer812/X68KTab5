#include "tab5_lp_broker.h"
#include <stddef.h>
#include <stdbool.h>


#include "esp_log.h"
#include "esp_timer.h"
#include "ulp_lp_core.h"
#include "ulp_px68k_lpbr.h"

#define LPBR_READY_MAGIC 0x4C504236u /* "LPB6" */
#define LPBR_COLLECT_MAGIC 0x4C504334u /* "LPC4" */
#define LPBR_COLLECT_CMD_ARM 1u
#define LPBR_COLLECT_STATE_CAPTURED 2u
#define LPBR_COLLECT_SELFTEST_CYCLES 800000u /* short pre-guest calibration window */
static const char *TAG = "TAB5_LPBR";
static volatile uint32_t s_active;
static volatile uint32_t s_touch_control_generation;
static uint16_t s_touch_last_pub_value;
static uint8_t s_touch_last_pub_valid;
static uint32_t s_touch_pub_seen_control_generation;
static uint32_t s_touch_pub_epoch;
static uint32_t s_touch_take_serial;
static uint32_t s_touch_control_epoch;
static uint32_t s_collect_cmd_seq;
static uint32_t s_collect_take_seq;
static uint32_t s_collect_hz;
static uint32_t s_collect_ok;

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
    s_collect_cmd_seq = 0u;
    s_collect_take_seq = 0u;
    s_collect_hz = 0u;
    s_collect_ok = 0u;

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

    /* R140P4S4 LP multi-window collector self-test.  This runs before the guest
     * task exists, so it cannot perturb emulation/audio timing.  LP owns the
     * complete measurement window and publishes result_seq only after the
     * result struct is stable. */
    const uint32_t collect_seq = ++s_collect_cmd_seq;
    ulp_lpbr_collect_window_cycles = LPBR_COLLECT_SELFTEST_CYCLES;
    ulp_lpbr_collect_cmd = LPBR_COLLECT_CMD_ARM;
    fence_rw();
    const int64_t collect_hp_t0 = esp_timer_get_time();
    ulp_lpbr_collect_cmd_seq = collect_seq;
    fence_rw();

    deadline = collect_hp_t0 + 200000;
    while (ulp_lpbr_collect_result_seq != collect_seq &&
           esp_timer_get_time() < deadline) {
    }
    const int64_t collect_hp_us = esp_timer_get_time() - collect_hp_t0;
    fence_rw();
    if (ulp_lpbr_collect_result_seq == collect_seq &&
        ulp_lpbr_collect_state == LPBR_COLLECT_STATE_CAPTURED &&
        ulp_lpbr_collect_result_magic == LPBR_COLLECT_MAGIC &&
        ulp_lpbr_collect_elapsed_cycles >= LPBR_COLLECT_SELFTEST_CYCLES &&
        ulp_lpbr_collect_lp_loops != 0u && collect_hp_us > 0) {
        const uint64_t hz=((uint64_t)ulp_lpbr_collect_elapsed_cycles*1000000ULL + (uint64_t)collect_hp_us/2ULL)/(uint64_t)collect_hp_us;
        if(hz>=10000000ULL && hz<=100000000ULL){s_collect_hz=(uint32_t)hz;s_collect_ok=1u;}
    }
    s_collect_take_seq = collect_seq;
    if (s_collect_ok) {
        ESP_LOGI(TAG,
                 "LPFAB_R140P4S4 COLLECTOR SELFTEST PASS seq=%lu hp=%lldus lpCycles=%lu lpHz=%lu loops=%lu",
                 (unsigned long)collect_seq,(long long)collect_hp_us,
                 (unsigned long)ulp_lpbr_collect_elapsed_cycles,(unsigned long)s_collect_hz,
                 (unsigned long)ulp_lpbr_collect_lp_loops);
    } else {
        /* Diagnostic collector failure must never disable the proven LPFAB
         * touch path.  Runtime audit automatically uses HP wall-clock fallback. */
        ESP_LOGW(TAG,
                 "LPFAB_R140P4S4 COLLECTOR SELFTEST FAIL -> HPFALLBACK seq=%lu/%lu state=%lu magic=%08lX hp=%lldus lpCycles=%lu loops=%lu",
                 (unsigned long)collect_seq,(unsigned long)ulp_lpbr_collect_result_seq,
                 (unsigned long)ulp_lpbr_collect_state,(unsigned long)ulp_lpbr_collect_result_magic,
                 (long long)collect_hp_us,(unsigned long)ulp_lpbr_collect_elapsed_cycles,
                 (unsigned long)ulp_lpbr_collect_lp_loops);
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



int tab5_lp_broker_collector_arm_us(uint32_t window_us, uint32_t *seq_out)
{
    if (__builtin_expect(!s_active, 0) || !s_collect_ok || window_us == 0u) return 0;
    uint64_t cycles=((uint64_t)s_collect_hz*(uint64_t)window_us+500000ULL)/1000000ULL;
    if(cycles==0u || cycles>0x7fffffffu)return 0;
    const uint32_t seq = ++s_collect_cmd_seq;
    ulp_lpbr_collect_window_cycles = (uint32_t)cycles;
    ulp_lpbr_collect_cmd = LPBR_COLLECT_CMD_ARM;
    fence_rw();
    ulp_lpbr_collect_cmd_seq = seq;
    fence_rw();
    if (seq_out) *seq_out = seq;
    return 1;
}

uint32_t tab5_lp_broker_collector_hz(void)
{
    return s_collect_ok ? s_collect_hz : 0u;
}

int tab5_lp_broker_collector_take(tab5_lp_broker_collector_result_t *out)
{
    if (!out || __builtin_expect(!s_active, 0)) return 0;
    const uint32_t seq = ulp_lpbr_collect_result_seq;
    fence_rw();
    if (seq == 0u || seq == s_collect_take_seq) return 0;
    out->result_seq = seq;
    out->state = ulp_lpbr_collect_state;
    out->window_cycles = ulp_lpbr_collect_window_cycles;
    out->start_cycle = ulp_lpbr_collect_start_cycle;
    out->end_cycle = ulp_lpbr_collect_end_cycle;
    out->elapsed_cycles = ulp_lpbr_collect_elapsed_cycles;
    out->lp_loops = ulp_lpbr_collect_lp_loops;
    out->result_magic = ulp_lpbr_collect_result_magic;
    fence_rw();
    s_collect_take_seq = seq;
    return 1;
}
