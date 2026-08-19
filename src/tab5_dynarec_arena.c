/* Build 6.13b2: ESP32-P4 runtime-code proof for the future 68000 Dynarec.
 *
 * 6.13b1 proved two useful things on the real Tab5:
 *   1) with PMP/MEMPROT disabled, the reserved internal SRAM is writable;
 *   2) a plain RISC-V fence.i is NOT sufficient for P4 cached internal SRAM:
 *      the CPU trapped with Illegal instruction at the exact arena base.
 *
 * ESP32-P4 routes internal SRAM through L1 caches.  Generated code therefore
 * needs an explicit D-cache clean followed by I-cache invalidation before the
 * first instruction fetch.  This build uses the public ESP-IDF esp_cache_msync
 * API and refuses to execute the probe if either cache operation fails. */
#include "tab5_dynarec_arena.h"

#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_cache.h"

#define TAB5_DYNAREC_ARENA_BYTES (32u * 1024u)
#define TAB5_DYNAREC_ARENA_WORDS (TAB5_DYNAREC_ARENA_BYTES / sizeof(uint32_t))
#define TAB5_DYNAREC_SYNC_BYTES 64u

#if defined(CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT) && CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT
#define TAB5_DYN613B2_PMP_ACTIVE 1
#else
#define TAB5_DYN613B2_PMP_ACTIVE 0
#endif

#if defined(CONFIG_ESP_SYSTEM_MEMPROT_FEATURE) && CONFIG_ESP_SYSTEM_MEMPROT_FEATURE
#define TAB5_DYN613B2_MEMPROT_ACTIVE 1
#else
#define TAB5_DYN613B2_MEMPROT_ACTIVE 0
#endif

/* Keep one exact 32 KiB accelerator budget.  The array starts at a 64-byte
 * cache-line boundary, allowing cache maintenance without UNALIGNED mode. */
static uint32_t s_tab5_dynarec_arena[TAB5_DYNAREC_ARENA_WORDS]
    __attribute__((section(".iram1.dynarec"), aligned(64), used));

static int s_tab5_dynarec_exec = 0;
static int s_tab5_dynarec_probe_ok = 0;
static const char *TAG = "TAB5_DYN613B2";

void *tab5_dynarec_arena_base(void)
{
    return (void *)s_tab5_dynarec_arena;
}

size_t tab5_dynarec_arena_bytes(void)
{
    return sizeof(s_tab5_dynarec_arena);
}

int tab5_dynarec_arena_is_executable(void)
{
    return s_tab5_dynarec_exec;
}

int tab5_dynarec_arena_probe_ok(void)
{
    return s_tab5_dynarec_probe_ok;
}

int tab5_dynarec_arena_sync(void *addr, unsigned int bytes)
{
    /* Phase 1: stores reached the data-side cache.  Push them to the backing
     * internal SRAM before instruction fetch can see the new words. */
    esp_err_t data_sync = esp_cache_msync(
        addr, bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

    /* Phase 2: discard any stale instruction-side cache line for the same
     * SRAM.  On P4 this is required in addition to fence.i because internal
     * memory itself can sit behind L1 caches. */
    esp_err_t inst_sync = ESP_FAIL;
    if (data_sync == ESP_OK) {
        inst_sync = esp_cache_msync(
            addr, bytes,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
            ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    }

#if defined(__riscv)
    /* Flush any core-local instruction-fetch state after the SDK cache sync. */
    __asm__ __volatile__("fence.i" ::: "memory");
#endif

    if (data_sync != ESP_OK || inst_sync != ESP_OK) {
        ESP_LOGE(TAG,
                 "code sync FAILED D:C2M=%s(%d) I:M2C=%s(%d) bytes=%u",
                 esp_err_to_name(data_sync), (int)data_sync,
                 esp_err_to_name(inst_sync), (int)inst_sync,
                 bytes);
    }
    return data_sync == ESP_OK && inst_sync == ESP_OK;
}

int tab5_dynarec_arena_probe(void)
{
    s_tab5_dynarec_exec = esp_ptr_executable((const void *)s_tab5_dynarec_arena) ? 1 : 0;
    s_tab5_dynarec_probe_ok = 0;

    ESP_LOGI(TAG,
             "arena preflight base=%p bytes=%u align64=%u executable=%d PMP=%d MEMPROT=%d",
             (void *)s_tab5_dynarec_arena,
             (unsigned)sizeof(s_tab5_dynarec_arena),
             (unsigned)(((uintptr_t)s_tab5_dynarec_arena & 63u) == 0u),
             s_tab5_dynarec_exec,
             TAB5_DYN613B2_PMP_ACTIVE,
             TAB5_DYN613B2_MEMPROT_ACTIVE);

    /* Never probe writability by taking a fault.  A stale sdkconfig must only
     * disable Dynarec research, never prevent X68K Tab from booting. */
    if (TAB5_DYN613B2_PMP_ACTIVE || TAB5_DYN613B2_MEMPROT_ACTIVE) {
        ESP_LOGE(TAG,
                 "RV32 probe SKIPPED safely: SRAM memory protection is active; clean/reconfigure build required");
        return 0;
    }

    if (!s_tab5_dynarec_exec) {
        ESP_LOGE(TAG,
                 "RV32 probe SKIPPED safely: reserved arena is not reported executable");
        return 0;
    }

    /* RV32I exact encodings, independently checked against clang/llvm-objdump:
     *   13 b0 05 13   addi a0,zero,0x13b   ; return 315
     *   67 80 00 00   jalr zero,0(ra)      ; ret
     * Keep the rest of the first cache line deterministic for diagnostics. */
    const uint32_t probe0 = 0x13b00513u;
    const uint32_t probe1 = 0x00008067u;
    typedef int (*probe_fn_t)(void);

    memset((void *)s_tab5_dynarec_arena, 0, TAB5_DYNAREC_SYNC_BYTES);
    s_tab5_dynarec_arena[0] = probe0;
    s_tab5_dynarec_arena[1] = probe1;

    const int writable =
        (s_tab5_dynarec_arena[0] == probe0 && s_tab5_dynarec_arena[1] == probe1) ? 1 : 0;

    ESP_LOGI(TAG,
             "emit readback writable=%d words=%08lx %08lx",
             writable,
             (unsigned long)s_tab5_dynarec_arena[0],
             (unsigned long)s_tab5_dynarec_arena[1]);

    if (!writable) {
        ESP_LOGE(TAG, "RV32 execute SKIPPED safely: emitted words failed data-side readback");
        return 0;
    }

    if (!tab5_dynarec_arena_sync((void *)s_tab5_dynarec_arena,
                                   TAB5_DYNAREC_SYNC_BYTES)) {
        ESP_LOGE(TAG,
                 "RV32 execute SKIPPED safely: cache synchronization failed; normal emulator boot continues");
        return 0;
    }

    ESP_LOGI(TAG, "code sync D:C2M=ESP_OK(0) I:M2C=ESP_OK(0) bytes=%u",
             (unsigned)TAB5_DYNAREC_SYNC_BYTES);
    probe_fn_t fn = (probe_fn_t)(uintptr_t)s_tab5_dynarec_arena;
    const int result = fn();

    s_tab5_dynarec_probe_ok = result == 0x13b;
    ESP_LOGI(TAG,
             "static IRAM arena writable=%d executable=%d RV32 probe=%s result=%d",
             writable, s_tab5_dynarec_exec,
             s_tab5_dynarec_probe_ok ? "PASS" : "FAIL", result);
    return s_tab5_dynarec_probe_ok;
}
