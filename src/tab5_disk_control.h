/*
 * Tab5 port-specific implementation.
 * Intent: Interface for Tab5 media insertion, eject, write-protect, and disk cycling.
 * Layer8 Aug/17/2026
 */
#ifndef TAB5_DISK_CONTROL_H
#define TAB5_DISK_CONTROL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int tab5_disk_mount_catalog(int drive, size_t catalog_index);

/* Find an XDF in the root catalog by basename (case-insensitive). */
int tab5_disk_find_named(const char *basename, char *out_path, size_t out_size);

/* Select (but do not mount) the next root .XDF/.DIM boot image. */
int tab5_disk_next_boot_media(const char *current_path,
                              char *out_path,
                              size_t out_size);

/*
 * Mount the first root-catalog XDF that is not exclude_path.
 * Used for the first real two-drive regression: A:=Human68k, B:=another XDF.
 * Returns non-zero on a mount request and optionally copies the selected path.
 */
int tab5_disk_mount_first_other(int drive,
                                const char *exclude_path,
                                char *out_path,
                                size_t out_size);


/* Direct runtime media changer paths; emulation-task only. */
int tab5_disk_mount_path(int drive, const char *path);
int tab5_hdd_mount_path(int target, const char *path);
int tab5_hdd_eject(int target);

/* Emulation-task-only runtime disk control. */
int tab5_disk_eject(int drive);
int tab5_disk_set_write_protect(int drive, int enabled);
int tab5_disk_mount_next_other(int drive,
                               const char *exclude_path,
                               const char *current_path,
                               char *out_path,
                               size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* TAB5_DISK_CONTROL_H */
