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

#include "common.h"
#include "../libretro/prop.h"
#include "m68000.h"
#include "x68kmemory.h"
#include "hostfs.h"
#include "midi.h"

/* PX68K_HOSTFS_R1_FINAL: Human68k remote-drive $40..$58 compatibility audit. */
#ifdef ESP_PLATFORM
extern int esp_vfs_fat_info(const char *base_path, uint64_t *out_total_bytes,
                            uint64_t *out_free_bytes);
#endif

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
#include <utime.h>

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
#define HFS_E_CANT_IOCTL        (-17)
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
    uint32_t pending_datetime;
    uint8_t pending_datetime_valid;
    /* X68KTAB_R1A15_HOSTFS_FASTPATH: host stdio cursor/flush ownership. */
    uint8_t writable;
    uint8_t host_pos_valid;
    uint32_t host_pos;
    uint32_t r2a2_read_ops;
    uint32_t r2a2_read_bytes;
} HostFile;

typedef struct
{
    uint32_t key;
    DIR *dir;
    char dirpath[HOSTFS_MAX_PATH];
    char pat_name1[9];
    char pat_name2[11];
    char pat_ext[4];
    uint8_t attr;
    uint8_t wildcard;
    uint8_t active;
#define HOSTFS_R2_SEEN_MAX 32u
    uint8_t r2_seen_count;
    uint8_t r2_seen[HOSTFS_R2_SEEN_MAX][21];
    uint32_t r2a1_serial;
    uint32_t r2a1_emit;
} HostSearch;

static HostFile s_files[HOSTFS_MAX_FILES];
static HostSearch s_search[HOSTFS_MAX_SEARCH];
static uint8_t s_io[HOSTFS_IO_CHUNK];
static uint32_t s_req;
static uint32_t s_driver_base;
static uint32_t s_driver_end;
static uint32_t s_calls;
static uint32_t s_read_calls;
static uint32_t s_write_calls;
static uint32_t s_files_calls;
static int s_drive = -1;
static int s_root_writable = -1;
static uint8_t s_drv_user_lock;
static uint8_t s_drv_os_lock;
static uint8_t s_drv_led;
/* PX68K_HOSTFS_R2_ENUM_PATH_FIX: exact-path fast path + FILES duplicate guard. */
static uint32_t s_hfs_r2_direct_hit;
static uint32_t s_hfs_r2_case_scan;
static uint32_t s_hfs_r2_dup_skip;
/* PX68K_HOSTFS_R2A1_ENUM_TRACE: diagnostic only; no HostFS semantics change. */
static uint32_t s_hfs_r2a1_serial;
static uint32_t s_hfs_r2a1_files_log;
static uint32_t s_hfs_r2a1_nfiles_log;
static uint32_t s_hfs_r2a1_emit_log;
static uint32_t s_hfs_r2a1_scan_log;
/* PX68K_HOSTFS_R2A2_FULLPASS: final one-pass ownership trace. */
static uint32_t s_hfs_r2a2_scan_entries;
static uint32_t s_hfs_r2a2_resolve_log;
static uint32_t s_hfs_r2a2_open_log;
static uint32_t s_hfs_r2a2_read_log;
static uint32_t s_hfs_r2a2_close_log;

/* X68KTAB_R1A14_MMDSP_ENUM_DIAG
 * Diagnostic only: remember compact hashes of recently emitted FILES entries
 * across independent _FILES search serials.  This does not suppress or alter
 * guest-visible directory results.  HFS_FILE z bit15 marks a cross-search
 * repeat so one MMDSP selector run can prove whether duplication originates
 * in HostFS enumeration or above it. */
#define HOSTFS_R1A14_RECENT_EMIT_MAX 128u
typedef struct {
    uint32_t fb;
    uint32_t key_hash;
    uint32_t serial;
} HostFSR1A14RecentEmit;
static HostFSR1A14RecentEmit s_hfs_r1a14_recent[HOSTFS_R1A14_RECENT_EMIT_MAX];
static uint16_t s_hfs_r1a14_recent_pos;

/* X68KTAB_R1A15_HOSTFS_FASTPATH
 * The Human68k remote-drive protocol is preserved byte-for-byte; only host
 * work with no guest-visible effect is removed.  Counters are diagnostics. */
static uint32_t s_hfs_r1a15_media_fast;
static uint32_t s_hfs_r1a15_drvctrl_state;
static uint32_t s_hfs_r1a15_flush_noop;
static uint32_t s_hfs_r1a15_flush_files;
static uint32_t s_hfs_r1a15_read_seek_skip;
static uint32_t s_hfs_r1a15_read_seek_do;
static uint32_t s_hfs_r1a15_write_seek_skip;
static uint32_t s_hfs_r1a15_write_seek_do;
static uint32_t s_hfs_r1a15_close_flush_skip;

/* X68KTAB_FULLPASS_RESULTS_PROD_R1A1_SOURCE_OVERWRITE */
/* X68KTAB_FULLPASS_RESULTS_PROD_R1A2_SOURCE_OVERWRITE: current-lineage ABI restored. */
/* X68KTAB_FULLPASS_RESULTS_PROD_R1A3_ADDRESS_ERROR_FIX: DOS search semantics restored. */
/* X68KTAB_HOSTFS_HF1_FATFS_FASTMISS_PROD: skip redundant ESP/FatFs case rescans + quiet CPU1 hot path. */
/* PX68K_HOSTFS_FULLPASS_RESULTS_PROD_R1
 * FULLPASS result fix:
 *  - Human68k _FILES attribute bits are selectors: an entry is returned only
 *    when at least one selected ordinary attribute bit matches.
 *  - cache exact full paths proven missing by the expensive case-fold scan.
 *    Positive exact-spelling hits already use stat() and need no cache.
 *  - any namespace mutation invalidates the negative cache generation.
 */
#define PX68K_FULLPASS_TRACE 0
#define HFS_PROD_NEG_SLOTS 8u
static char s_hfs_prod_neg_path[HFS_PROD_NEG_SLOTS][HOSTFS_MAX_PATH];
static uint32_t s_hfs_prod_neg_gen[HFS_PROD_NEG_SLOTS];
static uint32_t s_hfs_prod_neg_generation = 1u;
static uint8_t s_hfs_prod_neg_next;

/* X68KTAB_R1A4_HF1_ONEPASS_FINAL_ORACLE_R2: compact CPU1-only telemetry. */
#ifdef ESP_PLATFORM
static uint32_t hfs_onepass_hash(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) return 0u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static void hfs_onepass_name(uint8_t kind, const char *path)
{
    const char *b = path ? strrchr(path, '/') : NULL;
    b = b ? b + 1 : (path ? path : "");
    uint32_t x=0u, y=0u;
    for (unsigned i=0; i<4u && b[i]; ++i) ((uint8_t *)&x)[i] = (uint8_t)b[i];
    for (unsigned i=0; i<4u && b[i+4u]; ++i) ((uint8_t *)&y)[i] = (uint8_t)b[i+4u];
    MIDI_OnePassTrace(MIDI_OP_HFS_NAME, kind, (uint16_t)hfs_onepass_hash(b), x, y);
}
static void hfs_onepass_guest183(const uint8_t key[21])
{
    uint32_t h = 2166136261u, x=0u, y=0u;
    if (!key) return;
    for (unsigned i=0; i<21u; ++i) { h ^= key[i]; h *= 16777619u; }
    memcpy(&x, key, 4); memcpy(&y, key+4, 4);
    MIDI_OnePassTrace(MIDI_OP_HFS_NAME, 1u, (uint16_t)h, x, y);
}

static uint32_t hfs_r1a14_key_hash(const uint8_t key[21])
{
    uint32_t h = 2166136261u;
    if (!key) return 0u;
    for (unsigned i=0; i<21u; ++i) { h ^= key[i]; h *= 16777619u; }
    return h;
}

static int hfs_r1a14_note_emit(uint32_t fb, uint32_t serial, const uint8_t key[21])
{
    const uint32_t kh = hfs_r1a14_key_hash(key);
    int repeated = 0;
    for (unsigned i=0; i<HOSTFS_R1A14_RECENT_EMIT_MAX; ++i) {
        const HostFSR1A14RecentEmit *r=&s_hfs_r1a14_recent[i];
        if (r->fb==fb && r->key_hash==kh && r->serial!=0u && r->serial!=serial) {
            repeated = 1;
            break;
        }
    }
    HostFSR1A14RecentEmit *dst =
        &s_hfs_r1a14_recent[s_hfs_r1a14_recent_pos++ % HOSTFS_R1A14_RECENT_EMIT_MAX];
    dst->fb=fb; dst->key_hash=kh; dst->serial=serial;
    return repeated;
}
#endif

static void hfs_prod_neg_invalidate(void)
{
    ++s_hfs_prod_neg_generation;
    if (s_hfs_prod_neg_generation == 0u)
    {
        memset(s_hfs_prod_neg_gen, 0, sizeof(s_hfs_prod_neg_gen));
        s_hfs_prod_neg_generation = 1u;
    }
    s_hfs_prod_neg_next = 0u;
}

static int hfs_prod_neg_hit(const char *path)
{
    if (!path || !*path) return 0;
    for (unsigned i = 0; i < HFS_PROD_NEG_SLOTS; ++i)
        if (s_hfs_prod_neg_gen[i] == s_hfs_prod_neg_generation &&
            strcmp(s_hfs_prod_neg_path[i], path) == 0)
            return 1;
    return 0;
}

static void hfs_prod_neg_add(const char *path)
{
    if (!path || !*path) return;
    const size_t n = strlen(path);
    if (n >= HOSTFS_MAX_PATH) return;
    const unsigned slot = (unsigned)(s_hfs_prod_neg_next++ % HFS_PROD_NEG_SLOTS);
    memcpy(s_hfs_prod_neg_path[slot], path, n + 1u);
    s_hfs_prod_neg_gen[slot] = s_hfs_prod_neg_generation;
}

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

static int hfs_namests_pattern_183(uint32_t ns_addr,
                                   char name1[9], char name2[11], char ext[4])
{
    uint8_t ns[88];
    if (!ns_addr || !name1 || !name2 || !ext ||
        !hfs_guest_read(ns_addr, ns, sizeof(ns))) return 0;
    memset(name1, ' ', 8); name1[8] = 0;
    memset(name2, ' ', 10); name2[10] = 0;
    memset(ext, ' ', 3); ext[3] = 0;
    for (size_t i = 0; i < 8; ++i) if (ns[67 + i]) name1[i] = (char)ns[67 + i];
    for (size_t i = 0; i < 3; ++i) if (ns[75 + i]) ext[i] = (char)ns[75 + i];
    for (size_t i = 0; i < 10; ++i) if (ns[78 + i]) name2[i] = (char)ns[78 + i];
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

    /* R2 fast path: avoid opendir/readdir when guest spelling already resolves. */
    struct stat direct_st;
    if (stat(input, &direct_st) == 0)
    {
        size_t n = strlen(input);
        if (n + 1u > outcap) return 0;
        memcpy(output, input, n + 1u);
        if (exists_out) *exists_out = 1;
        ++s_hfs_r2_direct_hit;
#ifdef ESP_PLATFORM
        MIDI_OnePassTrace(MIDI_OP_HFS_PATH, 1u, 0u, hfs_onepass_hash(input), s_hfs_r2_direct_hit);
#endif
        return 1;
    }
    if (!allow_missing_leaf && hfs_prod_neg_hit(input))
    {
#ifdef ESP_PLATFORM
        MIDI_OnePassTrace(MIDI_OP_HFS_PATH, 2u, 0u, hfs_onepass_hash(input), s_hfs_prod_neg_generation);
#endif
        errno = ENOENT;
        return 0;
    }
#ifdef ESP_PLATFORM
    /* /sdcard is ESP-IDF FatFs. FAT object lookup is case-insensitive, so a
     * failed stat() already proves that an ASCII case-fold readdir() pass
     * cannot discover the same object. FULLPASS confirms this target behavior:
     * existing RCD.X opened as /sdcard/rcd.x via direct stat(), while every
     * observed fallback CASESCAN ended with match=''.
     *
     * Keep the generic scan below for non-ESP builds. For create/newfile,
     * preserve the requested spelling and let FatFs validate parent/path. */
    if (!allow_missing_leaf)
    {
        hfs_prod_neg_add(input);
        MIDI_OnePassTrace(MIDI_OP_HFS_PATH, 3u, 0u, hfs_onepass_hash(input), s_hfs_prod_neg_generation);
        errno = ENOENT;
        return 0;
    }
    {
        const size_t n = strlen(input);
        if (n + 1u > outcap) return 0;
        memcpy(output, input, n + 1u);
        if (exists_out) *exists_out = 0;
        return 1;
    }
#endif
    ++s_hfs_r2_case_scan;
    if (++s_hfs_r2a1_scan_log <= 32u)
        if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A1 CASESCAN #%lu input='%s' direct=%lu scan=%lu\n",
               (unsigned long)s_hfs_r2a1_scan_log, input,
               (unsigned long)s_hfs_r2_direct_hit, (unsigned long)s_hfs_r2_case_scan);

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
        uint32_t r2a2_here = 0;
        while ((de = readdir(d)) != NULL)
        {
            ++r2a2_here;
            if (hfs_ascii_name_eq(de->d_name, component))
            {
                snprintf(actual, sizeof(actual), "%s", de->d_name);
                break;
            }
        }
        closedir(d);
        s_hfs_r2a2_scan_entries += r2a2_here;
        if (++s_hfs_r2a2_resolve_log <= 96u)
            if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A2 RESOLVE #%lu input='%s' dir='%s' component='%s' match='%s' entries=%lu totalEntries=%lu\n",
                   (unsigned long)s_hfs_r2a2_resolve_log, input, output, component, actual,
                   (unsigned long)r2a2_here, (unsigned long)s_hfs_r2a2_scan_entries);

        if (!actual[0])
        {
            if (!(allow_missing_leaf && last))
            {
                hfs_prod_neg_add(input);
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
    if (PX68K_FULLPASS_TRACE)
        printf("PX68K_HOSTFS: %s FAILED fcb=$%06lX mode=%lu raw='%s' path='%s' errno=%d(%s) writable=%d\n",
               op ? op : "OP", (unsigned long)fcb, (unsigned long)mode,
               raw ? raw : "", resolved ? resolved : "", e, strerror(e), s_root_writable);
}

static int hfs_split_183(const char *name, char name1[9], char name2[11], char ext[4])
{
    memset(name1, ' ', 8); name1[8] = 0;
    memset(name2, ' ', 10); name2[10] = 0;
    memset(ext, ' ', 3); ext[3] = 0;
    if (!name || !*name) return 0;
    const char *dot = strrchr(name, '.');
    size_t main_len = (dot && dot != name) ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = (dot && dot != name) ? strlen(dot + 1) : 0u;
    if (main_len == 0u || main_len > 18u || ext_len > 3u) return 0;
    for (size_t i = 0; i < main_len; ++i)
    {
        unsigned char c = (unsigned char)name[i];
        if (i < 8u) name1[i] = (char)toupper(c);
        else name2[i - 8u] = (char)toupper(c);
    }
    if (dot && dot != name)
        for (size_t i = 0; i < ext_len; ++i)
            ext[i] = (char)toupper((unsigned char)dot[1 + i]);
    return 1;
}

static int hfs_pat_field(const char *pat, const char *val, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char p = (unsigned char)toupper((unsigned char)pat[i]);
        unsigned char v = (unsigned char)toupper((unsigned char)val[i]);
        if (p == '*') return 1; /* wildcard rest of this fixed field */
        if (p == '?') continue;
        if (p != v) return 0;
    }
    return 1;
}

static int hfs_match_183(const HostSearch *s, const char *name)
{
    char n1[9], n2[11], e[4];
    if (!hfs_split_183(name, n1, n2, e)) return 0;
    return hfs_pat_field(s->pat_name1, n1, 8) &&
           hfs_pat_field(s->pat_name2, n2, 10) &&
           hfs_pat_field(s->pat_ext, e, 3);
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
    char n1s[9], n2s[11], es[4];
    uint8_t n1[8], n2[10], ex[3];
    if (hfs_split_183(name, n1s, n2s, es))
    {
        memcpy(n1, n1s, 8); memcpy(n2, n2s, 10); memcpy(ex, es, 3);
    }
    else
    {
        memset(n1, ' ', sizeof(n1)); memset(n2, ' ', sizeof(n2)); memset(ex, ' ', sizeof(ex));
    }
    hfs_guest_write(fcb + FCB_NAME1, n1, sizeof(n1));
    hfs_guest_write(fcb + FCB_EXT, ex, sizeof(ex));
    hfs_guest_write(fcb + FCB_NAME2, n2, sizeof(n2));
    hfs_put8(fcb + FCB_ATTR, hfs_attr_for_stat(st));
    uint16_t d = 0, t = 0;
    hfs_dos_datetime(st ? st->st_mtime : 0, &d, &t);
    hfs_put16(fcb + FCB_TIME, t); hfs_put16(fcb + FCB_DATE, d);
    hfs_put32(fcb + FCB_SIZE, st && st->st_size > 0 ? (uint32_t)st->st_size : 0u);
    hfs_put32(fcb + FCB_POS, 0u);
}

static HostFile *hfs_file_find(uint32_t fcb)
{
    for (int i = 0; i < HOSTFS_MAX_FILES; ++i)
        if (s_files[i].fp && s_files[i].fcb == fcb) return &s_files[i];
    return NULL;
}

static int hfs_datetime_to_time(uint32_t datetime, time_t *out)
{
    if (!datetime || !out) return 0;
    uint16_t date = (uint16_t)(datetime >> 16), tim = (uint16_t)datetime;
    int year = 1980 + ((date >> 9) & 0x7f);
    int mon = (date >> 5) & 0x0f, day = date & 0x1f;
    int hour = (tim >> 11) & 0x1f, min = (tim >> 5) & 0x3f, sec = (tim & 0x1f) * 2;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) return 0;
    struct tm tmv; memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900; tmv.tm_mon = mon - 1; tmv.tm_mday = day;
    tmv.tm_hour = hour; tmv.tm_min = min; tmv.tm_sec = sec; tmv.tm_isdst = -1;
    time_t t = mktime(&tmv); if (t == (time_t)-1) return 0; *out = t; return 1;
}

static int hfs_file_close_one(HostFile *hf)
{
    if (!hf || !hf->fp) return 0;
    int rc = 0;
    /* fflush() on an input-only stream can force VFS/FatFs work even though
     * no guest data can be dirty.  Only writable handles need a flush. */
    if (hf->writable) {
        if (fflush(hf->fp) != 0) rc = HFS_E_CANT_WRITE;
    } else {
        ++s_hfs_r1a15_close_flush_skip;
    }
    if (fclose(hf->fp) != 0) rc = HFS_E_CANT_WRITE;
    hf->fp = NULL;
    if (hf->pending_datetime_valid)
    {
        time_t t;
        if (!hfs_datetime_to_time(hf->pending_datetime, &t)) rc = HFS_E_BAD_PARAM;
        else { struct utimbuf u; u.actime = t; u.modtime = t; if (utime(hf->path, &u) != 0) rc = HFS_E_CANT_WRITE; }
    }
    struct stat st;
    if (stat(hf->path, &st) == 0)
    {
        uint16_t d = 0, t = 0; hfs_dos_datetime(st.st_mtime, &d, &t);
        hfs_put8(hf->fcb + FCB_ATTR, hfs_attr_for_stat(&st));
        hfs_put16(hf->fcb + FCB_TIME, t); hfs_put16(hf->fcb + FCB_DATE, d);
        hfs_put32(hf->fcb + FCB_SIZE, st.st_size > 0 ? (uint32_t)st.st_size : 0u);
    }
    memset(hf, 0, sizeof(*hf));
    return rc;
}


static HostFile *hfs_file_alloc(uint32_t fcb)
{
    HostFile *old = hfs_file_find(fcb);
    if (old) (void)hfs_file_close_one(old);
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

static int hfs_r2_guest_key(const char *name, uint8_t key[21])
{
    char n1[9], n2[11], ext[4];
    if (!key || !hfs_split_183(name, n1, n2, ext)) return 0;
    memcpy(key, n1, 8);
    memcpy(key + 8, n2, 10);
    memcpy(key + 18, ext, 3);
    return 1;
}

static int hfs_r2_seen_or_add(HostSearch *s, const char *name, uint8_t key_out[21])
{
    uint8_t key[21];
    if (!s || !hfs_r2_guest_key(name, key)) return 0;
    if (key_out) memcpy(key_out, key, sizeof(key));
    for (uint8_t i = 0; i < s->r2_seen_count; ++i)
        if (!memcmp(s->r2_seen[i], key, sizeof(key))) return 1;
    if (s->r2_seen_count < HOSTFS_R2_SEEN_MAX)
    {
        memcpy(s->r2_seen[s->r2_seen_count], key, sizeof(key));
        ++s->r2_seen_count;
    }
    return 0;
}

static int hfs_fill_files_next(HostSearch *s, uint32_t fb)
{
    if (!s || !s->dir || !fb) return HFS_E_NO_MORE_FILES;
    for (;;)
    {
        struct dirent *de = readdir(s->dir);
        if (!de)
        {
            if (s_hfs_r2a1_files_log <= 32u)
                if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A1 EOF serial=%lu fb=$%06lX emitted=%lu direct=%lu scan=%lu dup=%lu\n",
                       (unsigned long)s->r2a1_serial, (unsigned long)fb,
                       (unsigned long)s->r2a1_emit, (unsigned long)s_hfs_r2_direct_hit,
                       (unsigned long)s_hfs_r2_case_scan, (unsigned long)s_hfs_r2_dup_skip);
            hfs_put16(fb + FILES_NEXT_OFF, 0xffffu);
            hfs_search_close(s);
            return HFS_E_NO_MORE_FILES;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!hfs_match_183(s, de->d_name)) continue;

        char path[HOSTFS_MAX_PATH];
        int n = snprintf(path, sizeof(path), "%s/%s", s->dirpath, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        const uint8_t attr = hfs_attr_for_stat(&st);
        /* PROD R1A3: restore the proven Human68k/DOS search contract from
         * the pre-PROD lineage. Ordinary files remain searchable; directories
         * are included only when requested attribute bit 0x10 is set. */
        if ((attr & 0x10u) && !(s->attr & 0x10u)) continue;

        uint8_t r2key[21];
        if (hfs_r2_seen_or_add(s, de->d_name, r2key))
        {
            ++s_hfs_r2_dup_skip;
            if (PX68K_FULLPASS_TRACE && s_hfs_r2_dup_skip <= 8u)
                (printf)("PX68K_HOSTFS_R2_DUP skip=%lu dir='%s' host='%s' guest='%.8s%.10s.%.3s'\n",
                       (unsigned long)s_hfs_r2_dup_skip, s->dirpath, de->d_name,
                       (const char *)r2key, (const char *)(r2key + 8), (const char *)(r2key + 18));
            continue;
        }

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
        ++s->r2a1_emit;
#ifdef ESP_PLATFORM
        hfs_onepass_guest183(r2key);
        const int r1a14_crossdup = hfs_r1a14_note_emit(
            fb, s->r2a1_serial, r2key);
        MIDI_OnePassTrace(MIDI_OP_HFS_FILE, attr,
                          (uint16_t)((s->r2a1_emit & 0x7fffu) |
                                     (r1a14_crossdup ? 0x8000u : 0u)),
                          st.st_size > 0 ? (uint32_t)st.st_size : 0u,
                          hfs_onepass_hash(de->d_name));
#endif
        if (++s_hfs_r2a1_emit_log <= 64u)
            if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A1 EMIT serial=%lu n=%lu fb=$%06lX host='%s' out='%s' direct=%lu scan=%lu\n",
                   (unsigned long)s->r2a1_serial, (unsigned long)s->r2a1_emit,
                   (unsigned long)fb, de->d_name, (const char *)out,
                   (unsigned long)s_hfs_r2_direct_hit, (unsigned long)s_hfs_r2_case_scan);
        if (s->wildcard) hfs_put16(fb + FILES_NEXT_OFF, 0u);
        else { hfs_put16(fb + FILES_NEXT_OFF, 0xffffu); hfs_search_close(s); }
        return 0;
    }
}

static int hfs_cmd_chdir(void)
{
    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH]; struct stat st;
    if (!hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 1))
        return HFS_E_BAD_NAME;
    if (!hfs_resolve_existing(raw, path, sizeof(path)) || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return HFS_E_DIR_NOT_FOUND;
    return 0;
}

static int hfs_cmd_mkdir(void)
{
    hfs_prod_neg_invalidate();
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
    hfs_prod_neg_invalidate();
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
    hfs_prod_neg_invalidate();
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
    hfs_prod_neg_invalidate();
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
        const uint32_t ns = hfs_get32(s_req + REQ_ADDR);
        char rawdir[HOSTFS_MAX_PATH], dir[HOSTFS_MAX_PATH];
        char p1[9], p2[11], pe[4];
        if (!hfs_namests(ns, rawdir, sizeof(rawdir), NULL, NULL, 1) ||
            !hfs_namests_pattern_183(ns, p1, p2, pe)) return HFS_E_BAD_NAME;
        if (!hfs_resolve_existing(rawdir, dir, sizeof(dir))) return HFS_E_DIR_NOT_FOUND;
        const int r2a1_restart = hfs_search_find(fb) != NULL;
        HostSearch *hs = hfs_search_alloc(fb); if (!hs) return HFS_E_TOO_MANY_OPEN;
        hs->r2a1_serial = ++s_hfs_r2a1_serial;
        strncpy(hs->dirpath, dir, sizeof(hs->dirpath) - 1); hs->dirpath[sizeof(hs->dirpath) - 1] = 0;
        memcpy(hs->pat_name1, p1, sizeof(hs->pat_name1));
        memcpy(hs->pat_name2, p2, sizeof(hs->pat_name2));
        memcpy(hs->pat_ext, pe, sizeof(hs->pat_ext));
        hs->wildcard = (uint8_t)(memchr(p1, '*', 8) || memchr(p1, '?', 8) ||
                                 memchr(p2, '*', 10) || memchr(p2, '?', 10) ||
                                 memchr(pe, '*', 3) || memchr(pe, '?', 3));
        hfs_guest_read(s_req + REQ_ATTR, &hs->attr, 1);
#ifdef ESP_PLATFORM
        MIDI_OnePassTrace(MIDI_OP_HFS_ENUM, hs->attr,
                          (uint16_t)((hs->wildcard ? 0x0100u : 0u) | (r2a1_restart ? 1u : 0u)),
                          fb, hfs_onepass_hash(rawdir));
        hfs_onepass_name(3u, rawdir);
#endif
        hs->dir = opendir(hs->dirpath); ++s_files_calls;
        if (++s_hfs_r2a1_files_log <= 32u)
            if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A1 FILES serial=%lu fb=$%06lX restart=%d dir='%s' pat='%.8s%.10s.%.3s' wild=%u attr=%02X\n",
                   (unsigned long)hs->r2a1_serial, (unsigned long)fb, r2a1_restart, hs->dirpath,
                   hs->pat_name1, hs->pat_name2, hs->pat_ext, (unsigned)hs->wildcard, (unsigned)hs->attr);
        if (!hs->dir) { hfs_search_close(hs); return HFS_E_DIR_NOT_FOUND; }
        return hfs_fill_files_next(hs, fb);
    }
    if (hfs_get16(fb + FILES_NEXT_OFF) == 0xffffu) return HFS_E_NO_MORE_FILES;
    HostSearch *hs = hfs_search_find(fb);
    if (!hs)
    {
        if (++s_hfs_r2a1_nfiles_log <= 64u)
            if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A1 NFILES-MISS fb=$%06lX next=%04X\n",
                   (unsigned long)fb, (unsigned)hfs_get16(fb + FILES_NEXT_OFF));
        return HFS_E_NO_MORE_FILES;
    }
    if (++s_hfs_r2a1_nfiles_log <= 64u)
        if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A1 NFILES serial=%lu fb=$%06lX emitted=%lu next=%04X\n",
               (unsigned long)hs->r2a1_serial, (unsigned long)fb,
               (unsigned long)hs->r2a1_emit, (unsigned)hfs_get16(fb + FILES_NEXT_OFF));
#ifdef ESP_PLATFORM
    MIDI_OnePassTrace(MIDI_OP_HFS_ENUM, hs->attr, 0x0200u, fb, hs->r2a1_emit);
#endif
    return hfs_fill_files_next(hs, fb);
}

static int hfs_open_common(int create)
{
    const uint32_t r2a2_direct0 = s_hfs_r2_direct_hit;
    const uint32_t r2a2_scan0 = s_hfs_r2_case_scan;
    const uint32_t r2a2_entries0 = s_hfs_r2a2_scan_entries;

    char raw[HOSTFS_MAX_PATH], path[HOSTFS_MAX_PATH]; struct stat st;
    const uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    if (!fcb || !hfs_namests(hfs_get32(s_req + REQ_ADDR), raw, sizeof(raw), NULL, NULL, 0))
        return HFS_E_BAD_NAME;
    if (create) hfs_prod_neg_invalidate();

    uint8_t mode = 0;
    hfs_guest_read(fcb + FCB_MODE, &mode, 1);
    if (mode > 2u && !create) return HFS_E_BAD_MODE;

    int exists = 0;
    if (!hfs_resolve_case_path(raw, create ? 1 : 0, path, sizeof(path), &exists))
    {
        int e = errno ? errno : ENOENT;
        hfs_log_path_failure(create ? "CREATE-RESOLVE" : "OPEN-RESOLVE", raw, "", e, fcb, mode);
#ifdef ESP_PLATFORM
        hfs_onepass_name(4u, raw);
        MIDI_OnePassTrace(MIDI_OP_HFS_FAIL, (uint8_t)(create ? 1u : 0u), (uint16_t)(e & 0xffff), fcb, hfs_onepass_hash(raw));
#endif
        if (++s_hfs_r2a2_open_log <= 96u)
            if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A2 OPEN-FAIL #%lu create=%d fcb=$%06lX raw='%s' directDelta=%lu scanDelta=%lu entriesDelta=%lu\n",
                   (unsigned long)s_hfs_r2a2_open_log, create, (unsigned long)fcb, raw,
                   (unsigned long)(s_hfs_r2_direct_hit-r2a2_direct0),
                   (unsigned long)(s_hfs_r2_case_scan-r2a2_scan0),
                   (unsigned long)(s_hfs_r2a2_scan_entries-r2a2_entries0));
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
        if (PX68K_FULLPASS_TRACE && s_calls <= 200u)
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
        if (PX68K_FULLPASS_TRACE && s_calls <= 200u)
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
#ifdef ESP_PLATFORM
        hfs_onepass_name(5u, path);
        MIDI_OnePassTrace(MIDI_OP_HFS_FAIL, (uint8_t)(create ? 3u : 2u), (uint16_t)(e & 0xffff), fcb, hfs_onepass_hash(path));
#endif
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
    hf->writable = (uint8_t)(create || mode != 0u);
    hf->host_pos = 0u;
    hf->host_pos_valid = 1u;

    if (stat(path, &st) != 0) memset(&st, 0, sizeof(st));
    hfs_fill_fcb(fcb, path, &st);
    if (++s_hfs_r2a2_open_log <= 96u)
        if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A2 OPEN #%lu create=%d fcb=$%06lX raw='%s' path='%s' size=%lu directDelta=%lu scanDelta=%lu entriesDelta=%lu\n",
                 (unsigned long)s_hfs_r2a2_open_log, create, (unsigned long)fcb, raw, path,
                 (unsigned long)(st.st_size > 0 ? st.st_size : 0),
                 (unsigned long)(s_hfs_r2_direct_hit-r2a2_direct0),
                 (unsigned long)(s_hfs_r2_case_scan-r2a2_scan0),
                 (unsigned long)(s_hfs_r2a2_scan_entries-r2a2_entries0));
    if (PX68K_FULLPASS_TRACE)
        printf("PX68K_HOSTFS: %s OK fcb=$%06lX mode=%u path=%s size=%lu\n",
               create ? "CREATE" : "OPEN", (unsigned long)fcb, (unsigned)mode, path,
               (unsigned long)(st.st_size > 0 ? st.st_size : 0));
#ifdef ESP_PLATFORM
    hfs_onepass_name(2u, path);
    MIDI_OnePassTrace(MIDI_OP_HFS_OPEN, (uint8_t)(create ? 1u : 0u), (uint16_t)mode, fcb,
                      st.st_size > 0 ? (uint32_t)st.st_size : 0u);
#endif
    return 0;
}

static int hfs_cmd_close(void)
{
    uint32_t fcb = hfs_get32(s_req + REQ_FCB);
    HostFile *hf = hfs_file_find(fcb);
    if (!hf) return 0;
    if (++s_hfs_r2a2_close_log <= 96u)
        if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A2 CLOSE #%lu fcb=$%06lX pos=%lu size=%lu readOps=%lu readBytes=%lu path='%s'\n",
               (unsigned long)s_hfs_r2a2_close_log, (unsigned long)fcb,
               (unsigned long)hfs_get32(fcb + FCB_POS), (unsigned long)hfs_get32(fcb + FCB_SIZE),
               (unsigned long)hf->r2a2_read_ops, (unsigned long)hf->r2a2_read_bytes, hf->path);
#ifdef ESP_PLATFORM
    MIDI_OnePassTrace(MIDI_OP_HFS_CLOSE, 0u, (uint16_t)(hf->r2a2_read_ops & 0xffffu), fcb, hf->r2a2_read_bytes);
#endif
    return hfs_file_close_one(hf);
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
    if (hf->host_pos_valid && hf->host_pos == pos) {
        ++s_hfs_r1a15_read_seek_skip;
    } else {
        if (fseek(hf->fp, (long)pos, SEEK_SET) != 0) { hf->host_pos_valid = 0u; return HFS_E_CANT_SEEK; }
        hf->host_pos = pos;
        hf->host_pos_valid = 1u;
        ++s_hfs_r1a15_read_seek_do;
    }
    while (done < want)
    {
        uint32_t chunk = want - done; if (chunk > HOSTFS_IO_CHUNK) chunk = HOSTFS_IO_CHUNK;
        size_t n = fread(s_io, 1, chunk, hf->fp);
        hf->host_pos += (uint32_t)n;
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
    ++hf->r2a2_read_ops;
    hf->r2a2_read_bytes += done;
    if (++s_hfs_r2a2_read_log <= 160u)
        if (PX68K_FULLPASS_TRACE) (printf)("PX68K_HOSTFS_R2A2 READ #%lu fcb=$%06lX op=%lu pos=%lu want=%lu got=%lu total=%lu path='%s'\n",
                 (unsigned long)s_hfs_r2a2_read_log, (unsigned long)fcb,
                 (unsigned long)hf->r2a2_read_ops, (unsigned long)pos,
                 (unsigned long)want, (unsigned long)done,
                 (unsigned long)hf->r2a2_read_bytes, hf->path);
    ++s_read_calls;
    if (PX68K_FULLPASS_TRACE && s_read_calls <= 32u)
        printf("PX68K_HOSTFS: READ #%lu fcb=$%06lX pos=%lu want=%lu got=%lu path=%s\n",
               (unsigned long)s_read_calls, (unsigned long)fcb,
               (unsigned long)pos, (unsigned long)want, (unsigned long)done, hf->path);
#ifdef ESP_PLATFORM
    MIDI_OnePassTrace(MIDI_OP_HFS_READ, (uint8_t)(fcb & 0xffu),
                      (uint16_t)(done > 0xffffu ? 0xffffu : done), pos,
                      ((want > 0xffffu ? 0xffffu : want) << 16) | (done > 0xffffu ? 0xffffu : done));
#endif
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
    if (hf->host_pos_valid && hf->host_pos == pos) {
        ++s_hfs_r1a15_write_seek_skip;
    } else {
        if (fseek(hf->fp, (long)pos, SEEK_SET) != 0) { hf->host_pos_valid = 0u; return HFS_E_CANT_SEEK; }
        hf->host_pos = pos;
        hf->host_pos_valid = 1u;
        ++s_hfs_r1a15_write_seek_do;
    }

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
        if (PX68K_FULLPASS_TRACE)
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
        hf->host_pos += (uint32_t)n;
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
    if (PX68K_FULLPASS_TRACE && s_write_calls <= 32u)
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
    HostFile *hf = hfs_file_find(fcb); if (!hf) return HFS_E_BAD_HANDLE;
    uint32_t v = hfs_get32(s_req + REQ_STATUS);
    if (v == 0) return (int)(((uint32_t)hfs_get16(fcb + FCB_DATE) << 16) | hfs_get16(fcb + FCB_TIME));
    time_t dummy; if (!hfs_datetime_to_time(v, &dummy)) return HFS_E_BAD_PARAM;
    hfs_put16(fcb + FCB_DATE, (uint16_t)(v >> 16)); hfs_put16(fcb + FCB_TIME, (uint16_t)v);
    hf->pending_datetime = v; hf->pending_datetime_valid = 1u;
    return (int)v;
}

static int hfs_cmd_dskfre(void)
{
    uint32_t a = hfs_get32(s_req + REQ_ADDR); if (!a) return HFS_E_BAD_PARAM;
    uint64_t total64 = 0x7fffffffULL, free64 = 0x3fffffffULL;
#ifdef ESP_PLATFORM
    if (esp_vfs_fat_info(HOSTFS_ROOT, &total64, &free64) != 0) return HFS_E_ACCESS;
#endif
    uint32_t total = total64 > 0x7fffffffULL ? 0x7fffffffu : (uint32_t)total64;
    uint32_t freeb = free64 > 0x7fffffffULL ? 0x7fffffffu : (uint32_t)free64;
    const uint32_t cluster_bytes = 32768u, sectors_per_cluster = 64u, bytes_per_sector = 512u;
    uint32_t tc = total / cluster_bytes, fc = freeb / cluster_bytes;
    if (tc > 0xffffu) tc = 0xffffu;
    if (fc > 0xffffu) fc = 0xffffu;
    /* PX68K_HOSTFS_R1A1_BUILD_FIX */
    hfs_put16(a + 0, (uint16_t)fc); hfs_put16(a + 2, (uint16_t)tc);
    hfs_put16(a + 4, (uint16_t)sectors_per_cluster); hfs_put16(a + 6, (uint16_t)bytes_per_sector);
    return (int)(fc * cluster_bytes);
}

static int hfs_cmd_drvctrl(void)
{
    uint8_t cmd = 0; hfs_guest_read(s_req + REQ_ATTR, &cmd, 1);
    switch (cmd)
    {
        case 0: ++s_hfs_r1a15_drvctrl_state; break; /* state check 1 */
        case 1: break; /* fixed mounted SD: eject intentionally ignored */
        case 2: s_drv_user_lock = 1u; break;
        case 3: s_drv_user_lock = 0u; break;
        case 4: s_drv_led = 1u; break;
        case 5: s_drv_led = 0u; break;
        case 6: s_drv_os_lock = 1u; break;
        case 7: s_drv_os_lock = 0u; break;
        case 9: ++s_hfs_r1a15_drvctrl_state; break; /* state check 2 */
        default: return HFS_E_BAD_PARAM;
    }
    uint8_t sense = 0x02u; /* media inserted */
    if (s_drv_led) sense |= 0x80u;
    if (s_drv_user_lock || s_drv_os_lock) sense |= 0x40u;
    if (s_drv_os_lock) sense |= 0x20u;
    if (s_drv_user_lock) sense |= 0x10u;
    if (cmd == 0u && !s_root_writable) sense |= 0x08u;
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

static int hfs_cmd_flush(void)
{
    unsigned writable_open = 0u;
    for (int i = 0; i < HOSTFS_MAX_FILES; ++i) {
        if (!s_files[i].fp || !s_files[i].writable) continue;
        ++writable_open;
        ++s_hfs_r1a15_flush_files;
        if (fflush(s_files[i].fp) != 0) return HFS_E_CANT_WRITE;
    }
    if (!writable_open) ++s_hfs_r1a15_flush_noop;
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
#ifdef ESP_PLATFORM
    MIDI_OnePassEnsureStarted();
#endif
    for (int i = 0; i < HOSTFS_MAX_FILES; ++i)
        if (s_files[i].fp) (void)hfs_file_close_one(&s_files[i]);
    for (int i = 0; i < HOSTFS_MAX_SEARCH; ++i)
        if (s_search[i].dir) closedir(s_search[i].dir);
    memset(s_files, 0, sizeof(s_files));
    memset(s_search, 0, sizeof(s_search));
    hfs_prod_neg_invalidate();
    s_req = s_driver_base = s_driver_end = 0;
    s_calls = s_read_calls = s_write_calls = s_files_calls = 0;
#ifdef ESP_PLATFORM
    memset(s_hfs_r1a14_recent, 0, sizeof(s_hfs_r1a14_recent));
    s_hfs_r1a14_recent_pos = 0u;
#endif
    s_drive = -1;
    s_root_writable = -1;
    s_drv_user_lock = s_drv_os_lock = s_drv_led = 0u;
    s_hfs_r1a15_media_fast = s_hfs_r1a15_drvctrl_state = 0u;
    s_hfs_r1a15_flush_noop = s_hfs_r1a15_flush_files = 0u;
    s_hfs_r1a15_read_seek_skip = s_hfs_r1a15_read_seek_do = 0u;
    s_hfs_r1a15_write_seek_skip = s_hfs_r1a15_write_seek_do = 0u;
    s_hfs_r1a15_close_flush_skip = 0u;
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
#ifdef ESP_PLATFORM
    {
        uint8_t unit = 0u; hfs_guest_read(s_req + REQ_UNIT, &unit, 1);
        MIDI_OnePassTrace(MIDI_OP_HFS_REQ, cmd, unit, s_req, s_calls);
    }
#endif
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
        case 0x53: case 0x54: case 0x55:
            status = HFS_E_CANT_IOCTL; break;
        case 0x56: status = hfs_cmd_flush(); break;
        case 0x57: ++s_hfs_r1a15_media_fast; status = 0; break; /* fixed medium: no media change */
        case 0x58: status = 0; break; /* DOS owns actual LOCK semantics */
        default: status = HFS_E_INVALID_FUNC; break;
    }

    hfs_req_status(status);
#ifdef ESP_PLATFORM
    MIDI_OnePassTrace(MIDI_OP_HFS_DONE, cmd, (uint16_t)status, s_req, s_calls);
#endif
    if (PX68K_FULLPASS_TRACE && (status < 0 || s_calls <= 24u) && cmd != 0x48)
    {
        uint8_t unit = 0;
        hfs_guest_read(s_req + REQ_UNIT, &unit, 1);
        printf("PX68K_HOSTFS: req #%lu cmd=%02X unit=%u status=%ld req=$%06lX\n",
               (unsigned long)s_calls, cmd, (unsigned)unit,
               (long)status, (unsigned long)s_req);
    }
}

uint32_t HostFS_DebugCalls(void) { return s_calls; }
int HostFS_DebugDrive(void) { return s_drive; }

void HostFS_R1A15GetStats(uint32_t out[9])
{
    if (!out) return;
    out[0] = s_hfs_r1a15_media_fast;
    out[1] = s_hfs_r1a15_drvctrl_state;
    out[2] = s_hfs_r1a15_flush_noop;
    out[3] = s_hfs_r1a15_flush_files;
    out[4] = s_hfs_r1a15_read_seek_skip;
    out[5] = s_hfs_r1a15_read_seek_do;
    out[6] = s_hfs_r1a15_write_seek_skip;
    out[7] = s_hfs_r1a15_write_seek_do;
    out[8] = s_hfs_r1a15_close_flush_skip;
}
