/*
 * Tab5 port-specific implementation.
 * Intent: Tab5 SD-card mount and boot-media catalog interface.
 * Layer8 Aug/17/2026
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int tab5_sd_mount_and_find_xdf(char *out_path, size_t out_size);

/*
 * Fast root-level XDF catalog for the future on-device disk picker.
 * The boot policy remains unchanged: HUMAN302.XDF is still preferred.
 */
size_t tab5_sd_xdf_count(void);
int tab5_sd_xdf_path(size_t index, char *out_path, size_t out_size);

/* Root-level boot-media catalog used by the F7 A: selector (.XDF + .DIM). */
size_t tab5_sd_floppy_count(void);
int tab5_sd_floppy_path(size_t index, char *out_path, size_t out_size);

#ifdef __cplusplus
}
#endif
