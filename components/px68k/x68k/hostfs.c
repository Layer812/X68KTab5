/*
 * hostfs.c - Build 5.96a Human68k remote-disk bridge for M5Stack Tab5.
 *
 * Human68k 3.02 has a filesystem-level "remote disk" device-driver ABI.
 * Instead of emulating another FAT controller, install a tiny synthetic
 * guest device driver (attribute $2000) and translate its $40..$58 request
 * packets directly to ESP-IDF's already-mounted /sdcard VFS.
 *
 * The guest-side driver is deliberately tiny:
 *   device header -> strategy stub -> interrupt stub
 * Both stubs write a private byte to PX68K's existing $E9F802 host trap.
 * The strategy trap remembers A5 (request header), then the interrupt trap
 * performs the requested filesystem operation synchronously on CPU1.
 */

/*
 * Tab5 port-specific implementation.
 * Intent: Provide a Human68k remote-disk bridge that maps the Tab5 SD FAT32 filesystem into a guest drive while validating paths and write access.
 * Layer8 Aug/17/2026
 */
#include "common.h"
#include "../libretro/prop.h"
#include "m68000.h"
#include "x68kmemory.h"
#include "hostfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PX68K_TAB5_DIAG_VERBOSE
#define PX68K_TAB5_DIAG_VERBOSE 0
#endif

/* Intent: Expose the SD FAT32 tree through Human68k while keeping all host paths rooted under /sdcard for safety and predictable drive semantics.  Layer8 Aug/17/2026 */
#define HOSTFS_ROOT             "/sdcard"
#define HOSTFS_DRIVER_ATTR      0x2000u
#define HOSTFS_DRIVER_NAME      "HOSTFS  "
#define HOSTFS_MAX_PATH         320
#define HOSTFS_MAX_FILES        12
#define HOSTFS_MAX_SEARCH       6
#define HOSTFS_IO_CHUNK         4096u
#define HOSTFS_DRIVER_BYTES     42u

/* Human68k DOS errors used by the remote-drive interface. */
#define HFS_E_INVALID_FUNC      (-1)
#define HFS_E_FILE_NOT_FOUND    (-2)
#define HFS_E_DIR_NOT_FOUND     (-3)
#define HFS_E_TOO_MANY_OPEN     (-4)
#define HFS_E_ACCESS            (-5)
#define HFS_E_BAD_HANDLE        (-6)
#define HFS_E_BAD_MODE          (-12)
#define HFS_E_BAD_NAME          (-13)
#define HFS_E_BAD_PARAM         (-14)
#define HFS_E_NO_MORE_FILES     (-18)
#define HFS_E_CANT_WRITE        (-19)
#define HFS_E_DIR_EXISTS        (-20)
#define HFS_E_CANT_DELETE       (-21)
#define HFS_E_RENAME_EXISTS     (-22)
#define HFS_E_DISK_FULL         (-23)
#define HFS_E_CANT_SEEK         (-25)
#define HFS_E_FILE_EXISTS       (-80)

/* FCB offsets used by Human68k remote-drive requests. */
#define FCB_POS                 0x06u
#define FCB_MODE                0x0eu
#define FCB_NAME1               0x24u
#define FCB_EXT                 0x2cu
#define FCB_ATTR                0x2fu
#define FCB_NAME2               0x30u
#define FCB_TIME                0x3au
#define FCB_DATE                0x3cu
#define FCB_SIZE                0x40u

/* FILES/NFILES buffer offsets.  Human fills 0..20; remote driver fills 21+. */
#define FILES_NEXT_OFF          0x08u
#define FILES_ATTR              0x15u
#define FILES_TIME              0x16u
#define FILES_DATE              0x18u
#define FILES_SIZE              0x1au
#define FILES_NAME              0x1eu
#define FILES_BYTES             53u

/* Request-header offsets. */
#define REQ_UNIT                1u
#define REQ_COMMAND             2u
#define REQ_ERR_LO              3u
#define REQ_ERR_HI              4u
#define REQ_ATTR                13u
#define REQ_ADDR                14u
#define REQ_STATUS              18u
#define REQ_FCB                 22u

typedef struct
{
    uint32_t fcb;
    FILE *fp;
    char path[HOSTFS_MAX_PATH];
} HostFile;

typedef struct
{
    uint32_t key;
    DIR *dir;
    char dirpath[HOSTFS_MAX_PATH];
    char pat_name[9];
    char pat_ext[4];
    uint8_t attr;
    uint8_t active;
} HostSearch;

static HostFile s_files[HOSTFS_MAX_FILES];
static HostSearch s_search[HOSTFS_MAX_SEARCH];
static uint8_t s_io[HOSTFS_IO_CHUNK];
static uint32_t s_req;
static uint32_t s_driver_base;
static uint32_t s_driver_end;
static uint32_t s_calls;
/* Build 6.12q: PANIC bootstrap launch-proof counters.  These count successful
 * HostFS opens of the staged player/data, allowing main.c to distinguish
 * "player is running but has not touched GVRAM yet" from "BAT really did not
 * dispatch".  This prevents the 10-second fallback command from becoming
 * accidental key input to an already-running PANIC.X title screen. */
static uint32_t s_panic_bat_opens;
static uint32_t s_panic_player_opens;
static uint32_t s_panic_pan_opens;
static uint32_t s_read_calls;
static uint32_t s_write_calls;
static uint32_t s_files_calls;
static int s_drive = -1;
static int s_root_writable = -1;

static int hfs_guest_read(uint32_t address, void *dstv, size_t length)
{
    uint8_t *dst = (uint8_t *)dstv;
    if (!dst) return 0;
    address &= 0x00ffffffu;
    if (MEM && address < (uint32_t)Config.ram_size &&
        length <= (size_t)((uint32_t)Config.ram_size - address))
    {
        for (size_t i = 0; i < length; ++i)
            dst[i] = MEM[(address + (uint32_t)i) ^ 1u];
        return 1;
    }
    for (size_t i = 0; i < length; ++i)
        dst[i] = (uint8_t)m68k_read_memory_8((address + (uint32_t)i) & 0x00ffffffu);
    return 1;
}

static int hfs_guest_write(uint32_t address, const void *srcv, size_t length)
{
    const uint8_t *src = (const uint8_t *)srcv;
    if (!src) return 0;
    address &= 0x00ffffffu;
    if (MEM && address < (uint32_t)Config.ram_size &&
        length <= (size_t)((uint32_t)Config.ram_size - address))
    {
        for (size_t i = 0; i < length; ++i)
            MEM[(address + (uint32_t)i) ^ 1u] = src[i];
#ifdef ESP_PLATFORM
        m68k_tab5_exec123_invalidate_range(address, (uint32_t)length);
#endif
        return 1;
    }
    for (size_t i = 0; i < length; ++i)
        m68k_write_memory_8((address + (uint32_t)i) & 0x00ffffffu, src[i]);
    return 1;
}

static uint16_t hfs_get16(uint32_t a)
{
    uint8_t b[2];
    if (!hfs_guest_read(a, b, 2)) return 0;
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static uint32_t hfs_get32(uint32_t a)
{
    uint8_t b[4];
    if (!hfs_guest_read(a, b, 4)) return 0;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static void hfs_put8(uint32_t a, uint8_t v)
{
    hfs_guest_write(a, &v, 1);
}

static void hfs_put16(uint32_t a, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    hfs_guest_write(a, b, 2);
}

static void hfs_put32(uint32_t a, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)v};
    hfs_guest_write(a, b, 4);
}

static void hfs_req_status(int32_t status)
{
    if (!s_req) return;
    hfs_put8(s_req + REQ_ERR_LO, 0);
    hfs_put8(s_req + REQ_ERR_HI, 0);
    hfs_put32(s_req + REQ_STATUS, (uint32_t)status);
}

static int hfs_errno_file(int e)
{
    switch (e)
    {
        case ENOENT: return HFS_E_FILE_NOT_FOUND;
        case EACCES: case EPERM: return HFS_E_ACCESS;
#ifdef EROFS
        case EROFS: return HFS_E_CANT_WRITE;
#endif
#ifdef ENOTDIR
        case ENOTDIR: return HFS_E_FILE_NOT_FOUND;
#endif
        case EEXIST: return HFS_E_FILE_EXISTS;
        case ENOSPC: return HFS_E_DISK_FULL;
        case EMFILE: case ENFILE: return HFS_E_TOO_MANY_OPEN;
        default: return HFS_E_ACCESS;
    }
}

static int hfs_errno_dir(int e)
{
    if (e == ENOENT) return HFS_E_DIR_NOT_FOUND;
    if (e == EEXIST) return HFS_E_DIR_EXISTS;
    return hfs_errno_file(e);
}

static int hfs_safe_component_char(unsigned char c)
{
    return c != 0 && c != ':' && c != '\\' && c != '/';
}

static size_t hfs_copy_trimmed(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t end = n;
    while (end && (src[end - 1] == ' ' || src[end - 1] == 0)) --end;
    size_t out = 0;
    for (size_t i = 0; i < end && out + 1 < cap; ++i)
    {
        unsigned char c = src[i];
        if (!c) break;
        dst[out++] = (char)c;
    }
    if (cap) dst[out] = 0;
    return out;
}

/* Convert a Human68k NAMESTS (88 bytes) into a VFS path below /sdcard.
 * For FILES the pattern fields are optionally returned separately. */
static int hfs_namests(uint32_t ns_addr, char *full, size_t fullcap,
                       char pat_name[9], char pat_ext[4], int pattern_only)
{
    uint8_t ns[88];
    char rel[96];
    char name1[9], name2[11], ext[4];
    if (!ns_addr || !full || fullcap < 12 || !hfs_guest_read(ns_addr, ns, sizeof(ns)))
        return 0;

    /* Human68k NAMESTS does not store directories with '/' or '\\'.
     * Bytes 2..66 contain directory components separated by 0x09.  Build
     * the host relative path exactly that way; treating 0x09 as a literal
     * byte was the main Build 5.96 subdirectory bug. */
    size_t rp = 0;
    int need_sep = 0;
    for (size_t i = 2; i < 67 && ns[i] != 0x00; )
    {
        while (i < 67 && ns[i] == 0x09) ++i;
        if (i >= 67 || ns[i] == 0x00) break;
        if (need_sep && rp + 1 < sizeof(rel)) rel[rp++] = '/';
        while (i < 67 && ns[i] != 0x00 && ns[i] != 0x09)
        {
            unsigned char c = ns[i++];
            if (c == ':' || c == '\\' || c == '/') continue;
            if (rp + 1 >= sizeof(rel)) return 0;
            rel[rp++] = (char)c;
        }
        need_sep = 1;
    }
    rel[rp] = 0;

    /* Reject host-root escape.  Human canonical paths normally contain no ..,
     * but do not trust a guest path enough to let it leave /sdcard. */
    if (strstr(rel, "../") || strstr(rel, "/..") || !strcmp(rel, ".."))
        return 0;

    hfs_copy_trimmed(name1, sizeof(name1), &ns[67], 8);
    hfs_copy_trimmed(ext, sizeof(ext), &ns[75], 3);
    hfs_copy_trimmed(name2, sizeof(name2), &ns[78], 10);

    if (pat_name)
    {
        memset(pat_name, ' ', 8); pat_name[8] = 0;
        for (size_t i = 0; i < 8 && ns[67 + i]; ++i)
            pat_name[i] = (char)ns[67 + i];
    }
    if (pat_ext)
    {
        memset(pat_ext, ' ', 3); pat_ext[3] = 0;
        for (size_t i = 0; i < 3 && ns[75 + i]; ++i)
            pat_ext[i] = (char)ns[75 + i];
    }

    size_t p = 0;
    p += (size_t)snprintf(full + p, fullcap - p, "%s", HOSTFS_ROOT);
    if (p >= fullcap) return 0;
    if (rel[0])
    {
        const char *r = rel;
        while (*r == '/') ++r;
        if (*r)
            p += (size_t)snprintf(full + p, fullcap - p, "/%s", r);
        if (p >= fullcap) return 0;
    }
    while (p > strlen(HOSTFS_ROOT) && full[p - 1] == '/') full[--p] = 0;
    if (p >= fullcap) return 0;

    if (!pattern_only && (name1[0] || name2[0] || ext[0]))
    {
        if (p + 2 >= fullcap) return 0;
        full[p++] = '/'; full[p] = 0;
        for (size_t i = 0; name1[i] && p + 1 < fullcap; ++i)
            if (hfs_safe_component_char((unsigned char)name1[i])) full[p++] = name1[i];
        for (size_t i = 0; name2[i] && p + 1 < fullcap; ++i)
            if (hfs_safe_component_char((unsigned char)name2[i])) full[p++] = name2[i];
        if (ext[0] && p + 2 < fullcap)
        {
            full[p++] = '.';
            for (size_t i = 0; ext[i] && p + 1 < fullcap; ++i)
                if (hfs_safe_component_char((unsigned char)ext[i])) full[p++] = ext[i];
        }
        full[p] = 0;
    }
    return 1;
}

/* FAT is case-insensitive, while the VFS layer/configuration used by a given
 * ESP-IDF build is not guaranteed to perform case folding for stat/fopen.
 * Human68k commonly canonicalizes NAMESTS to upper case.  Resolve every
 * existing component with an ASCII case-insensitive directory scan and keep
 * the actual on-disk spelling.  For CREATE/MKDIR/rename destination, the last
 * component may legitimately not exist. */
static int hfs_ascii_name_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int hfs_resolve_case_path(const char *input, int allow_missing_leaf,
                                 char *output, size_t outcap, int *exists_out)
{
    const size_t rootlen = strlen(HOSTFS_ROOT);
    if (exists_out) *exists_out = 0;
    if (!input || !output || outcap <= rootlen + 1u ||
        strncmp(input, HOSTFS_ROOT, rootlen) != 0 ||
        (input[rootlen] != 0 && input[rootlen] != '/'))
        return 0;

    snprintf(output, outcap, "%s", HOSTFS_ROOT);
    if (input[rootlen] == 0)
    {
        if (exists_out) *exists_out = 1;
        return 1;
    }

    const char *p = input + rootlen;
    while (*p == '/') ++p;
    while (*p)
    {
        char component[128];
        size_t cn = 0;
        while (*p && *p != '/')
        {
            if (cn + 1 >= sizeof(component)) return 0;
            component[cn++] = *p++;
        }
        component[cn] = 0;
        while (*p == '/') ++p;
        const int last = (*p == 0);
        if (!component[0] || !strcmp(component, ".") || !strcmp(component, ".."))
            return 0;

        DIR *d = opendir(output);
        if (!d) return 0;
        char actual[128];
        actual[0] = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL)
        {
            if (hfs_ascii_name_eq(de->d_name, component))
            {
                const size_t actual_len = strnlen(de->d_name, sizeof(actual));
                if (actual_len >= sizeof(actual))
                    continue;
                memcpy(actual, de->d_name, actual_len + 1);
                break;
            }
        }
        closedir(d);

        if (!actual[0])
        {
            if (!(allow_missing_leaf && last))
            {
                errno = ENOENT;
                return 0;
            }
            snprintf(actual, sizeof(actual), "%s", component);
        }

        size_t used = strlen(output);
        size_t an = strlen(actual);
        if (used + 1u + an + 1u > outcap) return 0;
        output[used++] = '/';
        memcpy(output + used, actual, an + 1u);

        if (last)
        {
            struct stat st;
            int ex = stat(output, &st) == 0;
            if (exists_out) *exists_out = ex;
        }
    }
    return 1;
}

static int hfs_resolve_existing(const char *raw, char *resolved, size_t cap)
{
    int exists = 0;
    if (!hfs_resolve_case_path(raw, 0, resolved, cap, &exists) || !exists)
        return 0;
    return 1;
}

static void hfs_log_path_failure(const char *op, const char *raw,
                                 const char *resolved, int e, uint32_t fcb, uint32_t mode)
{
#if PX68K_TAB5_DIAG_VERBOSE
    printf("PX68K_HOSTFS: %s FAILED fcb=$%06lX mode=%lu raw='%s' path='%s' errno=%d(%s) writable=%d\n",
           op ? op : "OP", (unsigned long)fcb, (unsigned long)mode,
           raw ? raw : "", resolved ? resolved : "", e, strerror(e), s_root_writable);
#else
    (void)op; (void)raw; (void)resolved; (void)e; (void)fcb; (void)mode;
#endif
}

static void hfs_split_83(const char *name, char base[9], char ext[4])
{
    memset(base, ' ', 8); base[8] = 0;
    memset(ext, ' ', 3); ext[3] = 0;
    if (!name) return;
    const char *dot = strrchr(name, '.');
    size_t bn = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (size_t i = 0; i < bn && i < 8; ++i)
        base[i] = (char)toupper((unsigned char)name[i]);
    if (dot)
        for (size_t i = 0; dot[1 + i] && i < 3; ++i)
            ext[i] = (char)toupper((unsigned char)dot[1 + i]);
}

static int hfs_pat_field(const char *pat, const char *val, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char p = (unsigned char)toupper((unsigned char)pat[i]);
        unsigned char v = (unsigned char)toupper((unsigned char)val[i]);
        if (p == '*' || p == '?') continue;
        if (p != v) return 0;
    }
    return 1;
}

static int hfs_match_83(const HostSearch *s, const char *name)
{
    char b[9], e[4];
    hfs_split_83(name, b, e);
    return hfs_pat_field(s->pat_name, b, 8) && hfs_pat_field(s->pat_ext, e, 3);
}

static void hfs_dos_datetime(time_t t, uint16_t *date, uint16_t *tim)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    int year = tmv.tm_year + 1900;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;
    if (date) *date = (uint16_t)(((year - 1980) << 9) |
                                 ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
    if (tim) *tim = (uint16_t)((tmv.tm_hour << 11) |
                               (tmv.tm_min << 5) | (tmv.tm_sec / 2));
}

static uint8_t hfs_attr_for_stat(const struct stat *st)
{
    if (st && S_ISDIR(st->st_mode)) return 0x10u;
    return 0x20u; /* archive */
}

static void hfs_fill_fcb(uint32_t fcb, const char *path, const struct stat *st)
{
    if (!fcb) return;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char b[9], e[4];
    hfs_split_83(name, b, e);
    uint8_t n1[8], ex[3], n2[10];
    memcpy(n1, b, 8); memcpy(ex, e, 3); memset(n2, ' ', sizeof(n2));
    hfs_guest_write(fcb + FCB_NAME1, n1, sizeof(n1));
    hfs_guest_write(fcb + FCB_EXT, ex, sizeof(ex));
    hfs_guest_write(fcb + FCB_NAME2, n2, sizeof(n2));
    hfs_put8(fcb + FCB_ATTR, hfs_attr_for_stat(st));
    uint16_t d = 0, t = 0;
    hfs_dos_datetime(st ? st->st_mtime : 0, &d, &t);
    hfs_put16(fcb + FCB_TIME, t);
    hfs_put16(fcb + FCB_DATE, d);
    hfs_put32(fcb + FCB_SIZE, st && st->st_size > 0 ? (uint32_t)st->st_size : 0u);
    hfs_put32(fcb + FCB_POS, 0u);
}

static HostFile *hfs_file_find(uint32_t fcb)
{
    for (int i = 0; i < HOSTFS_MAX_FILES; ++i)
        if (s_files[i].fp && s_files[i].fcb == fcb) return &s_files[i];
    return NULL;
}

static HostFile *hfs_file_alloc(uint32_t fcb)
{
    HostFile *old = hfs_file_find(fcb);
    if (old) { fclose(old->fp); memset(old, 0, sizeof(*old)); }
    for (int i = 0; i < HOSTFS_MAX_FILES; ++i)
        if (!s_files[i].fp) { s_files[i].fcb = fcb; return &s_files[i]; }
    return NULL;
}

static void hfs_search_close(HostSearch *s)
{
    if (!s) return;
    if (s->dir) closedir(s->dir);
    memset(s, 0, sizeof(*s));
}

static HostSearch *hfs_search_find(uint32_t key)
{
    for (int i = 0; i < HOSTFS_MAX_SEARCH; ++i)
        if (s_search[i].active && s_search[i].key == key) return &s_search[i];
    return NULL;
}

static HostSearch *hfs_search_alloc(uint32_t key)
{
    HostSearch *s = hfs_search_find(key);
    if (s) hfs_search_close(s);
    for (int i = 0; i < HOSTFS_MAX_SEARCH; ++i)
        if (!s_search[i].active) { s_search[i].active = 1; s_search[i].key = key; return &s_search[i]; }
    hfs_search_close(&s_search[0]);
    s_search[0].active = 1; s_search[0].key = key;
    return &s_search[0];
}

static int hfs_fill_files_next(HostSearch *s, uint32_t fb)
{
    if (!s || !s->dir || !fb) return HFS_E_NO_MORE_FILES;
    for (;;)
    {
        struct dirent *de = readdir(s->dir);
        if (!de)
        {
            hfs_put16(fb + FILES_NEXT_OFF, 0xffffu);
            hfs_search_close(s);
            return HFS_E_NO_MORE_FILES;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!hfs_match_83(s, de->d_name)) continue;

        char path[HOSTFS_MAX_PATH];
        int n = snprintf(path, sizeof(path), "%s/%s", s->dirpath, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        const uint8_t attr = hfs_attr_for_stat(&st);
        /* Standard DOS search: directories require bit 0x10 in requested attr. */
        if ((attr & 0x10u) && !(s->attr & 0x10u)) continue;

        uint16_t d = 0, t = 0;
        hfs_dos_datetime(st.st_mtime, &d, &t);
        hfs_put8(fb + FILES_ATTR, attr);
        hfs_put16(fb + FILES_TIME, t);
        hfs_put16(fb + FILES_DATE, d);
        hfs_put32(fb + FILES_SIZE, st.st_size > 0 ? (uint32_t)st.st_size : 0u);
        uint8_t out[23]; memset(out, 0, sizeof(out));
        size_t len = strlen(de->d_name); if (len > 22) len = 22;
        memcpy(out, de->d_name, len);
        hfs_guest_write(fb + FILES_NAME, out, sizeof(out));
        hfs_put16(fb + FILES_NEXT_OFF, 0u); /* external context owns continuation */
        return 0;
    }
}

static int hfs_cmd_chdir(void)
{
    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH]; struct stat st;
    if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;
    if (!hfs_resolve_existing(raw, path, sizeof(path)) || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return HFS_E_DIR_NOT_FOUND;
    return 0;
}

static int hfs_cmd_mkdir(void)
{
    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH]; int exists = 0;
    if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;
    if (!s_root_writable) return HFS_E_CANT_WRITE;
    if (!hfs_resolve_case_path(raw, 1, path, sizeof(path), &exists)) return HFS_E_DIR_NOT_FOUND;
    if (exists) return HFS_E_DIR_EXISTS;
    if (mkdir(path, 0777) == 0) return 0;
    return hfs_errno_dir(errno);
}

static int hfs_cmd_rmdir(void)
{
    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH];
    if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;
    if (!s_root_writable) return HFS_E_CANT_WRITE;
    if (!hfs_resolve_existing(raw, path, sizeof(path))) return HFS_E_DIR_NOT_FOUND;
    if (rmdir(path) == 0) return 0;
    return errno == ENOTEMPTY ? HFS_E_CANT_DELETE : hfs_errno_dir(errno);
}

static int hfs_cmd_rename(void)
{
    char oldraw[HOSTFS_MAX_PATH], newraw[HOSTFS_MAX_PATH];
    char oldp[HOSTFS_MAX_PATH], newp[HOSTFS_MAX_PATH]; int new_exists = 0;
    if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), oldraw, sizeof(oldraw), NULL, NULL, 0) ||
        !hfs_namests(hfs_get32(s_req + REQ_STATUS), newraw, sizeof(newraw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;
    if (!s_root_writable) return HFS_E_CANT_WRITE;
    if (!hfs_resolve_existing(oldraw, oldp, sizeof(oldp))) return HFS_E_FILE_NOT_FOUND;
    if (!hfs_resolve_case_path(newraw, 1, newp, sizeof(newp), &new_exists)) return HFS_E_DIR_NOT_FOUND;
    if (new_exists && !hfs_ascii_name_eq(oldp, newp)) return HFS_E_RENAME_EXISTS;
    if (rename(oldp, newp) == 0) return 0;
    return hfs_errno_file(errno);
}

static int hfs_cmd_delete(void)
{
    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH];
    if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;
    if (!s_root_writable) return HFS_E_CANT_WRITE;
    if (!hfs_resolve_existing(raw, path, sizeof(path))) return HFS_E_FILE_NOT_FOUND;
    if (unlink(path) == 0) return 0;
    return errno == EISDIR ? HFS_E_CANT_DELETE : hfs_errno_file(errno);
}

static int hfs_cmd_chmod(void)
{
    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH]; struct stat st;
    if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;
    if (!hfs_resolve_existing(raw, path, sizeof(path)) || stat(path, &st) != 0)
        return HFS_E_FILE_NOT_FOUND;
    uint8_t set = 0xffu; hfs_guest_read(s_req + REQ_ATTR, &set, 1);
    if (set == 0xffu)
    {
        uint8_t a = hfs_attr_for_stat(&st);
        if (!s_root_writable && !S_ISDIR(st.st_mode)) a |= 0x01u;
        return (int)a;
    }
    /* The mounted FAT volume is the authority.  We do not mutate FAT DOS
     * attributes here yet; report the requested ordinary bits as accepted. */
    return (int)(set & 0x3fu);
}

static int hfs_cmd_files(int first)
{
    const uint32_t fb = hfs_get32(s_req + REQ_STATUS);
    if (!fb) return HFS_E_BAD_PARAM;
    if (first)
    {
        char rawdir[HOSTFS_MAX_PATH], dir[HOSTFS_MAX_PATH], pn[9], pe[4];
        if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), rawdir, sizeof(rawdir), pn, pe, 1))
            return HFS_E_BAD_NAME;
        if (!hfs_resolve_existing(rawdir, dir, sizeof(dir))) return HFS_E_DIR_NOT_FOUND;
        HostSearch *s = hfs_search_alloc(fb);
        if (!s) return HFS_E_TOO_MANY_OPEN;
        strncpy(s->dirpath, dir, sizeof(s->dirpath) - 1);
        s->dirpath[sizeof(s->dirpath) - 1] = 0;
        memcpy(s->pat_name, pn, sizeof(s->pat_name));
        memcpy(s->pat_ext, pe, sizeof(s->pat_ext));
        hfs_guest_read(s_req + REQ_ATTR, &s->attr, 1);
        s->dir = opendir(s->dirpath);
        ++s_files_calls;
        if (PX68K_TAB5_DIAG_VERBOSE && s_files_calls <= 16u)
            printf("PX68K_HOSTFS: FILES #%lu dir=%s pat='%.8s.%.3s' attr=%02X fb=$%06lX\n",
                   (unsigned long)s_files_calls, s->dirpath, s->pat_name, s->pat_ext,
                   s->attr, (unsigned long)fb);
        if (!s->dir) { hfs_search_close(s); return HFS_E_DIR_NOT_FOUND; }
        return hfs_fill_files_next(s, fb);
    }
    HostSearch *s = hfs_search_find(fb);
    if (!s) return HFS_E_NO_MORE_FILES;
    return hfs_fill_files_next(s, fb);
}

static int hfs_open_common(int create)
{
    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH]; struct stat st;
    const uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    if (!fcb || !hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;

    uint8_t mode = 0;
    hfs_guest_read(fcb + FCB_MODE, &mode, 1);
    if (mode > 2u && !create) return HFS_E_BAD_MODE;

    int exists = 0;
    if (!hfs_resolve_case_path(raw, create ? 1 : 0, path, sizeof(path), &exists))
    {
        int e = errno ? errno : ENOENT;
        hfs_log_path_failure(create ? "CREATE-RESOLVE" : "OPEN-RESOLVE", raw, "", e, fcb, mode);
        return create ? hfs_errno_dir(e) : HFS_E_FILE_NOT_FOUND;
    }

    const char *fm = "rb";
    if (create)
    {
        const uint32_t cmode = hfs_get32(s_req + REQ_STATUS); /* 0=_NEWFILE, 1=_CREATE */
        if (!s_root_writable) return HFS_E_CANT_WRITE;
        if (exists)
        {
            if (stat(path, &st) != 0) return hfs_errno_file(errno);
            if (S_ISDIR(st.st_mode)) return HFS_E_CANT_WRITE;
            if (cmode == 0u) return HFS_E_FILE_EXISTS;
        }
        fm = "wb+"; /* _CREATE truncates; _NEWFILE reaches here only when absent. */
        if (PX68K_TAB5_DIAG_VERBOSE && s_calls <= 200u)
        {
            uint8_t create_attr = 0;
            hfs_guest_read(s_req + REQ_ATTR, &create_attr, 1);
            printf("PX68K_HOSTFS: CREATE TRY fcb=$%06lX attr=%02X newmode=%lu raw='%s' path='%s' exists=%d\n",
                   (unsigned long)fcb, (unsigned)create_attr,
                   (unsigned long)cmode, raw, path, exists);
        }
    }
    else
    {
        if (!exists || stat(path, &st) != 0) return HFS_E_FILE_NOT_FOUND;
        if (S_ISDIR(st.st_mode)) return HFS_E_ACCESS;
        if (mode != 0u && !s_root_writable) return HFS_E_CANT_WRITE;
        fm = mode == 0u ? "rb" : "rb+";
        if (PX68K_TAB5_DIAG_VERBOSE && s_calls <= 200u)
            printf("PX68K_HOSTFS: OPEN TRY fcb=$%06lX mode=%u raw='%s' path='%s' size=%lu\n",
                   (unsigned long)fcb, (unsigned)mode, raw, path,
                   (unsigned long)(st.st_size > 0 ? st.st_size : 0));
    }

    HostFile *hf = hfs_file_alloc(fcb);
    if (!hf) return HFS_E_TOO_MANY_OPEN;
    errno = 0;
    hf->fp = fopen(path, fm);
    if (!hf->fp)
    {
        int e = errno;
        hfs_log_path_failure(create ? "CREATE" : "OPEN", raw, path, e, fcb, mode);
        memset(hf, 0, sizeof(*hf));
        if (create || mode != 0u)
        {
#ifdef EROFS
            if (e == EROFS) return HFS_E_CANT_WRITE;
#endif
            if (e == EACCES || e == EPERM) return HFS_E_CANT_WRITE;
        }
        return hfs_errno_file(e);
    }
    strncpy(hf->path, path, sizeof(hf->path) - 1);
    hf->path[sizeof(hf->path) - 1] = 0;

    if (!create)
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (!strcasecmp(base, "P68K.BAT")) ++s_panic_bat_opens;
        else if (!strcasecmp(base, "P68K.X")) ++s_panic_player_opens;
        else if (!strcasecmp(base, "P68K.PAN")) ++s_panic_pan_opens;
    }
    if (stat(path, &st) != 0) memset(&st, 0, sizeof(st));
    hfs_fill_fcb(fcb, path, &st);
    if (PX68K_TAB5_DIAG_VERBOSE)
        printf("PX68K_HOSTFS: %s OK fcb=$%06lX mode=%u path=%s size=%lu\n",
               create ? "CREATE" : "OPEN", (unsigned long)fcb, (unsigned)mode, path,
               (unsigned long)(st.st_size > 0 ? st.st_size : 0));
    return 0;
}

static int hfs_cmd_close(void)
{
    uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    HostFile *hf = hfs_file_find(fcb);
    /* XEiJ/Human68k HFS treats an already-closed handle as a successful close. */
    if (!hf) return 0;
    fflush(hf->fp);
    fclose(hf->fp); memset(hf, 0, sizeof(*hf));
    return 0;
}

static int hfs_cmd_read(void)
{
    uint32_t addr = hfs_get32(s_req + REQ_ADDR);
    uint32_t want = hfs_get32(s_req + REQ_STATUS);
    uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    HostFile *hf = hfs_file_find(fcb);
    if (!hf) return HFS_E_BAD_HANDLE;
    uint32_t pos = hfs_get32(fcb + FCB_POS), done = 0;
    clearerr(hf->fp);
    if (fseek(hf->fp, (long)pos, SEEK_SET) != 0) return HFS_E_CANT_SEEK;
    while (done < want)
    {
        uint32_t chunk = want - done; if (chunk > HOSTFS_IO_CHUNK) chunk = HOSTFS_IO_CHUNK;
        size_t n = fread(s_io, 1, chunk, hf->fp);
        if (n && !hfs_guest_write(addr + done, s_io, n)) return HFS_E_ACCESS;
        done += (uint32_t)n;
        if (n < chunk)
        {
            if (ferror(hf->fp))
            {
                printf("PX68K_HOSTFS: READ FAIL path=%s errno=%d(%s)\n", hf->path, errno, strerror(errno));
                clearerr(hf->fp);
                return HFS_E_ACCESS;
            }
            break; /* EOF */
        }
    }
    hfs_put32(fcb + FCB_POS, pos + done);
    ++s_read_calls;
    if (PX68K_TAB5_DIAG_VERBOSE && s_read_calls <= 32u)
        printf("PX68K_HOSTFS: READ #%lu fcb=$%06lX pos=%lu want=%lu got=%lu path=%s\n",
               (unsigned long)s_read_calls, (unsigned long)fcb,
               (unsigned long)pos, (unsigned long)want, (unsigned long)done, hf->path);
    return (int)done;
}

static int hfs_cmd_write(void)
{
    uint32_t addr = hfs_get32(s_req + REQ_ADDR);
    uint32_t want = hfs_get32(s_req + REQ_STATUS);
    uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    HostFile *hf = hfs_file_find(fcb);
    if (!hf) return HFS_E_BAD_HANDLE;
    if (!s_root_writable) return HFS_E_CANT_WRITE;
    uint32_t pos = hfs_get32(fcb + FCB_POS), done = 0;
    clearerr(hf->fp);
    if (fseek(hf->fp, (long)pos, SEEK_SET) != 0) return HFS_E_CANT_SEEK;

    /* Human68k _WRITE length=0 means truncate at the current seek position. */
    if (want == 0u)
    {
        if (fflush(hf->fp) != 0) return HFS_E_CANT_WRITE;
        int fd = fileno(hf->fp);
        if (fd < 0 || ftruncate(fd, (off_t)pos) != 0)
        {
            printf("PX68K_HOSTFS: TRUNCATE FAIL fcb=$%06lX pos=%lu path=%s errno=%d(%s)\n",
                   (unsigned long)fcb, (unsigned long)pos, hf->path, errno, strerror(errno));
            return HFS_E_CANT_WRITE;
        }
        hfs_put32(fcb + FCB_SIZE, pos);
        ++s_write_calls;
        if (PX68K_TAB5_DIAG_VERBOSE)
            printf("PX68K_HOSTFS: WRITE/TRUNCATE #%lu fcb=$%06lX size=%lu path=%s\n",
                   (unsigned long)s_write_calls, (unsigned long)fcb, (unsigned long)pos, hf->path);
        return 0;
    }

    while (done < want)
    {
        uint32_t chunk = want - done; if (chunk > HOSTFS_IO_CHUNK) chunk = HOSTFS_IO_CHUNK;
        if (!hfs_guest_read(addr + done, s_io, chunk)) return HFS_E_ACCESS;
        errno = 0;
        size_t n = fwrite(s_io, 1, chunk, hf->fp);
        done += (uint32_t)n;
        if (n < chunk)
        {
            printf("PX68K_HOSTFS: WRITE FAIL fcb=$%06lX path=%s wrote=%lu/%lu errno=%d(%s) ferr=%d\n",
                   (unsigned long)fcb, hf->path, (unsigned long)n, (unsigned long)chunk,
                   errno, strerror(errno), ferror(hf->fp));
            clearerr(hf->fp);
            break;
        }
    }
    if (fflush(hf->fp) != 0)
    {
        printf("PX68K_HOSTFS: FLUSH FAIL fcb=$%06lX path=%s errno=%d(%s)\n",
               (unsigned long)fcb, hf->path, errno, strerror(errno));
        return HFS_E_CANT_WRITE;
    }
    hfs_put32(fcb + FCB_POS, pos + done);
    uint32_t size = hfs_get32(fcb + FCB_SIZE);
    if (pos + done > size) hfs_put32(fcb + FCB_SIZE, pos + done);
    ++s_write_calls;
    if (PX68K_TAB5_DIAG_VERBOSE && s_write_calls <= 32u)
        printf("PX68K_HOSTFS: WRITE #%lu fcb=$%06lX pos=%lu want=%lu wrote=%lu path=%s\n",
               (unsigned long)s_write_calls, (unsigned long)fcb,
               (unsigned long)pos, (unsigned long)want, (unsigned long)done, hf->path);
    return done == want ? (int)done : HFS_E_CANT_WRITE;
}

static int hfs_cmd_seek(void)
{
    uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    HostFile *hf = hfs_file_find(fcb);
    if (!hf) return HFS_E_BAD_HANDLE;
    uint8_t wh = 0; hfs_guest_read(s_req + REQ_ATTR, &wh, 1);
    int32_t off = (int32_t)hfs_get32(s_req + REQ_STATUS);
    int64_t base;
    if (wh == 0) base = 0;
    else if (wh == 1) base = hfs_get32(fcb + FCB_POS);
    else if (wh == 2) base = hfs_get32(fcb + FCB_SIZE);
    else return HFS_E_BAD_PARAM;
    int64_t np = base + off;
    if (np < 0 || np > (int64_t)hfs_get32(fcb + FCB_SIZE)) return HFS_E_CANT_SEEK;
    hfs_put32(fcb + FCB_POS, (uint32_t)np);
    return (int)(uint32_t)np;
}

static int hfs_cmd_filedate(void)
{
    uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    HostFile *hf = hfs_file_find(fcb);
    if (!hf) return HFS_E_BAD_HANDLE;
    uint32_t v = hfs_get32(s_req + REQ_STATUS);
    if (v == 0)
        return (int)(((uint32_t)hfs_get16(fcb + FCB_DATE) << 16) | hfs_get16(fcb + FCB_TIME));
    hfs_put16(fcb + FCB_DATE, (uint16_t)(v >> 16));
    hfs_put16(fcb + FCB_TIME, (uint16_t)v);
    return (int)v;
}

static int hfs_cmd_dskfre(void)
{
    uint32_t a = hfs_get32(s_req + REQ_ADDR);
    if (!a) return HFS_E_BAD_PARAM;
    /* 32 KiB clusters, deliberately capped below Human68k's practical 2 GiB
     * remote-volume ceiling.  This reports a stable useful capacity without
     * coupling the first implementation to statvfs availability in ESP-IDF. */
    hfs_put16(a + 0, 32767u); /* free clusters ~= 1 GiB */
    hfs_put16(a + 2, 65534u); /* total clusters ~= 2 GiB */
    hfs_put16(a + 4, 64u);    /* sectors / cluster */
    hfs_put16(a + 6, 512u);   /* bytes / sector */
    return 32767 * 64 * 512;
}

static int hfs_cmd_drvctrl(void)
{
    /* Human68k _DRVCTRL sense: bit1=media inserted, bit3=write protected.
     * The write probe is authoritative for the latter. */
    uint8_t sense = 0x02u;
    if (!s_root_writable) sense |= 0x08u;
    hfs_put8(s_req + REQ_ATTR, sense);
    return 0;
}

static int hfs_cmd_getdpb(void)
{
    uint32_t p = hfs_get32(s_req + REQ_ADDR);
    if (!p) return HFS_E_BAD_PARAM;
    /* Remote HFS is a special device.  XEiJ returns a zeroed 16-byte
     * geometry area here; a synthetic 512-byte-sector BPB makes Human68k
     * partially treat the drive like a kernel-managed FAT block device. */
    uint8_t z[16]; memset(z, 0, sizeof(z));
    hfs_guest_write(p, z, sizeof(z));
    return 0;
}

static int hfs_probe_root_write(void)
{
    char path[HOSTFS_MAX_PATH];
    path[0] = 0;
    for (unsigned i = 0; i < 10u; ++i)
    {
        char candidate[HOSTFS_MAX_PATH];
        snprintf(candidate, sizeof(candidate), "%s/PX96ARW%u.TMP", HOSTFS_ROOT, i);
        struct stat st;
        if (stat(candidate, &st) != 0)
        {
            snprintf(path, sizeof(path), "%s", candidate);
            break;
        }
    }
    if (!path[0])
    {
        printf("PX68K_HOSTFS: WRITE PROBE FAIL no free 8.3 probe name under %s\n", HOSTFS_ROOT);
        return 0;
    }

    errno = 0;
    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        int e = errno;
        printf("PX68K_HOSTFS: WRITE PROBE FAIL create=%s errno=%d(%s)\n", path, e, strerror(e));
        return 0;
    }
    static const uint8_t test[8] = { 'P','X','6','8','K','R','W','\n' };
    size_t n = fwrite(test, 1, sizeof(test), fp);
    int ferr = ferror(fp);
    int cerr = fclose(fp);
    int saved = errno;
    if (n != sizeof(test) || ferr || cerr != 0)
    {
        unlink(path);
        printf("PX68K_HOSTFS: WRITE PROBE FAIL write=%lu/%lu ferr=%d close=%d errno=%d(%s)\n",
               (unsigned long)n, (unsigned long)sizeof(test), ferr, cerr, saved, strerror(saved));
        return 0;
    }
    if (unlink(path) != 0)
    {
        int e = errno;
        printf("PX68K_HOSTFS: WRITE PROBE WARN delete failed errno=%d(%s) path=%s\n", e, strerror(e), path);
        /* The medium is nevertheless writable. */
    }
    printf("PX68K_HOSTFS: *** WRITE PROBE OK root=%s (CREATE/WRITE/DELETE) ***\n", HOSTFS_ROOT);
    return 1;
}

void HostFS_Reset(void)
{
    for (int i = 0; i < HOSTFS_MAX_FILES; ++i)
        if (s_files[i].fp) fclose(s_files[i].fp);
    for (int i = 0; i < HOSTFS_MAX_SEARCH; ++i)
        if (s_search[i].dir) closedir(s_search[i].dir);
    memset(s_files, 0, sizeof(s_files));
    memset(s_search, 0, sizeof(s_search));
    s_req = s_driver_base = s_driver_end = 0;
    s_calls = s_read_calls = s_write_calls = s_files_calls = 0;
    s_panic_bat_opens = s_panic_player_opens = s_panic_pan_opens = 0;
    s_drive = -1;
    s_root_writable = -1;
}

int HostFS_Available(void)
{
    struct stat st;
    return stat(HOSTFS_ROOT, &st) == 0 && S_ISDIR(st.st_mode);
}

uint32_t HostFS_InstallDriver(uint32_t guest_addr)
{
    if (!HostFS_Available()) return 0;
    if (s_root_writable < 0) s_root_writable = hfs_probe_root_write();
#if !PX68K_TAB5_DIAG_VERBOSE
    {
        static int r57e73_quiet_logged = 0;
        if (!r57e73_quiet_logged) {
            r57e73_quiet_logged = 1;
            printf("PX68K_HOSTFS_R57E73: production runtime negative-status/path stdio SUPPRESSED; counters/guest error returns unchanged\n");
        }
    }
#endif
    guest_addr &= 0x00ffffffu;
    const uint32_t strategy = guest_addr + 22u;
    const uint32_t intr = guest_addr + 32u;
    uint8_t image[HOSTFS_DRIVER_BYTES];
    memset(image, 0, sizeof(image));
    image[0] = image[1] = image[2] = image[3] = 0xff; /* last device header */
    image[4] = (uint8_t)(HOSTFS_DRIVER_ATTR >> 8);
    image[5] = (uint8_t)HOSTFS_DRIVER_ATTR;
    image[6] = (uint8_t)(strategy >> 24); image[7] = (uint8_t)(strategy >> 16);
    image[8] = (uint8_t)(strategy >> 8);  image[9] = (uint8_t)strategy;
    image[10] = (uint8_t)(intr >> 24); image[11] = (uint8_t)(intr >> 16);
    image[12] = (uint8_t)(intr >> 8); image[13] = (uint8_t)intr;
    memcpy(&image[14], HOSTFS_DRIVER_NAME, 8);
    /* move.b #$90,$E9F802 ; rts */
    { const uint8_t c[10] = {0x13,0xfc,0x00,0x90,0x00,0xe9,0xf8,0x02,0x4e,0x75}; memcpy(&image[22], c, 10); }
    /* move.b #$91,$E9F802 ; rts */
    { const uint8_t c[10] = {0x13,0xfc,0x00,0x91,0x00,0xe9,0xf8,0x02,0x4e,0x75}; memcpy(&image[32], c, 10); }
    if (!hfs_guest_write(guest_addr, image, sizeof(image))) return 0;
    s_driver_base = guest_addr;
    s_driver_end = guest_addr + sizeof(image);
    if (PX68K_TAB5_DIAG_VERBOSE)
        printf("PX68K_HOSTFS: remote driver installed guest=$%06lX end=$%06lX strategy=$%06lX interrupt=$%06lX root=%s\n",
               (unsigned long)guest_addr, (unsigned long)s_driver_end,
               (unsigned long)strategy, (unsigned long)intr, HOSTFS_ROOT);
    return (uint32_t)sizeof(image);
}

void HostFS_StrategyTrap(void)
{
    s_req = m68000_get_reg(M68K_A5) & 0x00ffffffu;
}

void HostFS_InterruptTrap(void)
{
    if (!s_req) return;
    uint8_t cmd = 0; hfs_guest_read(s_req + REQ_COMMAND, &cmd, 1);
    cmd = (uint8_t)((cmd & 0x1fu) | 0x40u);
    ++s_calls;
    int32_t status = HFS_E_INVALID_FUNC;

    switch (cmd)
    {
        case 0x40:
        {
            uint8_t drive8 = 0;
            hfs_guest_read(s_req + REQ_FCB, &drive8, 1);
            uint32_t drive = drive8;
            s_drive = (int)drive;
            /* Human68k 3.02 expects command byte cleared during init. */
            hfs_put8(s_req + REQ_COMMAND, 0u);
            hfs_put8(s_req + REQ_ATTR, 1u); /* one unit */
            hfs_put32(s_req + REQ_ADDR, s_driver_end);
            hfs_put8(s_req + REQ_ERR_LO, 0); hfs_put8(s_req + REQ_ERR_HI, 0);
            printf("PX68K_HOSTFS: *** INIT drive=%lu (%c:) units=1 root=%s driver=$%06lX-$%06lX ***\n",
                   (unsigned long)drive,
                   drive < 26 ? (char)('A' + drive) : '?', HOSTFS_ROOT,
                   (unsigned long)s_driver_base, (unsigned long)(s_driver_end - 1u));
            return;
        }
        case 0x41: status = hfs_cmd_chdir(); break;
        case 0x42: status = hfs_cmd_mkdir(); break;
        case 0x43: status = hfs_cmd_rmdir(); break;
        case 0x44: status = hfs_cmd_rename(); break;
        case 0x45: status = hfs_cmd_delete(); break;
        case 0x46: status = hfs_cmd_chmod(); break;
        case 0x47: status = hfs_cmd_files(1); break;
        case 0x48: status = hfs_cmd_files(0); break;
        case 0x49: status = hfs_open_common(1); break;
        case 0x4a: status = hfs_open_common(0); break;
        case 0x4b: status = hfs_cmd_close(); break;
        case 0x4c: status = hfs_cmd_read(); break;
        case 0x4d: status = hfs_cmd_write(); break;
        case 0x4e: status = hfs_cmd_seek(); break;
        case 0x4f: status = hfs_cmd_filedate(); break;
        case 0x50: status = hfs_cmd_dskfre(); break;
        case 0x51: status = hfs_cmd_drvctrl(); break;
        case 0x52: status = hfs_cmd_getdpb(); break;
        case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
            status = 0; break;
        default: status = HFS_E_INVALID_FUNC; break;
    }

    hfs_req_status(status);
#if PX68K_TAB5_DIAG_VERBOSE
    if (cmd != 0x48 && (status < 0 || s_calls <= 24u))
    {
        uint8_t unit = 0;
        hfs_guest_read(s_req + REQ_UNIT, &unit, 1);
        printf("PX68K_HOSTFS: req #%lu cmd=%02X unit=%u status=%ld req=$%06lX\n",
               (unsigned long)s_calls, cmd, (unsigned)unit,
               (long)status, (unsigned long)s_req);
    }
#endif
}

uint32_t HostFS_DebugCalls(void) { return s_calls; }
int HostFS_DebugDrive(void) { return s_drive; }
uint32_t HostFS_DebugPanicBatchOpens(void) { return s_panic_bat_opens; }
uint32_t HostFS_DebugPanicPlayerOpens(void) { return s_panic_player_opens; }
uint32_t HostFS_DebugPanicPanOpens(void) { return s_panic_pan_opens; }
