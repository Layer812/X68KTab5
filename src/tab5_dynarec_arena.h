#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Historical name retained for the X68P4 64 KiB PREDECODE DATA cache. */
int tab5_dynarec_arena_probe(void);
void *tab5_dynarec_arena_base(void);
size_t tab5_dynarec_arena_bytes(void);
int tab5_dynarec_arena_is_executable(void);
int tab5_dynarec_arena_probe_ok(void);
int tab5_dynarec_arena_sync(void *addr, unsigned int bytes);


#ifdef __cplusplus
}
#endif
