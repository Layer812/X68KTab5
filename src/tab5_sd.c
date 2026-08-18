/*
 * Tab5 port-specific implementation.
 * Intent: Mount the Tab5 SD card with SDMMC and enumerate XDF/DIM/HDS media for the launcher and HostFS.
 * Layer8 Aug/17/2026
 */
#include "tab5_sd.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "TAB5_SD";
static sdmmc_card_t *s_card = NULL;

#define TAB5_SD_MOUNT "/sdcard"
#define XDF_STANDARD_SIZE 1261568LL
#define TAB5_SD_MAX_XDF 32u
#define TAB5_SD_MAX_FLOPPY 64u
#define TAB5_SD_PATH_MAX 512u

static char s_xdf_catalog[TAB5_SD_MAX_XDF][TAB5_SD_PATH_MAX];
static size_t s_xdf_count = 0;
static char s_floppy_catalog[TAB5_SD_MAX_FLOPPY][TAB5_SD_PATH_MAX];
static size_t s_floppy_count = 0;

static int ascii_contains_nocase(const char *s, const char *needle)
{
    if (!s || !needle || !*needle)
        return 0;

    for (; *s; ++s)
    {
        const char *a = s;
        const char *b = needle;

        while (*a && *b &&
               tolower((unsigned char)*a) ==
               tolower((unsigned char)*b))
        {
            ++a;
            ++b;
        }

        if (!*b)
            return 1;
    }

    return 0;
}

static int has_ext3_nocase(const char *name, const char *ext3)
{
    size_t n;

    if (!name || !ext3)
        return 0;

    n = strlen(name);
    if (n < 4 || name[n - 4] != '.')
        return 0;

    return tolower((unsigned char)name[n - 3]) == tolower((unsigned char)ext3[0]) &&
           tolower((unsigned char)name[n - 2]) == tolower((unsigned char)ext3[1]) &&
           tolower((unsigned char)name[n - 1]) == tolower((unsigned char)ext3[2]);
}

static int is_xdf_name(const char *name)
{
    return has_ext3_nocase(name, "xdf");
}

static int is_boot_floppy_name(const char *name)
{
    /* PX68K's FDD_SetFD() dispatches .DIM to DIM_SetFD() and XDF to XDF_SetFD(). */
    return is_xdf_name(name) || has_ext3_nocase(name, "dim");
}

static void sort_catalog(char catalog[][TAB5_SD_PATH_MAX], size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        for (size_t j = i + 1; j < count; ++j)
        {
            if (strcmp(catalog[j], catalog[i]) < 0)
            {
                char tmp[TAB5_SD_PATH_MAX];
                memcpy(tmp, catalog[i], sizeof(tmp));
                memcpy(catalog[i], catalog[j], sizeof(tmp));
                memcpy(catalog[j], tmp, sizeof(tmp));
            }
        }
    }
}

static void build_root_xdf_catalog(void)
{
    DIR *dir;
    struct dirent *ent;

    s_xdf_count = 0;
    s_floppy_count = 0;
    memset(s_xdf_catalog, 0, sizeof(s_xdf_catalog));
    memset(s_floppy_catalog, 0, sizeof(s_floppy_catalog));

    dir = opendir(TAB5_SD_MOUNT);
    if (!dir)
        return;

    while ((ent = readdir(dir)) != NULL)
    {
        char path[TAB5_SD_PATH_MAX];
        struct stat st;
        const int is_xdf = is_xdf_name(ent->d_name);
        const int is_boot = is_boot_floppy_name(ent->d_name);

        if (!is_boot)
            continue;

        if (snprintf(path, sizeof(path), "%s/%s",
                     TAB5_SD_MOUNT, ent->d_name) >= (int)sizeof(path))
            continue;

        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (s_floppy_count < TAB5_SD_MAX_FLOPPY)
        {
            snprintf(s_floppy_catalog[s_floppy_count],
                     sizeof(s_floppy_catalog[s_floppy_count]),
                     "%s", path);
            ++s_floppy_count;
        }

        if (is_xdf && s_xdf_count < TAB5_SD_MAX_XDF)
        {
            snprintf(s_xdf_catalog[s_xdf_count],
                     sizeof(s_xdf_catalog[s_xdf_count]),
                     "%s", path);
            ++s_xdf_count;
        }
    }

    closedir(dir);
    sort_catalog(s_xdf_catalog, s_xdf_count);
    sort_catalog(s_floppy_catalog, s_floppy_count);

    ESP_LOGI(TAG, "Root XDF catalog ready: %u image(s)",
             (unsigned)s_xdf_count);
    ESP_LOGI(TAG, "Root boot-media catalog ready: %u image(s) (.XDF/.DIM)",
             (unsigned)s_floppy_count);
}

size_t tab5_sd_xdf_count(void)
{
    return s_xdf_count;
}

int tab5_sd_xdf_path(size_t index, char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0 || index >= s_xdf_count)
        return 0;

    snprintf(out_path, out_size, "%s", s_xdf_catalog[index]);
    return 1;
}

size_t tab5_sd_floppy_count(void)
{
    return s_floppy_count;
}

int tab5_sd_floppy_path(size_t index, char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0 || index >= s_floppy_count)
        return 0;

    snprintf(out_path, out_size, "%s", s_floppy_catalog[index]);
    return 1;
}

static int scan_xdf_dir(const char *dir_path,
                        int depth,
                        char *best_path,
                        size_t best_size,
                        int *best_score)
{
    DIR *dir;
    struct dirent *ent;
    int found = 0;

    if (depth > 5)
        return 0;

    dir = opendir(dir_path);

    if (!dir)
        return 0;

    while ((ent = readdir(dir)) != NULL)
    {
        char path[512];
        struct stat st;

        if (!strcmp(ent->d_name, ".") ||
            !strcmp(ent->d_name, ".."))
        {
            continue;
        }

        if (snprintf(path,
                     sizeof(path),
                     "%s/%s",
                     dir_path,
                     ent->d_name) >= (int)sizeof(path))
        {
            continue;
        }

        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
        {
            if (scan_xdf_dir(path,
                             depth + 1,
                             best_path,
                             best_size,
                             best_score))
            {
                found = 1;
            }

            continue;
        }

        if (!S_ISREG(st.st_mode) ||
            !is_xdf_name(ent->d_name))
        {
            continue;
        }

        int score = 0;

        if (ascii_contains_nocase(ent->d_name, "human"))
            score += 100;

        if ((long long)st.st_size == XDF_STANDARD_SIZE)
            score += 10;

        ESP_LOGI(TAG,
                 "XDF candidate: %s (%lld bytes) score=%d",
                 path,
                 (long long)st.st_size,
                 score);

        if (!found ||
            score > *best_score ||
            (score == *best_score &&
             strcmp(path, best_path) < 0))
        {
            snprintf(best_path, best_size, "%s", path);
            *best_score = score;
        }

        found = 1;
    }

    closedir(dir);
    return found;
}

int tab5_sd_mount_and_find_xdf(char *out_path, size_t out_size)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg;
    esp_err_t err;

    char best[512] = {0};
    int best_score = -1;

    if (!out_path || out_size == 0)
        return 0;

    out_path[0] = '\0';

    host.max_freq_khz = 20000;
    host.flags |= SDMMC_HOST_FLAG_4BIT;

    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0  = GPIO_NUM_39;
    slot.d1  = GPIO_NUM_40;
    slot.d2  = GPIO_NUM_41;
    slot.d3  = GPIO_NUM_42;
    slot.width = 4;

#ifdef SDMMC_SLOT_FLAG_INTERNAL_PULLUP
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    memset(&mount_cfg, 0, sizeof(mount_cfg));

    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 16;
    mount_cfg.allocation_unit_size = 16 * 1024;

    ESP_LOGI(TAG,
             "Mounting SDMMC 4-bit: "
             "CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42");

    err = esp_vfs_fat_sdmmc_mount(
        TAB5_SD_MOUNT,
        &host,
        &slot,
        &mount_cfg,
        &s_card
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "SD mount FAILED: %s",
                 esp_err_to_name(err));
        return 0;
    }

    ESP_LOGI(TAG, "SD mounted at %s", TAB5_SD_MOUNT);

    if (s_card)
        sdmmc_card_print_info(stdout, s_card);

    /* Build a fast root-level catalog in parallel with the fixed boot path. */
    build_root_xdf_catalog();

    /*
     * BUILD3_FAST_HUMAN302
     *
     * We already know the boot disk is in the SD root.
     * Avoid the ~30 second recursive scan whenever possible.
     */
    {
        const char *preferred =
            TAB5_SD_MOUNT "/HUMAN302.XDF";

        struct stat st;

        if (stat(preferred, &st) == 0 &&
            S_ISREG(st.st_mode))
        {
            snprintf(best,
                     sizeof(best),
                     "%s",
                     preferred);

            best_score = 1000;

            ESP_LOGI(TAG,
                     "Preferred Human68k XDF found directly: "
                     "%s (%lld bytes)",
                     best,
                     (long long)st.st_size);
        }
    }

    /*
     * Fallback only when HUMAN302.XDF is not in the SD root.
     */
    if (best[0] == '\0')
    {
        if (!scan_xdf_dir(TAB5_SD_MOUNT,
                          0,
                          best,
                          sizeof(best),
                          &best_score))
        {
            /* Build 5.95a: SD no longer has to carry Human68k.  The launcher
             * can still Quick Boot the Flash-embedded human302.xdf. */
            ESP_LOGW(TAG, "No .XDF found on SD; Flash Human68k Quick Boot remains available");
            out_path[0] = '\0';
            return 1;
        }
    }

    snprintf(out_path, out_size, "%s", best);

    ESP_LOGI(TAG,
             "Selected XDF for A: %s (score=%d)",
             out_path,
             best_score);

    /*
     * Verify VFS/file access before PX68K receives the path.
     */
    FILE *fp = fopen(out_path, "rb");

    if (!fp)
    {
        ESP_LOGE(TAG,
                 "Selected XDF cannot be opened");
        return 0;
    }

    uint8_t head[16] = {0};
    size_t got = fread(head, 1, sizeof(head), fp);

    fclose(fp);

    if (got >= 8)
    {
        ESP_LOGI(TAG,
                 "XDF first bytes: "
                 "%02X %02X %02X %02X "
                 "%02X %02X %02X %02X",
                 head[0], head[1],
                 head[2], head[3],
                 head[4], head[5],
                 head[6], head[7]);
    }

    return 1;
}