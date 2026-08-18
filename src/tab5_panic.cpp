/*
 * Tab5 port-specific PANIC integration.
 * Intent: Keep the original PanicPlayer user experience (PAN-only recursive
 * filer, tap=SPACE, upper-left hold=menu) while executing PANIC.X on the
 * already validated PX68K 6.00 X68000 runtime instead of carrying a second
 * emulator/audio/video stack.
 * Layer8 Aug/17/2026
 */
#include "tab5_panic.h"

#include <M5Unified.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "panic_x_bin.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

static const char *TAG = "TAB5_PANIC";

namespace {
constexpr const char *kSdRoot = "/sdcard";
constexpr const char *kPlayerHostPath = "/sdcard/P68K.X";
constexpr const char *kPanStageHostPath = "/sdcard/P68K.PAN";
constexpr const char *kBatchHostPath = "/sdcard/P68K.BAT";
constexpr const char *kLegacyRootPlayPath = "/sdcard/PLAY.PAN";
constexpr const char *kLegacyPlayPath = "/sdcard/PX68K/PLAY.PAN";
constexpr const char *kLegacyPlayerPath = "/sdcard/PX68K/PANIC.X";
constexpr const char *kLegacyPendingRestartPath = "/sdcard/P68K.NXT";
constexpr size_t kMaxEntries = 256;
constexpr size_t kDirStackEntries = 32;
constexpr int kRowsPerPage = 5;
constexpr int kHeaderH = 110;
constexpr int kRowH = 92;
constexpr int kBottomY = kHeaderH + kRowsPerPage * kRowH;
constexpr int64_t kReturnHoldUs = 1200000;

struct PanEntry {
    char full_path[512];
    char rel_path[384];
    uint32_t size;
};

static bool s_touch_active = false;
static bool s_touch_started_in_corner = false;
static int64_t s_touch_start_us = 0;

static bool ends_with_pan(const char *name)
{
    if (!name) return false;
    const size_t n = std::strlen(name);
    return n >= 4 && strcasecmp(name + n - 4, ".PAN") == 0;
}

static int entry_compare(const void *aa, const void *bb)
{
    const auto *a = static_cast<const PanEntry *>(aa);
    const auto *b = static_cast<const PanEntry *>(bb);
    return strcasecmp(a->rel_path, b->rel_path);
}

static PanEntry *alloc_entries()
{
    auto *p = static_cast<PanEntry *>(heap_caps_calloc(
        kMaxEntries, sizeof(PanEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p)
        p = static_cast<PanEntry *>(std::calloc(kMaxEntries, sizeof(PanEntry)));
    return p;
}

static size_t scan_pan_library(PanEntry *entries, size_t cap)
{
    if (!entries || !cap) return 0;
    using DirPath = char[512];
    auto *stack = static_cast<DirPath *>(heap_caps_malloc(
        kDirStackEntries * sizeof(DirPath), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!stack)
        stack = static_cast<DirPath *>(std::malloc(kDirStackEntries * sizeof(DirPath)));
    if (!stack) return 0;

    size_t stack_count = 0;
    std::snprintf(stack[stack_count++], sizeof(stack[0]), "%s", kSdRoot);
    size_t count = 0;
    unsigned visited = 0;

    while (stack_count && count < cap) {
        char dir_path[512];
        std::snprintf(dir_path, sizeof(dir_path), "%s", stack[--stack_count]);
        DIR *dir = opendir(dir_path);
        if (!dir) continue;

        struct dirent *de;
        while ((de = readdir(dir)) != nullptr && count < cap) {
            if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, "..")) continue;
            if (de->d_name[0] == '.') continue;
            if (((++visited) & 31u) == 0u) vTaskDelay(1);

            char full[512];
            if (std::snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full)) continue;
            struct stat st = {};
            if (stat(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                if (stack_count < kDirStackEntries && std::strlen(full) < sizeof(stack[0]))
                    std::snprintf(stack[stack_count++], sizeof(stack[0]), "%s", full);
                continue;
            }
            if (!S_ISREG(st.st_mode) || !ends_with_pan(de->d_name)) continue;
            /* Intent: Temporary PAN files created by this integration or its
             * earlier 6.10 builds are never user library content.
             * Layer8 Aug/17/2026 */
            if (!strcasecmp(full, kPanStageHostPath) ||
                !strcasecmp(full, kLegacyRootPlayPath) ||
                !strcasecmp(full, kLegacyPlayPath)) continue;

            PanEntry &e = entries[count++];
            std::snprintf(e.full_path, sizeof(e.full_path), "%s", full);
            const char *rel = full + std::strlen(kSdRoot);
            if (*rel == '/') ++rel;
            std::snprintf(e.rel_path, sizeof(e.rel_path), "%s", *rel ? rel : de->d_name);
            e.size = st.st_size > 0 ? (uint32_t)st.st_size : 0u;
        }
        closedir(dir);
    }

    qsort(entries, count, sizeof(entries[0]), entry_compare);
    std::free(stack);
    return count;
}

static const char *base_name(const char *path)
{
    const char *p = path ? std::strrchr(path, '/') : nullptr;
    return p ? p + 1 : (path ? path : "");
}

static void parent_path(const char *rel, char *out, size_t cap)
{
    if (!out || !cap) return;
    std::snprintf(out, cap, "%s", rel ? rel : "");
    char *slash = std::strrchr(out, '/');
    if (!slash) std::snprintf(out, cap, "/");
    else { *slash = 0; if (!out[0]) std::snprintf(out, cap, "/"); }
}

static void wait_release()
{
    for (int i = 0; i < 160; ++i) {
        M5.update();
        if (!M5.Touch.getCount()) break;
        vTaskDelay(pdMS_TO_TICKS(8) ? pdMS_TO_TICKS(8) : 1);
    }
}

static bool get_click(int &x, int &y)
{
    M5.update();
    auto t = M5.Touch.getDetail();
    if (!t.wasClicked()) return false;
    x = t.x; y = t.y; return true;
}

static void draw_page(const PanEntry *entries, size_t count, size_t page)
{
    auto &d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextDatum(textdatum_t::top_left);
    d.setTextSize(3);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString("PAN FILES  -  SD CARD", 24, 12);
    d.setTextSize(2);
    d.setTextColor(0xBDF7, TFT_BLACK);
    char info[96];
    std::snprintf(info, sizeof(info), "PAN files: %u     Tap one to play", (unsigned)count);
    d.drawString(info, 26, 63);
    d.setTextDatum(textdatum_t::top_right);
    const size_t pages = count ? ((count + kRowsPerPage - 1) / kRowsPerPage) : 1;
    char pg[32]; std::snprintf(pg, sizeof(pg), "%u / %u", (unsigned)(page + 1), (unsigned)pages);
    d.setTextColor(0xC618, TFT_BLACK); d.drawString(pg, d.width() - 24, 63);
    d.drawFastHLine(0, kHeaderH - 1, d.width(), 0x4208);

    const size_t first = page * kRowsPerPage;
    for (int row = 0; row < kRowsPerPage; ++row) {
        const size_t idx = first + (size_t)row;
        const int y = kHeaderH + row * kRowH;
        const uint16_t bg = (row & 1) ? 0x1082 : 0x0841;
        d.fillRect(0, y, d.width(), kRowH - 1, bg);
        if (idx >= count) continue;
        const PanEntry &e = entries[idx];
        char name[96];
        const char *base = base_name(e.rel_path);
        if (std::strlen(base) > 46) std::snprintf(name, sizeof(name), "%.43s...", base);
        else std::snprintf(name, sizeof(name), "%s", base);
        d.setTextDatum(textdatum_t::top_left);
        d.setTextSize(3); d.setTextColor(TFT_WHITE, bg); d.drawString(name, 28, y + 10);
        char parent[180]; parent_path(e.rel_path, parent, sizeof(parent));
        if (std::strlen(parent) > 68) { char tmp[180]; std::snprintf(tmp, sizeof(tmp), "...%s", parent + std::strlen(parent) - 65); std::snprintf(parent, sizeof(parent), "%s", tmp); }
        d.setTextSize(2); d.setTextColor(0x9CD3, bg); d.drawString(parent, 30, y + 51);
        char sz[48];
        if (e.size >= 1024u*1024u) std::snprintf(sz, sizeof(sz), "%lu.%02lu MB", (unsigned long)(e.size/(1024u*1024u)), (unsigned long)((e.size%(1024u*1024u))*100u/(1024u*1024u)));
        else std::snprintf(sz, sizeof(sz), "%lu KB", (unsigned long)((e.size + 1023u)/1024u));
        d.setTextDatum(textdatum_t::top_right); d.setTextColor(0xC618, bg); d.drawString(sz, d.width()-30, y+51);
    }

    d.fillRect(0, kBottomY, d.width(), 720-kBottomY, TFT_BLACK);
    /*
     * Intent: Random one-shot belongs only on the launcher's upper-left PANIC
     * shortcut.  The PAN filer itself is deliberately just browse/back/page.
     * Layer8 Aug/17/2026
     */
    const int bw = d.width()/3;
    const char *labels[3] = {"< BACK", "< PREV", "NEXT >"};
    for (int i=0;i<3;++i) {
        const int x=i*bw;
        d.drawRect(x+8, kBottomY+18, bw-16, 86, 0x8410);
        d.setTextDatum(textdatum_t::middle_center); d.setTextSize(2); d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.drawString(labels[i], x+bw/2, kBottomY+61);
    }
}

static void draw_loading(const char *path, const char *mode)
{
    auto &d=M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextDatum(textdatum_t::middle_center);
    d.setTextColor(0xFBE0, TFT_BLACK); d.setTextSize(3); d.drawString("LOADING PAN DATA", d.width()/2, 260);
    d.setTextColor(TFT_WHITE, TFT_BLACK); d.setTextSize(2); d.drawString(mode ? mode : "PLEASE WAIT", d.width()/2, 330);
    d.setTextColor(0xBDF7, TFT_BLACK); d.setTextSize(2); d.drawString(base_name(path), d.width()/2, 405);
}

static bool ensure_player_file()
{
    /*
     * Intent: P68K.X is only 15 KiB; rewrite it every PANIC launch so a stale
     * same-sized file from an older experiment can never shadow the embedded
     * V1.38 payload.
     * Layer8 Aug/17/2026
     */
    FILE *fp = std::fopen(kPlayerHostPath, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot materialize PANIC.X at %s errno=%d", kPlayerHostPath, errno);
        return false;
    }
    const size_t n = std::fwrite(panic_x_bin, 1, panic_x_bin_len, fp);
    const int close_rc = std::fclose(fp);
    if (n != panic_x_bin_len || close_rc != 0) {
        ESP_LOGE(TAG, "PANIC.X write failed %u/%u", (unsigned)n, (unsigned)panic_x_bin_len);
        return false;
    }
    ESP_LOGI(TAG, "Embedded PANIC.X materialized: %s (%u bytes)", kPlayerHostPath, (unsigned)panic_x_bin_len);
    return true;
}

static bool stage_pan_file(const char *selected_pan_path)
{
    if (!selected_pan_path || !selected_pan_path[0]) return false;
    if (!std::strcmp(selected_pan_path, kPanStageHostPath)) return true;

    FILE *src = std::fopen(selected_pan_path, "rb");
    if (!src) {
        ESP_LOGE(TAG, "Cannot open selected PAN for staging: %s errno=%d", selected_pan_path, errno);
        return false;
    }
    FILE *dst = std::fopen(kPanStageHostPath, "wb");
    if (!dst) {
        ESP_LOGE(TAG, "Cannot create %s errno=%d", kPanStageHostPath, errno);
        std::fclose(src);
        return false;
    }

    size_t chunk = 16 * 1024;
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) { chunk = 4096; buf = static_cast<uint8_t *>(std::malloc(chunk)); }
    if (!buf) {
        ESP_LOGE(TAG, "PAN staging buffer allocation failed");
        if (buf) std::free(buf);
        std::fclose(src);
        std::fclose(dst);
        unlink(kPanStageHostPath);
        return false;
    }

    bool ok = true;
    size_t total = 0;
    for (;;) {
        const size_t n = std::fread(buf, 1, chunk, src);
        if (n) {
            if (std::fwrite(buf, 1, n, dst) != n) { ok = false; break; }
            total += n;
        }
        if (n < chunk) {
            if (std::ferror(src)) ok = false;
            break;
        }
        if ((total & 0xFFFFu) == 0u) vTaskDelay(1);
    }
    const int dst_err = std::fclose(dst);
    std::fclose(src);
    std::free(buf);
    if (!ok || dst_err != 0) {
        ESP_LOGE(TAG, "PAN staging failed after %u bytes", (unsigned)total);
        unlink(kPanStageHostPath);
        return false;
    }
    ESP_LOGI(TAG, "PAN staged for Human68k: %s -> %s (%u bytes)",
             selected_pan_path, kPanStageHostPath, (unsigned)total);
    return true;
}

static bool choose_random_pan_path(char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) return false;
    out_path[0] = 0;
    PanEntry *entries = alloc_entries();
    if (!entries) return false;
    const size_t count = scan_pan_library(entries, kMaxEntries);
    if (!count) { std::free(entries); return false; }
    const size_t idx = (size_t)(esp_random() % (uint32_t)count);
    std::snprintf(out_path, out_size, "%s", entries[idx].full_path);
    ESP_LOGI(TAG, "Random PAN %u/%u: %s", (unsigned)(idx+1), (unsigned)count, out_path);
    std::free(entries);
    return true;
}

} // namespace

extern "C" void tab5_panic_cleanup_staging(void)
{
    /*
     * Intent: PANIC launch files are transport artifacts, never user media.
     * Clean both current 6.11 root-level names and legacy 6.10 PLAY.PAN names
     * whenever the native launcher owns the SD card.  Only exact known paths
     * are removed; user-supplied PAN files are never touched.
     * Layer8 Aug/17/2026
     */
    const char *paths[] = {
        kPlayerHostPath, kPanStageHostPath, kBatchHostPath, kLegacyPendingRestartPath,
        kLegacyRootPlayPath, kLegacyPlayPath, kLegacyPlayerPath
    };
    unsigned removed = 0;
    for (const char *path : paths) {
        if (unlink(path) == 0) ++removed;
    }
    if (removed) ESP_LOGI(TAG, "PANIC staging cleanup: removed %u temporary file(s)", removed);
}

extern "C" int tab5_panic_prepare_random_runtime(char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) return 0;
    out_path[0] = 0;
    if (!choose_random_pan_path(out_path, out_size)) return 0;
    if (!tab5_panic_prepare_runtime(out_path)) {
        out_path[0] = 0;
        return 0;
    }
    ESP_LOGI(TAG, "PANIC live runtime staged (no ESP restart): %s", out_path);
    return 1;
}

extern "C" int tab5_panic_select_random(char *out_path, size_t out_size)
{
    if (!choose_random_pan_path(out_path, out_size)) return 0;
    draw_loading(out_path, "RANDOM ONE-SHOT");
    wait_release();
    return 1;
}

extern "C" int tab5_panic_select_file(char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) return 0;
    out_path[0] = 0;
    PanEntry *entries = alloc_entries();
    if (!entries) return 0;
    size_t count = scan_pan_library(entries, kMaxEntries);
    size_t page = 0;
    wait_release();
    for (;;) {
        const size_t pages = count ? ((count + kRowsPerPage - 1) / kRowsPerPage) : 1;
        if (page >= pages) page = pages - 1;
        draw_page(entries, count, page);
        wait_release();
        for (;;) {
            int x=0,y=0;
            if (!get_click(x,y)) { vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1); continue; }
            if (y >= kHeaderH && y < kBottomY) {
                const int row=(y-kHeaderH)/kRowH;
                const size_t idx=page*kRowsPerPage+(size_t)row;
                if (idx < count) {
                    std::snprintf(out_path, out_size, "%s", entries[idx].full_path);
                    ESP_LOGI(TAG, "PAN selected: %s", out_path);
                    draw_loading(out_path, "LOADING PAN");
                    std::free(entries); wait_release(); return 1;
                }
            }
            if (y >= kBottomY) {
                const int third_w = std::max(1, (int)M5.Display.width() / 3);
                const int button = std::min(2, std::max(0, x / third_w));
                if (button == 0) { std::free(entries); wait_release(); return 0; }
                if (button == 1) { if (page > 0) --page; break; }
                if (button == 2) { if (page + 1 < pages) ++page; break; }
            }
        }
    }
}

extern "C" int tab5_panic_prepare_runtime(const char *selected_pan_path)
{
    if (!selected_pan_path || !selected_pan_path[0]) return 0;
    /* Build 6.12c: old 6.10 transport names are never runtime inputs now.
     * Remove them again immediately before staging, not only at launcher boot,
     * so PLAY.PAN cannot survive a mixed-version/test session. */
    const char *legacy[] = { kLegacyRootPlayPath, kLegacyPlayPath, kLegacyPlayerPath };
    for (const char *path : legacy) {
        if (unlink(path) == 0) ESP_LOGI(TAG, "Removed legacy PANIC staging file: %s", path);
        else if (errno != ENOENT) ESP_LOGW(TAG, "Could not remove legacy PANIC staging file: %s errno=%d", path, errno);
    }
    struct stat st = {};
    if (stat(selected_pan_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGE(TAG, "Selected PAN disappeared: %s", selected_pan_path);
        return 0;
    }
    if (!ensure_player_file()) return 0;
    /*
     * Intent: Stage the selected file under an 8.3-safe name so every
     * Human68k DOS operation (FILES/CHMOD/OPEN/SEEK), not just OPEN, sees
     * ordinary HostFS semantics. The original PAN file is never modified.
     * Layer8 Aug/17/2026
     */
    if (!stage_pan_file(selected_pan_path)) return 0;
    ESP_LOGI(TAG, "PANIC runtime ready: P68K.X + P68K.PAN from %s", selected_pan_path);
    return 1;
}

extern "C" int tab5_panic_prepare_batch(char drive_letter)
{
    if (drive_letter < 'A' || drive_letter > 'Z') return 0;
    FILE *fp = std::fopen(kBatchHostPath, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot create %s errno=%d", kBatchHostPath, errno);
        return 0;
    }

    /*
     * Build 6.12c: restore the hardware-proven 6.11b bootstrap.  COMMAND.X
     * gets a tiny 8.3 batch first; if that does not launch PANIC, main.c
     * sends one direct P68K.X command 10 seconds later.  The user's 6.11b
     * hardware log showed that exact two-stage sequence starting PANIC.
     */
    char line[96];
    const int len = std::snprintf(line, sizeof(line),
                                  "%c:\\P68K.X %c:\\P68K.PAN\r\n",
                                  drive_letter, drive_letter);
    const size_t n = (len > 0) ? std::fwrite(line, 1, (size_t)len, fp) : 0u;
    const int close_rc = std::fclose(fp);
    if (len <= 0 || n != (size_t)len || close_rc != 0) {
        ESP_LOGE(TAG, "P68K.BAT write failed %u/%d", (unsigned)n, len);
        unlink(kBatchHostPath);
        return 0;
    }
    ESP_LOGI(TAG, "PANIC batch ready: %s -> %c:\\P68K.X %c:\\P68K.PAN",
             kBatchHostPath, drive_letter, drive_letter);
    return 1;
}

extern "C" void tab5_panic_reset_touch(void)
{
    s_touch_active = false;
    s_touch_started_in_corner = false;
    s_touch_start_us = 0;
}

extern "C" int tab5_panic_poll_playback_touch(void)
{
    M5.update();
    const int64_t now = esp_timer_get_time();
    const size_t touch_count = M5.Touch.getCount();
    bool touched = false;
    m5::touch_detail_t t = {};
    for (size_t i = 0; i < touch_count; ++i) {
        const auto td = M5.Touch.getDetail(i);
        if (!td.isPressed()) continue;
        t = td;
        touched = true;
        break;
    }
    if (touched) {
        const bool corner = t.x < 190 && t.y < 130;
        if (!s_touch_active) {
            s_touch_active = true;
            s_touch_started_in_corner = corner;
            s_touch_start_us = now;
        } else if (!corner) {
            s_touch_started_in_corner = false;
        }
        if (s_touch_started_in_corner && now - s_touch_start_us >= kReturnHoldUs) {
            tab5_panic_reset_touch();
            return 2;
        }
        return 0;
    }
    if (!s_touch_active) return 0;
    const int64_t held = now - s_touch_start_us;
    const bool was_corner_long = s_touch_started_in_corner && held >= kReturnHoldUs;
    tab5_panic_reset_touch();
    return was_corner_long ? 2 : 1;
}
