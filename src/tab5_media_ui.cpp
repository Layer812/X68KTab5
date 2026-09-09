#include "tab5_media_ui.h"
#include "tab5_launcher.h"
#include "tab5_branding.h"

#include <M5Unified.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static bool inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static const char *badge_for(uint8_t type)
{
    switch (type) {
        case TAB5_MEDIA_UI_XDF: return "XDF";
        case TAB5_MEDIA_UI_DIM: return "DIM";
        case TAB5_MEDIA_UI_HDS: return "HDS";
        default: return "?";
    }
}

static const char *slot_title(tab5_media_ui_slot_t slot)
{
    switch (slot) {
        case TAB5_MEDIA_UI_FDD0: return "SELECT MEDIA FOR FLOPPY 0";
        case TAB5_MEDIA_UI_FDD1: return "SELECT MEDIA FOR FLOPPY 1";
        case TAB5_MEDIA_UI_HDD0: return "SELECT MEDIA FOR HDD 0";
        default: return "SELECT MEDIA";
    }
}

static void draw_radio(int x, int y, bool selected, bool enabled)
{
    const uint16_t c = enabled ? 0xBDF7 : 0x528A;
    M5.Display.drawCircle(x, y, 13, c);
    if (selected) M5.Display.fillCircle(x, y, 7, c);
}

static void draw_setup_row(int y, const char *label, const char *path,
                           bool boot_selected, bool boot_enabled,
                           bool change_enabled, const char *note)
{
    const uint16_t fill = change_enabled ? 0x0841 : 0x1082;
    M5.Display.fillRoundRect(24, y, 1232, 64, 10, fill);
    M5.Display.drawRoundRect(24, y, 1232, 64, 10, change_enabled ? 0x39E7 : 0x2945);
    draw_radio(52, y + 32, boot_selected, boot_enabled);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(change_enabled ? TFT_WHITE : 0x632C, fill);
    M5.Display.setCursor(82, y + 12);
    M5.Display.print(label);
    M5.Display.setTextColor(change_enabled ? 0xBDF7 : 0x632C, fill);
    M5.Display.setCursor(300, y + 12);
    M5.Display.printf("[ %-34.34s ]", tab5_media_ui_basename(path));
    M5.Display.fillRoundRect(1040, y + 9, 180, 46, 8, change_enabled ? 0x2104 : 0x18E3);
    M5.Display.setTextColor(change_enabled ? TFT_WHITE : 0x632C,
                            change_enabled ? 0x2104 : 0x18E3);
    M5.Display.setCursor(1075, y + 21);
    M5.Display.print("CHANGE");
    if (note) {
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0x632C, fill);
        M5.Display.setCursor(300, y + 43);
        M5.Display.print(note);
    }
}

} // namespace


extern "C" size_t tab5_media_ui_scan_sd_root(tab5_media_ui_entry_t *items, size_t capacity)
{
    if (!items || !capacity) return 0;
    size_t count = 0;
    DIR *dir = opendir("/sdcard");
    if (!dir) return 0;
    struct dirent *ent;
    unsigned scanned = 0;
    while ((ent = readdir(dir)) != nullptr && count < capacity) {
        if (!std::strcmp(ent->d_name, ".") || !std::strcmp(ent->d_name, "..")) continue;
        if (((++scanned) & 31u) == 0u) vTaskDelay(1);
        const char *dot = std::strrchr(ent->d_name, '.');
        if (!dot || !dot[1]) continue;
        char ext[4] = {};
        size_t n = std::strlen(dot + 1);
        if (n != 3) continue;
        for (size_t i=0; i<3; ++i) ext[i] = (char)std::tolower((unsigned char)dot[1+i]);
        uint8_t type;
        if (!std::strcmp(ext, "xdf")) type = TAB5_MEDIA_UI_XDF;
        else if (!std::strcmp(ext, "dim")) type = TAB5_MEDIA_UI_DIM;
        else if (!std::strcmp(ext, "hds")) type = TAB5_MEDIA_UI_HDS;
        else continue;
        char full[TAB5_MEDIA_UI_PATH_MAX];
        if (std::snprintf(full, sizeof(full), "/sdcard/%s", ent->d_name) >= (int)sizeof(full)) continue;
        struct stat st = {};
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        items[count].type = type;
        std::snprintf(items[count].path, sizeof(items[count].path), "%s", full);
        ++count;
    }
    closedir(dir);
    std::sort(items, items + count, [](const tab5_media_ui_entry_t &a, const tab5_media_ui_entry_t &b) {
        const unsigned char *pa = reinterpret_cast<const unsigned char *>(a.path);
        const unsigned char *pb = reinterpret_cast<const unsigned char *>(b.path);
        while (*pa && *pb) {
            const int ca = std::tolower(*pa++);
            const int cb = std::tolower(*pb++);
            if (ca != cb) return ca < cb;
        }
        return *pa < *pb;
    });
    return count;
}

extern "C" const char *tab5_media_ui_basename(const char *path)
{
    if (!path || !path[0]) return "Empty";
    const char *p = std::strrchr(path, '/');
    return p ? p + 1 : path;
}

extern "C" int tab5_media_ui_type_allowed(uint8_t type, tab5_media_ui_slot_t slot)
{
    if (slot == TAB5_MEDIA_UI_HDD0) return type == TAB5_MEDIA_UI_HDS;
    return type == TAB5_MEDIA_UI_XDF || type == TAB5_MEDIA_UI_DIM;
}

extern "C" size_t tab5_media_ui_filtered_count(const tab5_media_ui_entry_t *items,
                                                size_t count,
                                                tab5_media_ui_slot_t slot)
{
    size_t n = 0;
    for (size_t i = 0; items && i < count; ++i)
        if (tab5_media_ui_type_allowed(items[i].type, slot)) ++n;
    return n;
}

extern "C" const tab5_media_ui_entry_t *tab5_media_ui_filtered_at(
    const tab5_media_ui_entry_t *items, size_t count, tab5_media_ui_slot_t slot, size_t wanted)
{
    for (size_t i = 0, n = 0; items && i < count; ++i) {
        if (!tab5_media_ui_type_allowed(items[i].type, slot)) continue;
        if (n++ == wanted) return &items[i];
    }
    return nullptr;
}

extern "C" size_t tab5_media_ui_browser_pages(const tab5_media_ui_entry_t *items,
                                                size_t count,
                                                tab5_media_ui_slot_t slot)
{
    const size_t rows_total = tab5_media_ui_filtered_count(items, count, slot) + 1;
    return std::max<size_t>(1, (rows_total + TAB5_MEDIA_UI_ROWS_PER_PAGE - 1) /
                              TAB5_MEDIA_UI_ROWS_PER_PAGE);
}

extern "C" void tab5_media_ui_draw_setup(const tab5_media_ui_setup_view_t *view)
{
    if (!view) return;
    const bool fdd0_boot = view->boot_source == TAB5_LAUNCH_BOOT_FLOPPY0;
    const bool fdd1_boot = view->boot_source == TAB5_LAUNCH_BOOT_FLOPPY1;
    const bool hdd0_boot = view->boot_source == TAB5_LAUNCH_BOOT_HDD0;

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(34, 20);
    M5.Display.print("X68000 MEDIA SETUP");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x8410, TFT_BLACK);
    M5.Display.setCursor(850, 36);
    if (view->header_note && view->header_note[0]) M5.Display.print(view->header_note);

    /* Build 6.12j: keep Media Setup intentionally clean. Disabled rows are
     * self-explanatory from their grey treatment; tiny per-row debug/help
     * captions were removed from the release UI. */
    draw_setup_row(76,  "FLOPPY 0", view->floppy0, fdd0_boot, true, true, nullptr);
    draw_setup_row(146, "FLOPPY 1", view->floppy1, fdd1_boot, true, true, nullptr);
    draw_setup_row(216, "MONITOR", "MULTISCAN AUTO   15 / 24 / 31 kHz", false, false, false, nullptr);
    draw_setup_row(286, "SYSTEM", "Standard 12 MHz   Audio 44.1 kHz", false, false, false, nullptr);
    draw_setup_row(356, "HDD 0", view->hdd0, hdd0_boot, true, true, nullptr);

    const uint16_t lfill = view->bottom_left_enabled ? 0x1948 : 0x18E3;
    M5.Display.fillRoundRect(110, 455, 430, 86, 14, lfill);
    M5.Display.drawRoundRect(110, 455, 430, 86, 14,
                             view->bottom_left_enabled ? 0x5E7F : 0x3186);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(view->bottom_left_enabled ? TFT_WHITE : 0x632C, lfill);
    const char *left_label = view->bottom_left_label ? view->bottom_left_label : "BACK";
    M5.Display.setCursor(110 + (430 - M5.Display.textWidth(left_label)) / 2, 480);
    M5.Display.print(left_label);

    const uint16_t rfill = view->bottom_right_enabled ? 0x1948 : 0x18E3;
    M5.Display.fillRoundRect(740, 455, 430, 86, 14, rfill);
    M5.Display.drawRoundRect(740, 455, 430, 86, 14,
                             view->bottom_right_enabled ? 0x5E7F : 0x3186);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(view->bottom_right_enabled ? TFT_WHITE : 0x632C, rfill);
    const char *right_label = view->bottom_right_label ? view->bottom_right_label : "(re)BOOT";
    M5.Display.setCursor(740 + (430 - M5.Display.textWidth(right_label)) / 2, 480);
    M5.Display.print(right_label);

    /* Release footer: product provenance belongs here instead of transient
     * debug/help text. Keep the same small type on launcher and runtime UI. */
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x630C, TFT_BLACK);
    M5.Display.setCursor(50, 676);
    M5.Display.print(X68K_TAB_CREDIT);
}

extern "C" void tab5_media_ui_draw_browser(const tab5_media_ui_entry_t *items,
                                             size_t count,
                                             tab5_media_ui_slot_t slot,
                                             size_t page)
{
    const int lcd_w = M5.Display.width();
    const size_t filtered = tab5_media_ui_filtered_count(items, count, slot);
    const size_t rows_total = filtered + 1;
    const size_t pages = tab5_media_ui_browser_pages(items, count, slot);
    if (page >= pages) page = pages - 1;

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.fillRoundRect(20, 18, 150, 48, 12, 0x2104);
    M5.Display.drawRoundRect(20, 18, 150, 48, 12, 0x8410);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, 0x2104);
    M5.Display.setCursor(58, 31);
    M5.Display.print("< BACK");
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(210, 24);
    M5.Display.print(slot_title(slot));

    char page_text[64];
    std::snprintf(page_text, sizeof(page_text), "%u choice(s)   page %u/%u",
                  (unsigned)rows_total, (unsigned)(page + 1), (unsigned)pages);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x8410, TFT_BLACK);
    M5.Display.setCursor(lcd_w - 300, 45);
    M5.Display.print(page_text);

    /* PX68K_R56S2_PSRAM_UI_STRING_BRIDGE
     * R23 intentionally keeps the cold media catalog in PSRAM. Keep that
     * memory policy, but do not hand PSRAM-backed C strings directly to the
     * M5GFX Print path on ESP32-P4. Stage only the currently drawn row into
     * this function's internal stack first. Selection/filtering still uses
     * the original catalog and is behavior-identical. */
    char r56s2_path_buf[TAB5_MEDIA_UI_PATH_MAX];
    char r56s2_name_buf[128];

    const size_t first = page * TAB5_MEDIA_UI_ROWS_PER_PAGE;
    for (int r = 0; r < TAB5_MEDIA_UI_ROWS_PER_PAGE; ++r) {
        const size_t fidx = first + (size_t)r;
        if (fidx >= rows_total) break;
        const int y = 92 + r * 72;
        const bool empty = fidx == 0;
        const tab5_media_ui_entry_t *e = empty ? nullptr :
            tab5_media_ui_filtered_at(items, count, slot, fidx - 1);
        if (!empty && !e) continue;
        const uint16_t fill = 0x0841;
        const uint16_t badge_fill = empty ? 0x2104 : (e->type == TAB5_MEDIA_UI_HDS ? 0x4010 : 0x1948);
        M5.Display.fillRoundRect(32, y, lcd_w - 64, 64, 10, fill);
        M5.Display.drawRoundRect(32, y, lcd_w - 64, 64, 10, 0x39E7);
        M5.Display.fillRoundRect(50, y + 14, 78, 36, 8, badge_fill);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(TFT_WHITE, badge_fill);
        M5.Display.setCursor(empty ? 60 : 65, y + 23);
        M5.Display.print(empty ? "----" : badge_for(e->type));
        const char *r56s2_name = "Empty / eject";
        const char *r56s2_path = "Leave this slot empty";
        if (!empty) {
            std::snprintf(r56s2_path_buf, sizeof(r56s2_path_buf), "%s", e->path);
            std::snprintf(r56s2_name_buf, sizeof(r56s2_name_buf), "%s",
                          tab5_media_ui_basename(r56s2_path_buf));
            r56s2_name = r56s2_name_buf;
            r56s2_path = r56s2_path_buf;
        }
        M5.Display.setTextColor(TFT_WHITE, fill);
        M5.Display.setCursor(150, y + 11);
        M5.Display.print(r56s2_name);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(0x8C71, fill);
        M5.Display.setCursor(151, y + 42);
        M5.Display.print(r56s2_path);
    }

    const bool has_prev = page > 0;
    const bool has_next = page + 1 < pages;
    M5.Display.fillRoundRect(300, 610, 250, 60, 12, has_prev ? 0x2104 : 0x1082);
    M5.Display.fillRoundRect(730, 610, 250, 60, 12, has_next ? 0x2104 : 0x1082);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(has_prev ? TFT_WHITE : 0x632C, has_prev ? 0x2104 : 0x1082);
    M5.Display.setCursor(355, 630);
    M5.Display.print("< PREV");
    M5.Display.setTextColor(has_next ? TFT_WHITE : 0x632C, has_next ? 0x2104 : 0x1082);
    M5.Display.setCursor(790, 630);
    M5.Display.print("NEXT >");
}

extern "C" tab5_media_ui_setup_hit_t tab5_media_ui_setup_hit(int x, int y)
{
    /* Build 6.12j: setup-level BACK lives in the shared bottom-left button. */
    if (inside(x, y, 24, 76, 80, 64)) return TAB5_MEDIA_UI_SETUP_BOOT_FDD0;
    if (inside(x, y, 24, 146, 80, 64)) return TAB5_MEDIA_UI_SETUP_BOOT_FDD1;
    if (inside(x, y, 24, 216, 1232, 64) || inside(x, y, 24, 286, 1232, 64))
        return TAB5_MEDIA_UI_SETUP_NONE;
    if (inside(x, y, 24, 356, 80, 64)) return TAB5_MEDIA_UI_SETUP_BOOT_HDD0;
    if (inside(x, y, 1040, 85, 180, 46)) return TAB5_MEDIA_UI_SETUP_CHANGE_FDD0;
    if (inside(x, y, 1040, 155, 180, 46)) return TAB5_MEDIA_UI_SETUP_CHANGE_FDD1;
    if (inside(x, y, 1040, 365, 180, 46)) return TAB5_MEDIA_UI_SETUP_CHANGE_HDD0;
    if (inside(x, y, 110, 455, 430, 86)) return TAB5_MEDIA_UI_SETUP_BOTTOM_LEFT;
    if (inside(x, y, 740, 455, 430, 86)) return TAB5_MEDIA_UI_SETUP_BOTTOM_RIGHT;
    return TAB5_MEDIA_UI_SETUP_NONE;
}

extern "C" tab5_media_ui_browser_hit_t tab5_media_ui_browser_hit(
    const tab5_media_ui_entry_t *items, size_t count, tab5_media_ui_slot_t slot,
    size_t page, int x, int y, size_t *filtered_index, int *select_empty)
{
    if (filtered_index) *filtered_index = 0;
    if (select_empty) *select_empty = 0;
    if (inside(x, y, 20, 18, 150, 48)) return TAB5_MEDIA_UI_BROWSER_BACK;

    const size_t rows_total = tab5_media_ui_filtered_count(items, count, slot) + 1;
    const size_t pages = tab5_media_ui_browser_pages(items, count, slot);
    if (y >= 92 && y < 92 + TAB5_MEDIA_UI_ROWS_PER_PAGE * 72 && x >= 32 && x < M5.Display.width() - 32) {
        const size_t row = (size_t)((y - 92) / 72);
        const size_t fidx = page * TAB5_MEDIA_UI_ROWS_PER_PAGE + row;
        if (fidx < rows_total) {
            if (fidx == 0) {
                if (select_empty) *select_empty = 1;
            } else if (filtered_index) {
                *filtered_index = fidx - 1;
            }
            return TAB5_MEDIA_UI_BROWSER_SELECT;
        }
    }
    if (inside(x, y, 300, 610, 250, 60) && page > 0)
        return TAB5_MEDIA_UI_BROWSER_PREV;
    if (inside(x, y, 730, 610, 250, 60) && page + 1 < pages)
        return TAB5_MEDIA_UI_BROWSER_NEXT;
    return TAB5_MEDIA_UI_BROWSER_NONE;
}
