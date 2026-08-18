/*
 * Tab5 port-specific implementation.
 * Intent: Touch launcher for direct FDD0/HDD0 boot selection on Tab5 before the guest task starts.
 * Layer8 Aug/17/2026
 */
#include "tab5_launcher.h"
#include "tab5_panic.h"
#include "tab5_media_ui.h"
#include "tab5_branding.h"

#include <M5Unified.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "TAB5_LAUNCH";

namespace {

constexpr size_t kMaxMedia = 128;

using SlotKind = tab5_media_ui_slot_t;
using MediaEntry = tab5_media_ui_entry_t;

struct MediaList {
    MediaEntry *items = nullptr;
    size_t count = 0;
};

static const char *badge_for(uint8_t type)
{
    switch (type) {
        case TAB5_MEDIA_UI_XDF: return "XDF";
        case TAB5_MEDIA_UI_DIM: return "DIM";
        case TAB5_MEDIA_UI_HDS: return "HDS";
        default: return "?";
    }
}

static void scan_root(MediaList &list)
{
    list.count = tab5_media_ui_scan_sd_root(list.items, kMaxMedia);
}

static void sort_media(MediaList &list)
{
    /* Shared scanner already returns the common launcher/runtime order. */
    (void)list;
}

static void wait_touch_release()
{
    for (int i = 0; i < 120; ++i) {
        M5.update();
        if (!M5.Touch.getCount()) break;
        vTaskDelay(pdMS_TO_TICKS(8) ? pdMS_TO_TICKS(8) : 1);
    }
}

static bool get_click(int &x, int &y)
{
    M5.update();
    auto touch = M5.Touch.getDetail();
    if (!touch.wasClicked()) return false;
    x = touch.x;
    y = touch.y;
    return true;
}

static bool inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void draw_centered(const char *text, int y, int text_size, uint16_t color)
{
    M5.Display.setTextSize(text_size);
    M5.Display.setTextColor(color, TFT_BLACK);
    int x = (M5.Display.width() - M5.Display.textWidth(text)) / 2;
    if (x < 8) x = 8;
    M5.Display.setCursor(x, y);
    M5.Display.print(text);
}

static void toast(const char *line1, const char *line2)
{
    const int x = 220, y = 270, w = M5.Display.width() - 440, h = 170;
    M5.Display.fillRoundRect(x, y, w, h, 18, 0x2104);
    M5.Display.drawRoundRect(x, y, w, h, 18, 0x8410);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, 0x2104);
    M5.Display.setCursor(x + 28, y + 34);
    M5.Display.print(line1 ? line1 : "");
    M5.Display.setTextColor(0x9CD3, 0x2104);
    M5.Display.setCursor(x + 28, y + 84);
    M5.Display.print(line2 ? line2 : "");
    vTaskDelay(pdMS_TO_TICKS(850));
    wait_touch_release();
}

static void draw_card(int x, int y, int w, int h, const char *title, const char *subtitle, bool enabled)
{
    const uint16_t fill = enabled ? 0x1082 : 0x18E3;
    const uint16_t border = enabled ? 0x5E7F : 0x4208;
    const uint16_t tc = enabled ? TFT_WHITE : 0x8410;
    const uint16_t sc = enabled ? 0xBDF7 : 0x632C;
    M5.Display.fillRoundRect(x, y, w, h, 18, fill);
    M5.Display.drawRoundRect(x, y, w, h, 18, border);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(tc, fill);
    M5.Display.setCursor(x + 32, y + 24);
    M5.Display.print(title);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(sc, fill);
    M5.Display.setCursor(x + 32, y + 78);
    M5.Display.print(subtitle);
}

static void draw_main_menu(size_t media_count)
{
    M5.Display.fillScreen(TFT_BLACK);

    /* Build 6.12c: the launcher has no separate PANIC shortcut button.
     * PANIC PLAYER remains the single native filer entry; the running-game
     * left-side PANIC button is intentionally unchanged. */
    draw_centered(X68K_TAB_APP_NAME, 28, 4, TFT_WHITE);
    draw_centered(X68K_TAB_SUBTITLE, 80, 2, 0x9CD3);
    const int x = 80, w = M5.Display.width() - 160;
    draw_card(x, 138, w, 142, "1. X68000 MEDIA SETUP",
              media_count ? "Configure FDD0 / FDD1 / HDD0, then boot" : "No media found on SD", media_count != 0);
    draw_card(x, 300, w, 142, "2. HUMAN68K QUICK BOOT", "FLASH Human302 + SD HostFS", true);
    draw_card(x, 462, w, 142, "3. PANIC PLAYER",
              "Browse all .PAN files on SD and play one", true);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x630C, TFT_BLACK);
    M5.Display.setCursor(50, 676);
    M5.Display.print(X68K_TAB_CREDIT);
}

static const MediaEntry *filtered_at(const MediaList &list, SlotKind slot, size_t wanted)
{
    return tab5_media_ui_filtered_at(list.items, list.count, slot, wanted);
}

static void draw_media_browser(const MediaList &list, SlotKind slot, size_t page)
{
    tab5_media_ui_draw_browser(list.items, list.count, slot, page);
}

static bool select_media(const MediaList &list, SlotKind slot, char *out_path, size_t out_size)
{
    size_t page = 0;
    draw_media_browser(list, slot, page);
    wait_touch_release();
    for (;;) {
        int x = 0, y = 0;
        if (!get_click(x, y)) { vTaskDelay(pdMS_TO_TICKS(8) ? pdMS_TO_TICKS(8) : 1); continue; }
        size_t selected = 0;
        int empty = 0;
        switch (tab5_media_ui_browser_hit(list.items, list.count, slot, page, x, y, &selected, &empty)) {
            case TAB5_MEDIA_UI_BROWSER_BACK:
                wait_touch_release();
                return false;
            case TAB5_MEDIA_UI_BROWSER_SELECT:
                if (empty) out_path[0] = '\0';
                else {
                    const MediaEntry *e = filtered_at(list, slot, selected);
                    if (e) std::snprintf(out_path, out_size, "%s", e->path);
                }
                ESP_LOGI(TAG, "Media slot %u selected: %s", (unsigned)slot,
                         out_path[0] ? out_path : "<empty>");
                wait_touch_release();
                return true;
            case TAB5_MEDIA_UI_BROWSER_PREV:
                if (page > 0) --page;
                draw_media_browser(list, slot, page);
                break;
            case TAB5_MEDIA_UI_BROWSER_NEXT:
                ++page;
                draw_media_browser(list, slot, page);
                break;
            default:
                break;
        }
    }
}

static void draw_setup(const tab5_launcher_config_t &cfg)
{
    const bool can_boot = (cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0) ? (cfg.hdd0[0] != '\0')
                                                                  : (cfg.floppy0[0] != '\0');
    tab5_media_ui_setup_view_t view = {};
    view.back_label = nullptr;
    view.header_note = "FDD0 / FDD1 / HDD0 media selection";
    view.floppy0 = cfg.floppy0;
    view.floppy1 = cfg.floppy1;
    view.hdd0 = cfg.hdd0;
    view.boot_source = cfg.boot_source;
    view.bottom_left_label = "BACK";
    view.bottom_left_note = nullptr;
    view.bottom_left_enabled = 1;
    view.bottom_right_label = "(re)BOOT";
    view.bottom_right_note = nullptr;
    view.bottom_right_enabled = can_boot ? 1 : 0;
    view.footer_note = nullptr;
    tab5_media_ui_draw_setup(&view);
}

static void choose_default_floppy0(const MediaList &list, const char *human_path, char *dst, size_t dst_size)
{
    /* Build 5.97: normal Media Setup starts with both floppy drives empty.
     * Earlier regression builds implicitly selected SH.XDF for FDD0. */
    (void)list;
    (void)human_path;
    (void)dst_size;
    dst[0] = '\0';
}

static void choose_default_floppy1(const MediaList &list, const char *human_path, char *dst, size_t dst_size)
{
    /* Build 5.97: no implicit DISKMAG1/first-XDF insertion into FDD1. */
    (void)list;
    (void)human_path;
    (void)dst_size;
    dst[0] = '\0';
}

static void configure_panic_boot(tab5_launcher_config_t &cfg, const char *pan_path)
{
    std::memset(&cfg, 0, sizeof(cfg));
    std::snprintf(cfg.floppy0, sizeof(cfg.floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
    std::snprintf(cfg.panic_path, sizeof(cfg.panic_path), "%s", pan_path ? pan_path : "");
    cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
    cfg.mode = TAB5_LAUNCH_MODE_PANIC;
}

static bool setup_screen(const MediaList &list, tab5_launcher_config_t &cfg)
{
    draw_setup(cfg);
    wait_touch_release();
    for (;;) {
        int x = 0, y = 0;
        if (!get_click(x, y)) { vTaskDelay(pdMS_TO_TICKS(8) ? pdMS_TO_TICKS(8) : 1); continue; }
        const tab5_media_ui_setup_hit_t hit = tab5_media_ui_setup_hit(x, y);
        switch (hit) {
            case TAB5_MEDIA_UI_SETUP_BACK:
                wait_touch_release();
                return false;
            case TAB5_MEDIA_UI_SETUP_BOOT_FDD0:
                cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                draw_setup(cfg);
                break;
            case TAB5_MEDIA_UI_SETUP_BOOT_FDD1:
                toast("FLOPPY 1 boot selector", "Media mount works; explicit boot source is not wired yet.");
                draw_setup(cfg);
                break;
            case TAB5_MEDIA_UI_SETUP_DISABLED_FDD23:
                toast("FLOPPY 2 / 3", "Visible for the final UI; backend remains disabled for now.");
                draw_setup(cfg);
                break;
            case TAB5_MEDIA_UI_SETUP_BOOT_HDD0:
                if (!cfg.hdd0[0]) toast("HDD 0 boot", "Select an .HDS image with CHANGE, then choose this radio.");
                else cfg.boot_source = TAB5_LAUNCH_BOOT_HDD0;
                draw_setup(cfg);
                break;
            case TAB5_MEDIA_UI_SETUP_CHANGE_FDD0:
                if (select_media(list, TAB5_MEDIA_UI_FDD0, cfg.floppy0, sizeof(cfg.floppy0)))
                    cfg.boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                draw_setup(cfg);
                break;
            case TAB5_MEDIA_UI_SETUP_CHANGE_FDD1:
                (void)select_media(list, TAB5_MEDIA_UI_FDD1, cfg.floppy1, sizeof(cfg.floppy1));
                draw_setup(cfg);
                break;
            case TAB5_MEDIA_UI_SETUP_CHANGE_HDD0:
                if (select_media(list, TAB5_MEDIA_UI_HDD0, cfg.hdd0, sizeof(cfg.hdd0)))
                    cfg.boot_source = TAB5_LAUNCH_BOOT_HDD0;
                draw_setup(cfg);
                break;
            case TAB5_MEDIA_UI_SETUP_BOTTOM_LEFT:
                /* Shared Media Setup BACK: return to the screen that opened it. */
                wait_touch_release();
                return false;
            case TAB5_MEDIA_UI_SETUP_BOTTOM_RIGHT: {
                const bool hdd_boot = cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0;
                if ((hdd_boot && !cfg.hdd0[0]) || (!hdd_boot && !cfg.floppy0[0])) {
                    toast("Cannot boot yet", hdd_boot ? "Select an .HDS image for HDD 0." : "Select FLOPPY 0.");
                    draw_setup(cfg);
                    break;
                }
                ESP_LOGI(TAG, "Media Setup BOOT: source=%s FDD0=%s FDD1=%s HDD0=%s",
                         cfg.boot_source == TAB5_LAUNCH_BOOT_HDD0 ? "HDD0" : "FLOPPY0",
                         cfg.floppy0[0] ? cfg.floppy0 : "<empty>",
                         cfg.floppy1[0] ? cfg.floppy1 : "<empty>",
                         cfg.hdd0[0] ? cfg.hdd0 : "<empty>");
                wait_touch_release();
                return true;
            }
            default:
                break;
        }
    }
}

} // namespace


extern "C" int tab5_launcher_run(const char *human_path, tab5_launcher_config_t *out_config)
{
    if (!out_config) return 0;
    if (!human_path) human_path = "";
    std::memset(out_config, 0, sizeof(*out_config));
    out_config->boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
    out_config->mode = TAB5_LAUNCH_MODE_PX68K;

    MediaList list;
    list.items = static_cast<MediaEntry *>(heap_caps_calloc(kMaxMedia, sizeof(MediaEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!list.items) list.items = static_cast<MediaEntry *>(std::calloc(kMaxMedia, sizeof(MediaEntry)));
    if (!list.items) { ESP_LOGE(TAG, "Launcher media catalog allocation failed"); return 0; }

    scan_root(list);
    sort_media(list);
    choose_default_floppy0(list, human_path, out_config->floppy0, sizeof(out_config->floppy0));
    choose_default_floppy1(list, human_path, out_config->floppy1, sizeof(out_config->floppy1));

    ESP_LOGI(TAG, "Launcher media scan: %u item(s) (.XDF/.DIM/.HDS)", (unsigned)list.count);
    for (size_t i = 0; i < list.count; ++i) {
        ESP_LOGI(TAG, "Launcher media[%u] [%s]: %s", (unsigned)i, badge_for(list.items[i].type), list.items[i].path);
    }

    wait_touch_release();
    for (;;) {
        draw_main_menu(list.count);
        wait_touch_release();
        bool redraw = false;
        while (!redraw) {
            int x = 0, y = 0;
            if (!get_click(x, y)) { vTaskDelay(pdMS_TO_TICKS(8) ? pdMS_TO_TICKS(8) : 1); continue; }
            if (inside(x, y, 80, 138, M5.Display.width() - 160, 142)) {
                if (!list.count) { toast("No X68000 media found", "Copy .XDF/.DIM/.HDS files to /sdcard."); redraw = true; continue; }
                if (setup_screen(list, *out_config)) {
                    out_config->mode = TAB5_LAUNCH_MODE_PX68K;
                    out_config->panic_path[0] = '\0';
                    std::free(list.items);
                    M5.Display.fillScreen(TFT_BLACK);
                    return 1;
                }
                redraw = true;
                continue;
            }
            if (inside(x, y, 80, 300, M5.Display.width() - 160, 142)) {
                std::memset(out_config, 0, sizeof(*out_config));
                std::snprintf(out_config->floppy0, sizeof(out_config->floppy0), "%s", TAB5_FLASH_HUMAN_PATH);
                out_config->floppy1[0] = '\0';
                out_config->boot_source = TAB5_LAUNCH_BOOT_FLOPPY0;
                out_config->mode = TAB5_LAUNCH_MODE_PX68K;
                ESP_LOGI(TAG, "Launcher Flash Human68k quick boot: FDD0=%s FDD1=%s", out_config->floppy0,
                         out_config->floppy1[0] ? out_config->floppy1 : "<empty>");
                std::free(list.items);
                M5.Display.fillScreen(TFT_BLACK);
                return 1;
            }
            if (inside(x, y, 80, 462, M5.Display.width() - 160, 142)) {
                char pan[TAB5_LAUNCHER_PATH_MAX];
                if (tab5_panic_select_file(pan, sizeof(pan))) {
                    configure_panic_boot(*out_config, pan);
                    ESP_LOGI(TAG, "PANIC filer selection: %s", out_config->panic_path);
                    std::free(list.items);
                    M5.Display.fillScreen(TFT_BLACK);
                    return 1;
                }
                redraw = true;
                continue;
            }
        }
    }
}
