/*
 * RetroP4 / X68K Tab - M5Stack Unit Synth (SAM2695)
 * Production R3B clean backend.
 *
 * Tab5 PORT.A:
 *   GND        -> Unit Synth GND
 *   5V         -> Unit Synth 5V
 *   GPIO53 TX  -> Unit Synth UART_RX
 *   GPIO54     -> NC
 *
 * MIDI: UART1, 31250 baud, 8N1
 *
 * Guest invariant:
 *   CPU1 never waits for UART or CPU0.
 */

#include "retrop4_midi_uart.h"
#include "midmod/midi_engine.h"
#include "midmod/gs2sam_processor.h"
#include "midmod/gs2sam_profile_csv.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <M5Unified.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uart_port_t kMidiUart = UART_NUM_1;
constexpr int kMidiTxPin = 53;
constexpr int kMidiBaud = 31250;

constexpr uint32_t kRingSize = 8192u;
constexpr uint32_t kRingMask = kRingSize - 1u;
static_assert((kRingSize & (kRingSize - 1u)) == 0u,
              "MIDI ring must be power-of-two");

alignas(4) static uint8_t s_ring[kRingSize];
/* PHASE_PACING_R1
 * Transport annotation only. CPU1 writes the guest-cycle value for the same
 * SPSC slot immediately before the exact 985 submit_byte().
 * PSRAM avoids the historical 32KiB internal-BSS regression. */
static uint32_t *s_guest_cycle_ring = nullptr;


static uint32_t s_head = 0;
static uint32_t s_tail = 0;
static uint32_t s_overflow = 0;
static uint32_t s_ready = 0;

/* PHASE985 WIRE-AWARE R3
 * Physical MIDI is 31250 baud 8N1 = 320 us/byte.  The UART already provides
 * exact intra-stream byte serialization, so do not arm a software timer for
 * guest spacing that is already covered by bytes queued on the physical wire.
 * Only true guest gaps beyond the predicted wire drain need wall-clock wait. */
constexpr int64_t kMidiWireByteUs = 320;
static int64_t s_wire_busy_until_us = 0;
static int64_t s_phase_cycle_now_us = 0;
static uint32_t s_phase_cycles_per_us = 360u;
static uint32_t s_phase_cycle_last = 0u;
static uint32_t s_phase_cycle_remainder = 0u;
static bool s_phase_clock_active = false;

static uint32_t s_note_on = 0;
/* RP_SAM2695_MASTER_VOLUME_32_R1
 * Host presentation policy only: Unit Synth master output = 32/127.
 * Guest MIDI channel volume/expression/velocity remain byte-exact. */
/* RP_SAM2695_VOLBTN_R1
 * Physical Unit Synth output policy.  This is host gain only; guest velocity,
 * CC7 and CC11 are never rewritten.  API callers only publish desired state;
 * the CPU0 MIDI worker remains the sole UART owner. */
constexpr uint8_t kSam2695MasterVolumeDefault = 32u;
constexpr uint8_t kSam2695MasterVolumeStep = 8u;
static uint32_t s_master_volume_desired = kSam2695MasterVolumeDefault;
static uint32_t s_master_volume_dirty = 0u;
static bool s_master_volume_reassert_pending = false;


static uint8_t s_running_status = 0;
static uint8_t s_need = 0;
static uint8_t s_pos = 0;
static uint8_t s_data[2] = {0, 0};
static bool s_in_sysex = false;

static TaskHandle_t s_task = nullptr;

/* MidMod lives strictly behind the existing CPU1->CPU0 SPSC queue.
 * Keep the proven R1A20 internal-SRAM layout intact: MidMod is CPU0-only and
 * its 5.5 KiB state is explicitly allocated from PSRAM instead of consuming
 * internal BSS and pushing the R1A20 audio hot buffers into PSRAM. */
struct midmod_state_t {
    midi_engine_t engine;
    gs2sam_processor_t gs;
};
static midmod_state_t *s_midmod = nullptr;
static gs2sam_csv_profile_t *s_midimap_profile = nullptr;
constexpr const char *kMidiMapPath = "/sdcard/midimap.csv";

static bool load_midimap_override_cpu0(void)
{
    FILE *fp = fopen(kMidiMapPath, "rb");
    if (fp == nullptr) {
        return false;
    }

    gs2sam_csv_profile_t *profile = static_cast<gs2sam_csv_profile_t *>(
        heap_caps_calloc(1u, sizeof(gs2sam_csv_profile_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (profile == nullptr) {
        fclose(fp);
        return false;
    }

    gs2sam_csv_profile_init(profile);
    char line[512];
    uint32_t line_no = 0u;
    bool parse_bad = false;
    while (fgets(line, sizeof(line), fp) != nullptr) {
        ++line_no;
        const size_t n = strlen(line);
        if (n != 0u && line[n - 1u] != '\n' && !feof(fp)) {
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF) {}
            parse_bad = true;
            break;
        }
        if (gs2sam_csv_profile_parse_line(profile, line, line_no) < 0) {
            parse_bad = true;
            break;
        }
    }
    if (ferror(fp)) {
        parse_bad = true;
    }
    fclose(fp);

    const size_t rule_count = profile->tone_rule_count +
                              profile->tone_family_rule_count +
                              profile->drum_kit_rule_count +
                              profile->drum_note_rule_count;
    if (parse_bad || profile->errors != 0u || !profile->signature_seen ||
        rule_count == 0u) {
        heap_caps_free(profile);
        return false;
    }

    /* /midimap.csv replaces mapping tables only. Production transport policy
     * remains fixed so a mapping file cannot reintroduce the old rhythm CC7
     * attenuation or alter pass-through / emulation semantics. */
    gs2sam_config_t map_cfg;
    gs2sam_csv_profile_make_config(profile, &map_cfg,
                                   midi_engine_output, &s_midmod->engine);
    map_cfg.pass_unknown_sysex = 1u;
    map_cfg.pass_malformed_sysex = 1u;
    map_cfg.capital_tone_fallback = 1u;
    map_cfg.drum_fallback = 1u;
    map_cfg.drum_retarget = 1u;
    map_cfg.emulate_shared_drum_maps = 1u;
    map_cfg.emulate_drum_rx = 1u;
    map_cfg.emulate_key_range = 1u;
    map_cfg.rhythm_volume_percent = 100u;
    gs2sam_processor_apply_config(&s_midmod->gs, &map_cfg);
    s_midimap_profile = profile;

    const char *label = profile->name[0] != '\0' ? profile->name : "MIDIMAP";
    midi_engine_set_profile_label(&s_midmod->engine, label);
    return true;
}

static inline void phase_clock_anchor_cpu0(int64_t wall_us)
{
    s_phase_cycle_now_us = wall_us;
    s_phase_cycle_last = (uint32_t)esp_cpu_get_cycle_count();
    s_phase_cycle_remainder = 0u;
    s_phase_clock_active = true;
}

static inline int64_t phase_now_us_cpu0(void)
{
    if (!s_phase_clock_active)
        return 0;
    const uint32_t cc = (uint32_t)esp_cpu_get_cycle_count();
    const uint32_t dc = cc - s_phase_cycle_last;
    s_phase_cycle_last = cc;
    const uint64_t total =
        (uint64_t)s_phase_cycle_remainder + (uint64_t)dc;
    s_phase_cycle_now_us += (int64_t)(total / s_phase_cycles_per_us);
    s_phase_cycle_remainder = (uint32_t)(total % s_phase_cycles_per_us);
    return s_phase_cycle_now_us;
}

static inline void midi_wire_account_bytes_cpu0(size_t n)
{
    if (!s_phase_clock_active || n == 0u)
        return;

    /* Same CPU0 worker owns pacing and UART enqueue.  The extended cycle
     * clock accounts for task preemption and uart_write_bytes() blocking
     * without the moderate-overhead esp_timer_get_time() hot-path call. */
    const int64_t now = phase_now_us_cpu0();
    int64_t base = s_wire_busy_until_us;
    if (base < now)
        base = now;
    s_wire_busy_until_us = base + (int64_t)n * kMidiWireByteUs;
}

static inline void phase_busy_wait_us_cpu0(uint32_t wait_us)
{
    if (wait_us == 0u)
        return;
    const uint32_t start = (uint32_t)esp_cpu_get_cycle_count();
    const uint32_t target = wait_us * s_phase_cycles_per_us;
    while ((uint32_t)((uint32_t)esp_cpu_get_cycle_count() - start) < target) {
    }
    (void)phase_now_us_cpu0();
}

static bool uart_write_all_cpu0(const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int n = uart_write_bytes(
            kMidiUart,
            reinterpret_cast<const char *>(data + off),
            len - off);

        if (n <= 0) {
            return false;
        }
        midi_wire_account_bytes_cpu0((size_t)n);
        off += static_cast<size_t>(n);
    }

    return true;
}

static void midmod_uart_write_cpu0(void *user, const uint8_t *bytes, size_t len)
{
    (void)user;
    (void)uart_write_all_cpu0(bytes, len);
}

static bool sam2695_set_master_volume_cpu0(void)
{
    const uint32_t desired = __atomic_load_n(&s_master_volume_desired, __ATOMIC_ACQUIRE);
    const uint8_t level = (uint8_t)(desired > 127u ? 127u : desired);
    /* Universal Real-Time SysEx / Device Control / Master Volume:
     * F0 7F 7F 04 01 00 <level> F7 */
    const uint8_t msg[] = {
        0xF0u, 0x7Fu, 0x7Fu, 0x04u, 0x01u, 0x00u, level, 0xF7u
    };
    const bool ok = uart_write_all_cpu0(msg, sizeof(msg));
    return ok;
}

static void sam2695_service_master_volume_cpu0(void)
{
    if (!__atomic_load_n(&s_master_volume_dirty, __ATOMIC_ACQUIRE))
        return;

    /* Never splice host SysEx into the guest byte stream.  Wait for the SPSC
     * ring to become empty; then the worker owns an unambiguous UART boundary. */
    const uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    if (head != tail)
        return;

    if (!__atomic_exchange_n(&s_master_volume_dirty, 0u, __ATOMIC_ACQ_REL))
        return;
    if (!sam2695_set_master_volume_cpu0())
        __atomic_store_n(&s_master_volume_dirty, 1u, __ATOMIC_RELEASE);
}

static uint8_t data_bytes_for_status(uint8_t status)
{
    if (status < 0x80u || status >= 0xF0u) return 0u;
    const uint8_t hi = status & 0xF0u;
    return (hi == 0xC0u || hi == 0xD0u) ? 1u : 2u;
}

static void complete_channel_message(uint8_t st)
{
    if ((st & 0xF0u) == 0x90u && s_data[1] != 0u)
        ++s_note_on;
}

static void parse_guest_byte_cpu0(uint8_t b)
{
    if (b >= 0xF8u) {
        /* Realtime bytes do not disturb running status. */
        return;
    }

    if (s_in_sysex) {
        if (b == 0xF7u) {
            s_in_sysex = false;
            /* A guest reset/config SysEx may restore the synth master level.
             * Reassert the host-side output level after the guest packet has
             * physically been queued to UART. */
            s_master_volume_reassert_pending = true;
        }
        return;
    }

    if (b & 0x80u) {
        if (b == 0xF0u) {
            s_in_sysex = true;
            s_running_status = 0;
            s_need = 0;
            s_pos = 0;
            return;
        }

        if (b < 0xF0u) {
            s_running_status = b;
            s_need = data_bytes_for_status(b);
            s_pos = 0;
            return;
        }

        /* Other system-common bytes: not relevant to channel note proof. */
        s_running_status = 0;
        s_need = 0;
        s_pos = 0;
        return;
    }

    if (s_running_status == 0u || s_need == 0u) {
        return;
    }

    if (s_pos < sizeof(s_data)) {
        s_data[s_pos] = b;
    }
    ++s_pos;

    if (s_pos >= s_need) {
        complete_channel_message(s_running_status);
        s_pos = 0;
    }
}

static void phase_pace_timer_cb(void *)
{
    TaskHandle_t task = s_task;
    if (task != nullptr)
        xTaskNotifyGive(task);
}

static void midi_uart_worker(void *)
{

    /* RP_SAM2695_USBKBD_R1A2_USB_CALL_ANCHOR:
     * Production startup does not generate a synthetic SAM2695 note.
     * MIDI output begins only from X68000 guest traffic. */

    uint8_t chunk[128];
    bool music_pacing = false;
    bool pace_anchored = false;
    uint32_t pace_prev_guest10 = 0u;
    uint32_t pace_tenth_remainder = 0u;
    int64_t pace_due_us = 0;
    esp_timer_handle_t pace_timer = nullptr;

    for (;;) {
        sam2695_service_master_volume_cpu0();
        const uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
        const uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
        const uint32_t avail = head - tail;

        if (avail != 0u) {
            uint32_t count = avail;
            if (count > sizeof(chunk)) count = sizeof(chunk);

            for (uint32_t i = 0; i < count; ++i) {
                chunk[i] = s_ring[(tail + i) & kRingMask];
            }

            const uint32_t note_on_before = s_note_on;
            for (uint32_t i = 0; i < count; ++i) {
                parse_guest_byte_cpu0(chunk[i]);
            }
            const bool activate_after_chunk =
                (!music_pacing && s_note_on > note_on_before);

            if (!music_pacing) {
                /* Exact 985 startup ordering:
                 * parse full chunk -> MidMod full chunk -> publish tail.
                 * The chunk containing the first complete NoteOn also remains
                 * entirely on the direct buffered path. */
                for (uint32_t i = 0; i < count; ++i) {
                    midi_engine_feed_byte(&s_midmod->engine, chunk[i]);
                }
                __atomic_store_n(
                    &s_tail, tail + count, __ATOMIC_RELEASE);

                if (s_master_volume_reassert_pending) {
                    s_master_volume_reassert_pending = false;
                    (void)sam2695_set_master_volume_cpu0();
                }

                if (activate_after_chunk) {
                    if (s_guest_cycle_ring == nullptr) {
                    } else {
                        esp_timer_create_args_t timer_args = {};
                        timer_args.callback = &phase_pace_timer_cb;
                        timer_args.arg = nullptr;
                        timer_args.dispatch_method = ESP_TIMER_TASK;
                        timer_args.name = "rp_midi_phase";

                        const esp_err_t timer_err =
                            esp_timer_create(&timer_args, &pace_timer);

                        if (timer_err != ESP_OK) {
                            pace_timer = nullptr;
                        } else {
                            music_pacing = true;
                            pace_anchored = false;
                            pace_prev_guest10 = 0u;
                            pace_tenth_remainder = 0u;
                            pace_due_us = 0;
                            const int64_t phase_wall_anchor = esp_timer_get_time();
                            phase_clock_anchor_cpu0(phase_wall_anchor);
                            s_wire_busy_until_us = phase_wall_anchor;
                        }
                    }
                }
            } else {
                /* Music phase only. Same worker, same MidMod callback, same
                 * buffered UART. Only guest-relative spacing is restored. */
                for (uint32_t i = 0; i < count; ++i) {
                    const uint32_t slot = (tail + i) & kRingMask;
                    const uint32_t guest10 = s_guest_cycle_ring[slot];

                    if (guest10 != 0u) {
                        if (!pace_anchored) {
                            pace_anchored = true;
                            pace_prev_guest10 = guest10;
                            pace_tenth_remainder = 0u;
                            pace_due_us = phase_now_us_cpu0(); /* no added lead */
                        } else {
                            const uint32_t delta10 =
                                (uint32_t)(guest10 - pace_prev_guest10);
                            const uint64_t tenths =
                                (uint64_t)pace_tenth_remainder +
                                (uint64_t)delta10;
                            pace_due_us += (int64_t)(tenths / 10u);
                            pace_tenth_remainder =
                                (uint32_t)(tenths % 10u);
                            pace_prev_guest10 = guest10;
                        }

                        /* The UART wire itself already consumes 320 us/byte.
                         * If queued bytes carry the physical wire through this
                         * guest due time, another software timer would only add
                         * scheduling jitter.  Wait only for an actual gap not
                         * covered by physical serialization. */
                        const int64_t now = phase_now_us_cpu0();
                        const int64_t late_us = now - pace_due_us;
                        if (late_us > 2000LL) {
                            /* Host lateness is not guest time. Rebase;
                             * never burst-catch-up. */
                            pace_due_us = now;
                        }

                        if (s_wire_busy_until_us < pace_due_us) {
                            const int64_t wait_us = pace_due_us - now;
                            if (wait_us >= 100LL && pace_timer != nullptr) {
                                const esp_err_t e = esp_timer_start_once(
                                    pace_timer, (uint64_t)wait_us);
                                if (e == ESP_OK) {
                                    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                                    (void)phase_now_us_cpu0();
                                } else {
                                    /* Fail open. Next uncovered guest gap
                                     * obtains fresh wall time and rebases. */
                                    (void)phase_now_us_cpu0();
                                }
                            } else if (wait_us > 20LL) {
                                /* Bounded fine wait: cycle counter is CPU-local
                                 * and avoids repeatedly calling esp_timer. */
                                phase_busy_wait_us_cpu0((uint32_t)wait_us);
                            }
                        }
                    }

                    midi_engine_feed_byte(&s_midmod->engine, chunk[i]);
                    __atomic_store_n(
                        &s_tail, tail + i + 1u, __ATOMIC_RELEASE);
                }

                if (s_master_volume_reassert_pending) {
                    s_master_volume_reassert_pending = false;
                    (void)sam2695_set_master_volume_cpu0();
                }
            }
        } else {
            vTaskDelay(1);
            if (music_pacing)
                (void)phase_now_us_cpu0();
        }

        /* Production path: R3 wire-aware pacing only; no diagnostic work. */
    }
}

}  // namespace

extern "C" void rp_midi_uart_set_master_volume(uint8_t level)
{
    uint32_t next = level > 127u ? 127u : (uint32_t)level;
    const uint32_t old = __atomic_exchange_n(&s_master_volume_desired, next, __ATOMIC_ACQ_REL);
    if (old != next)
        __atomic_store_n(&s_master_volume_dirty, 1u, __ATOMIC_RELEASE);
}

extern "C" void rp_midi_uart_adjust_master_volume(int direction)
{
    if (direction == 0)
        return;
    uint32_t old = __atomic_load_n(&s_master_volume_desired, __ATOMIC_ACQUIRE);
    for (;;) {
        int next_i = (int)old + (direction > 0 ? (int)kSam2695MasterVolumeStep
                                               : -(int)kSam2695MasterVolumeStep);
        if (next_i < 0) next_i = 0;
        if (next_i > 127) next_i = 127;
        const uint32_t next = (uint32_t)next_i;
        if (next == old)
            return;
        uint32_t expected = old;
        if (__atomic_compare_exchange_n(&s_master_volume_desired, &expected, next,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s_master_volume_dirty, 1u, __ATOMIC_RELEASE);
            return;
        }
        old = expected;
    }
}

extern "C" bool rp_midi_uart_start(void)
{
    if (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) != 0u) {
        return true;
    }


    M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_PA);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (uart_is_driver_installed(kMidiUart)) {
        return false;
    }

    uart_config_t cfg = {};
    cfg.baud_rate = kMidiBaud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_param_config(kMidiUart, &cfg);
    if (err != ESP_OK) {
        return false;
    }

    err = uart_set_pin(kMidiUart,
                       kMidiTxPin,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return false;
    }

    err = uart_driver_install(kMidiUart, 256, 1024, 0, nullptr, 0);
    if (err != ESP_OK) {
        return false;
    }

    __atomic_store_n(&s_head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tail, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_overflow, 0u, __ATOMIC_RELAXED);
    s_master_volume_reassert_pending = false;
    s_wire_busy_until_us = 0;
    s_phase_cycle_now_us = 0;
    s_phase_cycle_last = 0u;
    s_phase_cycle_remainder = 0u;
    s_phase_clock_active = false;
    /* Production P12R1 runs the ESP32-P4 HP cores at the fixed validated
     * sdkconfig clock.  Dynamic PM is disabled, and the retired 400 MHz
     * experiment is not linked into this product.  Avoid any runtime clock
     * helper on the MIDI path: the cycle counter therefore converts directly
     * with the build-time MHz value. */
    static_assert(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ == 360,
                  "PHASE985 wire-aware pacing is certified for the Production 360 MHz profile");
    s_phase_cycles_per_us = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    if (s_guest_cycle_ring == nullptr) {
        s_guest_cycle_ring = static_cast<uint32_t *>(
            heap_caps_malloc(kRingSize * sizeof(uint32_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_guest_cycle_ring == nullptr) {
        } else {
        }
    }

    if (s_midmod == nullptr) {
        s_midmod = static_cast<midmod_state_t *>(
            heap_caps_calloc(1u, sizeof(midmod_state_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_midmod == nullptr) {
            uart_driver_delete(kMidiUart);
            return false;
        }
    }

    const midi_engine_backend_t midmod_backend = {
        midmod_uart_write_cpu0, nullptr, nullptr
    };
    midi_engine_init(&s_midmod->engine, &midmod_backend,
                     kSam2695MasterVolumeDefault);
    gs2sam_processor_init_sc55mk2(&s_midmod->gs, &s_midmod->engine, 100u);
    midi_processor_t midmod_proc = gs2sam_processor_interface(&s_midmod->gs);
    midi_engine_set_processor(&s_midmod->engine, &midmod_proc);
    midi_engine_set_profile_label(&s_midmod->engine, "SC55");
    (void)load_midimap_override_cpu0();

    /* Set the physical Unit Synth output level before guest MIDI begins.
     * RCD/guest SysEx can reset module state, so completed guest SysEx packets
     * also trigger a silent reassert in the CPU0 worker. */
    (void)sam2695_set_master_volume_cpu0();
    (void)uart_wait_tx_done(kMidiUart, pdMS_TO_TICKS(100));

    const BaseType_t ok = xTaskCreatePinnedToCore(
        midi_uart_worker,
        "rp_midi_uart",
        4096,
        nullptr,
        2,
        &s_task,
        0);

    if (ok != pdPASS) {
        uart_driver_delete(kMidiUart);
        s_task = nullptr;
        if (s_midimap_profile != nullptr) {
            heap_caps_free(s_midimap_profile);
            s_midimap_profile = nullptr;
        }
        if (s_midmod != nullptr) {
            heap_caps_free(s_midmod);
            s_midmod = nullptr;
        }
        return false;
    }

    __atomic_store_n(&s_ready, 1u, __ATOMIC_RELEASE);


    return true;
}

extern "C" bool rp_midi_uart_ready(void)
{
    return __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) != 0u;
}

extern "C" void *rp_midi_uart_handle(void)
{
    if (!rp_midi_uart_ready()) return nullptr;
    return reinterpret_cast<void *>(&s_ready);
}

extern "C" void rp_midi_uart_annotate_next_guest_cycle(uint32_t guest_cycle)
{
    uint32_t *ring = s_guest_cycle_ring;
    if (ring == nullptr || !rp_midi_uart_ready())
        return;

    const uint32_t head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    const uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);

    if ((head - tail) >= kRingSize)
        return;

    ring[head & kRingMask] = guest_cycle;
}

extern "C" bool rp_midi_uart_submit_byte(uint8_t byte)
{
    if (!rp_midi_uart_ready()) return false;

    const uint32_t head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    const uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);

    if ((head - tail) >= kRingSize) {
        __atomic_fetch_add(&s_overflow, 1u, __ATOMIC_RELAXED);
        return false;
    }

    s_ring[head & kRingMask] = byte;
    __atomic_store_n(&s_head, head + 1u, __ATOMIC_RELEASE);
    return true;
}



extern "C" void rp_midi_diag_event(uint8_t kind, uint32_t value)
{
    /* Production ABI compatibility: intentionally no-op. */
    (void)kind;
    (void)value;
}

extern "C" void rp_midi_diag_guest_tdr_byte(uint8_t byte)
{
    /* Production ABI compatibility: intentionally no-op. */
    (void)byte;
}

extern "C" uint32_t rp_midi_uart_overflow_count(void)
{
    return __atomic_load_n(&s_overflow, __ATOMIC_RELAXED);
}
