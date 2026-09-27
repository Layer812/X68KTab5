/*
 * Tab5 port-specific implementation.
 * Intent: Boot-media selection contract produced by the Tab5 launcher.
 * Layer8 Aug/17/2026
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define TAB5_LAUNCHER_PATH_MAX 512
#define TAB5_FLASH_HUMAN_PATH ":FLASH:HUMAN302.XDF"

typedef enum {
    TAB5_LAUNCH_BOOT_AUTO = 0,
    TAB5_LAUNCH_BOOT_FLOPPY0 = 1,
    TAB5_LAUNCH_BOOT_FLOPPY1 = 2,
    TAB5_LAUNCH_BOOT_HDD0 = 3,
} tab5_launcher_boot_source_t;

typedef struct {
    char floppy0[TAB5_LAUNCHER_PATH_MAX];
    char floppy1[TAB5_LAUNCHER_PATH_MAX];
    char hdd0[TAB5_LAUNCHER_PATH_MAX];
    tab5_launcher_boot_source_t boot_source;
} tab5_launcher_config_t;

/*
 * Build 5.16 Media Setup launcher.
 *
 * The launcher stays entirely outside the PX68K core.  It only returns the
 * media configuration that should be mounted after WinX68k_Reset().
 *
 * Proven in this build:
 *   - FLOPPY 0: XDF/DIM
 *   - FLOPPY 1: XDF/DIM
 *   - HDD 0:    HDS as SCSI target 0 (read/write)
 *   - BOOT:     FLOPPY0, FLOPPY1 or HDD0 selected directly in Media Setup
 *
 * FLOPPY2/3 are absent because the PX68K backend exposes two drives.
 */
int tab5_launcher_run(const char *human_path, tab5_launcher_config_t *out_config);

#ifdef __cplusplus
}
#endif
