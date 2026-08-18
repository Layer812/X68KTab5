/*
 * Tab5 PANIC launcher/runtime bridge.
 * Intent: Reuse the proven PX68K 6.00 guest/host stack to run the original
 * PANIC.X while keeping PAN file selection in the native Tab5 GUI.
 * Layer8 Aug/17/2026
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Select a random .PAN anywhere below /sdcard. */
int tab5_panic_select_random(char *out_path, size_t out_size);

/* Native touch filer: recursively scans SD and returns one selected .PAN. */
int tab5_panic_select_file(char *out_path, size_t out_size);

/* Remove only temporary files created by PANIC integration. Safe before the launcher and guest reset. */
void tab5_panic_cleanup_staging(void);

/* Game-screen PANIC button: choose + stage one random PAN without rebooting ESP32-P4. */
int tab5_panic_prepare_random_runtime(char *out_path, size_t out_size);

/* Prepare the embedded player and 8.3-safe staged PAN in the HostFS root. */
int tab5_panic_prepare_runtime(const char *selected_pan_path);

/* Create the short HostFS batch command after the runtime drive letter is known. */
int tab5_panic_prepare_batch(char drive_letter);

/* Playback touch result: 0=none, 1=SPACE tap, 2=return-to-GUI long hold. */
int tab5_panic_poll_playback_touch(void);
void tab5_panic_reset_touch(void);

#ifdef __cplusplus
}
#endif
