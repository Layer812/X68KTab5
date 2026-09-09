/* R140N1: keep X68P4's proven 64 KiB predecode DATA cache and restore a
 * completely separate 32 KiB executable IRAM arena for NativeBlock v2.
 *
 * Runtime code on ESP32-P4 requires:
 *   D-cache C2M writeback -> I-cache M2C invalidate -> fence.i.
 * The probe refuses to write/execute when SRAM PMP/MEMPROT is still active.
 */
#include "tab5_dynarec_arena.h"

#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

#define X68P4_PRE138_CACHE_BYTES (64u * 1024u)
#define X68P4_PRE138_ALIGN       64u
#define X68P4_N1_NATIVE_BYTES    (32u * 1024u)
#define X68P4_N1_NATIVE_WORDS    (X68P4_N1_NATIVE_BYTES / sizeof(uint32_t))
#define X68P4_N1_SYNC_LINE       64u

#if defined(CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT) && CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT
#define X68P4_N1_PMP_ACTIVE 1
#else
#define X68P4_N1_PMP_ACTIVE 0
#endif
#if defined(CONFIG_ESP_SYSTEM_MEMPROT_FEATURE) && CONFIG_ESP_SYSTEM_MEMPROT_FEATURE
#define X68P4_N1_MEMPROT_ACTIVE 1
#else
#define X68P4_N1_MEMPROT_ACTIVE 0
#endif

static DRAM_ATTR uint8_t s_x68p4_pre138_cache[X68P4_PRE138_CACHE_BYTES]
    __attribute__((aligned(X68P4_PRE138_ALIGN), used));

/* Independent code arena.  Do not reuse the 64 KiB predecode payload: X2 and
 * the A4/A5 measurements showed that shrinking/churning the instruction-page
 * working set is expensive. */
static uint32_t s_x68p4_n1_native[X68P4_N1_NATIVE_WORDS]
    __attribute__((section(".iram1.r140n1_native"), aligned(64), used));

static int s_x68p4_pre138_probe_ok;
static int s_x68p4_n1_probe_ok;
static int s_x68p4_n1_exec;
static const char *TAG_PRE = "X68P4_OPCACHE139";
static const char *TAG_N1 = "TAB5_NATIVE140N1";

void *tab5_dynarec_arena_base(void) { return (void *)s_x68p4_pre138_cache; }
size_t tab5_dynarec_arena_bytes(void) { return sizeof(s_x68p4_pre138_cache); }
int tab5_dynarec_arena_is_executable(void) { return 0; }
int tab5_dynarec_arena_probe_ok(void) { return s_x68p4_pre138_probe_ok; }
int tab5_dynarec_arena_sync(void *addr, unsigned int bytes)
{ (void)addr; (void)bytes; return 1; }

int tab5_dynarec_arena_probe(void)
{
    const int internal = esp_ptr_internal((const void *)s_x68p4_pre138_cache) ? 1 : 0;
    const int aligned = (((uintptr_t)s_x68p4_pre138_cache & 63u) == 0u) ? 1 : 0;
    static const uint8_t canary[16] = {
        0x58,0x36,0x38,0x50,0x34,0x2d,0x50,0x52,
        0x45,0x44,0x45,0x43,0x4f,0x44,0x45,0x00
    };
    memset(s_x68p4_pre138_cache, 0, 64u);
    memcpy(s_x68p4_pre138_cache, canary, sizeof(canary));
    const int writable = memcmp(s_x68p4_pre138_cache, canary, sizeof(canary)) == 0;
    memset(s_x68p4_pre138_cache, 0, 64u);
    s_x68p4_pre138_probe_ok = internal && aligned && writable;
    ESP_LOGI(TAG_PRE,
             "PREDECODE SRAM base=%p bytes=%u internal=%d align64=%d writable=%d executable=0 runtime-code=SEPARATE-N1",
             (void *)s_x68p4_pre138_cache, (unsigned)sizeof(s_x68p4_pre138_cache),
             internal, aligned, writable);
    ESP_LOGI(TAG_PRE,
             "layout target: 128B/2way demand-decode; 64KiB payload preserved byte-for-byte in capacity");
    if (!s_x68p4_pre138_probe_ok)
        ESP_LOGE(TAG_PRE, "PREDECODE SRAM probe FAILED; X68P4 page executor will remain disabled safely");
    return s_x68p4_pre138_probe_ok;
}

void *tab5_native140n1_base(void) { return (void *)s_x68p4_n1_native; }
size_t tab5_native140n1_bytes(void) { return sizeof(s_x68p4_n1_native); }
int tab5_native140n1_probe_ok(void) { return s_x68p4_n1_probe_ok; }

int tab5_native140n1_sync(void *addr, unsigned int bytes)
{
    if (!addr || !bytes || X68P4_N1_PMP_ACTIVE || X68P4_N1_MEMPROT_ACTIVE)
        return 0;
    const uintptr_t a = (uintptr_t)addr;
    const uintptr_t base = (uintptr_t)s_x68p4_n1_native;
    const uintptr_t end = base + sizeof(s_x68p4_n1_native);
    if (a < base || a + bytes < a || a + bytes > end || (a & 63u) || (bytes & 63u))
        return 0;

    esp_err_t d = esp_cache_msync(addr, bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_err_t i = ESP_FAIL;
    if (d == ESP_OK) {
        i = esp_cache_msync(addr, bytes,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    }
#if defined(__riscv)
    __asm__ __volatile__("fence.i" ::: "memory");
#endif
    return d == ESP_OK && i == ESP_OK;
}

int tab5_native140n1_probe(void)
{
    s_x68p4_n1_probe_ok = 0;
    s_x68p4_n1_exec = esp_ptr_executable((const void *)s_x68p4_n1_native) ? 1 : 0;
    ESP_LOGI(TAG_N1,
             "arena preflight base=%p bytes=%u align64=%u executable=%d PMP=%d MEMPROT=%d",
             (void *)s_x68p4_n1_native, (unsigned)sizeof(s_x68p4_n1_native),
             (unsigned)(((uintptr_t)s_x68p4_n1_native & 63u) == 0u),
             s_x68p4_n1_exec, X68P4_N1_PMP_ACTIVE, X68P4_N1_MEMPROT_ACTIVE);

    if (X68P4_N1_PMP_ACTIVE || X68P4_N1_MEMPROT_ACTIVE) {
        ESP_LOGE(TAG_N1, "RV32 probe SKIPPED safely: SRAM PMP/MEMPROT still active");
        return 0;
    }
    if (!s_x68p4_n1_exec) {
        ESP_LOGE(TAG_N1, "RV32 probe SKIPPED safely: N1 arena is not executable");
        return 0;
    }

    const uint32_t p0 = 0x13b00513u; /* addi a0,zero,315 */
    const uint32_t p1 = 0x00008067u; /* ret */
    typedef int (*probe_fn_t)(void);
    memset((void *)s_x68p4_n1_native, 0, X68P4_N1_SYNC_LINE);
    s_x68p4_n1_native[0] = p0;
    s_x68p4_n1_native[1] = p1;
    const int writable = s_x68p4_n1_native[0] == p0 && s_x68p4_n1_native[1] == p1;
    ESP_LOGI(TAG_N1, "emit readback writable=%d words=%08lx %08lx",
             writable, (unsigned long)s_x68p4_n1_native[0], (unsigned long)s_x68p4_n1_native[1]);
    if (!writable) return 0;

    const int sync_ok = tab5_native140n1_sync((void *)s_x68p4_n1_native, X68P4_N1_SYNC_LINE);
    ESP_LOGI(TAG_N1, "code sync D:C2M + I:M2C + fence.i = %s bytes=%u",
             sync_ok ? "PASS" : "FAIL", (unsigned)X68P4_N1_SYNC_LINE);
    if (!sync_ok) return 0;

    probe_fn_t fn = (probe_fn_t)(uintptr_t)s_x68p4_n1_native;
    const int result = fn();
    s_x68p4_n1_probe_ok = result == 315;
    ESP_LOGI(TAG_N1, "static IRAM arena writable=%d executable=%d RV32 probe=%s result=%d",
             writable, s_x68p4_n1_exec, s_x68p4_n1_probe_ok ? "PASS" : "FAIL", result);
    return s_x68p4_n1_probe_ok;
}
