/*
 * Tab5 port-specific implementation.
 * Intent: M5Stack Tab5 integration: keep the X68000 guest timeline on CPU1 and host services on CPU0, while coordinating boot media, HostFS, video, audio, and USB lifecycle.
 * Layer8 Aug/17/2026
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "libretro.h"
#include "libretro/state.h"
#include "m68000/m68000.h"
#include "m68000/musashi/m68k.h"
#include "libretro/keyboard.h"
#include "x68k/mfp.h"
#include "x68k/dmac.h"
#include "x68k/crtc.h"
#include "x68k/bg.h"
#include "x68k/gvram.h"
#include "x68k/tvram.h"
#include "x68k/palette.h"
#include "x68k/scc.h"
#include "x68k/hostfs.h"
#include "fmgen/fmg_wrap.h"

#include "tab5_video.h"
#include "tab5_branding.h"
#include "tab5_launcher.h"
#include "tab5_panic.h"
#include "tab5_sd.h"
#include "tab5_disk_control.h"
#include "tab5_textview.h"
#include "tab5_guest_input.h"
#include "tab5_usb_keyboard.h"
#include "tab5_audio.h"
#include "tab5_compose.h"
#include "tab5_screen_manager.h"
#include "tab5_guest_bus.h"
#include "tab5_lp_broker.h"
#include "tab5_dynarec_arena.h"
#include "tab5_guest_video_state.h"
#include "tab5_video_cpu1.h"

static const char *TAG = "PX68K_TAB5";

/* P12R1: absolute 44.1-kHz guest timeline is also the product real-time
 * authority.  CPU1 paces only against the local HP timer and never waits for
 * CPU0, LCD, audio consumer or another host task. */
extern void DSound_FlushPending(void);
extern uint64_t DSound_AbsGuestTick64(void);



/* R57E95: M5Stack Tab5 Keyboard A164 ExtPort1 bridge.
 *
 * Protocol/key layout follows M5Stack's official A164 implementation:
 *   - Default/production mode: Normal (REG_MODE_KEYBOARD=0)
 *   - Normal event register: REG_KEY_EVENT=0x20
 *   - Event byte: bit7=pressed, bits6:4=row(0..4), bits3:0=col(0..13)
 *   - Queue-empty sentinel: 0xFF
 *   - INT_CFG bit0 enables Normal-mode interrupt
 *   - Matrix→HID mappings (base/Sym) are derived from the official
 *     M5Unit-KEYBOARD UnitTab5Keyboard MIT implementation.
 *
 * The dedicated AUTO-selected ESP-IDF I2C controller on GPIO0/GPIO1 from
 * R57E92B is retained because it is proven on this Tab5/M5Unified build.
 * All A164 transactions remain on CPU0; CPU1 only consumes atomic hotkeys. */
#define TAB5KBD_ADDR            0x6Du
#define TAB5KBD_REG_INT_CFG     0x00u
#define TAB5KBD_REG_INT_STAT    0x01u
#define TAB5KBD_REG_EVENT_NUM   0x02u
#define TAB5KBD_REG_BRIGHTNESS  0x03u
#define TAB5KBD_REG_MODE        0x10u
#define TAB5KBD_REG_KEY_EVENT   0x20u
#define TAB5KBD_REG_FW_VERSION  0xFEu
#define TAB5KBD_MODE_NORMAL     0u
#define TAB5KBD_EVENT_EMPTY     0xFFu
#define TAB5KBD_ROWS            5u
#define TAB5KBD_COLS            14u
#define TAB5KBD_KEYS            70u
#define TAB5KBD_I2C_PORT_AUTO   ((i2c_port_num_t)-1)
#define TAB5KBD_I2C_HZ          400000u
#define TAB5KBD_SDA_GPIO        GPIO_NUM_0
#define TAB5KBD_SCL_GPIO        GPIO_NUM_1
#define TAB5KBD_INT_GPIO        GPIO_NUM_50

#define TAB5KBD_KIDX_SYM        (3u * TAB5KBD_COLS + 0u)
#define TAB5KBD_KIDX_AA         (3u * TAB5KBD_COLS + 1u)
#define TAB5KBD_KIDX_CTRL       (4u * TAB5KBD_COLS + 0u)
#define TAB5KBD_KIDX_ALT        (4u * TAB5KBD_COLS + 1u)

typedef struct {
    uint8_t scan;
    uint8_t modifier;
} tab5kbd_map_t;

/* R57E105P: A164 official 5x14 key-top/Sym semantics -> native X68000 scan map.
 * IMPORTANT: the A164 firmware's HID table is PC/US-semantic.  For example
 * A164 Sym+';' denotes ':' and is represented in HID as SHIFT+SEMICOLON.
 * Carrying that HID chord literally into an X68000 is WRONG: X68000 0x27 is
 * ';' / '+' while ':' is the dedicated 0x28 key.  Therefore we first resolve
 * the A164 physical key+Sym layer to the intended character, then emit the
 * X68000-native scan/SHIFT combination for that character.
 * modifier bit 0x02 means hold X68000 SHIFT while this physical key is down.
 * R102P acquisition/priority/timing/RTQ code is intentionally untouched. */
static const tab5kbd_map_t s_tab5kbd_map_base[TAB5KBD_KEYS] = {
    /* row0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del */
    {0x01,0x00},{0x02,0x00},{0x03,0x00},{0x04,0x00},{0x05,0x00},{0x06,0x00},{0x07,0x00},
    {0x08,0x00},{0x09,0x00},{0x0A,0x00},{0x0B,0x00},{0x0C,0x00},{0x27,0x02},{0x37,0x00},
    /* row1: ` ! @ # $ % ^ & * ( ) [ ] \\ */
    {0x1B,0x02},{0x02,0x02},{0x1B,0x00},{0x04,0x02},{0x05,0x02},{0x06,0x02},{0x0D,0x00},
    {0x07,0x02},{0x28,0x02},{0x09,0x02},{0x0A,0x02},{0x1C,0x00},{0x29,0x00},{0x0E,0x00},
    /* row2: Tab q w e r t y u i o p ; ' Backspace */
    {0x10,0x00},{0x11,0x00},{0x12,0x00},{0x13,0x00},{0x14,0x00},{0x15,0x00},{0x16,0x00},
    {0x17,0x00},{0x18,0x00},{0x19,0x00},{0x1A,0x00},{0x27,0x00},{0x08,0x02},{0x0F,0x00},
    /* row3: Sym Aa a s d f g h j k l Up _ Enter */
    {0x00,0x00},{0x00,0x00},{0x1E,0x00},{0x1F,0x00},{0x20,0x00},{0x21,0x00},{0x22,0x00},
    {0x23,0x00},{0x24,0x00},{0x25,0x00},{0x26,0x00},{0x3C,0x00},{0x34,0x02},{0x1D,0x00},
    /* row4: Ctrl Alt z x c v b n m . Left Down Right Space */
    {0x00,0x00},{0x00,0x00},{0x2A,0x00},{0x2B,0x00},{0x2C,0x00},{0x2D,0x00},{0x2E,0x00},
    {0x2F,0x00},{0x30,0x00},{0x32,0x00},{0x3B,0x00},{0x3E,0x00},{0x3D,0x00},{0x35,0x00},
};

static const tab5kbd_map_t s_tab5kbd_map_sym[TAB5KBD_KEYS] = {
    /* row0: identical physical legends */
    {0x01,0x00},{0x02,0x00},{0x03,0x00},{0x04,0x00},{0x05,0x00},{0x06,0x00},{0x07,0x00},
    {0x08,0x00},{0x09,0x00},{0x0A,0x00},{0x0B,0x00},{0x0C,0x00},{0x27,0x02},{0x37,0x00},
    /* row1: `->~, !->?, @, #, $, %, ^, &, *->/, (-><, )->>, [->{, ]->}, \->| */
    {0x0D,0x02},{0x33,0x02},{0x1B,0x00},{0x04,0x02},{0x05,0x02},{0x06,0x02},{0x0D,0x00},
    {0x07,0x02},{0x33,0x00},{0x31,0x02},{0x32,0x02},{0x1C,0x02},{0x29,0x02},{0x0E,0x02},
    /* row2: ;->:, '->" */
    {0x10,0x00},{0x11,0x00},{0x12,0x00},{0x13,0x00},{0x14,0x00},{0x15,0x00},{0x16,0x00},
    {0x17,0x00},{0x18,0x00},{0x19,0x00},{0x1A,0x00},{0x28,0x00},{0x03,0x02},{0x0F,0x00},
    /* row3: _->= */
    {0x00,0x00},{0x00,0x00},{0x1E,0x00},{0x1F,0x00},{0x20,0x00},{0x21,0x00},{0x22,0x00},
    {0x23,0x00},{0x24,0x00},{0x25,0x00},{0x26,0x00},{0x3C,0x00},{0x0C,0x02},{0x1D,0x00},
    /* row4: .->, */
    {0x00,0x00},{0x00,0x00},{0x2A,0x00},{0x2B,0x00},{0x2C,0x00},{0x2D,0x00},{0x2E,0x00},
    {0x2F,0x00},{0x30,0x00},{0x31,0x00},{0x3B,0x00},{0x3E,0x00},{0x3D,0x00},{0x35,0x00},
};

/* R57E105P boot-time map certificate for the nine punctuation cases reported
 * on real hardware.  This is diagnostic only; it does not touch A164 I2C,
 * task priority, 10-ms service cadence, KEY_EVENT acquisition, or RTQ flow. */
typedef struct {
    uint8_t idx;
    bool sym;
    uint8_t scan;
    uint8_t modifier;
    const char *name;
} tab5kbd_r105p_expect_t;

static void tab5kbd_r105p_map_selfcheck(void)
{
    static const tab5kbd_r105p_expect_t expect[] = {
        {(uint8_t)(1u * TAB5KBD_COLS + 0u), false, 0x1B, 0x02, "`"},
        {(uint8_t)(1u * TAB5KBD_COLS + 0u), true,  0x0D, 0x02, "~"},
        {(uint8_t)(1u * TAB5KBD_COLS + 1u), true,  0x33, 0x02, "?"},
        {(uint8_t)(2u * TAB5KBD_COLS + 11u),true,  0x28, 0x00, ":"},
        {(uint8_t)(2u * TAB5KBD_COLS + 12u),true,  0x03, 0x02, "\""},
        {(uint8_t)(3u * TAB5KBD_COLS + 12u),false, 0x34, 0x02, "_"},
        {(uint8_t)(3u * TAB5KBD_COLS + 12u),true,  0x0C, 0x02, "="},
        {(uint8_t)(4u * TAB5KBD_COLS + 9u), true,  0x31, 0x00, ","},
        {(uint8_t)(1u * TAB5KBD_COLS + 8u), true,  0x33, 0x00, "/"},
    };
    bool ok = true;
    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); ++i) {
        const tab5kbd_r105p_expect_t *e = &expect[i];
        const tab5kbd_map_t m = e->sym ? s_tab5kbd_map_sym[e->idx] : s_tab5kbd_map_base[e->idx];
        if (m.scan != e->scan || m.modifier != e->modifier) {
            ok = false;
            ESP_LOGE(TAG,
                     "PX68K_TAB5KBD_R57E105P_MAPFAIL char=%s idx=%u sym=%u got=%02X/%02X want=%02X/%02X",
                     e->name, (unsigned)e->idx, e->sym ? 1u : 0u,
                     (unsigned)m.scan, (unsigned)m.modifier,
                     (unsigned)e->scan, (unsigned)e->modifier);
        }
    }
    if (ok) {
        ESP_LOGI(TAG,
                 "PX68K_TAB5KBD_R57E105P_MAPCERT PASS: ` ~ ? : quote _ = , / -> X68K native scans; A164 Sym layer source-exact");
    }
}

static volatile bool s_tab5kbd_present = false;
static volatile bool s_tab5kbd_input_armed = false;
static uint8_t s_tab5kbd_fw = 0u;
static uint32_t s_tab5kbd_hotkeys = 0u;
static uint32_t s_tab5kbd_events = 0u;
static uint32_t s_tab5kbd_i2c_errors = 0u;
static uint32_t s_tab5kbd_raw_events = 0u;
static uint32_t s_tab5kbd_key_downs = 0u;
static uint32_t s_tab5kbd_key_ups = 0u;
static uint32_t s_tab5kbd_modifier_edges = 0u;
static uint32_t s_tab5kbd_repeat_reports = 0u;
static uint32_t s_tab5kbd_unmapped = 0u;
static uint32_t s_tab5kbd_invalid_events = 0u;
static uint8_t s_tab5kbd_last_raw = TAB5KBD_EVENT_EMPTY;
static uint8_t s_tab5kbd_last_row = 0u;
static uint8_t s_tab5kbd_last_col = 0u;
static bool s_tab5kbd_last_pressed = false;
static bool s_tab5kbd_pressed[TAB5KBD_KEYS];
static int16_t s_tab5kbd_active_scan[TAB5KBD_KEYS];
static bool s_tab5kbd_active_forced_shift[TAB5KBD_KEYS];
static uint8_t s_tab5kbd_forced_shift_count = 0u;
static bool s_tab5kbd_sym_down = false;
static bool s_tab5kbd_aa_down = false;
static bool s_tab5kbd_ctrl_down = false;
static bool s_tab5kbd_alt_down = false;
static bool s_tab5kbd_guest_shift_down = false;
static TaskHandle_t s_tab5kbd_task = NULL;
static bool s_tab5kbd_i2c_ready = false;
static i2c_master_bus_handle_t s_tab5kbd_bus = NULL;
static i2c_master_dev_handle_t s_tab5kbd_dev = NULL;
static esp_err_t s_tab5kbd_last_err = ESP_OK;
static bool s_tab5kbd_missing_logged = false;
/* R57E97T: mirror M5Stack's current UnitTab5Keyboard lifecycle exactly:
 * mode -> clear INT -> clear queue -> configure NEGEDGE IRQ -> enable the
 * Normal-mode interrupt.  Runtime draining is EVENT_NUM driven, just like
 * the official library.  A bounded health audit remains so a silent device
 * is distinguishable from a PX68K key-mapping problem. */
static volatile bool s_tab5kbd_irq_pending = false;
static volatile uint32_t s_tab5kbd_irq_edges = 0u;
static bool s_tab5kbd_irq_installed = false;
/* R57E99K: official UnitTab5Keyboard readiness semantics.  The current
 * M5Stack library drains when an IRQ is pending OR the active-low INT pin
 * is still low.  Keep an unconditional 50-ms safety poll as well so shared
 * ISR-service/edge-delivery quirks cannot make the keyboard silently dead. */
/* R57E100K: diagnostic-only cross-core breadcrumb. CPU0 publishes the latest
 * A164->X68K enqueue attempt; CPU1 samples it immediately after guest_input_tick().
 * No input timing or queue semantics are changed. */
static volatile uint32_t s_tab5kbd_pipe_seq_pub = 0u;
static volatile uint8_t s_tab5kbd_pipe_scan_pub = 0u;
static volatile uint8_t s_tab5kbd_pipe_down_pub = 0u;
static volatile uint8_t s_tab5kbd_pipe_enq_pub = 0u;
static uint8_t s_tab5kbd_last_mode_reg = 0xffu;
static uint8_t s_tab5kbd_last_intcfg_reg = 0xffu;
static uint8_t s_tab5kbd_last_intstat_reg = 0xffu;

static void IRAM_ATTR tab5kbd_irq_handler(void *arg)
{
    (void)arg;
    s_tab5kbd_irq_pending = true;
    ++s_tab5kbd_irq_edges;
}

extern void tab5_video_set_tab5_keyboard_orientation(int enabled);
extern int tab5_video_turbo_enabled(void);
extern uint32_t tab5_video_turbo_mode(void);
extern uint32_t tab5_video_turbo_period_us_r128(void);
extern uint32_t tab5_video_turbo_fps_r128(void);
extern void tab5_video_set_turbo_fps_r128(uint32_t fps);
/* R57E97T V2: shared 30 Hz wall-clock phase for the emulation task. */
/* PX68K_AV_R57E118: End-to-end HOST visual cadence.
 * Guest CRTC/DMA/MFP/audio clocks and guest memory writes remain full-rate.
 * One wall decision gates expensive host construction and the matching present. */
#define R118_NA_VISUAL_PERIOD_US     16000LL /* P12R1: 62.5Hz ceiling, not a 15fps cap */
static int64_t s_r118_next_visual_us = 0;
static int s_p12r6_last_turbo = -1;
/* RED uses half the GREEN source-frame count for the same wall time. */
static inline uint32_t p12r6a4_policy_frames(uint32_t frames, uint32_t mode)
{
    if (mode < 2u)
        return frames;
    return frames > (UINT32_MAX / 2u) ? UINT32_MAX : frames * 2u;
}
/* P12R6A4: NORMAL retains the P12R5 16-ms ceiling. Both Turbo levels reuse
 * the proven R118 host-window gate with a production maximum of 24fps.
 * R128 host-side cadence selection, so expensive host construction and the
 * physical presenter share one 15/20/24-fps budget. */
static bool r118_host_visual_window_open(int turbo)
{
    const int t = turbo ? 1 : 0;
    const int64_t now = esp_timer_get_time();
    if (s_p12r6_last_turbo != t) {
        s_p12r6_last_turbo = t;
        s_r118_next_visual_us = 0;
    }
    return (s_r118_next_visual_us == 0 || now >= s_r118_next_visual_us);
}

static void r118_host_visual_commit_now(int turbo)
{
    const int t = turbo ? 1 : 0;
    const int64_t period = t ? (int64_t)tab5_video_turbo_period_us_r128()
                             : R118_NA_VISUAL_PERIOD_US;
    s_p12r6_last_turbo = t;
    s_r118_next_visual_us = esp_timer_get_time() + period;
}

static bool tab5kbd_i2c_init(void)
{
    if (s_tab5kbd_i2c_ready && s_tab5kbd_bus && s_tab5kbd_dev)
        return true;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TAB5KBD_I2C_PORT_AUTO,
        .sda_io_num = TAB5KBD_SDA_GPIO,
        .scl_io_num = TAB5KBD_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        },
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        s_tab5kbd_last_err = e;
        return false;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TAB5KBD_ADDR,
        .scl_speed_hz = TAB5KBD_I2C_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    i2c_master_dev_handle_t dev = NULL;
    e = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (e != ESP_OK) {
        (void)i2c_del_master_bus(bus);
        s_tab5kbd_last_err = e;
        return false;
    }

    s_tab5kbd_bus = bus;
    s_tab5kbd_dev = dev;
    s_tab5kbd_i2c_ready = true;
    s_tab5kbd_last_err = ESP_OK;


    ESP_LOGI(TAG,
             "PX68K_TAB5KBD_R57E98T: dedicated AUTO I2C ready sda=0 scl=1 addr=0x6D; IRQ setup deferred until official lifecycle stage");
    return true;
}

static bool tab5kbd_configure_irq_pin(void)
{
    gpio_config_t irq = {
        .pin_bit_mask = (1ULL << TAB5KBD_INT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    const esp_err_t irq_cfg = gpio_config(&irq);
    esp_err_t irq_svc = gpio_install_isr_service(0);
    if (irq_svc == ESP_ERR_INVALID_STATE) irq_svc = ESP_OK;
    esp_err_t irq_add = ESP_FAIL;
    if (irq_cfg == ESP_OK && irq_svc == ESP_OK)
        irq_add = gpio_isr_handler_add(TAB5KBD_INT_GPIO, tab5kbd_irq_handler, NULL);
    s_tab5kbd_irq_installed = (irq_add == ESP_OK);
    if (!s_tab5kbd_irq_installed)
        (void)gpio_set_intr_type(TAB5KBD_INT_GPIO, GPIO_INTR_DISABLE);
    return s_tab5kbd_irq_installed;
}

static bool tab5kbd_write_reg(uint8_t reg, uint8_t value)
{
    if (!tab5kbd_i2c_init()) return false;
    const uint8_t msg[2] = { reg, value };
    const esp_err_t e = i2c_master_transmit(s_tab5kbd_dev, msg, sizeof(msg), 20);
    if (e != ESP_OK) {
        s_tab5kbd_last_err = e;
        ++s_tab5kbd_i2c_errors;
        return false;
    }
    return true;
}

static bool tab5kbd_read_reg(uint8_t reg, uint8_t *dst, size_t n)
{
    if (!dst || !n || !tab5kbd_i2c_init()) return false;
    const esp_err_t e = i2c_master_transmit_receive(s_tab5kbd_dev,
                                                     &reg, 1u,
                                                     dst, n,
                                                     20);
    if (e != ESP_OK) {
        s_tab5kbd_last_err = e;
        ++s_tab5kbd_i2c_errors;
        return false;
    }
    return true;
}

static void tab5kbd_reset_state(void)
{
    memset(s_tab5kbd_pressed, 0, sizeof(s_tab5kbd_pressed));
    memset(s_tab5kbd_active_forced_shift, 0, sizeof(s_tab5kbd_active_forced_shift));
    for (unsigned i = 0; i < TAB5KBD_KEYS; ++i)
        s_tab5kbd_active_scan[i] = -1;
    s_tab5kbd_forced_shift_count = 0u;
    s_tab5kbd_sym_down = false;
    s_tab5kbd_aa_down = false;
    s_tab5kbd_ctrl_down = false;
    s_tab5kbd_alt_down = false;
    s_tab5kbd_guest_shift_down = false;
}

static bool tab5kbd_detect_and_configure(void)
{
    if (!tab5kbd_i2c_init()) return false;

    uint8_t fw = 0u;
    esp_err_t last = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 3u; ++attempt) {
        last = i2c_master_probe(s_tab5kbd_bus, TAB5KBD_ADDR, 20);
        if (last == ESP_OK) break;
        if (last == ESP_ERR_TIMEOUT || last == ESP_ERR_INVALID_STATE)
            (void)i2c_master_bus_reset(s_tab5kbd_bus);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (last == ESP_OK) {
        for (unsigned attempt = 0; attempt < 3u; ++attempt) {
            if (tab5kbd_read_reg(TAB5KBD_REG_FW_VERSION, &fw, 1u)) {
                last = ESP_OK;
                break;
            }
            last = s_tab5kbd_last_err;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (last != ESP_OK) {
        if (!s_tab5kbd_missing_logged) {
            s_tab5kbd_missing_logged = true;
            ESP_LOGW(TAG,
                     "PX68K_TAB5KBD_R57E98T: A164 not detected addr=0x6D err=%s INT=%d; hotplug retry active",
                     esp_err_to_name(last), gpio_get_level(TAB5KBD_INT_GPIO));
        }
        return false;
    }

    /* Current M5Unit-KEYBOARD begin() lifecycle, in the same order:
     *  1) select Normal mode (mode switch flushes the old-mode queue/INT),
     *  2) clear residual INT status,
     *  3) clear the active-mode event queue,
     *  4) configure GPIO50 falling-edge IRQ,
     *  5) enable only the Normal-mode INT bit (startPeriodic equivalent).
     * Do not touch brightness/RGB here; it is unrelated to keyboard scanning. */
    uint8_t mode_before = 0xffu;
    (void)tab5kbd_read_reg(TAB5KBD_REG_MODE, &mode_before, 1u);
    if (!tab5kbd_write_reg(TAB5KBD_REG_MODE, TAB5KBD_MODE_NORMAL)) return false;
    vTaskDelay(1); /* R101K: real 10-ms tick; 2ms rounded to zero at 100Hz */
    if (!tab5kbd_write_reg(TAB5KBD_REG_INT_STAT, 0u)) return false;
    if (!tab5kbd_write_reg(TAB5KBD_REG_EVENT_NUM, 0u)) return false;

    /* gpio_install_isr_service() may already have been called by touch/USB;
     * ESP_ERR_INVALID_STATE is the official accepted outcome. */
    (void)tab5kbd_configure_irq_pin();
    s_tab5kbd_irq_pending = false;

    if (!tab5kbd_write_reg(TAB5KBD_REG_INT_CFG, 0x01u)) return false;

    uint8_t mode_verify = 0xffu, int_verify = 0xffu;
    uint8_t stat0 = 0xffu, cnt0 = 0xffu;
    if (!tab5kbd_read_reg(TAB5KBD_REG_MODE, &mode_verify, 1u) || mode_verify != TAB5KBD_MODE_NORMAL ||
        !tab5kbd_read_reg(TAB5KBD_REG_INT_CFG, &int_verify, 1u) || (int_verify & 0x01u) == 0u) {
        ESP_LOGE(TAG,
                 "PX68K_TAB5KBD_R57E98T: official lifecycle verify FAILED mode=0x%02X intcfg=0x%02X",
                 (unsigned)mode_verify, (unsigned)int_verify);
        return false;
    }
    (void)tab5kbd_read_reg(TAB5KBD_REG_INT_STAT, &stat0, 1u);
    (void)tab5kbd_read_reg(TAB5KBD_REG_EVENT_NUM, &cnt0, 1u);

    tab5kbd_reset_state();
    s_tab5kbd_fw = fw;
    s_tab5kbd_missing_logged = false;
    s_tab5kbd_present = true;
    s_tab5kbd_last_mode_reg = mode_verify;
    s_tab5kbd_last_intcfg_reg = int_verify;
    s_tab5kbd_last_intstat_reg = stat0;
    ESP_LOGI(TAG,
             "PX68K_TAB5KBD_R57E98T: A164 detected fw=0x%02X officialLifecycle mode %u->NORMAL intcfg=0x%02X IRQ50=%s INT=%d stat=0x%02X count=%u orientation=180deg",
             (unsigned)fw, (unsigned)mode_before, (unsigned)int_verify,
             s_tab5kbd_irq_installed ? "NEGEDGE" : "POLL",
             gpio_get_level(TAB5KBD_INT_GPIO), (unsigned)stat0, (unsigned)cnt0);
    return true;
}

/* R57E98T: A164 no longer goes through Core_Key_State/RETROK state merging.
 * Use the same X68000 scan codes as PX68K KeyTable and the already-proven
 * touch/software-keyboard realtime queue.  This preserves real press/release
 * edges from the A164 Normal-mode event stream. */
static int tab5kbd_retrok_to_x68k_scan(int rk)
{
    switch (rk) {
    case RETROK_ESCAPE: return 0x01;
    case RETROK_1: return 0x02; case RETROK_2: return 0x03; case RETROK_3: return 0x04;
    case RETROK_4: return 0x05; case RETROK_5: return 0x06; case RETROK_6: return 0x07;
    case RETROK_7: return 0x08; case RETROK_8: return 0x09; case RETROK_9: return 0x0a;
    case RETROK_0: return 0x0b; case RETROK_MINUS: return 0x0c; case RETROK_EQUALS: return 0x0d;
    case RETROK_BACKSLASH: return 0x0e; case RETROK_BACKSPACE: return 0x0f;
    case RETROK_TAB: return 0x10;
    case RETROK_q: return 0x11; case RETROK_w: return 0x12; case RETROK_e: return 0x13;
    case RETROK_r: return 0x14; case RETROK_t: return 0x15; case RETROK_y: return 0x16;
    case RETROK_u: return 0x17; case RETROK_i: return 0x18; case RETROK_o: return 0x19; case RETROK_p: return 0x1a;
    case RETROK_BACKQUOTE: return 0x1b; case RETROK_LEFTBRACKET: return 0x1c; case RETROK_RETURN: return 0x1d;
    case RETROK_a: return 0x1e; case RETROK_s: return 0x1f; case RETROK_d: return 0x20;
    case RETROK_f: return 0x21; case RETROK_g: return 0x22; case RETROK_h: return 0x23;
    case RETROK_j: return 0x24; case RETROK_k: return 0x25; case RETROK_l: return 0x26;
    case RETROK_SEMICOLON: return 0x27; case RETROK_QUOTE: return 0x28; case RETROK_RIGHTBRACKET: return 0x29;
    case RETROK_z: return 0x2a; case RETROK_x: return 0x2b; case RETROK_c: return 0x2c;
    case RETROK_v: return 0x2d; case RETROK_b: return 0x2e; case RETROK_n: return 0x2f; case RETROK_m: return 0x30;
    case RETROK_COMMA: return 0x31; case RETROK_PERIOD: return 0x32; case RETROK_SLASH: return 0x33;
    case RETROK_SPACE: return 0x35; case RETROK_HOME: return 0x36; case RETROK_DELETE: return 0x37;
    case RETROK_PAGEDOWN: return 0x38; case RETROK_PAGEUP: return 0x39; case RETROK_END: return 0x3a;
    case RETROK_LEFT: return 0x3b; case RETROK_UP: return 0x3c; case RETROK_RIGHT: return 0x3d; case RETROK_DOWN: return 0x3e;
    case RETROK_F1: return 0x63; case RETROK_F2: return 0x64; case RETROK_F3: return 0x65;
    case RETROK_F4: return 0x66; case RETROK_F5: return 0x67; case RETROK_F6: return 0x68;
    case RETROK_F7: return 0x69; case RETROK_F8: return 0x6a; case RETROK_F9: return 0x6b; case RETROK_F10: return 0x6c;
    case RETROK_LSHIFT: case RETROK_RSHIFT: return 0x70;
    case RETROK_LCTRL: case RETROK_RCTRL: return 0x71;
    case RETROK_LALT: return 0x72; case RETROK_RALT: return 0x73;
    default: return -1;
    }
}

static void tab5kbd_queue_x68k_scan(uint8_t scan, bool down)
{
    /* R57E106P production: only the proven RT queue write remains here.
     * R100/R105 cross-core breadcrumbs, atomics and UART trace are retired. */
    (void)tab5_guest_input_queue_x68k_scancode(scan, down ? 1 : 0);
}

static void tab5kbd_queue_x68k_retrok(int rk, bool down)
{
    const int scan = tab5kbd_retrok_to_x68k_scan(rk);
    if (scan < 0) {
        ++s_tab5kbd_unmapped;
        return;
    }
    tab5kbd_queue_x68k_scan((uint8_t)scan, down);
}

static void tab5kbd_set_guest_modifier(unsigned rk, bool *state, bool down)
{
    if (*state == down) return;
    *state = down;
    tab5kbd_queue_x68k_retrok((int)rk, down);
    ++s_tab5kbd_modifier_edges;
}

static void tab5kbd_sync_shift(void)
{
    const bool want = s_tab5kbd_aa_down || (s_tab5kbd_forced_shift_count != 0u);
    tab5kbd_set_guest_modifier(RETROK_LSHIFT, &s_tab5kbd_guest_shift_down, want);
}

static void tab5kbd_process_normal(uint8_t raw)
{
    ++s_tab5kbd_raw_events;
    s_tab5kbd_last_raw = raw;

    if (raw == TAB5KBD_EVENT_EMPTY)
        return;

    const bool pressed = (raw & 0x80u) != 0u;
    const uint8_t row = (uint8_t)((raw >> 4u) & 0x07u);
    const uint8_t col = (uint8_t)(raw & 0x0fu);
    s_tab5kbd_last_row = row;
    s_tab5kbd_last_col = col;
    s_tab5kbd_last_pressed = pressed;

    if (row >= TAB5KBD_ROWS || col >= TAB5KBD_COLS) {
        ++s_tab5kbd_invalid_events;
        return;
    }

    const uint8_t idx = (uint8_t)(row * TAB5KBD_COLS + col);

    /* Official modifier positions in the 5x14 Normal-mode matrix. */
    if (idx == TAB5KBD_KIDX_SYM) {
        if (s_tab5kbd_pressed[idx] == pressed) { ++s_tab5kbd_repeat_reports; return; }
        s_tab5kbd_pressed[idx] = pressed;
        s_tab5kbd_sym_down = pressed;
        ++s_tab5kbd_events;
        return;
    }
    if (idx == TAB5KBD_KIDX_AA) {
        if (s_tab5kbd_pressed[idx] == pressed) { ++s_tab5kbd_repeat_reports; return; }
        s_tab5kbd_pressed[idx] = pressed;
        s_tab5kbd_aa_down = pressed;
        tab5kbd_sync_shift();
        ++s_tab5kbd_events;
        return;
    }
    if (idx == TAB5KBD_KIDX_CTRL) {
        if (s_tab5kbd_pressed[idx] == pressed) { ++s_tab5kbd_repeat_reports; return; }
        s_tab5kbd_pressed[idx] = pressed;
        tab5kbd_set_guest_modifier(RETROK_LCTRL, &s_tab5kbd_ctrl_down, pressed);
        ++s_tab5kbd_events;
        return;
    }
    if (idx == TAB5KBD_KIDX_ALT) {
        if (s_tab5kbd_pressed[idx] == pressed) { ++s_tab5kbd_repeat_reports; return; }
        s_tab5kbd_pressed[idx] = pressed;
        tab5kbd_set_guest_modifier(RETROK_LALT, &s_tab5kbd_alt_down, pressed);
        ++s_tab5kbd_events;
        return;
    }

    if (pressed) {
        if (s_tab5kbd_pressed[idx]) {
            ++s_tab5kbd_repeat_reports;
            return;
        }
        s_tab5kbd_pressed[idx] = true;
        const tab5kbd_map_t m = s_tab5kbd_sym_down ? s_tab5kbd_map_sym[idx] : s_tab5kbd_map_base[idx];
        if (m.scan == 0u) {
            s_tab5kbd_active_scan[idx] = -1;
            ++s_tab5kbd_unmapped;
            ++s_tab5kbd_events;
            return;
        }

        const bool forced_shift = (m.modifier & 0x02u) != 0u;
        s_tab5kbd_active_scan[idx] = (int16_t)m.scan;
        s_tab5kbd_active_forced_shift[idx] = forced_shift;
        if (forced_shift && s_tab5kbd_forced_shift_count != 0xffu)
            ++s_tab5kbd_forced_shift_count;
        tab5kbd_sync_shift();
        tab5kbd_queue_x68k_scan(m.scan, true);
        ++s_tab5kbd_key_downs;
        ++s_tab5kbd_events;
        return;
    }

    /* Release uses the exact mapping captured at press time.  This matters if
     * Sym/Aa changed while another key was held, and it also supports genuine
     * multi-key operation without a single-active-key shortcut. */
    if (!s_tab5kbd_pressed[idx]) {
        ++s_tab5kbd_repeat_reports;
        return;
    }
    s_tab5kbd_pressed[idx] = false;
    const int scan = s_tab5kbd_active_scan[idx];
    if (scan >= 0) {
        tab5kbd_queue_x68k_scan((uint8_t)scan, false);
        ++s_tab5kbd_key_ups;
    }
    if (s_tab5kbd_active_forced_shift[idx]) {
        s_tab5kbd_active_forced_shift[idx] = false;
        if (s_tab5kbd_forced_shift_count != 0u)
            --s_tab5kbd_forced_shift_count;
    }
    s_tab5kbd_active_scan[idx] = -1;
    tab5kbd_sync_shift();
    ++s_tab5kbd_events;
}

static void tab5kbd_drain_cpu0(void)
{
    if (!s_tab5kbd_present || !s_tab5kbd_input_armed) return;

    /* Official M5Unit path: EVENT_NUM owns the drain loop.  KEY_EVENT is read
     * only while the device reports queued events; 0xFF remains a defensive
     * empty sentinel, not a polling mechanism. */
    unsigned drained = 0u;
    while (drained < 32u) {
        uint8_t count = 0u;
        if (!tab5kbd_read_reg(TAB5KBD_REG_EVENT_NUM, &count, 1u)) break;
        if (count == 0u) break;

        uint8_t raw = TAB5KBD_EVENT_EMPTY;
        if (!tab5kbd_read_reg(TAB5KBD_REG_KEY_EVENT, &raw, 1u)) break;
        if (raw == TAB5KBD_EVENT_EMPTY) {
            break;
        }
        ++drained;
        tab5kbd_process_normal(raw);
    }
    (void)tab5kbd_write_reg(TAB5KBD_REG_INT_STAT, 0u);
}

/* R57E101K: decisive acquisition probe/fix.  The official API exposes
 * read_key_event() as a direct KEY_EVENT (0x20) operation with 0xFF as the
 * empty sentinel.  Do not gate the physical event read on EVENT_NUM/IRQ.
 * Poll KEY_EVENT once per real 10-ms CPU0 service tick and drain a bounded
 * burst until 0xFF.  EVENT_NUM/INT remain telemetry/fallback only. */
static void tab5kbd_r101_direct_poll_cpu0(void)
{
    if (!s_tab5kbd_present || !s_tab5kbd_input_armed) return;

    unsigned drained = 0u;
    bool hit = false;
    while (drained < 32u) {
        uint8_t raw = TAB5KBD_EVENT_EMPTY;
        if (!tab5kbd_read_reg(TAB5KBD_REG_KEY_EVENT, &raw, 1u))
            break;
        if (raw == TAB5KBD_EVENT_EMPTY) {
            break;
        }

        hit = true;
        ++drained;
        tab5kbd_process_normal(raw);
    }

    /* Do not hammer INT_STAT on empty polls.  Clear it only after an actual
     * device event burst; queue data itself is consumed through KEY_EVENT. */
    if (hit)
        (void)tab5kbd_write_reg(TAB5KBD_REG_INT_STAT, 0u);
}


static void tab5kbd_cpu0_task(void *arg)
{
    (void)arg;
    /* R57E102P: R101 health stopped after #2 once host RT load became active;
     * promote bounded A164 service to priority 3 peer while preserving 10ms sleep.
     * R57E98T: CONFIG_FREERTOS_HZ=100, so pdMS_TO_TICKS(5)==0.
     * R97T accidentally busy-looped this prio-2 task and could starve the
     * prio-1 app/main initialization (notably HDS/SFXVI startup).  Use one
     * real 10-ms RTOS tick and express all periodic divisors in 10-ms units. */
    unsigned tick10 = 0u;
    for (;;) {
        if (!s_tab5kbd_present) {
            if ((tick10++ % 100u) == 0u && tab5kbd_detect_and_configure()) {
                s_tab5kbd_input_armed = true;
                tab5_video_set_tab5_keyboard_orientation(1);
                ESP_LOGI(TAG, "PX68K_TAB5KBD_R57E99K: hotplug detected; official Normal-mode level-aware bridge ARMED on CPU0");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* R57E99K: match the official UnitTab5Keyboard readiness rule:
         * service an IRQ edge OR an already-asserted active-low INT level.
         * Additionally do a tiny 50-ms safety poll regardless of whether the
         * ISR handler was installed.  At 400 kHz this is negligible traffic
         * (20 EVENT_NUM reads/s) and prevents a shared ISR/edge miss from
         * permanently suppressing A164 input.  The 10-ms blocking tick stays. */
        const bool irq = __atomic_exchange_n(&s_tab5kbd_irq_pending, false, __ATOMIC_ACQ_REL);
        const bool int_low = (gpio_get_level(TAB5KBD_INT_GPIO) == 0);
        /* R101K primary acquisition: direct 0x20 read every real 10ms tick. */
        tab5kbd_r101_direct_poll_cpu0();

        /* Retain the count-driven path only as a secondary diagnostic/fallback.
         * It should normally find nothing after the direct drain. */
        if (irq || int_low)
            tab5kbd_drain_cpu0();
        ++tick10;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool tab5kbd_start_cpu0_task(void)
{
    if (s_tab5kbd_task) return true;
    const BaseType_t ok = xTaskCreatePinnedToCore(tab5kbd_cpu0_task, "tab5_a164",
                                                  4096, NULL, 3,
                                                  &s_tab5kbd_task, 0);
    if (ok != pdPASS) {
        s_tab5kbd_task = NULL;
        ESP_LOGE(TAG, "PX68K_TAB5KBD_R57E99K: CPU0 keyboard task create FAILED");
        return false;
    }
    ESP_LOGI(TAG, "PX68K_TAB5KBD_R57E102P: CPU0 direct KEY_EVENT service READY prio=3 host-RT peer; 0x20 every 10ms; audio/speaker remain higher priority");
    return true;
}




/* R57E128: FB127 hot-path counters retired after characterization. */
extern uint32_t m68k_tab5_dispatch_tcm_bytes(void);
extern uint32_t m68k_tab5_dispatch_l2_bytes(void);

#if defined(MALLOC_CAP_SPM)
#define TAB5_FASTMEM_CAP MALLOC_CAP_SPM
#define TAB5_FASTMEM_LABEL "SPM"
#elif defined(MALLOC_CAP_TCM)
#define TAB5_FASTMEM_CAP MALLOC_CAP_TCM
#define TAB5_FASTMEM_LABEL "TCM/SPM"
#else
#define TAB5_FASTMEM_CAP 0u
#define TAB5_FASTMEM_LABEL "TCM/SPM-unavailable"
#endif

/* R57E138A: runtime code generation is retired.  Keep this build gate ON only
 * so the historical arena API can reserve/bind the new 64 KiB X68P4 data cache. */
#ifdef PX68K_TAB5_DYNAREC
#undef PX68K_TAB5_DYNAREC
#endif
#define PX68K_TAB5_DYNAREC 1
#define PX68K_TAB5_DYNAREC_GENERIC 0
#define PX68K_TAB5_TRACE134 0
#define PX68K_TAB5_X68P4_PREDECODE 1

extern int WinX68k_LoadEmbeddedROMs(void);
extern void WinX68k_Reset(void);

extern int WinX68k_VideoProbeInit(void);
extern int WinX68k_ExecVideoProbeFrame(void);
extern uint32_t WinX68k_GetGuestClockHz(void);
extern void m68k_tab5_x68p4_predecode_bind(void *cache, unsigned int bytes);

extern uint32_t WinX68k_AudioGetSourceRate(void);

extern void WinX68k_ProductionFramePolicy(uint32_t effective_q, int host_render_enabled);



extern uint32_t WinX68k_GetVideoWidth(void);
extern uint32_t WinX68k_GetVideoHeight(void);
extern uint32_t WinX68k_GetVideoPitchPixels(void);
extern uint32_t WinX68k_GetMonitorClass(void);
extern uint32_t WinX68k_GetDirtyLineCount(void);
extern uint32_t WinX68k_GetRasterHeight(void);
extern void WinX68k_MarkAllVideoDirty(void);
extern void WinX68k_MarkVideoLineDirty(uint32_t y);
extern void tab5_cpu1_video_frame_boundary_sync(void);

extern int WinX68k_MountFloppy(int drive, const char *path);
extern int WinX68k_StandaloneInit(void);
extern void WinX68k_ProductSealFixedConfig(void);
extern int WinX68k_FloppyReady(int drive);
extern int WinX68k_MountSCSIHD(int target, const char *path, int readonly);
extern int WinX68k_SCSIHDReady(int target);
extern int WinX68k_SCSIHDProbe(int target, uint32_t *hash_out);
extern int WinX68k_SCSIHDProbeLayout(int target, uint32_t *partition_count);
extern int WinX68k_SCSIArmDirectBoot(int target);
extern uint32_t WinX68k_SCSIDebugIOCSCalls(void);
extern uint32_t WinX68k_SCSIDebugReads(void);
extern uint32_t WinX68k_SCSIDebugInstallerCalls(void);
extern uint32_t WinX68k_SCSIDebugInitCalls(void);
extern uint32_t WinX68k_SCSIDebugDriverInstalls(void);
extern uint32_t WinX68k_SCSIDebugPartitionCount(void);
extern uint32_t WinX68k_SCSIDebugIOCSVector(void);

typedef enum
{
    TAB5_BUDGET_NORMAL = 0,
    TAB5_BUDGET_AUDIO_GUARD = 1,
    TAB5_BUDGET_AUDIO_CRITICAL = 2
} tab5_budget_mode_t;

typedef struct
{
    tab5_budget_mode_t mode;
    uint32_t preexec_q_effective;
} tab5_budget_state_t;

/* Build 5.63 Frame Budget Manager.
 *
 * Guest-visible X68000 work is never skipped.  Only host rendering/LCD work
 * is shed when the audio reserve approaches its real-time deadline.  Hysteresis
 * prevents oscillation.  CRITICAL no longer means a black/frozen LCD: one full
 * host render is forced every 6 guest frames, matching the normal composite
 * presentation cadence once the text renderer has activated.
 *
 * A nearly-full ring applies a tiny wall-clock pace so a fast guest cannot
 * overflow the producer ring while the speaker drains. */
static tab5_budget_mode_t tab5_budget_next_mode(tab5_budget_mode_t mode,
                                                uint32_t queued,
                                                uint32_t submitted,
                                                int turbo_lowlat)
{
    /* R57E94: R88 deliberately moved the steady audio reservoir to the
     * 8K/16K domain, but the visual Frame Budget Manager was still using the
     * pre-R88 384/1024 cliff thresholds.  The R92B/R93 logs show real
     * 90-120-ms guest stalls: waiting until <23 ms cannot protect the speaker.
     * Enter GUARD with ~186 ms left and CRITICAL with ~93 ms left, then keep
     * hysteresis until reserve is rebuilt.  Guest-visible X68000 work is still
     * never skipped; only host video cadence is reduced.  P12R6 keeps NORMAL at 44.1 kHz and uses the proven low-latency thresholds only while manual TURBO is active; budget state never changes the user-selected audio rate. */
    const uint32_t Q_CRITICAL_ENTER = turbo_lowlat ? 512u : 4096u;
    const uint32_t Q_GUARD_ENTER    = turbo_lowlat ? 1024u : 8192u;
    const uint32_t Q_CRITICAL_EXIT  = turbo_lowlat ? 1024u : 8192u;
    const uint32_t Q_NORMAL_EXIT    = turbo_lowlat ? 1536u : 12288u;
    const uint32_t Q_POLICY_ARM     = turbo_lowlat ? 512u : 4096u;

    if (submitted < Q_POLICY_ARM)
        return TAB5_BUDGET_NORMAL;

    switch (mode)
    {
        case TAB5_BUDGET_AUDIO_CRITICAL:
            return (queued >= Q_CRITICAL_EXIT) ? TAB5_BUDGET_AUDIO_GUARD
                                               : TAB5_BUDGET_AUDIO_CRITICAL;
        case TAB5_BUDGET_AUDIO_GUARD:
            if (queued < Q_CRITICAL_ENTER)
                return TAB5_BUDGET_AUDIO_CRITICAL;
            if (queued >= Q_NORMAL_EXIT)
                return TAB5_BUDGET_NORMAL;
            return TAB5_BUDGET_AUDIO_GUARD;
        default:
            if (queued < Q_CRITICAL_ENTER)
                return TAB5_BUDGET_AUDIO_CRITICAL;
            if (queued < Q_GUARD_ENTER)
                return TAB5_BUDGET_AUDIO_GUARD;
            return TAB5_BUDGET_NORMAL;
    }
}


static void halt_forever(void)
{
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

/* R56: composite Screen pixels are not read directly by CPU1 diagnostics. */

typedef struct
{
    char xdf_path[512];
    char human_path[512];
    char b_xdf_path[512];
    char diskmag_path[512];
    char a_boot_path[512];
    char hds_path[512];
    tab5_launcher_config_t launcher_cfg;
    bool audio_host_ready;
    bool compose_host_ready;
    bool usb_host_ready;
} px68k_run_context_t;

static px68k_run_context_t s_run_ctx;
static TaskHandle_t s_guest_task = NULL;
static void px68k_emulation_task(void *arg);

typedef struct
{
    bool direct_hdd_boot;
    bool direct_fdd1_boot;
    bool hds_layout_ok;
    bool b_inserted;
    bool deferred_fdds_pending;
} tab5_guest_boot_result_t;

/* Build 6.12u: common in-process X68000 reboot path used by runtime FILE
 * (re)BOOT and by PANIC -> GUI launcher selection.  The ESP32-P4 host side
 * (M5Unified, ES8388/I2S, USB, LCD task and CPU0 audio workers) remains alive.
 * Only guest devices/CPU state are reset and selected media are reattached. */
static bool tab5_guest_apply_boot_config(px68k_run_context_t *ctx,
                                         const tab5_launcher_config_t *requested,
                                         tab5_guest_boot_result_t *result,
                                         const char *reason)
{
    if (!ctx || !requested || !result)
        return false;

    tab5_launcher_config_t cfg = *requested;
    bool hds_layout_ok = false;

    if (cfg.mode == TAB5_LAUNCH_MODE_PANIC)
    {
        cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
        snprintf(cfg.floppy0, sizeof(cfg.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
        cfg.floppy1[0] = '\0';
        cfg.hdd0[0] = '\0';
    }

    const bool direct_hdd = cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0;
    const bool direct_fdd1 = cfg.boot_source == TAB5_LAUNCH_BOOT_FLOPPY1;

    if (cfg.boot_source == TAB5_LAUNCH_BOOT_FLOPPY0 && !cfg.floppy0[0])
    {
        ESP_LOGE(TAG, "%s: guest reboot rejected: FDD0 boot selected with no media",
                 reason ? reason : "guest reboot");
        return false;
    }
    if (cfg.boot_source == TAB5_LAUNCH_BOOT_FLOPPY1 && !cfg.floppy1[0])
    {
        ESP_LOGE(TAG, "%s: guest reboot rejected: FDD1 boot selected with no media",
                 reason ? reason : "guest reboot");
        return false;
    }
    if (cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0 && !cfg.hdd0[0])
    {
        ESP_LOGE(TAG, "%s: guest reboot rejected: HDD0 boot selected with no media",
                 reason ? reason : "guest reboot");
        return false;
    }

    tab5_guest_input_cancel_text();
    (void)tab5_disk_eject(0);
    (void)tab5_disk_eject(1);
    (void)tab5_hdd_eject(0);

    ESP_LOGI(TAG, "%s: WinX68k_Reset only; host audio/LCD/USB preserved",
             reason ? reason : "guest reboot");
    tab5_video_touch_transition_begin(0u, 0u, 0u);
    tab5_guest_video_state_reset();
    tab5_screen_video_reset();
    WinX68k_Reset();
    WinX68k_ProductSealFixedConfig();

    ctx->launcher_cfg = cfg;
    snprintf(ctx->xdf_path, sizeof(ctx->xdf_path), "%s", cfg.floppy0);
    snprintf(ctx->b_xdf_path, sizeof(ctx->b_xdf_path), "%s", cfg.floppy1);
    snprintf(ctx->hds_path, sizeof(ctx->hds_path), "%s", cfg.hdd0);

    if (ctx->hds_path[0])
    {
        if (!WinX68k_MountSCSIHD(0, ctx->hds_path, 0))
        {
            ESP_LOGE(TAG, "%s: HDD0 attach failed: %s",
                     reason ? reason : "guest reboot", ctx->hds_path);
            return false;
        }
        uint32_t partitions = 0;
        hds_layout_ok = WinX68k_SCSIHDProbeLayout(0, &partitions) != 0;
        ESP_LOGI(TAG, "%s: HDD0 attached %s layout=%s partitions=%lu",
                 reason ? reason : "guest reboot", ctx->hds_path,
                 hds_layout_ok ? "OK" : "unrecognized",
                 (unsigned long)partitions);
    }

    if (direct_hdd)
    {
        if (!ctx->hds_path[0] || !hds_layout_ok || !WinX68k_SCSIArmDirectBoot(0))
        {
            ESP_LOGE(TAG, "%s: HDD0 direct boot arm failed",
                     reason ? reason : "guest reboot");
            return false;
        }
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->hds_path);
    }
    else if (direct_fdd1)
    {
        /* True Drive-1 IPL: Drive 0 stays absent during boot selection. */
        if (!WinX68k_MountFloppy(1, ctx->b_xdf_path))
        {
            ESP_LOGE(TAG, "%s: FDD1 boot mount failed: %s",
                     reason ? reason : "guest reboot", ctx->b_xdf_path);
            return false;
        }
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->b_xdf_path);
    }
    else
    {
        if (!WinX68k_MountFloppy(0, ctx->xdf_path))
        {
            ESP_LOGE(TAG, "%s: FDD0 mount failed: %s",
                     reason ? reason : "guest reboot", ctx->xdf_path);
            return false;
        }
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->xdf_path);

        if (ctx->b_xdf_path[0] && !WinX68k_MountFloppy(1, ctx->b_xdf_path))
        {
            ESP_LOGW(TAG, "%s: FDD1 mount failed, leaving B: empty: %s",
                     reason ? reason : "guest reboot", ctx->b_xdf_path);
            ctx->b_xdf_path[0] = '\0';
            ctx->launcher_cfg.floppy1[0] = '\0';
        }
    }

    result->direct_hdd_boot = direct_hdd;
    result->direct_fdd1_boot = direct_fdd1;
    result->hds_layout_ok = hds_layout_ok;
    result->b_inserted = direct_fdd1 || (!direct_hdd && ctx->b_xdf_path[0] != '\0');
    result->deferred_fdds_pending = (direct_hdd && (ctx->xdf_path[0] || ctx->b_xdf_path[0])) ||
                                    (direct_fdd1 && ctx->xdf_path[0]);

    tab5_video_set_runtime_media_paths(ctx->xdf_path, ctx->b_xdf_path, ctx->hds_path);
    tab5_video_set_runtime_boot_source(ctx->launcher_cfg.boot_source);

    ESP_LOGI(TAG,
             "%s complete: mode=%s source=%s A=%s B=%s HDD0=%s; ESP host not restarted",
             reason ? reason : "guest reboot",
             ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC ? "PANIC" : "PX68K",
             direct_hdd ? "HDD0" : (direct_fdd1 ? "FDD1" : "FDD0"),
             ctx->a_boot_path[0] ? ctx->a_boot_path : "<empty>",
             ctx->b_xdf_path[0] ? ctx->b_xdf_path : "<empty>",
             ctx->hds_path[0] ? ctx->hds_path : "<empty>");
    return true;
}

/* R140J2 retained WDT policy.
 * CPU0 is intentionally a real-time host-services core, so IDLE0 stays out of
 * TWDT exactly as R56h1 established.  CPU1 IDLE remains monitored; the guest
 * task therefore gives IDLE1 one real tick before timeout/3.  X3 exposed that
 * the old "soft audio-safe" branch advanced its deadline without actually
 * yielding, which could starve IDLE1 indefinitely and produce false 15-second
 * TWDT reports while the guest was still making progress.
 * Interrupt watchdog (IWDT) remains unchanged. */
static void tab5_r56h_apply_cpu0_rt_twdt_policy(void)
{
#if defined(CONFIG_ESP_TASK_WDT_EN) && CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_config_t cfg = {
        .timeout_ms = (uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000u,
        .idle_core_mask = 0u,
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#endif

    const esp_err_t rc = esp_task_wdt_reconfigure(&cfg);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG,
                 "PX68K_PRODUCT_P12R1_WDT: TWDT idleMask=0x%lX timeout=%lums panic=%d; dedicated CPU cores are not idle-task paced",
                 (unsigned long)cfg.idle_core_mask,
                 (unsigned long)cfg.timeout_ms,
                 cfg.trigger_panic ? 1 : 0);
    } else {
        ESP_LOGW(TAG,
                 "PX68K_R56H1: TWDT reconfigure failed err=%d; leaving IDF policy unchanged",
                 (int)rc);
    }
#else
    ESP_LOGI(TAG, "PX68K_R56H1: TWDT disabled by sdkconfig; no runtime change");
#endif
}

void app_main(void)
{
    /* P12R2 Production Quiet. Boot ROM/bootloader output may precede app_main;
     * application log levels are muted, while Newlib stdio objects remain alive. */
    esp_log_level_set("*", ESP_LOG_NONE);
    /* P12R6A4 Production Quiet: no diagnostic UART task/marker. */
    px68k_run_context_t *ctx = &s_run_ctx;
    memset(ctx, 0, sizeof(*ctx));


/* Intent: Core ownership rule: CPU1 advances the X68000 timeline; CPU0 handles host-only services so host work cannot stall or duplicate guest execution.  Layer8 Aug/17/2026 */
    /* Build 5.47: keep peripheral/host initialization on CPU0 so peripheral
     * driver work stays with the ESP-IDF/system side.  Only after all host
     * services exist do we launch the X68000 time-axis as a dedicated CPU1
     * task. */
    tab5_video_init();
    ESP_LOGI(TAG, "PX68K_PRODUCT_P12R2: Standard12 44.1k MULTISCAN-AUTO FDD1-BOOT PRODUCTION-QUIET");
    ESP_LOGI(TAG, "PX68K_R57E92B: A164 dedicated AUTO-I2C base retained; M5Unified internal I2C untouched");
    ESP_LOGI(TAG, "PX68K_R57E105P: A164 official key-top/Sym semantic bridge ACTIVE");
    ESP_LOGI(TAG, "PX68K_R57E107X: XVI16 BASE + CPU0 ADPCM COMMAND ENGINE; production-quiet input/video/audio paths retained");
    ESP_LOGI(TAG, "PX68K_R57E121A: MDX622 ZERO-RUN REVALIDATION ACTIVE; exact signature + scheduler fence retained; R120A audio/video unchanged");
    ESP_LOGI(TAG, "PX68K_R57E123A: legacy fused cache RETIRED by R138; touch liveness retained");
    ESP_LOGI(TAG, "PX68K_R57E139A6: X68P4 PRODUCTION-CLEAN MACHINE KERNEL ACTIVE; PREDECODE + Event Horizon + device-time ownership + CPU/DMA page coherency");
    ESP_LOGI(TAG, "PX68K_R57E139A6A3R2: FINAL PRODUCTION CLEAN FULL-TREE ACTIVE; backend/LP/video-flow observers retired; control/correctness semantics preserved");
    tab5kbd_r105p_map_selfcheck();
    if (tab5kbd_detect_and_configure())
        tab5_video_set_tab5_keyboard_orientation(1);
    /* PX68K_R56S1_LAUNCHER_HOST_UI
     * Keep cold-start Media Setup + picker on the M5GFX/FB0 ownership
     * path for the entire launcher session. R49 quarantines FB1 scanout;
     * begin_host_ui() prepares the safe M5GFX front and prevents the LCD
     * presenter from racing native UI draws. */
    tab5_video_begin_host_ui();
    tab5_video_show_message(X68K_TAB_APP_NAME, X68K_TAB_SUBTITLE);

    if (!tab5_sd_mount_and_find_xdf(ctx->human_path, sizeof(ctx->human_path)))
    {
        ESP_LOGE(TAG, "SD/XDF discovery FAILED");
        halt_forever();
    }

    /* Intent: Remove transport files from interrupted/older PANIC sessions before
     * the user sees the SD library. This also cleans legacy PLAY.PAN.
     * Layer8 Aug/17/2026 */
    tab5_panic_cleanup_staging();

    /* Build 6.12u: no normal UI path restarts the ESP32-P4.  The launcher is
     * authoritative at cold start; a PANIC staging failure simply returns to
     * the same native launcher instead of rebooting M5Unified/ES8388/I2S. */
    for (;;)
    {
        if (!tab5_launcher_run(ctx->human_path, &ctx->launcher_cfg))
        {
            ESP_LOGE(TAG, "Launcher failed");
            halt_forever();
        }

        if (ctx->launcher_cfg.mode != TAB5_LAUNCH_MODE_PANIC)
            break;

        if (tab5_panic_prepare_runtime(ctx->launcher_cfg.panic_path))
        {
            ESP_LOGI(TAG, "PANIC mode selected: %s", ctx->launcher_cfg.panic_path);
            break;
        }

        ESP_LOGE(TAG, "PANIC runtime preparation failed: %s", ctx->launcher_cfg.panic_path);
        tab5_panic_cleanup_staging();
        tab5_video_show_message("PANIC setup failed", "Returning to X68K Tab GUI...");
        vTaskDelay(pdMS_TO_TICKS(900));
    }

    /*
     * Intent: Normal PX68K games own the 160px side bars as touch JOY1.
     * PANIC mode keeps the whole touch surface for tap=SPACE / corner-hold.
     * Layer8 Aug/17/2026
     */
    tab5_video_set_game_controls_enabled(ctx->launcher_cfg.mode != TAB5_LAUNCH_MODE_PANIC);
    tab5_video_set_panic_compat_enabled(ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC);

    snprintf(ctx->xdf_path, sizeof(ctx->xdf_path), "%s", ctx->launcher_cfg.floppy0);
    snprintf(ctx->b_xdf_path, sizeof(ctx->b_xdf_path), "%s", ctx->launcher_cfg.floppy1);
    snprintf(ctx->hds_path, sizeof(ctx->hds_path), "%s", ctx->launcher_cfg.hdd0);
    if (ctx->launcher_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0)
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->hds_path);
    else
        snprintf(ctx->a_boot_path, sizeof(ctx->a_boot_path), "%s", ctx->xdf_path);
    ESP_LOGI(TAG, "Launcher boot selection: mode=%s source=%s FDD0=%s FDD1=%s HDD0=%s",
             ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC ? "PANIC" : "PX68K",
             ctx->launcher_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0 ? "HDD0" : "FLOPPY0",
             ctx->xdf_path[0] ? ctx->xdf_path : "<empty>",
             ctx->b_xdf_path[0] ? ctx->b_xdf_path : "<empty>",
             ctx->hds_path[0] ? ctx->hds_path : "<empty>");

    /* R56s1: launcher selection is complete; return panel ownership before
     * starting audio/compose/USB workers and the guest. */
    tab5_video_end_host_ui();
#if PX68K_TAB5_DYNAREC
    const int tab5_dyn_probe_ok = tab5_dynarec_arena_probe();
    /* R138 reuses the old arena API as a fixed DATA reservation.  There is no
     * generated code, executable probe, cache sync or native trace binding. */
    m68k_tab5_x68p4_predecode_bind(tab5_dyn_probe_ok ? tab5_dynarec_arena_base() : NULL,
                                   tab5_dyn_probe_ok ? (unsigned int)tab5_dynarec_arena_bytes() : 0u);
#else
    ESP_LOGI(TAG, "PX68K_CPU615H23: R22 CPU path retained; memory hierarchy pass only (cold SRAM->PSRAM, FM/ADPCM->Internal, hot palettes/YM ctrl->SPM)");
#endif
    tab5_r56h_apply_cpu0_rt_twdt_policy();
    ctx->audio_host_ready = tab5_audio_init() != 0;
    /* R23: reserve the compositor arena before guest/FM initialization.  The
     * FM PCM FIFO is now a fixed 16 KiB Internal-SRAM object; 48 KiB of cold SD
     * catalogs plus dead JIT metadata were moved out first, so compositor headroom
     * is preserved without shrinking the CPU dispatch caches. */
    /* Build 5.53a: after M5 speaker DMA is allocated, reserve one contiguous
     * Internal L2/DMA arena before text/USB/task allocations fragment it. */
    (void)tab5_compose_reserve_arena();
    ctx->compose_host_ready = tab5_compose_init() != 0;
    ESP_LOGI(TAG, "CPU0 host compositor: %s",
             ctx->compose_host_ready ? "R57E56 STUTTERFIX ACTIVE - CPU1 guest/MDX real-time priority" : "UNAVAILABLE - NW18 CPU1 exact fallback");

    /* R56: Screen Manager must exist before WinDraw_Init() on CPU1.  It is the
     * sole owner/writer of displayable ScreenVersion state; emulator/renderers
     * only exchange video facts, opaque render tickets and result messages. */
    if (!tab5_screen_manager_init()) {
        ESP_LOGE(TAG, "PX68K_SCREEN_R56: Screen Manager init FAILED");
        halt_forever();
    }
    ESP_LOGI(TAG, "PX68K_SCREEN_R56: ownership/timeline manager READY before guest start");
    ESP_LOGI(TAG,
             "PX68K_R57E2: CPU0 host scheduler contract ACTIVE YM/audio/screen/compose/LCD/R57bus=prio3 speaker=prio4; bounded bus drain + complete-work boundary yields");

    if (!tab5_textview_init())
        ESP_LOGW(TAG, "Core text view unavailable; composite view only");

    tab5_guest_input_init();
    tab5_guest_input_set_interval_frames(12u);
    s_tab5kbd_input_armed = s_tab5kbd_present;
    if (s_tab5kbd_input_armed)
        ESP_LOGI(TAG, "PX68K_TAB5KBD_R57E99K: guest direct-X68K Normal-mode bridge ARMED; USB-A keyboard remains available in parallel");
    (void)tab5kbd_start_cpu0_task();

    /* LPFAB R2 starts only after guest-input state has been reset.  The CPU0
     * touch presenter therefore uses the proven direct fallback during the
     * launcher/host-init phase, and no pre-init touch state can leak through
     * the LP latest slot into the first guest frame. */
    if (!tab5_lp_broker_init())
        ESP_LOGW(TAG, "LPFAB_R10 unavailable; R57 + direct TouchJoy fallback retained");

    ctx->usb_host_ready = tab5_usb_keyboard_start() != 0;
    if (ctx->usb_host_ready)
    {
        ESP_LOGI(TAG, "USB keyboard/mouse/JoyPAD host ready on Type-A (CPU0 host side)");
        tab5_video_status("USB-A HID host ready",
                          "Keyboard / mouse / JoyPAD supported");
    }
    else
    {
        ESP_LOGW(TAG, "USB HID host unavailable; core regression continues");
        tab5_video_status("USB HID host unavailable",
                          "PX68K boot will continue");
    }
#if portNUM_PROCESSORS > 1
    BaseType_t ok = xTaskCreatePinnedToCore(px68k_emulation_task, "px68k_guest",
                                            16384, ctx, 1, &s_guest_task, 1);
#else
    BaseType_t ok = xTaskCreate(px68k_emulation_task, "px68k_guest",
                                16384, ctx, 1, &s_guest_task);
#endif
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Unable to create CPU1 X68000 guest task");
        halt_forever();
    }

    ESP_LOGI(TAG, "Build 6.15h17R26-device-exact host init complete (DoubleFB + exact PPA dirty-band union + restored virtual Joypad chrome)");
    vTaskDelete(NULL);
}

/* P12R1 Standard12 product pacer.
 *
 * The absolute guest-audio tick advances from the exact 10-MHz device domain
 * even during silence.  Mapping it to 44.1 kHz therefore gives one stable
 * guest-time clock independent of CRTC mode, host rendering and CPU0 load.
 * CPU1 only delays when ahead.  If behind, it immediately runs full-speed.
 * A guest reset rewinds the tick and rebases the wall clock. */
static uint64_t s_p12r1_pace_tick0 = 0u;
static uint64_t s_p12r1_pace_last_tick = 0u;
static int64_t  s_p12r1_pace_wall0_us = 0;
static bool     s_p12r1_pace_valid = false;

static void p12r1_guest_wallclock_pace(void)
{
    const uint64_t tick = DSound_AbsGuestTick64();
    int64_t now_us = esp_timer_get_time();

    if (!s_p12r1_pace_valid || tick < s_p12r1_pace_last_tick)
    {
        s_p12r1_pace_tick0 = tick;
        s_p12r1_pace_last_tick = tick;
        s_p12r1_pace_wall0_us = now_us;
        s_p12r1_pace_valid = true;
        return;
    }

    s_p12r1_pace_last_tick = tick;
    const uint64_t dtick = tick - s_p12r1_pace_tick0;
    const int64_t target_us = s_p12r1_pace_wall0_us +
        (int64_t)((dtick * 1000000ULL + 22050ULL) / 44100ULL);

    int64_t ahead_us = target_us - now_us;
    while (ahead_us > 0)
    {
        /* Keep each local spin bounded so interrupts remain responsive.
         * This does not schedule or wait on CPU0. */
        uint32_t slice = (ahead_us > 1000) ? 1000u : (uint32_t)ahead_us;
        esp_rom_delay_us(slice);
        now_us = esp_timer_get_time();
        ahead_us = target_us - now_us;
    }
}

static void px68k_emulation_task(void *arg)
{
    px68k_run_context_t *ctx = (px68k_run_context_t *)arg;
    if (!ctx)
        vTaskDelete(NULL);

#define xdf_path         (ctx->xdf_path)
#define human_path       (ctx->human_path)
#define b_xdf_path       (ctx->b_xdf_path)
#define diskmag_path     (ctx->diskmag_path)
#define a_boot_path      (ctx->a_boot_path)
#define hds_path         (ctx->hds_path)
#define audio_host_ready (ctx->audio_host_ready)
#define usb_host_ready   (ctx->usb_host_ready)
#define panic_mode       (ctx->launcher_cfg.mode == TAB5_LAUNCH_MODE_PANIC)

    bool direct_hdd_boot = ctx->launcher_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0;
    bool direct_fdd1_boot = ctx->launcher_cfg.boot_source == TAB5_LAUNCH_BOOT_FLOPPY1;

    ESP_LOGI(TAG, "Build 6.15h17R26-device-exact guest task started: R25-measured DMA3 + MFP exact device paths armed; CPU path unchanged");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, " X68K Tab - P12R2 Production: Standard12 + 12MiB + 44.1k + MULTISCAN AUTO + Tab5 DSI");
    ESP_LOGI(TAG, " Build 5.91 baseline + Flash Human68k Quick Boot + HDS/SCSI 5.94c");
    ESP_LOGI(TAG, "=======================================");

    ESP_LOGI(TAG, "Human68k Quick Boot: %s (project-root human302.xdf in dedicated flash partition)", TAB5_FLASH_HUMAN_PATH);
    if (human_path[0])
        ESP_LOGI(TAG, "SD Human302 candidate (not auto-inserted): %s", human_path);
    else
        ESP_LOGI(TAG, "SD Human302 candidate: <none>; Flash Human68k remains bootable");
    ESP_LOGI(TAG, "Build 6.15h17R26-device-exact: R25 measured modes specialized exactly; DMA3 OCR32/DCR80/SCR04 + MFP B/10 C/500; CPU execution unchanged");
    ESP_LOGI(TAG, "LP core: LPFAB_R10 SAFE BASE retained; research FM semantic sampler compiled OUT");
    ESP_LOGI(TAG, "Root XDF catalog: %u image(s)",
             (unsigned)tab5_sd_xdf_count());
    ESP_LOGI(TAG, "Root boot-media catalog: %u image(s) (.XDF/.DIM)",
             (unsigned)tab5_sd_floppy_count());

    if (tab5_disk_find_named("DISKMAG1.XDF", diskmag_path, sizeof(diskmag_path)))
    {
        ESP_LOGI(TAG, "Alternate boot XDF ready for F8: %s", diskmag_path);
    }
    else
    {
        diskmag_path[0] = '\0';
        ESP_LOGW(TAG, "DISKMAG1.XDF not found; F8 alternate boot disabled");
    }

    tab5_video_show_message(panic_mode ? "PANIC Player" : (direct_hdd_boot ? "X68K Tab HDD0 boot" : (direct_fdd1_boot ? "X68K Tab FDD1 boot" : "X68K Tab boot media")),
                            panic_mode ? ctx->launcher_cfg.panic_path : (direct_hdd_boot ? hds_path : (direct_fdd1_boot ? b_xdf_path : xdf_path)));
    ESP_LOGI(TAG,
             "PSRAM free before PX68K: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (!retro_load_game(NULL))
    {
        ESP_LOGE(TAG, "retro_load_game(NULL) FAILED");
        halt_forever();
    }

    ESP_LOGI(TAG, "PX68K allocation: OK");

    ESP_LOGI(TAG, "Musashi init BEGIN");
    m68000_init();
    ESP_LOGI(TAG, "Musashi init: OK; Build 5.62 FDC+GPIP/fill fast-forward + host arithmetic sweep + DISPATCH256; Musashi context=%u bytes",
             (unsigned)m68k_context_size());
    ESP_LOGI(TAG, "Embedded ROM load BEGIN");

    if (!WinX68k_LoadEmbeddedROMs())
    {
        ESP_LOGE(TAG, "Embedded ROM load FAILED");
        halt_forever();
    }

    ESP_LOGI(TAG, "Embedded ROM load: OK");

    if (!WinX68k_StandaloneInit())
    {
        ESP_LOGE(TAG, "WinX68k_StandaloneInit FAILED");
        halt_forever();
    }

    /*
     * Build 5.7a: WinDraw_Init() MUST run before WinX68k_Reset().
     *
     * WinX68k_Reset() calls Pal_Init(), and Pal_Init()/Pal_SetColor() build
     * PX68K's Pal16[] conversion table from WinDraw_Pal16R/G/B.  Those RGB565
     * masks are established by WinDraw_Init().  The old standalone order did
     * Reset first, so native TextPal/GrphPal/composite colors were generated
     * with uninitialized pixel-format masks.  The old monochrome proof view
     * hid this because 0x0000/0xffff are byte-order/palette invariant.
     *
     * Match PX68K pmain(): WinDraw_Init -> WinX68k_Reset.
     */
    if (!WinX68k_VideoProbeInit())
    {
        ESP_LOGE(TAG, "WinX68k_VideoProbeInit FAILED");
        halt_forever();
    }

    ESP_LOGI(TAG,
             "WinDraw private renderer initialized BEFORE reset: size=%lux%lu pitch=%lu; managed Screen pointer is not exposed to CPU1",
             (unsigned long)WinX68k_GetVideoWidth(),
             (unsigned long)WinX68k_GetVideoHeight(),
             (unsigned long)WinX68k_GetVideoPitchPixels());

    ESP_LOGI(TAG, "WinX68k_Reset BEGIN");
    tab5_video_touch_transition_begin(0u, 0u, 0u);
    tab5_guest_video_state_reset();
    tab5_screen_video_reset();
    WinX68k_Reset();
    WinX68k_ProductSealFixedConfig();
    ESP_LOGI(TAG, "WinX68k_Reset END");

    bool hds_layout_ok = false;
    if (hds_path[0])
    {
        /* Build 5.94b keeps the proven 5.15 high-level SCSI IOCS bridge, but
         * can now make HDD0 an explicit boot source.  Attachment still occurs
         * after reset and before the first guest instruction. */
        if (!WinX68k_MountSCSIHD(0, hds_path, 0))
        {
            ESP_LOGE(TAG, "HDS SCSI0 attach FAILED: %s", hds_path);
            tab5_video_status("HDS attach failed", hds_path);
        }
        else
        {
            ESP_LOGI(TAG, "*** HDS SCSI0 ATTACHED READ-WRITE: %s ready=%d ***",
                     hds_path, WinX68k_SCSIHDReady(0));

            uint32_t hds_lba0_hash = 0;
            if (WinX68k_SCSIHDProbe(0, &hds_lba0_hash))
            {
                ESP_LOGI(TAG, "*** HDD0 HOST READ OK: LBA0 hash=%08lX ***",
                         (unsigned long)hds_lba0_hash);
            }
            else
            {
                ESP_LOGE(TAG, "*** HDD0 HOST READ FAILED: %s ***", hds_path);
            }

            uint32_t hds_partitions = 0;
            if (WinX68k_SCSIHDProbeLayout(0, &hds_partitions))
            {
                hds_layout_ok = true;
                ESP_LOGI(TAG, "*** HDD0 HOST LAYOUT OK: X68SCSI1 + X68K table; usable Human68k partitions=%lu ***",
                         (unsigned long)hds_partitions);
                tab5_video_status("HDD0 host layout OK",
                                  direct_hdd_boot ? "Arming native IPL HD0 boot"
                                                  : "Waiting for Human68k device installer");
            }
            else
            {
                ESP_LOGE(TAG, "*** HDD0 HOST LAYOUT INVALID: expected X68SCSI1/X68K SCSI disk ***");
                tab5_video_status("HDD0 layout invalid",
                                  "Not a recognized X68000 SCSI disk");
            }

            /*
             * Do NOT force IOCS vector $7D4 here.  Human68k correctly chooses
             * the external fake SCSI ROM entry at $EA004A.  Build 5.15a's
             * $FC002A restore was backwards and is intentionally removed.
             */
            ESP_LOGI(TAG, "HDD0 IOCS vector before Human68k enumeration: $%08lX (informational)",
                     (unsigned long)WinX68k_SCSIDebugIOCSVector());
        }
    }

    if (direct_hdd_boot)
    {
        if (!hds_path[0] || !hds_layout_ok || !WinX68k_SCSIArmDirectBoot(0))
        {
            ESP_LOGE(TAG, "HDD0 boot arm FAILED: HDS=%s layout=%d ready=%d",
                     hds_path[0] ? hds_path : "<empty>", hds_layout_ok ? 1 : 0,
                     WinX68k_SCSIHDReady(0));
            tab5_video_status("HDD0 boot failed",
                              "Select a valid X68SCSI1 .HDS image");
            halt_forever();
        }
        ESP_LOGI(TAG, "*** HDD0 BOOT ARMED: normal IPL -> HD0 priority $8000 -> X68000 HDD boot path ***");
        tab5_video_status("HDD0 boot armed", "Normal IPL -> HD0 -> HDS");
    }

    /* P12R1 boot-priority guard. HDD0 keeps both floppies absent during IPL.
     * FDD1 boot keeps Drive 0 absent and mounts only Drive 1 until RAM entry. */
    if (direct_hdd_boot)
    {
        if (xdf_path[0]) ESP_LOGI(TAG, "HDD0 deferred FDD0 media: %s", xdf_path);
        if (b_xdf_path[0]) ESP_LOGI(TAG, "HDD0 deferred FDD1 media: %s", b_xdf_path);
        snprintf(a_boot_path, sizeof(a_boot_path), "%s", hds_path);
    }
    else if (direct_fdd1_boot)
    {
        if (!b_xdf_path[0] || !WinX68k_MountFloppy(1, b_xdf_path))
        {
            ESP_LOGE(TAG, "FDD1 boot mount FAILED: %s", b_xdf_path[0] ? b_xdf_path : "<empty>");
            halt_forever();
        }
        snprintf(a_boot_path, sizeof(a_boot_path), "%s", b_xdf_path);
    }
    else
    {
        if (!xdf_path[0] || !WinX68k_MountFloppy(0, xdf_path))
        {
            ESP_LOGE(TAG, "FDD0 boot mount FAILED: %s", xdf_path[0] ? xdf_path : "<empty>");
            halt_forever();
        }
        snprintf(a_boot_path, sizeof(a_boot_path), "%s", xdf_path);
        if (b_xdf_path[0] && !WinX68k_MountFloppy(1, b_xdf_path))
        {
            ESP_LOGE(TAG, "FDD1 launcher mount FAILED: %s", b_xdf_path);
            b_xdf_path[0] = '\0';
        }
    }

    /* Runtime FILE screen starts from the actual launcher-selected media.
     * Per-slot CHANGE hot-swaps immediately; (re)BOOT performs guest-only reset. */
    tab5_video_set_runtime_media_paths(xdf_path, b_xdf_path, hds_path);
    tab5_video_set_runtime_boot_source(ctx->launcher_cfg.boot_source);

    ESP_LOGI(TAG,
             "RESET PC=$%08lX A7=$%08lX SR=$%04lX",
             (unsigned long)m68k_get_reg(NULL, M68K_REG_PC),
             (unsigned long)m68k_get_reg(NULL, M68K_REG_A7),
             (unsigned long)m68k_get_reg(NULL, M68K_REG_SR));

    ESP_LOGI(TAG, "=======================================");
    if (panic_mode)
    {
        ESP_LOGI(TAG, " PANIC Player mode - Human68k bootstrap");
    }
    else
    {
        ESP_LOGI(TAG, " Human68k + USB keyboard + manual console mode");
    }
    ESP_LOGI(TAG, "=======================================");

    bool ram_execution_seen = false;
    bool textview_active = false;
    uint32_t panic_text_ready_frame = 0;
    uint32_t panic_hostfs_ready_frame = 0;
    uint32_t panic_touch_enable_frame = 0;
    uint32_t panic_command_frame = 0;
    uint32_t panic_command_gfx_base = 0;
    uint32_t panic_command_pal_base = 0;
    uint32_t panic_command_bat_open_base = 0;
    uint32_t panic_command_player_open_base = 0;
    uint32_t panic_command_pan_open_base = 0;
    bool panic_command_sent = false;
    bool panic_direct_fallback_sent = false;
    /* Build 5.63: the real emulator output is the default.  COLOR TEXT is now
     * an F9 diagnostic view rather than a boot-time mode the user must leave. */
    bool composite_view = true;
    tab5_textview_frame_t tv = {0};

    bool hds_enumeration_logged_in_ram = false;

    /* Build 5.1 legacy diagnosis: retained, but compiled quiet by default. */

    /* Build 5.6: B: runtime media + write-protect state. */
    bool b_inserted = direct_fdd1_boot || (!direct_hdd_boot && b_xdf_path[0] != '\0');
    bool b_write_protected = false;
    bool deferred_fdds_pending = (direct_hdd_boot && (xdf_path[0] || b_xdf_path[0])) ||
                                 (direct_fdd1_boot && xdf_path[0]);

    /* Build 5.9: host-controlled A: boot disk switch. */
    bool a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);

    /* Build 5.10: announce only the first real USB mouse report. */

    /* Build 5.32: standard generic USB HID gamepad -> X68000 JOY1. */

    /* Build 5.8: graphics activity watch, armed after Human text appears. */
    bool gfx_watch_armed = false;
    uint32_t gfx_base_writes = 0;
    uint32_t gfx_base_pal_writes = 0;
    uint32_t gfx_base_fast_clear = 0;

    /* R57E3: semantic source geometry published to the CPU0 touch fence. */
    uint32_t touch_video_w = 0u;
    uint32_t touch_video_h = 0u;
    uint32_t touch_video_pitch = 0u;

#define TAB5_REARM_GUEST_OBSERVERS() do { \
        ram_execution_seen = false; \
        textview_active = false; \
        composite_view = true; \
        panic_text_ready_frame = 0; \
        panic_hostfs_ready_frame = 0; \
        panic_touch_enable_frame = 0; \
        panic_command_frame = 0; \
        panic_command_gfx_base = 0; \
        panic_command_pal_base = 0; \
        panic_command_bat_open_base = 0; \
        panic_command_player_open_base = 0; \
        panic_command_pan_open_base = 0; \
        panic_command_sent = false; \
        panic_direct_fallback_sent = false; \
        hds_enumeration_logged_in_ram = false; \
        gfx_watch_armed = false; \
        gfx_base_writes = 0; \
        gfx_base_pal_writes = 0; \
        gfx_base_fast_clear = 0; \
        memset(&tv, 0, sizeof(tv)); \
    } while (0)

    ESP_LOGI(TAG, "A: boot controls: F7=next .XDF/.DIM + reset, F8=Human68k / DISKMAG1");
    ESP_LOGI(TAG, "Video control: F9=color text / PX68K composite");
    ESP_LOGI(TAG, "B: controls: F10=write-protect, F11=eject/reinsert, F12=next XDF");
    ESP_LOGI(TAG, "B: write test: COPY A:COMMAND.X B:PXWTEST.X then F11 eject/reinsert and DIR B:");
    if (hds_path[0])
        ESP_LOGI(TAG, "HDD0: SCSI0=%s READ-WRITE; mode=%s", hds_path,
                 direct_hdd_boot ? "DIRECT BOOT" :
                 (!strcmp(a_boot_path, TAB5_FLASH_HUMAN_PATH) ? "Flash Human68k FDD0 enumeration" : "mounted data disk"));
    ESP_LOGI(TAG, "Diagnostics: QUIET (set PX68K_TAB5_DIAG_VERBOSE=1 to restore legacy traces)");
    ESP_LOGI(TAG, "PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored");
    ESP_LOGI(TAG, "PX68K_R56S1: cold launcher Host-UI FB0 ownership fence active");
    ESP_LOGI(TAG, "PX68K_R56S2: media browser PSRAM-string staging bridge active");
    ESP_LOGI(TAG, "PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload");
    ESP_LOGI(TAG, "PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active");
    ESP_LOGI(TAG, "PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active");
    ESP_LOGI(TAG, "PX68K_R56S5F: validator forced to ordinary queue by brace-scoped function patch; burst after PASS");
    ESP_LOGW(TAG, "PX68K_R56S5K: visible TEXT host-BT quarantined after false one-line PASS; R56s4 exact BT authoritative");
    ESP_LOGI(TAG, "PX68K_R57A: CPU1 guest/render posted-write journal ACTIVE; R56s5k correctness path retained");
    ESP_LOGI(TAG, "PX68K_R57A2: dedicated CPU0 journal consumer ACTIVE; CPU1 posted writes never wait; R56s5k correctness retained");
    ESP_LOGI(TAG, "PX68K_R57B: CPU0 BG/Sprite render-shadow mirror ACTIVE; ownership switch deferred to raster-token phase");
    ESP_LOGI(TAG, "PX68K_R57C: ordered raster-token boundaries ACTIVE; renderer ownership still correctness-fenced");
    ESP_LOGI(TAG, "PX68K_R57D: class-certified visible TEXT + ordered CPU0 BG/Sprite shadow ownership ACTIVE; CPU1 never waits");
    ESP_LOGI(TAG, "PX68K_R57E2: CPU1 final-video detach ACTIVE; priority-safe shadow consumer prio3 + bounded drain512; 512KiB CPU0 TVRAM shadow + 1KiB CPU1 fallback line");
    ESP_LOGI(TAG, "PX68K_R57E3: touch transition fence ACTIVE; geometry/reset publish is nonblocking, CPU0 rearms only after physical present + release");
    ESP_LOGI(TAG, "PX68K_R57E4: Screen Manager rotating fair admission ACTIVE; max inflight remains 32; R57E3 touch fence retained");
    ESP_LOGI(TAG, "PX68K_R57E5: CPU0 packed-TEXT shadow ACTIVE; certification requires host + packed-shadow exactness; mismatch self-repairs on CPU0, no permanent CPU1 fallback");
    ESP_LOGI(TAG, "PX68K_R57E6: latest-wins 32px tile mailbox + common logical guestSeq ACTIVE; CPU1 never waits for physical refresh; carry dirty belongs next ScreenVersion");
    ESP_LOGI(TAG, "PX68K_R57E7: mailbox memory placement fix superseded by R57E8 double-PSRAM banks; CPU1 stack headroom retained");
    ESP_LOGI(TAG, "PX68K_R57E8: lock-free DOUBLE PSRAM dirty-tile mailbox ACTIVE; CPU0 ownership switch is one atomic active_idx; CPU1 never waits; guestSeq remains common logical order");
    ESP_LOGI(TAG, "PX68K_R57E9: adaptive mailbox rescue ACTIVE; physical ownership cut + up to 3 extra light-load rescue cuts; CPU1 remains refresh-blind and CPU0 never waits");
    ESP_LOGI(TAG, "PX68K_R57E10: audio-safe edge-latched rescue ACTIVE; Screen Manager evaluates each clean->dirty mailbox edge at most once; LP input ABI planning uses archived proven LP bring-up but LP execution remains disabled in this build");
    ESP_LOGI(TAG, "PX68K_R57E11: Audio Deadline Shield + adaptive16 mailbox rescue ACTIVE; GUARD sheds 1/4 physical presents, CRIT sheds 1/2; rescue limit follows audio reserve 0/1/3/7/15");
    ESP_LOGI(TAG, "PX68K_R57E20/BAT167M1: BAT166M0 CPU1 video module retained; legacy/special final RGB565 priority/transparency compositor moved to CPU0 via immutable snapshots; guest layer generation unchanged; LP/R57/CPU fast paths frozen; exact CPU1 fallback on queue pressure");
    ESP_LOGI(TAG, "PX68K_R57E48/BAT177NW18 FINAL BASE: NW18 exact compact compositor retained only for semantic/path reject, not host queue pressure");
    ESP_LOGI(TAG, "PX68K_R57E57/AUDIOCONT: 512f feeder + prio4 retained; TEXT override allowed only in NORMAL audio mode");
    ESP_LOGI(TAG, "PX68K_R57E44/BAT177NW14: video baseline retained byte-for-byte in renderer paths; exact common 65K GRP decode + GBT selector fusion ACTIVE");
    ESP_LOGI(TAG, "PX68K_DIRTY_R57E44: NW13 BG fuse retained; 65K GRP materialization deferred only for exact common CPU1 GBT rows");
    ESP_LOGI(TAG, "Console: manual HID only; retired automatic CLS/DIR regression injector removed");
    if (panic_mode)
        ESP_LOGI(TAG, "PANIC controls: tap screen=SPACE, hold upper-left 1.2s=return to GUI; PAN=%s", ctx->launcher_cfg.panic_path);
    ESP_LOGI(TAG, "Mouse: USB HID Boot Mouse -> PX68K SCC (left/right + relative motion)");
    ESP_LOGI(TAG, "JoyPAD: standard USB HID generic -> X68000 JOY1 CPSF/MD (X/Y or Hat + learned B1..B6,L,R; 2-button compatible)");
    ESP_LOGI(TAG, "R139A6 production-clean: periodic CPU/render/audio/device telemetry OFF");
#if PX68K_TAB5_DYNAREC
    ESP_LOGI(TAG, "X68P4_R140J2: X5 demand-decode/coldmeta + DEADLINE-DRIVEN Machine Kernel; fixed 200/800 scheduler horizon retired; Timer-A/vline exact; CPU+HD63450 coherency retained");
    ESP_LOGI(TAG, "X68P4_MACHINE_R140J2: DEADLINE-DRIVEN Event Kernel owns MFP/RTC/DMA/IRQ/audio-clock/input/FDD guest time; R108U fixed 200/800 horizon retired");
    ESP_LOGI(TAG, "X68P4_R140S2: exact J2 CPU + STATIC SFXVI superinstructions ACTIVE; generic warm chain RETIRED; runtime trace/JIT/discovery ZERO; immutable 65K snapshot reuse retained");
    ESP_LOGI(TAG, "X68P4_R140M2: MDX exact-page fused basic blocks ACTIVE; M1B profiler RETIRED; fixed $19Dxxx paths only; page-version certified; runtime discovery ZERO");
    ESP_LOGI(TAG, "X68P4_R140N2R6: SAFELEAN M2-SEM ACTIVE; M2 CPU/cycle/callback/opcode topology restored; only dead bus-error rollback + runtime stats/log accounting removed; HOT14/page-cert rejected; SCSI unsupported IOCS is FAIL");
    ESP_LOGI(TAG, "X68P4_R140P1: PRODUCTION FLATTEN ACTIVE; scalar code epoch + direct PX68K MMIO dispatch + audio-ready latch; R6A3 semantics frozen");
    ESP_LOGI(TAG, "X68P4_R140P2: EXECUTOR POLICY COLLAPSE ACTIVE; XVI16 fixed machine kernel + one-time RAM advert + scanline invariant hoist + R57 policy commit collapse; R6A3/P1 semantics retained");
    ESP_LOGI(TAG, "X68P4_R140P4: AUDIO COMPLETION-DRIVEN WAKE ACTIVE; guest-frame audio kick + 10ms source polling retired; FM mix lock shortened");
#endif
    ESP_LOGI(TAG, "MFP timer cache: exact 5.63 semantics retained; Timer-A TACR bit3 exclusion preserved");
    ESP_LOGI(TAG, "Speaker volume target: 33/255 (~13%%); guest FM/ADPCM/PCM8 amplitude unchanged");
    ESP_LOGI(TAG, "OPM backend: CPU1 lightweight timer/status + CPU0 YM2151-only @44.1kHz (5.42 direct algorithm hot-path)");
    ESP_LOGI(TAG, "Tab5 audio host: %s", audio_host_ready ? "READY" : "UNAVAILABLE");

    tab5_budget_state_t budget = {0};
    budget.mode = TAB5_BUDGET_NORMAL;



    /* P12R1: guest/device cycles remain exact.  Product speed is bounded only
     * by the absolute 44.1-kHz guest timeline; there is no CPU0/queue governor. */

    /* R57E66 TEXT priority: actual changed TVRAM bytes advance this epoch.
     * It is only a presentation hint; R57 ordered shadow remains authoritative.
     * Keep a short hold so console bursts are not hidden by frame shedding. */
    uint32_t r57e66_text_epoch = TVRAM_Tab5TextWriteEpoch();
    uint32_t r57e66_text_hold = 0u;

    /* R57E57: when the final audio reserve is genuinely close to starvation,
     * temporarily spend the host budget on guest/audio rather than pixels.
     * Hysteresis prevents chatter. A sparse rescue render keeps the display
     * visibly alive if a very heavy MDX passage lasts for many frames. */
    int r57e57_audio_recharge = 0;


    ESP_LOGI(TAG,
             "PX68K_R57E74: managed ScreenVersion exact dirty-map pass-through ACTIVE; unchanged source rows bypass CPU0 scale/diff");
    ESP_LOGI(TAG,
             "PX68K_R57E75: Screen Manager mailbox rescue budget rebased to 10MHz peripheral-time audio reserve domain; edge-latch policy unchanged");
    ESP_LOGI(TAG,
             "PX68K_R57E76: audio deadline protection ACTIVE; speaker chunk=1024 + eager 2-slot prefill + silent producer-rate counter");
    ESP_LOGI(TAG,
             "PX68K_R57E77: R76 producer result accepted; feeder quantum restored 512/eager, one natural render + one skip core profile armed");
    ESP_LOGI(TAG,
             "PX68K_R57E82: R81 contention result accepted; diagnostic freeze RETIRED; normal video + sparse immutable presenter-slot transport ACTIVE");
    ESP_LOGI(TAG,
             "PX68K_R57E83: R82 sparse transport retained; managed presenter phase attribution one-shot ACTIVE; no behavior change");
    ESP_LOGI(TAG,
             "PX68K_R57E84: R83 PPA bottleneck result accepted; managed steady sparse frames use direct-native FB0, full/recovery retains PPA fallback");
    ESP_LOGI(TAG,
             "PX68K_R57E85: R84 native-arm bug fixed; managed direct-native follows current physical front and ignores legacy LIVE-only R49 full token");
    ESP_LOGI(TAG,
             "PX68K_P12R6A4: 3-stage A/V ACTIVE; NORMAL=44.1k GREEN=22.05k RED=11.025k; Turbo 15/20/24fps MAX24; guest12/peripheral10 exact");
    ESP_LOGI(TAG,
             "PX68K_R57E88: mid-ring audio reservoir ACTIVE; measured 124ms burst headroom protected; R85 video unchanged");
    ESP_LOGI(TAG, "PX68K_R57E89: screen liveness measurement retired for production; R88/R85 behavior retained");
    ESP_LOGI(TAG, "PX68K_R57E91: continuity forensics retired for production; audio behavior retained");

    /* R56: SOURCE/MASS-DIRTY transition heuristics are retired.  Screen
     * lifecycle is owned by tab5_screen_manager and driven by guest-sequenced
     * line/frame facts plus explicit host present opportunities. */

    for (uint32_t frame = 1; ; ++frame)
    {

        /* BAT177NW1: no CPU1 readback from Screen Manager.  Guest dirtiness
         * is owned entirely by the CPU1 video module; host lag is downstream. */

        tab5_audio_stats_t budget_audio = {0};
        if (audio_host_ready)
            tab5_audio_get_stats(&budget_audio);

        budget.preexec_q_effective = budget_audio.queued_frames + budget_audio.speaker_queued_frames;
        const uint32_t p12r6_mode = tab5_video_turbo_mode();
        const bool p12r6_turbo = (p12r6_mode != 0u);
        const bool p12r6_red = (p12r6_mode >= 2u);
        const uint32_t p12r6_policy_q =
            p12r6a4_policy_frames(budget.preexec_q_effective, p12r6_mode);
        const uint32_t p12r6_policy_submitted =
            p12r6a4_policy_frames(budget_audio.submitted_frames, p12r6_mode);
        /* RED samples observational MULTISCAN only when a visual window is due. */
        const bool r118_visual_window = r118_host_visual_window_open(p12r6_turbo ? 1 : 0);
        if (!p12r6_red || r118_visual_window)
            (void)WinX68k_GetMonitorClass();

        tab5_screen_manager_production_reserve_hint(p12r6_policy_q,
                                                    p12r6_policy_submitted);

        budget.mode = tab5_budget_next_mode(budget.mode,
                                           p12r6_policy_q,
                                           p12r6_policy_submitted,
                                           p12r6_turbo ? 1 : 0);
        /* R57E95T: sample rate is explicit user state, not a load heuristic.
         * NORMAL stays 44.1 kHz; TURBO stays true 22.05 kHz.  Budget state continues
         * to shed only host video work and never changes audio rate or buffer
         * geometry automatically. */

        /* P12R1: no normal-mode queue-pressure FPS limiter.  Standard12
         * presents every due guest frame; only real audio GUARD/CRITICAL or
         * emergency recharge may shed downstream host video. */
        const uint32_t visual_div = 1u;

        /* R57E57 audio-reserve recharge.  The R57E56 log proved the final
         * residual stutter is true speaker underflow while R57 and pacing are
         * healthy.  Enter before the ring reaches the cliff, render only one
         * rescue frame out of 12, and leave once roughly 90 ms of effective
         * PCM reserve has been rebuilt. */
        /* R57E94: emergency recharge now lives in the R88 reservoir domain.
         * A 2048-frame entry (~46 ms) is the last-resort band; remain sparse
         * until 8192 frames (~186 ms) have been rebuilt so one 100-ms guest
         * spike cannot immediately empty the speaker again. */
        if (p12r6_policy_submitted >= (p12r6_turbo ? 2048u : 4096u)) {
            const uint32_t r114_recharge_enter = p12r6_turbo ? 384u : 2048u;
            const uint32_t r114_recharge_exit  = p12r6_turbo ? 1536u : 8192u;
            if (!r57e57_audio_recharge && p12r6_policy_q < r114_recharge_enter) {
                r57e57_audio_recharge = 1;
            } else if (r57e57_audio_recharge && p12r6_policy_q >= r114_recharge_exit) {
                r57e57_audio_recharge = 0;
            }
        }

        const uint32_t text_epoch_now = TVRAM_Tab5TextWriteEpoch();
        if (text_epoch_now != r57e66_text_epoch)
        {
            r57e66_text_epoch = text_epoch_now;
            r57e66_text_hold = 4u;
        }
        bool budget_render = true;
        if (r57e57_audio_recharge)
        {
            /* P12R6A4 Production Audio Guard. NORMAL retains the proven HF5
             * audio-only recharge. GREEN keeps sparse 1/2 rescue. RED is also
             * audio-only here: continuity wins over forced visuals. */
            budget_render = p12r6_red ? false
                                      : (p12r6_turbo ? ((frame & 1u) == 0u)
                                                     : false);
        }
        else if (budget.mode == TAB5_BUDGET_AUDIO_CRITICAL)
        {
            budget_render = p12r6_red
                                ? ((frame & 1u) == 0u)
                                : (p12r6_turbo
                                       ? ((p12r6_policy_q >= 2048u) || ((frame & 1u) == 0u))
                                       : ((frame % 4u) == 0u));
        }
        else if (budget.mode == TAB5_BUDGET_AUDIO_GUARD)
        {
            if (p12r6_red)
                budget_render = ((frame & 1u) == 0u);
            else if (p12r6_turbo)
                budget_render = true;
            else {
                const uint32_t div = (visual_div < 2u) ? 2u : visual_div;
                budget_render = ((frame % div) == 0u);
            }
        }
        else
        {
            budget_render = ((frame % visual_div) == 0u);
        }

        /* R57E66: keep the Human68k TEXT responsiveness win, but do not let
         * MDX/status TVRAM traffic override audio protection.  TEXT can force
         * an otherwise skipped frame only in NORMAL audio mode with a healthy
         * effective reserve.  GUARD, CRITICAL and recharge retain their sparse
         * visual cadence and therefore spend CPU0 on audio first. */
        if (r57e66_text_hold && !budget_render)
        {
            const bool text_force =
                !p12r6_turbo &&
                (budget.mode == TAB5_BUDGET_NORMAL) &&
                !r57e57_audio_recharge &&
                (budget.preexec_q_effective >= 1536u);

            if (text_force)
            {
                budget_render = true;
            }
        }
        if (r57e66_text_hold)
            --r57e66_text_hold;

        /* P12R6A4: 30fps is intentionally retired. GREEN and RED use
         * adaptive 15/20/24fps MAX24. RED GUARD stays at 20fps. */
        if (p12r6_turbo) {
            uint32_t turbo_fps = 24u;
            if (r57e57_audio_recharge)
                turbo_fps = 15u;
            else if (budget.mode == TAB5_BUDGET_AUDIO_CRITICAL)
                turbo_fps = 20u;
            else if (p12r6_red && budget.mode == TAB5_BUDGET_AUDIO_GUARD)
                turbo_fps = 20u;
            if (tab5_video_turbo_fps_r128() != turbo_fps)
                tab5_video_set_turbo_fps_r128(turbo_fps);
        }

        /* R118-X2: audio/queue policy and wall-clock window form ONE commit.
         * A rejected due window stays open; only a real render advances time. */
        const bool r118_visual_commit = budget_render && r118_visual_window;
        budget_render = r118_visual_commit;
        if (r118_visual_commit)
            r118_host_visual_commit_now(p12r6_turbo ? 1 : 0);

        /* BAT177NW1: geometry invalidation is CPU1 video-state owned.
         * There is deliberately no Screen Manager readback/force-render gate. */


        /* R57E66: no queue-derived delay and no frame-end busy wait.
         * Device scheduling remains 200-cycle exact; pacing itself is sampled
         * only at the coarse guest-time quantum inside the core loop. */

        /* R140P2: one cross-TU commit replaces the former R87 reserve setter,
         * R114 Turbo setter and host-render setter. */
        WinX68k_ProductionFramePolicy(p12r6_policy_q,
                                     budget_render ? 1 : 0);

        int cycles = WinX68k_ExecVideoProbeFrame();


        (void)cycles;


        /* R140P4: CPU1 no longer wakes the audio feeder once per guest frame.
         * FM publication and ADPCM render-event publication wake the CPU0
         * feeder at the actual source-completion boundaries. */

        /* R140P4S3: absolute COMMIT event.  DSound_FlushPending()
         * does not render on CPU1; it stamps the current authoritative guest
         * sample tick into the single ordered audio timeline.  CPU0 advances
         * both FM and ADPCM to exactly that tick before any later event. */
        if (audio_host_ready)
            DSound_FlushPending();

        uint32_t w = WinX68k_GetVideoWidth();
        uint32_t h = WinX68k_GetVideoHeight();
        uint32_t pitch = WinX68k_GetVideoPitchPixels();
        uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

        /* R57E3: publish the semantic geometry transition before Screen Manager
         * starts constructing the new screen.  CPU1 never waits; CPU0 sees the
         * epoch before its next IRQ/active/safety touch sample and quarantines any finger
         * that straddles the transition. */
        if (w != touch_video_w || h != touch_video_h || pitch != touch_video_pitch) {
            tab5_video_touch_transition_begin(w, h, pitch);
            touch_video_w = w;
            touch_video_h = h;
            touch_video_pitch = pitch;
        }

        /* BAT172E0: CPU1 semantic video state is independent of Screen/LCD.
         * It observes only guest facts and stamps later render requests.
         * The Screen Manager still keeps BAT167M1 behavior; epoch is diagnostic. */
        tab5_guest_video_state_frame_boundary(w, h, pitch, VCReg0[1], CRTC_Regs[0x29]);
        tab5_cpu1_video_frame_boundary_sync();


        /* R57E62 production freeze: periodic performance telemetry removed. */

        /* R56 guest video fact.  This records guest ordering/geometry only; it
         * does NOT mean present and it does not inspect dirty-count magnitude. */
        tab5_screen_video_frame_boundary(w, h, pitch);

        if (!ram_execution_seen &&
            pc >= 0x00002000u &&
            pc <  0x00C00000u)
        {
            ram_execution_seen = true;

            if (hds_path[0] && !hds_enumeration_logged_in_ram)
            {
                hds_enumeration_logged_in_ram = true;
                ESP_LOGI(TAG,
                         "*** HDD0 RAM EXEC: vec=$%08lX init_calls=%lu installer_calls=%lu installs=%lu partitions=%lu ***",
                         (unsigned long)WinX68k_SCSIDebugIOCSVector(),
                         (unsigned long)WinX68k_SCSIDebugInitCalls(),
                         (unsigned long)WinX68k_SCSIDebugInstallerCalls(),
                         (unsigned long)WinX68k_SCSIDebugDriverInstalls(),
                         (unsigned long)WinX68k_SCSIDebugPartitionCount());
            }

            ESP_LOGI(TAG,
                     "*** RAM EXECUTION REACHED: PC=$%08lX frame=%lu ***",
                     (unsigned long)pc,
                     (unsigned long)frame);

            if ((direct_hdd_boot || direct_fdd1_boot) && deferred_fdds_pending)
            {
                if (xdf_path[0])
                    (void)WinX68k_MountFloppy(0, xdf_path);
                if (direct_hdd_boot && b_xdf_path[0])
                    b_inserted = WinX68k_MountFloppy(1, b_xdf_path) ? true : false;
                /* FDD1 boot already owns Drive 1; only deferred Drive 0 is inserted. */
                deferred_fdds_pending = false;
                tab5_video_status(direct_hdd_boot ? "HDD0 boot confirmed" : "FDD1 boot confirmed",
                                  "Deferred floppy media inserted after IPL");
            }

            if (strcmp(a_boot_path, TAB5_FLASH_HUMAN_PATH) != 0 &&
                (!human_path[0] || strcmp(a_boot_path, human_path) != 0))
            {
                tab5_video_status("Alternate boot code running",
                                  "Watching native graphics activity");

                gfx_base_writes = GVRAM_DebugWriteCount();
                gfx_base_pal_writes = Pal_DebugGrphWriteCount();
                gfx_base_fast_clear = GVRAM_DebugFastClearCount();
                gfx_watch_armed = true;

                ESP_LOGI(TAG,
                         "Graphics watch armed for alternate boot (%s): GVRAM=%lu GrphPal=%lu FastClr=%lu",
                         a_boot_path,
                         (unsigned long)gfx_base_writes,
                         (unsigned long)gfx_base_pal_writes,
                         (unsigned long)gfx_base_fast_clear);
            }
            else
            {
                tab5_video_status(
                    "Human68k code loaded",
                    "Waiting for console text...");
            }
        }

        /* R118-X2: present uses the SAME committed host-visual frame. */
        const bool do_present = r118_visual_commit;

        /* Build 5.45: while viewing COMPOSITE the expensive host text renderer
         * is diagnostic-only.  Refresh it once per second instead of every
         * LCD present; switching back to TEXT catches up on the next frame. */
        const bool do_text_sample_raw =
            (!textview_active || !composite_view)
                ? (do_present || ((frame % 60u) == 0u))
                : ((frame % 60u) == 0u);
        const bool do_text_sample = budget_render && do_text_sample_raw;

        if (do_text_sample && tab5_textview_render(&tv))
        {
            if (!textview_active &&
                ram_execution_seen &&
                tv.nonzero_pixels > 128u)
            {
                textview_active = true;
                if (panic_mode && !panic_text_ready_frame) panic_text_ready_frame = frame;

                ESP_LOGI(TAG,
                         "*** CORE COLOR TEXT VIEW ACTIVE: NZ=%lu HASH=%08lX SY=%lu PAL=%d ***",
                         (unsigned long)tv.nonzero_pixels,
                         (unsigned long)tv.hash,
                         (unsigned long)tv.scroll_y,
                         tv.palette_active);

                tab5_video_status(
                    "Human68k screen active",
                    "F9 toggles PX68K composite");

                if (!gfx_watch_armed)
                {
                    gfx_base_writes = GVRAM_DebugWriteCount();
                    gfx_base_pal_writes = Pal_DebugGrphWriteCount();
                    gfx_base_fast_clear = GVRAM_DebugFastClearCount();
                                gfx_watch_armed = true;

                    ESP_LOGI(TAG,
                             "Graphics watch armed: GVRAM=%lu GrphPal=%lu FastClr=%lu",
                             (unsigned long)gfx_base_writes,
                             (unsigned long)gfx_base_pal_writes,
                             (unsigned long)gfx_base_fast_clear);
                }
            }
        }

        /* RC: historical automatic CLS/DIR and C:/D: guest probes were OFF since
         * 5.96b and had no transition into their state machines.  Manual HID
         * input remains the sole console path. */

        /* LPFAB R2: CPU1 is the sole consumer of the brokered TouchJoy LATEST
         * state.  Apply it immediately before the existing guest-input tick so
         * guest-visible joystick ownership stays on the emulation task. */
        {
            uint16_t lp_touch_joy = 0u;
            if (tab5_lp_broker_touch_latest_take(&lp_touch_joy))
                (void)tab5_guest_input_queue_touch_joypad(lp_touch_joy);
        }

        /* Drain USB/A164 real-time events, then optional automatic text, on this task only. */
        tab5_guest_input_tick(frame);


        /* In-game side-bar actions.  CPU0 owns touch/UI; CPU1 applies disk
         * mutations here between guest frames so PX68K media state is never
         * changed concurrently with emulation.  FILE hot-swaps and ejects do
         * not reset or alter the boot source.  Layer8 Aug/17/2026 */
        if (!panic_mode)
        {
            tab5_video_action_t ui_action = {0};
            while (tab5_video_poll_action(&ui_action))
            {
                int ok = 0;
                switch (ui_action.type)
                {
                    case TAB5_VIDEO_ACTION_PANIC_RANDOM:
                    {
                        /* Build 6.12t: game->PANIC is an in-process X68000 guest
                         * reboot, NOT an ESP32-P4 reboot.  The old host-reset
                         * path tore down M5Unified/ES8388/I2S and produced the
                         * visible white hardware reboot reported on Tab5.
                         *
                         * CPU0 already selected and staged P68K.X/P68K.PAN. At
                         * this guest-frame boundary, switch A: to the dedicated
                         * Flash Human68k image and reset only PX68K.  The host
                         * speaker/audio feeder remains alive continuously. */
                        if (!ui_action.path[0]) {
                            ESP_LOGE(TAG, "In-game PANIC live switch rejected: missing staged PAN path");
                            break;
                        }

                        ESP_LOGI(TAG, "In-game PANIC live switch: guest-only reset, ESP/ES8388 preserved; PAN=%s",
                                 ui_action.path);
                        tab5_guest_input_cancel_text();
                        tab5_video_status("PANIC", "Switching X68000 to Human68k...");

                        /* Match launcher PANIC media state without restarting the host. */
                        (void)tab5_disk_eject(0);
                        (void)tab5_disk_eject(1);
                        if (hds_path[0]) (void)tab5_hdd_eject(0);

                        /* Preserve the live host audio stream here. WinX68k_Reset()
                         * resets guest OPM/ADPCM state via DSound_Stop/Play, but
                         * leaves the physical Tab5 speaker and CPU0 feeder up. */
                        tab5_video_touch_transition_begin(0u, 0u, 0u);
                        tab5_guest_video_state_reset();
                        tab5_screen_video_reset();
                        WinX68k_Reset();
                        WinX68k_ProductSealFixedConfig();
                        if (!WinX68k_MountFloppy(0, TAB5_FLASH_HUMAN_PATH)) {
                            ESP_LOGE(TAG, "In-game PANIC live switch: Flash Human68k mount FAILED");
                            tab5_video_status("PANIC start failed", "Flash Human68k mount failed");
                            break;
                        }

                        snprintf(ctx->launcher_cfg.floppy0, sizeof(ctx->launcher_cfg.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                        ctx->launcher_cfg.floppy1[0] = '\0';
                        ctx->launcher_cfg.hdd0[0] = '\0';
                        snprintf(ctx->launcher_cfg.panic_path, sizeof(ctx->launcher_cfg.panic_path), "%s", ui_action.path);
                        ctx->launcher_cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                        ctx->launcher_cfg.mode = TAB5_LAUNCH_MODE_PANIC;

                        snprintf(xdf_path, 512, "%s", TAB5_FLASH_HUMAN_PATH);
                        b_xdf_path[0] = '\0';
                        hds_path[0] = '\0';
                        snprintf(a_boot_path, 512, "%s", TAB5_FLASH_HUMAN_PATH);
                        a_diskmag_boot = false;
                        b_inserted = false;
                        b_write_protected = false;
                        direct_hdd_boot = false;
                        direct_fdd1_boot = false;
                        hds_layout_ok = false;
                        deferred_fdds_pending = false;

                        /* Re-arm the normal PANIC bootstrap state machine from
                         * its initial Human68k boot state. */
                        TAB5_REARM_GUEST_OBSERVERS();

                        tab5_video_set_runtime_media_paths(xdf_path, b_xdf_path, hds_path);
                        tab5_video_set_runtime_boot_source(TAB5_LAUNCH_BOOT_FLOPPY0);
                        tab5_video_set_game_controls_enabled(0);
                        tab5_video_set_panic_compat_enabled(1);
                        tab5_panic_reset_touch();

                        ESP_LOGI(TAG, "In-game PANIC live switch complete: PC=$%08lX A:=%s; no ESP restart",
                                 (unsigned long)m68k_get_reg(NULL, M68K_REG_PC), TAB5_FLASH_HUMAN_PATH);
                        break;
                    }
                    case TAB5_VIDEO_ACTION_MOUNT_FDD0:
                        ok = tab5_disk_mount_path(0, ui_action.path);
                        if (ok) {
                            snprintf(xdf_path, 512, "%s", ui_action.path);
                            snprintf(a_boot_path, 512, "%s", ui_action.path);
                            a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD0 %s: %s", ok ? "inserted" : "FAILED", ui_action.path);
                        break;
                    case TAB5_VIDEO_ACTION_MOUNT_FDD1:
                        ok = tab5_disk_mount_path(1, ui_action.path);
                        if (ok) {
                            snprintf(b_xdf_path, 512, "%s", ui_action.path);
                            b_inserted = true;
                            b_write_protected = false;
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD1 %s: %s", ok ? "inserted" : "FAILED", ui_action.path);
                        break;
                    case TAB5_VIDEO_ACTION_EJECT_FDD0:
                        ok = tab5_disk_eject(0);
                        if (ok) {
                            xdf_path[0] = '\0';
                            a_boot_path[0] = '\0';
                            a_diskmag_boot = false;
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD0 eject: %s", ok ? "OK" : "FAILED");
                        break;
                    case TAB5_VIDEO_ACTION_EJECT_FDD1:
                        ok = tab5_disk_eject(1);
                        if (ok) {
                            b_xdf_path[0] = '\0';
                            b_inserted = false;
                            b_write_protected = false;
                        }
                        ESP_LOGI(TAG, "Runtime FILE FDD1 eject: %s", ok ? "OK" : "FAILED");
                        break;
                    case TAB5_VIDEO_ACTION_MOUNT_HDD0:
                        ok = tab5_hdd_mount_path(0, ui_action.path);
                        if (ok) snprintf(hds_path, 512, "%s", ui_action.path);
                        ESP_LOGI(TAG, "Runtime FILE HDD0 %s: %s ready=%d", ok ? "inserted" : "FAILED",
                                 ui_action.path, WinX68k_SCSIHDReady(0));
                        break;
                    case TAB5_VIDEO_ACTION_EJECT_HDD0:
                        ok = tab5_hdd_eject(0);
                        if (ok) hds_path[0] = '\0';
                        ESP_LOGI(TAG, "Runtime FILE HDD0 eject: %s", ok ? "OK" : "FAILED");
                        break;
                    case TAB5_VIDEO_ACTION_REBOOT_GUEST:
                    {
                        tab5_launcher_config_t reboot_cfg = ctx->launcher_cfg;
                        tab5_guest_boot_result_t boot_result = {0};
                        snprintf(reboot_cfg.floppy0, sizeof(reboot_cfg.floppy0), "%s", xdf_path);
                        snprintf(reboot_cfg.floppy1, sizeof(reboot_cfg.floppy1), "%s", b_xdf_path);
                        snprintf(reboot_cfg.hdd0, sizeof(reboot_cfg.hdd0), "%s", hds_path);
                        reboot_cfg.panic_path[0] = '\0';
                        reboot_cfg.mode = TAB5_LAUNCH_MODE_PX68K;
                        reboot_cfg.boot_source = !strcmp(ui_action.path, "HDD0") ? TAB5_LAUNCH_BOOT_HDD0 :
                                                 (!strcmp(ui_action.path, "FDD1") ? TAB5_LAUNCH_BOOT_FLOPPY1 : TAB5_LAUNCH_BOOT_FLOPPY0);

                        ESP_LOGI(TAG,
                                 "Runtime FILE (re)BOOT: guest-only source=%s; ESP/ES8388/I2S stay alive",
                                 reboot_cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0 ? "HDD0" :
                                 (reboot_cfg.boot_source == TAB5_LAUNCH_BOOT_FLOPPY1 ? "FDD1" : "FDD0"));
                        tab5_video_set_game_controls_enabled(0);
                        tab5_video_set_panic_compat_enabled(0);

                        if (!tab5_guest_apply_boot_config(ctx, &reboot_cfg, &boot_result,
                                                         "Runtime FILE (re)BOOT"))
                        {
                            tab5_launcher_config_t recovery = {0};
                            snprintf(recovery.floppy0, sizeof(recovery.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                            recovery.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                            recovery.mode = TAB5_LAUNCH_MODE_PX68K;
                            ESP_LOGW(TAG, "Runtime FILE (re)BOOT failed; recovering to Flash Human68k without host reset");
                            if (!tab5_guest_apply_boot_config(ctx, &recovery, &boot_result,
                                                             "Runtime FILE recovery"))
                            {
                                ESP_LOGE(TAG, "Runtime FILE recovery failed; guest stopped with host still alive");
                                tab5_video_show_message("Guest boot failed", "Host remains alive; reboot device manually");
                                break;
                            }
                        }

                        direct_hdd_boot = boot_result.direct_hdd_boot;
                        direct_fdd1_boot = boot_result.direct_fdd1_boot;
                        hds_layout_ok = boot_result.hds_layout_ok;
                        b_inserted = boot_result.b_inserted;
                        b_write_protected = false;
                        deferred_fdds_pending = boot_result.deferred_fdds_pending;
                        a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                        TAB5_REARM_GUEST_OBSERVERS();
                        tab5_panic_reset_touch();
                        tab5_video_set_game_controls_enabled(1);
                        tab5_video_set_panic_compat_enabled(0);
                        ESP_LOGI(TAG, "Runtime FILE (re)BOOT complete: no ESP restart");
                        break;
                    }
                    default:
                        break;
                }
                tab5_video_set_runtime_media_paths(xdf_path, b_xdf_path, hds_path);
            }
        }

        /*
         * PANIC one-shot bootstrap.  Keep the proven BAT-first sequence and
         * direct retry policy.  R57E62 additionally accepts HostFS-ready after
         * RAM execution when the host color-text detector never becomes active.
         * No direct guest-memory probing is used.
         */
        if (panic_mode && !panic_command_sent && ram_execution_seen &&
            !panic_hostfs_ready_frame)
        {
            const int ready_drive = HostFS_DebugDrive();
            if (ready_drive >= 0 && ready_drive < 26) {
                panic_hostfs_ready_frame = frame;
                ESP_LOGI(TAG,
                         "PANIC HostFS-ready fallback armed: drive=%c: frame=%lu; command eligible in 120 frames",
                         (char)('A' + ready_drive), (unsigned long)frame);
            }
        }

        const bool panic_text_trigger =
            textview_active && panic_text_ready_frame &&
            frame >= panic_text_ready_frame + 90u;
        const bool panic_hostfs_trigger =
            panic_hostfs_ready_frame &&
            frame >= panic_hostfs_ready_frame + 120u;

        if (panic_mode && !panic_command_sent &&
            (panic_text_trigger || panic_hostfs_trigger) &&
            !tab5_guest_input_busy())
        {
            const int host_drive = HostFS_DebugDrive();
            if (host_drive >= 0 && host_drive < 26)
            {
                const char drive = (char)('A' + host_drive);
                char command[40];
                const bool batch_ready = tab5_panic_prepare_batch(drive) != 0;
                if (batch_ready)
                    snprintf(command, sizeof(command), "%c:\\P68K.BAT\r", drive);
                else
                    snprintf(command, sizeof(command), "%c:\\P68K.X %c:\\P68K.PAN\r", drive, drive);

                ESP_LOGI(TAG, "PANIC audio launch: preserving live PCM/FM stream (6.11b behavior; no host flush)");
                tab5_guest_input_set_interval_frames(3u);
                if (tab5_guest_input_queue_text(command))
                {
                    panic_command_sent = true;
                    panic_command_frame = frame;
                    panic_command_gfx_base = GVRAM_DebugWriteCount();
                    panic_command_pal_base = Pal_DebugGrphWriteCount();
                    panic_command_bat_open_base = HostFS_DebugPanicBatchOpens();
                    panic_command_player_open_base = HostFS_DebugPanicPlayerOpens();
                    panic_command_pan_open_base = HostFS_DebugPanicPanOpens();
                    panic_touch_enable_frame = frame + 240u;
                    tab5_panic_reset_touch();
                    ESP_LOGI(TAG, "*** PANIC AUTO START: drive=%c: via=%s trigger=%s selected=%s ***",
                             drive, batch_ready ? "P68K.BAT" : "direct P68K.X",
                             panic_text_trigger ? "TEXT" : "HOSTFS-FALLBACK",
                             ctx->launcher_cfg.panic_path);
                    tab5_video_status("PANIC.X starting", "Tap=SPACE / hold upper-left=menu");
                }
            }
        }

        /* Build 6.12q: the old fallback used only GVRAM/palette activity as
         * its launch proof.  PANIC.X can be alive on its own title/initial
         * screen before those counters move; in that case typing the direct
         * command 10 seconds later becomes ordinary key input to PANIC.X,
         * skipping the title and potentially disturbing its audio init.
         *
         * HostFS now counts successful opens of P68K.X/P68K.PAN.  If either
         * was opened after our command, the guest really dispatched the
         * player and the fallback MUST stay silent.  Only a genuinely
         * undispatched BAT retains the proven direct retry. */
        if (panic_mode && panic_command_sent && !panic_direct_fallback_sent &&
            frame >= panic_command_frame + 600u && !tab5_guest_input_busy())
        {
            const uint32_t bat_opens = HostFS_DebugPanicBatchOpens();
            const uint32_t player_opens = HostFS_DebugPanicPlayerOpens();
            const uint32_t pan_opens = HostFS_DebugPanicPanOpens();
            const bool player_dispatched =
                player_opens > panic_command_player_open_base ||
                pan_opens > panic_command_pan_open_base;

            if (player_dispatched)
            {
                panic_direct_fallback_sent = true;
                ESP_LOGI(TAG,
                         "PANIC launch confirmed by HostFS opens; direct retry suppressed: BAT=%lu(+%lu) X=%lu(+%lu) PAN=%lu(+%lu)",
                         (unsigned long)bat_opens,
                         (unsigned long)(bat_opens - panic_command_bat_open_base),
                         (unsigned long)player_opens,
                         (unsigned long)(player_opens - panic_command_player_open_base),
                         (unsigned long)pan_opens,
                         (unsigned long)(pan_opens - panic_command_pan_open_base));
            }
            else if (GVRAM_DebugWriteCount() <= panic_command_gfx_base + 32u &&
                     Pal_DebugGrphWriteCount() <= panic_command_pal_base + 16u)
            {
                const int host_drive = HostFS_DebugDrive();
                if (host_drive >= 0 && host_drive < 26)
                {
                    const char drive = (char)('A' + host_drive);
                    char direct[40];
                    snprintf(direct, sizeof(direct), "%c:\\P68K.X %c:\\P68K.PAN\r", drive, drive);
                    tab5_guest_input_set_interval_frames(3u);
                    if (tab5_guest_input_queue_text(direct))
                    {
                        panic_direct_fallback_sent = true;
                        panic_touch_enable_frame = frame + 240u;
                        ESP_LOGW(TAG, "PANIC BAT did not open player after 10s; one direct P68K.X retry queued on %c: BATopens=%lu Xopens=%lu PANopens=%lu input_sent=%lu keyq=%lu delivered=%lu drop=%lu unmapped=%lu",
                                 drive,
                                 (unsigned long)bat_opens,
                                 (unsigned long)player_opens,
                                 (unsigned long)pan_opens,
                                 (unsigned long)tab5_guest_input_sent_chars(),
                                 (unsigned long)Keyboard_DebugQueued(),
                                 (unsigned long)Keyboard_DebugIntDelivered(),
                                 (unsigned long)Keyboard_DebugQueueDropped(),
                                 (unsigned long)Keyboard_DebugUnmapped());
                    }
                }
            }
        }

        /* R57E62 production freeze: one-shot PANIC audio diagnostics removed. */

        if (panic_mode && panic_command_sent && frame >= panic_touch_enable_frame && (frame % 3u) == 0u)
        {
            const int touch_action = tab5_panic_poll_playback_touch();
            if (touch_action == 1)
            {
                (void)tab5_guest_input_queue_key(RETROK_SPACE, 1);
                (void)tab5_guest_input_queue_key(RETROK_SPACE, 0);
            }
            else if (touch_action == 2)
            {
                tab5_launcher_config_t next_cfg = {0};
                tab5_guest_boot_result_t boot_result = {0};
                bool launcher_ok = false;

                ESP_LOGI(TAG, "PANIC return-to-GUI requested: entering native launcher without ESP restart");
                tab5_guest_input_cancel_text();
                tab5_video_set_game_controls_enabled(0);
                tab5_video_set_panic_compat_enabled(0);
                tab5_video_begin_host_ui();
                tab5_panic_cleanup_staging();

                for (;;)
                {
                    if (!tab5_launcher_run(human_path, &next_cfg))
                    {
                        ESP_LOGE(TAG, "Runtime launcher failed; falling back to Flash Human68k");
                        memset(&next_cfg, 0, sizeof(next_cfg));
                        snprintf(next_cfg.floppy0, sizeof(next_cfg.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                        next_cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                        next_cfg.mode = TAB5_LAUNCH_MODE_PX68K;
                        launcher_ok = true;
                        break;
                    }

                    if (next_cfg.mode != TAB5_LAUNCH_MODE_PANIC)
                    {
                        launcher_ok = true;
                        break;
                    }

                    if (tab5_panic_prepare_runtime(next_cfg.panic_path))
                    {
                        launcher_ok = true;
                        break;
                    }

                    ESP_LOGE(TAG, "Runtime launcher PANIC staging failed: %s", next_cfg.panic_path);
                    tab5_panic_cleanup_staging();
                    tab5_video_show_message("PANIC setup failed", "Returning to X68K Tab GUI...");
                    vTaskDelay(pdMS_TO_TICKS(900));
                }

                if (launcher_ok && !tab5_guest_apply_boot_config(ctx, &next_cfg, &boot_result,
                                                                  "PANIC -> GUI selection"))
                {
                    tab5_launcher_config_t recovery = {0};
                    snprintf(recovery.floppy0, sizeof(recovery.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                    recovery.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                    recovery.mode = TAB5_LAUNCH_MODE_PX68K;
                    ESP_LOGW(TAG, "PANIC -> GUI selected boot failed; recovering Flash Human68k without host reset");
                    launcher_ok = tab5_guest_apply_boot_config(ctx, &recovery, &boot_result,
                                                               "PANIC -> GUI recovery");
                }

                if (!launcher_ok)
                {
                    tab5_video_end_host_ui();
                    tab5_video_show_message("Guest boot failed", "ESP host/audio are still alive");
                    ESP_LOGE(TAG, "PANIC -> GUI recovery failed; no ESP restart performed");
                    break;
                }

                direct_hdd_boot = boot_result.direct_hdd_boot;
                direct_fdd1_boot = boot_result.direct_fdd1_boot;
                hds_layout_ok = boot_result.hds_layout_ok;
                b_inserted = boot_result.b_inserted;
                b_write_protected = false;
                deferred_fdds_pending = boot_result.deferred_fdds_pending;
                a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                TAB5_REARM_GUEST_OBSERVERS();
                tab5_panic_reset_touch();

                tab5_video_end_host_ui();
                tab5_video_set_game_controls_enabled(panic_mode ? 0 : 1);
                tab5_video_set_panic_compat_enabled(panic_mode ? 1 : 0);

                ESP_LOGI(TAG,
                         "PANIC -> GUI complete: selected mode=%s source=%s; ESP/M5/ES8388/I2S never restarted",
                         panic_mode ? "PANIC" : "PX68K",
                         direct_hdd_boot ? "HDD0" : "FDD0");
            }
        }

        /* R139A6A3: USB mouse/JoyPAD one-shot activity observers retired;
         * functional HID -> guest input transport is unchanged. */

        /*
         * Build 5.5/5.10 host hotkeys are consumed in the USB layer but executed
         * here, on the emulation task, so PX68K/FDD state remains single-threaded.
         */
        {
            const uint32_t builtin_hotkeys = __atomic_exchange_n(&s_tab5kbd_hotkeys, 0u, __ATOMIC_ACQ_REL);
            const uint32_t hotkeys = tab5_usb_keyboard_take_hotkeys() | builtin_hotkeys;

            if (hotkeys & TAB5_USB_HOTKEY_A_BOOT_NEXT)
            {
                char target[512];

                if (!tab5_disk_next_boot_media(a_boot_path, target, sizeof(target)))
                {
                    ESP_LOGW(TAG, "A: F7 boot-media catalog is empty");
                    tab5_video_status("A: boot selector unavailable", "No .XDF/.DIM in SD root");
                }
                else
                {
                    /* Never keep the same writable image mounted in both drives. */
                    if (b_inserted && b_xdf_path[0] && !strcmp(b_xdf_path, target))
                    {
                        if (tab5_disk_eject(1))
                        {
                            b_inserted = false;
                            ESP_LOGI(TAG, "B: auto-ejected because selected F7 boot media is becoming A:");
                        }
                    }

                    tab5_guest_input_cancel_text();
                    (void)tab5_disk_eject(0);

                    ESP_LOGI(TAG, "*** A: BOOT NEXT (F7): guest reset for %s; host audio preserved ***", target);
                    tab5_video_touch_transition_begin(0u, 0u, 0u);
                    tab5_guest_video_state_reset();
                    tab5_screen_video_reset();
                    WinX68k_Reset();
                    WinX68k_ProductSealFixedConfig();

                    if (WinX68k_MountFloppy(0, target))
                    {
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", target);
                        a_diskmag_boot = diskmag_path[0] && !strcmp(a_boot_path, diskmag_path);
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                                gfx_base_writes = 0;
                        gfx_base_pal_writes = 0;
                        gfx_base_fast_clear = 0;
                        memset(&tv, 0, sizeof(tv));

                        ESP_LOGI(TAG, "*** A: BOOT MEDIA (F7): %s ***", a_boot_path);
                        tab5_video_status("Booting next SD image", a_boot_path);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "A: F7 mount failed after reset: %s", target);
                        (void)WinX68k_MountFloppy(0, TAB5_FLASH_HUMAN_PATH);
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", TAB5_FLASH_HUMAN_PATH);
                        a_diskmag_boot = false;
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                                memset(&tv, 0, sizeof(tv));
                        tab5_video_status("A: boot switch failed", "Recovered HUMAN302.XDF");
                    }
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_A_BOOT_TOGGLE)
            {
                const char *target = a_diskmag_boot ? TAB5_FLASH_HUMAN_PATH : diskmag_path;

                if (!target || !target[0])
                {
                    ESP_LOGW(TAG, "A: F8 alternate boot unavailable (DISKMAG1.XDF not found)");
                    tab5_video_status("A: boot switch unavailable", "DISKMAG1.XDF not found");
                }
                else
                {
                    /* Never keep the same writable XDF mounted in both drives. */
                    if (!a_diskmag_boot && b_inserted &&
                        b_xdf_path[0] && !strcmp(b_xdf_path, diskmag_path))
                    {
                        if (tab5_disk_eject(1))
                        {
                            b_inserted = false;
                            ESP_LOGI(TAG, "B: auto-ejected because DISKMAG1 is becoming A: boot media");
                        }
                    }

                    tab5_guest_input_cancel_text();
                    (void)tab5_disk_eject(0);

                    ESP_LOGI(TAG, "*** A: BOOT SWITCH (F8): guest reset for %s; host audio preserved ***", target);
                    tab5_video_touch_transition_begin(0u, 0u, 0u);
                    tab5_guest_video_state_reset();
                    tab5_screen_video_reset();
                    WinX68k_Reset();
                    WinX68k_ProductSealFixedConfig();

                    if (WinX68k_MountFloppy(0, target))
                    {
                        a_diskmag_boot = !a_diskmag_boot;
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", target);
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                                gfx_base_writes = 0;
                        gfx_base_pal_writes = 0;
                        gfx_base_fast_clear = 0;
                        memset(&tv, 0, sizeof(tv));

                        ESP_LOGI(TAG, "*** A: BOOT MEDIA (F8): %s ***", target);
                        tab5_video_status(a_diskmag_boot ? "Booting DISKMAG1.XDF" : "Booting HUMAN302.XDF",
                                          "PX68K composite view");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "A: F8 mount failed after reset: %s", target);
                        /* Recover the known Human68k boot media instead of leaving A: empty. */
                        (void)WinX68k_MountFloppy(0, TAB5_FLASH_HUMAN_PATH);
                        snprintf(a_boot_path, sizeof(a_boot_path), "%s", TAB5_FLASH_HUMAN_PATH);
                        a_diskmag_boot = false;
                        ram_execution_seen = false;
                        textview_active = false;
                        composite_view = true;
                        gfx_watch_armed = false;
                                memset(&tv, 0, sizeof(tv));
                        tab5_video_status("A: boot switch failed", "Recovered HUMAN302.XDF");
                    }
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_VIDEO_TOGGLE)
            {
                tab5_video_touch_transition_begin(0u, 0u, 0u);
                composite_view = !composite_view;
                ESP_LOGI(TAG, "*** VIDEO VIEW (F9): %s ***",
                         composite_view ? "PX68K COMPOSITE" : "COLOR TEXT");
                tab5_video_status(composite_view ? "PX68K composite view" : "PX68K color text view",
                                  composite_view ? "F9=color text" : "F9=composite");
            }

            if (hotkeys & TAB5_USB_HOTKEY_B_WP)
            {
                if (!b_inserted)
                {
                    ESP_LOGW(TAG, "B: F10 ignored while ejected");
                }
                else
                {
                    const bool requested = !b_write_protected;
                    if (tab5_disk_set_write_protect(1, requested ? 1 : 0))
                    {
                        b_write_protected = requested;
                        ESP_LOGI(TAG, "*** B: WRITE PROTECT %s (F10, safe remount) ***",
                                 b_write_protected ? "ON" : "OFF");
                        tab5_video_status(b_write_protected ? "B: write protected" : "B: writable",
                                          b_xdf_path[0] ? b_xdf_path : "No disk");
                    }
                    else
                    {
                        ESP_LOGW(TAG, "B: F10 write-protect change failed");
                    }
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_B_TOGGLE)
            {
                if (b_inserted)
                {
                    if (tab5_disk_eject(1))
                    {
                        b_inserted = false;
                        ESP_LOGI(TAG, "*** B: EJECTED (F11), selected=%s ***",
                                 b_xdf_path[0] ? b_xdf_path : "(none)");
                        tab5_video_status("B: ejected", "F11=insert, F12=next disk");
                    }
                }
                else if (b_xdf_path[0] && WinX68k_MountFloppy(1, b_xdf_path))
                {
                    b_inserted = true;
                    b_write_protected = false;
                    ESP_LOGI(TAG, "*** B: REINSERTED (F11): %s ***", b_xdf_path);
                    tab5_video_status("B: inserted", b_xdf_path);
                }
            }

            if (hotkeys & TAB5_USB_HOTKEY_B_NEXT)
            {
                char next_path[512];

                if (b_inserted)
                {
                    (void)tab5_disk_eject(1);
                    b_inserted = false;
                }

                if (tab5_disk_mount_next_other(1,
                                               a_boot_path,
                                               b_xdf_path,
                                               next_path,
                                               sizeof(next_path)))
                {
                    snprintf(b_xdf_path, sizeof(b_xdf_path), "%s", next_path);
                    b_inserted = true;
                    b_write_protected = false;
                    ESP_LOGI(TAG, "*** B: NEXT XDF (F12): %s ***", b_xdf_path);
                    tab5_video_status("B: next disk inserted", b_xdf_path);
                }
                else
                {
                    ESP_LOGW(TAG, "B: F12 could not select another XDF");
                    tab5_video_status("B: disk change failed", "No alternate XDF");
                }
            }
        }



        /* R118-X2: no second physical-present shed.  Audio protection already
         * happened before expensive host construction via budget_render. */
        const bool budget_present = budget_render;

        const bool do_present_budget = do_present && budget_present;

        /* P12R1: legacy Turbo adaptive visual governor retired. */

        /* R140P2: R140N2/R6 AUDIO5S one-shot measurement retired.
         * Performance is judged by exact frame209->392 wall time and playback;
         * no per-frame timer/source-counter probe remains in production. */

        if (do_present_budget)
        {
            if (textview_active && tv.pixels && !composite_view)
            {
                tab5_video_present_px68k(
                    tv.pixels,
                    tv.width,
                    tv.height,
                    tv.pitch_pixels);
            }
            else
            {
                /* R56: host policy offers an opportunity only.  It never names
                 * a ScreenVersion and never passes a framebuffer pointer.
                 * Screen Manager seals only after every opaque render ticket
                 * for the selected guest-sequence cutoff has completed. */
                tab5_screen_present_opportunity();
            }
        }


        if (frame == 600u)
        {
            /* R56k: CPU0 realtime workers must not discover/allocate newlib
             * stdio locks under sustained load.  Keep warnings/errors, but
             * move steady health reporting to the CPU1 600-frame sampler. */
            esp_log_level_set("TAB5_SCREEN", ESP_LOG_WARN);
            esp_log_level_set("TAB5_COMPOSE", ESP_LOG_WARN);
            esp_log_level_set("TAB5_VIDEO", ESP_LOG_WARN);
            esp_log_level_set("TAB5_AUDIO", ESP_LOG_WARN);
        }

        /* P12R1: cap only guest-time lead.  No delay is applied while behind. */
        p12r1_guest_wallclock_pace();

    }

#undef TAB5_REARM_GUEST_OBSERVERS
#undef panic_mode
#undef xdf_path
#undef human_path
#undef b_xdf_path
#undef diskmag_path
#undef a_boot_path
#undef hds_path
#undef audio_host_ready
#undef usb_host_ready
}
