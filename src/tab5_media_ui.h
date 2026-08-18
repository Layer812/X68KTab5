#pragma once

#include <stddef.h>
#include <stdint.h>

#define TAB5_MEDIA_UI_PATH_MAX 512
#define TAB5_MEDIA_UI_ROWS_PER_PAGE 7

typedef enum {
    TAB5_MEDIA_UI_XDF = 0,
    TAB5_MEDIA_UI_DIM = 1,
    TAB5_MEDIA_UI_HDS = 2,
} tab5_media_ui_type_t;

typedef enum {
    TAB5_MEDIA_UI_FDD0 = 0,
    TAB5_MEDIA_UI_FDD1 = 1,
    TAB5_MEDIA_UI_HDD0 = 2,
} tab5_media_ui_slot_t;

typedef struct {
    uint8_t type;
    char path[TAB5_MEDIA_UI_PATH_MAX];
} tab5_media_ui_entry_t;

typedef struct {
    const char *back_label;
    const char *header_note;
    const char *floppy0;
    const char *floppy1;
    const char *hdd0;
    int boot_source; /* TAB5_LAUNCH_BOOT_* value. */
    const char *bottom_left_label;
    const char *bottom_left_note;
    int bottom_left_enabled;
    const char *bottom_right_label;
    const char *bottom_right_note;
    int bottom_right_enabled;
    const char *footer_note;
} tab5_media_ui_setup_view_t;

typedef enum {
    TAB5_MEDIA_UI_SETUP_NONE = 0,
    TAB5_MEDIA_UI_SETUP_BACK,
    TAB5_MEDIA_UI_SETUP_BOOT_FDD0,
    TAB5_MEDIA_UI_SETUP_BOOT_FDD1,
    TAB5_MEDIA_UI_SETUP_DISABLED_FDD23,
    TAB5_MEDIA_UI_SETUP_BOOT_HDD0,
    TAB5_MEDIA_UI_SETUP_CHANGE_FDD0,
    TAB5_MEDIA_UI_SETUP_CHANGE_FDD1,
    TAB5_MEDIA_UI_SETUP_CHANGE_HDD0,
    TAB5_MEDIA_UI_SETUP_BOTTOM_LEFT,
    TAB5_MEDIA_UI_SETUP_BOTTOM_RIGHT,
} tab5_media_ui_setup_hit_t;

typedef enum {
    TAB5_MEDIA_UI_BROWSER_NONE = 0,
    TAB5_MEDIA_UI_BROWSER_BACK,
    TAB5_MEDIA_UI_BROWSER_SELECT,
    TAB5_MEDIA_UI_BROWSER_PREV,
    TAB5_MEDIA_UI_BROWSER_NEXT,
} tab5_media_ui_browser_hit_t;

#ifdef __cplusplus
extern "C" {
#endif

const char *tab5_media_ui_basename(const char *path);
size_t tab5_media_ui_scan_sd_root(tab5_media_ui_entry_t *items, size_t capacity);
int tab5_media_ui_type_allowed(uint8_t type, tab5_media_ui_slot_t slot);
size_t tab5_media_ui_filtered_count(const tab5_media_ui_entry_t *items, size_t count,
                                    tab5_media_ui_slot_t slot);
const tab5_media_ui_entry_t *tab5_media_ui_filtered_at(const tab5_media_ui_entry_t *items,
                                                        size_t count,
                                                        tab5_media_ui_slot_t slot,
                                                        size_t wanted);
size_t tab5_media_ui_browser_pages(const tab5_media_ui_entry_t *items, size_t count,
                                   tab5_media_ui_slot_t slot);

void tab5_media_ui_draw_setup(const tab5_media_ui_setup_view_t *view);
void tab5_media_ui_draw_browser(const tab5_media_ui_entry_t *items, size_t count,
                                tab5_media_ui_slot_t slot, size_t page);

tab5_media_ui_setup_hit_t tab5_media_ui_setup_hit(int x, int y);
tab5_media_ui_browser_hit_t tab5_media_ui_browser_hit(const tab5_media_ui_entry_t *items,
                                                       size_t count,
                                                       tab5_media_ui_slot_t slot,
                                                       size_t page,
                                                       int x, int y,
                                                       size_t *filtered_index,
                                                       int *select_empty);

#ifdef __cplusplus
}
#endif
