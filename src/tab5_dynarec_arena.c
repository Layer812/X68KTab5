/* Production: X68P4 64 KiB predecode DATA cache.
 * The retired Native140N1 executable IRAM experiment is intentionally absent.
 */
#include "tab5_dynarec_arena.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

#define X68P4_PRE138_CACHE_BYTES (64u * 1024u)
#define X68P4_PRE138_ALIGN       64u

static DRAM_ATTR uint8_t s_x68p4_pre138_cache[X68P4_PRE138_CACHE_BYTES]
    __attribute__((aligned(X68P4_PRE138_ALIGN), used));

static int s_x68p4_pre138_probe_ok;
static const char *TAG_PRE = "X68P4_OPCACHE139";

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
             "PREDECODE SRAM base=%p bytes=%u internal=%d align64=%d writable=%d executable=0",
             (void *)s_x68p4_pre138_cache, (unsigned)sizeof(s_x68p4_pre138_cache),
             internal, aligned, writable);
    ESP_LOGI(TAG_PRE,
             "layout target: 128B/2way demand-decode; 64KiB payload preserved byte-for-byte in capacity");
    if (!s_x68p4_pre138_probe_ok)
        ESP_LOGE(TAG_PRE, "PREDECODE SRAM probe FAILED; X68P4 page executor will remain disabled safely");
    return s_x68p4_pre138_probe_ok;
}
