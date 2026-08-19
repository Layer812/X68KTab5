#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build 6.13b2: fixed internal executable-RAM budget for the first P4 68k
 * Dynarec experiment.  This is deliberately static rather than heap-backed:
 * ESP32-P4's normal heap reports no MALLOC_CAP_EXEC pool in the current Tab5
 * linker layout, while .iram1 is mapped by ESP-IDF into executable internal
 * SRAM. */
int tab5_dynarec_arena_probe(void);
void *tab5_dynarec_arena_base(void);
size_t tab5_dynarec_arena_bytes(void);
int tab5_dynarec_arena_is_executable(void);
int tab5_dynarec_arena_probe_ok(void);
int tab5_dynarec_arena_sync(void *addr, unsigned int bytes);

#ifdef __cplusplus
}
#endif
