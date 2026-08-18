/*
 * Tab5 port-specific implementation.
 * Intent: Tab5 host-side floppy/media control used by the launcher hotkeys without changing guest disk semantics.
 * Layer8 Aug/17/2026
 */
#include "tab5_disk_control.h"

#include <string.h>
#include <strings.h>

#include "tab5_sd.h"

extern int WinX68k_MountFloppy(int drive, const char *path);
extern int WinX68k_EjectFloppy(int drive);
extern int WinX68k_SetFloppyReadOnly(int drive, int readonly);
extern int WinX68k_MountSCSIHD(int target, const char *path, int readonly);
extern void WinX68k_EjectSCSIHD(int target);


static const char *path_basename(const char *path)
{
    const char *base;

    if (!path)
        return "";

    base = strrchr(path, '/');
    return base ? base + 1 : path;
}

int tab5_disk_find_named(const char *basename, char *out_path, size_t out_size)
{
    char path[512];
    const size_t count = tab5_sd_floppy_count();

    if (out_path && out_size)
        out_path[0] = '\0';

    if (!basename || !basename[0] || !out_path || out_size == 0)
        return 0;

    for (size_t i = 0; i < count; ++i)
    {
        if (!tab5_sd_floppy_path(i, path, sizeof(path)))
            continue;

        if (strcasecmp(path_basename(path), basename) != 0)
            continue;

        strncpy(out_path, path, out_size - 1u);
        out_path[out_size - 1u] = '\0';
        return 1;
    }

    return 0;
}

int tab5_disk_mount_catalog(int drive, size_t catalog_index)
{
    char path[512];

    if (drive < 0 || drive > 1)
        return 0;

    if (!tab5_sd_xdf_path(catalog_index, path, sizeof(path)))
        return 0;

    return WinX68k_MountFloppy(drive, path);
}

int tab5_disk_mount_first_other(int drive,
                                const char *exclude_path,
                                char *out_path,
                                size_t out_size)
{
    char path[512];
    const size_t count = tab5_sd_xdf_count();

    if (out_path && out_size)
        out_path[0] = '\0';

    if (drive < 0 || drive > 1)
        return 0;

    for (size_t i = 0; i < count; ++i)
    {
        if (!tab5_sd_xdf_path(i, path, sizeof(path)))
            continue;

        if (exclude_path && exclude_path[0] && !strcmp(path, exclude_path))
            continue;

        if (!WinX68k_MountFloppy(drive, path))
            continue;

        if (out_path && out_size)
        {
            strncpy(out_path, path, out_size - 1u);
            out_path[out_size - 1u] = '\0';
        }

        return 1;
    }

    return 0;
}


/* Intent: FILE mode hot-swaps removable media through the same authoritative
 * PX68K mount helpers used by libretro disk control, but never resets the guest.
 * Layer8 Aug/17/2026
 */
int tab5_disk_mount_path(int drive, const char *path)
{
    if (drive < 0 || drive > 1 || !path || !path[0]) return 0;
    return WinX68k_MountFloppy(drive, path);
}

int tab5_hdd_mount_path(int target, const char *path)
{
    if (target < 0 || target > 7 || !path || !path[0]) return 0;
    return WinX68k_MountSCSIHD(target, path, 0);
}

int tab5_hdd_eject(int target)
{
    if (target < 0 || target > 7) return 0;
    /* Intent: Reuse the existing PX68K SCSI eject helper; runtime FILE mode
     * changes media state only and never resets or reboots the guest.
     * Layer8 Aug/17/2026
     */
    WinX68k_EjectSCSIHD(target);
    return 1;
}

int tab5_disk_set_write_protect(int drive, int enabled)
{
    if (drive < 0 || drive > 1)
        return 0;
    return WinX68k_SetFloppyReadOnly(drive, enabled ? 1 : 0);
}

int tab5_disk_eject(int drive)
{
    if (drive < 0 || drive > 1)
        return 0;

    return WinX68k_EjectFloppy(drive);
}

int tab5_disk_mount_next_other(int drive,
                               const char *exclude_path,
                               const char *current_path,
                               char *out_path,
                               size_t out_size)
{
    char path[512];
    const size_t count = tab5_sd_xdf_count();
    size_t start = 0;

    if (out_path && out_size)
        out_path[0] = '\0';

    if (drive < 0 || drive > 1 || count == 0)
        return 0;

    if (current_path && current_path[0])
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (tab5_sd_xdf_path(i, path, sizeof(path)) &&
                !strcmp(path, current_path))
            {
                start = (i + 1u) % count;
                break;
            }
        }
    }

    for (size_t n = 0; n < count; ++n)
    {
        const size_t i = (start + n) % count;

        if (!tab5_sd_xdf_path(i, path, sizeof(path)))
            continue;

        if (exclude_path && exclude_path[0] && !strcmp(path, exclude_path))
            continue;

        if (current_path && current_path[0] && !strcmp(path, current_path) && count > 1u)
            continue;

        if (!WinX68k_MountFloppy(drive, path))
            continue;

        if (out_path && out_size)
        {
            strncpy(out_path, path, out_size - 1u);
            out_path[out_size - 1u] = '\0';
        }

        return 1;
    }

    return 0;
}

int tab5_disk_next_boot_media(const char *current_path,
                              char *out_path,
                              size_t out_size)
{
    char path[512];
    const size_t count = tab5_sd_floppy_count();
    size_t start = 0;

    if (out_path && out_size)
        out_path[0] = '\0';

    if (!out_path || out_size == 0 || count == 0)
        return 0;

    if (current_path && current_path[0])
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (tab5_sd_floppy_path(i, path, sizeof(path)) &&
                !strcmp(path, current_path))
            {
                start = (i + 1u) % count;
                break;
            }
        }
    }

    for (size_t n = 0; n < count; ++n)
    {
        const size_t i = (start + n) % count;

        if (!tab5_sd_floppy_path(i, path, sizeof(path)))
            continue;

        if (current_path && current_path[0] && !strcmp(path, current_path) && count > 1u)
            continue;

        strncpy(out_path, path, out_size - 1u);
        out_path[out_size - 1u] = '\0';
        return 1;
    }

    return 0;
}
