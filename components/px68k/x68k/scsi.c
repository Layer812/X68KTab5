/*
 * SCSI.C - External/built-in SCSI IOCS shim for PX68K
 *
 * Original PX68K already supplies a tiny fake SCSI ROM whose IOCS entry
 * writes the SCSI sub-function number to $e9f800.  Upstream left that trap
 * unhandled.  Build 5.15 completes the hook for standalone Tab5 so a raw
 * X68000 SCSI HDD image can be exposed through the high-level SCSI IOCS
 * calls used by Human68k, without emulating the MB89352/SPC yet.
 *
 * Build 5.15b also implements the Human68k 3.02 ROM device-installer
 * contract so the SCSI disk driver stored inside a standard .HDS can be
 * copied/relocated into guest RAM without emulating the MB89352/SPC yet.
 *
 * Scope of this baseline:
 *   - target IDs 0..7, LUN 0
 *   - raw 1024-byte logical blocks (.HDS)
 *   - TEST UNIT / INQUIRY / READ CAPACITY
 *   - READ / READ EXT / READI
 *   - WRITE / WRITE EXT
 *   - MODE SENSE / REQUEST SENSE / SEEK / STARTSTOP / REZERO
 *   - low-level SCSI bus phase calls remain unsupported
 *
 * Build 5.94a corrected launcher HDD boot selection to the real IPL HD0 mode
 * ($ED0018=$8000 for HDD0).  Build 5.94b completes the next missing link:
 * upstream PX68K's synthetic internal SCSI BIOS at $FC0000 advertised a boot
 * entry that was only RTS.  The internal BIOS boot entry now invokes the same
 * high-level HDS bridge after the real IPL has selected HD0.
 */

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Implement Tab5 HDS/SCSI direct boot and Human68k driver installation on top of the original X68000 SCSI contract.
 * Layer8 Aug/17/2026
 */
#include "common.h"
#include "../libretro/dosio.h"
#include "../libretro/prop.h"
#include "../libretro/winx68k.h"
#include "m68000.h"
#include "scsi.h"
#include "hostfs.h"
#include "sram.h"
#include "x68kmemory.h"

#include <stdio.h>
#include <string.h>

#ifndef PX68K_TAB5_DIAG_VERBOSE
#define PX68K_TAB5_DIAG_VERBOSE 0
#endif

uint8_t SCSIIPL[0x2000];

#define SCSI_TARGETS 8
#define SCSI_HOSTFS_INSTALL_TARGET 8u
#define SCSI_INSTALL_DONE_TARGET   9u
#define SCSI_BLOCK_SIZE 1024u
#define SCSI_IO_CHUNK 4096u
#define SCSI_IOCS_TRAP_ADDR      0x00e9f800u
#define SCSI_INSTALL_TRAP_ADDR   0x00e9f802u
#define SCSI_PART_TABLE_OFFSET   0x00000800u
#define SCSI_DRIVER_OFFSET       0x00000c00u
#define SCSI_DRIVER_BYTES        0x00003400u
/* Intent: Boot through the normal X68000 SCSI contract, but stage the standard HDS boot block into guest RAM so standalone Tab5 can enter Human68k without a libretro frontend.  Layer8 Aug/17/2026 */
#define SCSI_BOOT_IMAGE_OFFSET    0x00000400u
#define SCSI_BOOT_GUEST_ADDR      0x00002000u
#define SCSI_BOOT_BYTES           (SCSI_BLOCK_SIZE * 8u)

typedef struct
{
    char path[MAX_PATH];
    uint64_t bytes;
    uint32_t blocks;
    uint8_t mounted;
    uint8_t readonly;
    uint8_t started;
} SCSIImage;

static SCSIImage s_images[SCSI_TARGETS];
static uint8_t s_io_buf[SCSI_IO_CHUNK];
static uint8_t s_last_sense[SCSI_TARGETS];
static uint32_t s_iocs_calls;
static uint32_t s_reads;
static uint32_t s_writes;
static uint32_t s_unknown;
static uint32_t s_installer_calls;
static uint32_t s_init_calls;
static uint32_t s_driver_installs;
static uint32_t s_last_partition_count;

/* Build 5.94b HDD-boot state.  SRAM exposes a transient read overlay for
 * $ED0018, so the one-shot HDD choice never mutates the persisted SWITCH.X
 * bytes in sram.dat. */
static int s_direct_boot_target = -1;
static uint8_t s_direct_boot_armed;
static uint8_t s_saved_boot_prio_hi;
static uint8_t s_saved_boot_prio_lo;

static uint32_t scsi_reg(int reg)
{
    return m68000_get_reg(reg);
}

static void scsi_set_d0(uint32_t value)
{
    m68000_set_reg(M68K_D0, value);
}

static void scsi_set_d2(uint32_t value)
{
    m68000_set_reg(M68K_D2, value);
}

static uint32_t scsi_block_bytes_from_code(uint32_t code)
{
    switch (code & 0xffu)
    {
        case 0: return 256u;
        case 1: return 512u;
        case 2: return 1024u;
        default: return 0u;
    }
}

static int scsi_target_from_d4(void)
{
    return (int)(scsi_reg(M68K_D4) & 0xffu);
}

static int scsi_target_ready(int target)
{
    return target >= 0 && target < SCSI_TARGETS &&
           s_images[target].mounted && s_images[target].started;
}

static void scsi_set_sense(int target, uint8_t sense)
{
    if (target >= 0 && target < SCSI_TARGETS)
        s_last_sense[target] = sense;
}

static int guest_write_bytes(uint32_t address, const uint8_t *src, size_t length)
{
    if (!src)
        return 0;

    address &= 0x00ffffffu;

    /* Fast path for normal Human68k buffers in main RAM. */
    if (MEM && address < (uint32_t)Config.ram_size &&
        length <= (size_t)((uint32_t)Config.ram_size - address))
    {
        for (size_t i = 0; i < length; ++i)
            MEM[(address + (uint32_t)i) ^ 1u] = src[i];
        return 1;
    }

    /* Safe fallback for unusual buffers. */
    for (size_t i = 0; i < length; ++i)
        m68k_write_memory_8((address + (uint32_t)i) & 0x00ffffffu, src[i]);
    return 1;
}

static int guest_read_bytes(uint32_t address, uint8_t *dst, size_t length)
{
    if (!dst)
        return 0;

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

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}


static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/*
 * BAT177NW2 / R57E32: SDMMC DMA-safe SCSI image read bridge.
 *
 * The ESP32-P4 guest task stack can legitimately land in RTC/internal memory
 * when ordinary DRAM is fragmented.  FatFS/SDMMC may pass a caller buffer
 * directly to esp_cache_msync()/DMA, and RTC stack addresses are not valid
 * cache-sync/DMA targets.  Never expose an arbitrary caller pointer to the
 * SDMMC path.  All SCSI file reads are staged through s_io_buf, which is the
 * same ordinary-DRAM buffer already proven by SCSI_ProbeFirstBlock().
 *
 * This is deliberately a transport rule, not a guest/host synchronization
 * rule: CPU1 never waits for CPU0 and no Screen/renderer ownership changes.
 */
static int scsi_image_read_at(int target, uint32_t offset, uint8_t *dst, size_t length)
{
    if (!scsi_target_ready(target) || !dst || length == 0)
        return 0;

    SCSIImage *img = &s_images[target];
    if ((uint64_t)offset + (uint64_t)length > img->bytes)
        return 0;

    void *fp = file_open(img->path);
    if (!fp)
        return 0;

    if (file_seek(fp, (long)offset, FSEEK_SET) != (size_t)offset)
    {
        file_close(fp);
        return 0;
    }

    size_t done = 0;
    while (done < length)
    {
        size_t chunk = length - done;
        if (chunk > sizeof(s_io_buf))
            chunk = sizeof(s_io_buf);

        /* Direct read is safe only for our known ordinary-DRAM staging area. */
        if (dst == s_io_buf && done == 0 && chunk == length)
        {
            if (file_lread(fp, s_io_buf, chunk) != chunk)
            {
                file_close(fp);
                return 0;
            }
        }
        else
        {
            if (file_lread(fp, s_io_buf, chunk) != chunk)
            {
                file_close(fp);
                return 0;
            }
            memcpy(dst + done, s_io_buf, chunk);
        }
        done += chunk;
    }

    file_close(fp);
    return 1;
}

static int scsi_image_copy_to_guest(int target, uint32_t offset,
                                    uint32_t guest_addr, uint32_t length)
{
    uint32_t done = 0;
    while (done < length)
    {
        uint32_t chunk = length - done;
        if (chunk > SCSI_IO_CHUNK)
            chunk = SCSI_IO_CHUNK;
        if (!scsi_image_read_at(target, offset + done, s_io_buf, chunk) ||
            !guest_write_bytes(guest_addr + done, s_io_buf, chunk))
            return 0;
        done += chunk;
    }
    return 1;
}

static int scsi_guest_get_be32(uint32_t address, uint32_t *value)
{
    uint8_t b[4];
    if (!value || !guest_read_bytes(address, b, sizeof(b)))
        return 0;
    *value = get_be32(b);
    return 1;
}

static int scsi_guest_put_be32(uint32_t address, uint32_t value)
{
    uint8_t b[4];
    put_be32(b, value);
    return guest_write_bytes(address, b, sizeof(b));
}

static int scsi_probe_hds_layout(int target, uint32_t *partition_count)
{
    uint8_t block[SCSI_BLOCK_SIZE];
    uint32_t count = 0;

    if (!scsi_image_read_at(target, 0, block, sizeof(block)))
        return 0;
    if (memcmp(block, "X68SCSI1", 8) != 0)
    {
        printf("PX68K_HDS: target=%d invalid disk magic at $0000: %02X %02X %02X %02X %02X %02X %02X %02X\n",
               target, block[0], block[1], block[2], block[3],
               block[4], block[5], block[6], block[7]);
        return 0;
    }

    if (!scsi_image_read_at(target, SCSI_PART_TABLE_OFFSET, block, sizeof(block)))
        return 0;
    if (memcmp(block, "X68K", 4) != 0)
    {
        printf("PX68K_HDS: target=%d no X68K partition table at $0800: %02X %02X %02X %02X\n",
               target, block[0], block[1], block[2], block[3]);
        return 0;
    }

    /* X68000 SCSI partition table: 16-byte header, then up to 15 entries. */
    for (unsigned i = 0; i < 15; ++i)
    {
        const uint8_t *e = &block[16u + i * 16u];
        if (e[0] == 0)
            continue;
        if (memcmp(e, "Human68k", 8) != 0)
            continue;
        /* status bit0: 0 = auto-boot/usable, 1 = unavailable. */
        if ((e[8] & 1u) != 0)
            continue;
        ++count;
        if (PX68K_TAB5_DIAG_VERBOSE && count <= 8u)
        {
            printf("PX68K_HDS: partition[%u] Human68k flags/data=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                   i, e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15]);
        }
    }

    if (partition_count)
        *partition_count = count;
    return 1;
}

static int scsi_relocate_guest_driver(uint32_t base, uint32_t partition_count)
{
    uint32_t current = base;
    const uint32_t limit = base + SCSI_DRIVER_BYTES;

    for (unsigned headers = 0; headers < 16; ++headers)
    {
        uint8_t id[8];
        uint32_t next_rel, strategy_rel, intr_rel;
        if (current < base || current + 23u >= limit ||
            !guest_read_bytes(current + 14u, id, sizeof(id)) ||
            id[0] != 0x01 || memcmp(&id[1], "SCHDISK", 7) != 0 ||
            !scsi_guest_get_be32(current + 0u, &next_rel) ||
            !scsi_guest_get_be32(current + 6u, &strategy_rel) ||
            !scsi_guest_get_be32(current + 10u, &intr_rel))
        {
            printf("PX68K_HDS: SCSI device driver header invalid at guest=$%06lX\n",
                   (unsigned long)(current & 0x00ffffffu));
            return 0;
        }

        if (!scsi_guest_put_be32(current + 6u, current + strategy_rel) ||
            !scsi_guest_put_be32(current + 10u, current + intr_rel))
            return 0;

        if (next_rel == 0xffffffffu)
        {
            uint8_t pc = (uint8_t)(partition_count > 255u ? 255u : partition_count);
            if (!guest_write_bytes(current + 22u, &pc, 1))
                return 0;
            if (PX68K_TAB5_DIAG_VERBOSE)
                printf("PX68K_HDS: SCSI device driver relocated headers=%u guest=$%06lX partitions=%lu\n",
                       headers + 1u,
                       (unsigned long)(base & 0x00ffffffu),
                       (unsigned long)partition_count);
            return 1;
        }

        const uint32_t next = current + next_rel;
        if (next <= current || next >= limit || !scsi_guest_put_be32(current, next))
            return 0;
        current = next;
    }

    return 0;
}

static void scsi_device_installer(void)
{
    ++s_installer_calls;

    const uint32_t in_d2 = scsi_reg(M68K_D2);
    const uint32_t prior_partitions = in_d2 >> 16;
    uint32_t target = in_d2 & 0xffffu;
    const uint32_t guest_addr = scsi_reg(M68K_A1) & 0x00ffffffu;

    if (PX68K_TAB5_DIAG_VERBOSE && s_installer_calls <= 16u)
    {
        printf("PX68K_HDS: installer #%lu d2=%08lX target=%lu A1=$%06lX\n",
               (unsigned long)s_installer_calls,
               (unsigned long)in_d2,
               (unsigned long)target,
               (unsigned long)guest_addr);
    }

    if (target >= SCSI_INSTALL_DONE_TARGET)
    {
        scsi_set_d2(0xffffffffu);
        return;
    }

    /*
     * Human68k/SCSI16 contract: on the first installer call SCSI IOCS must
     * already be available before the loaded SCHDISK driver is initialized.
     * The historical PX68K fake ROM installs this exact vector at $07D4.
     */
    if (s_installer_calls == 1u)
    {
        const uint32_t a = 0x000007d4u;
        const uint32_t v = 0x00ea00a0u;
        if (MEM && Config.ram_size > (int)(a + 3u))
        {
            MEM[(a + 0u) ^ 1u] = (uint8_t)(v >> 24);
            MEM[(a + 1u) ^ 1u] = (uint8_t)(v >> 16);
            MEM[(a + 2u) ^ 1u] = (uint8_t)(v >> 8);
            MEM[(a + 3u) ^ 1u] = (uint8_t)v;
            if (PX68K_TAB5_DIAG_VERBOSE)
                printf("PX68K_HDS: installer preinstalled _SCSIDRV vector=$%08lX\n",
                       (unsigned long)v);
        }
    }

    /*
     * Build 5.96a: after the eight SCSI IDs, expose one synthetic installer
     * state for the Human68k remote-disk HostFS driver.  Human68k already
     * calls this installer during normal floppy/Flash-Human boot, so HostFS
     * can be installed without patching HUMAN.SYS or adding AUTOEXEC files.
     */
    while (target < SCSI_TARGETS && !scsi_target_ready((int)target))
        ++target;

    if (target == SCSI_HOSTFS_INSTALL_TARGET)
    {
        if (HostFS_Available() && HostFS_InstallDriver(guest_addr) != 0u)
        {
            const uint32_t next_d2 = (prior_partitions << 16) | SCSI_INSTALL_DONE_TARGET;
            scsi_set_d2(next_d2);
            if (PX68K_TAB5_DIAG_VERBOSE)
                printf("PX68K_HOSTFS: installer accepted state=%lu A1=$%06lX nextD2=%08lX\n",
                       (unsigned long)target, (unsigned long)guest_addr,
                       (unsigned long)next_d2);
            return;
        }
        printf("PX68K_HOSTFS: installer skipped; /sdcard is not available\n");
        scsi_set_d2(0xffffffffu);
        return;
    }

    if (target >= SCSI_TARGETS)
    {
        scsi_set_d2(0xffffffffu);
        return;
    }

    uint32_t partitions = 0;
    if (!scsi_probe_hds_layout((int)target, &partitions) || partitions == 0)
    {
        printf("PX68K_HDS: installer target=%lu rejected: no usable Human68k partition\n",
               (unsigned long)target);
        scsi_set_d2(0xffffffffu);
        return;
    }

    if (!scsi_image_copy_to_guest((int)target, SCSI_DRIVER_OFFSET,
                                  guest_addr, SCSI_DRIVER_BYTES) ||
        !scsi_relocate_guest_driver(guest_addr, partitions))
    {
        printf("PX68K_HDS: installer target=%lu failed copying/relocating disk driver\n",
               (unsigned long)target);
        scsi_set_d2(0xffffffffu);
        return;
    }

    ++s_driver_installs;
    s_last_partition_count = partitions;

    /*
     * Match the real SCSI16 installer ABI exactly.  A successful target does
     * NOT jump straight to ID 8; it returns the next SCSI ID in the low word.
     * Human68k calls the installer again and the installer eventually returns
     * -1 when ID 8 is reached.
     */
    const uint32_t next_d2 = ((prior_partitions + partitions) << 16) |
                             ((target + 1u) & 0xffffu);
    scsi_set_d2(next_d2);
    printf("PX68K_HDS: *** DEVICE INSTALLER OK target=%lu partitions=%lu nextD2=%08lX ***\n",
           (unsigned long)target,
           (unsigned long)partitions,
           (unsigned long)next_d2);
}

static int scsi_rw_blocks(int target, int write_to_disk,
                          uint32_t lba, uint32_t count,
                          uint32_t guest_addr, uint32_t requested_block_size)
{
    if (!scsi_target_ready(target))
    {
        scsi_set_sense(target, 0x02); /* not ready */
        return -1;
    }

    SCSIImage *img = &s_images[target];

    if (requested_block_size != SCSI_BLOCK_SIZE || count == 0)
    {
        scsi_set_sense(target, 0x05); /* illegal request */
        return -2;
    }

    if (lba >= img->blocks || count > img->blocks - lba)
    {
        scsi_set_sense(target, 0x05);
        return -2;
    }

    if (write_to_disk && img->readonly)
    {
        scsi_set_sense(target, 0x07); /* data protect */
        return -1;
    }

    void *fp = file_open(img->path);
    if (!fp)
    {
        scsi_set_sense(target, 0x02);
        return -1;
    }

    const uint64_t byte_offset = (uint64_t)lba * SCSI_BLOCK_SIZE;
    if (byte_offset > 0x7fffffffull ||
        file_seek(fp, (long)byte_offset, FSEEK_SET) != (size_t)byte_offset)
    {
        file_close(fp);
        scsi_set_sense(target, 0x03); /* medium error */
        return -1;
    }

    uint32_t blocks_left = count;
    uint32_t addr = guest_addr;

    while (blocks_left)
    {
        uint32_t chunk_blocks = blocks_left;
        if (chunk_blocks > SCSI_IO_CHUNK / SCSI_BLOCK_SIZE)
            chunk_blocks = SCSI_IO_CHUNK / SCSI_BLOCK_SIZE;
        const size_t chunk_bytes = (size_t)chunk_blocks * SCSI_BLOCK_SIZE;

        if (write_to_disk)
        {
            if (!guest_read_bytes(addr, s_io_buf, chunk_bytes) ||
                file_lwrite(fp, s_io_buf, chunk_bytes) != chunk_bytes)
            {
                file_close(fp);
                scsi_set_sense(target, 0x03);
                return -1;
            }
        }
        else
        {
            if (file_lread(fp, s_io_buf, chunk_bytes) != chunk_bytes ||
                !guest_write_bytes(addr, s_io_buf, chunk_bytes))
            {
                file_close(fp);
                scsi_set_sense(target, 0x03);
                return -1;
            }
        }

        addr += (uint32_t)chunk_bytes;
        blocks_left -= chunk_blocks;
    }

    file_close(fp);
    scsi_set_sense(target, 0);

    if (write_to_disk)
        ++s_writes;
    else
        ++s_reads;

    if (PX68K_TAB5_DIAG_VERBOSE && (write_to_disk ? s_writes : s_reads) <= 12u)
    {
        printf("PX68K_SCSI: %s #%lu target=%d lba=%lu blocks=%lu bytes=%lu path=%s\n",
               write_to_disk ? "WRITE" : "READ",
               (unsigned long)(write_to_disk ? s_writes : s_reads),
               target,
               (unsigned long)lba,
               (unsigned long)count,
               (unsigned long)(count * SCSI_BLOCK_SIZE),
               img->path);
    }

    return 0;
}

int SCSI_MountImage(int target, const char *path, int readonly)
{
    if (target < 0 || target >= SCSI_TARGETS || !path || !path[0])
        return 0;

    void *fp = file_open(path);
    if (!fp)
        return 0;

    const size_t size = file_seek(fp, 0, FSEEK_END);
    file_close(fp);

    if (size == (size_t)-1 || size < SCSI_BLOCK_SIZE)
        return 0;

    static uint8_t s_dma_bounce_proof_logged;
    if (!s_dma_bounce_proof_logged)
    {
        s_dma_bounce_proof_logged = 1u;
        printf("PX68K_SCSI_R57E32: DMA-safe file-read bounce ACTIVE bytes=%u; caller stack/RTC pointers never reach SDMMC DMA\n",
               (unsigned)sizeof(s_io_buf));
    }

    SCSIImage *img = &s_images[target];
    memset(img, 0, sizeof(*img));
    snprintf(img->path, sizeof(img->path), "%s", path);
    img->bytes = (uint64_t)size;
    img->blocks = (uint32_t)(img->bytes / SCSI_BLOCK_SIZE);
    img->readonly = readonly ? 1u : 0u;
    img->started = 1u;
    img->mounted = img->blocks != 0;
    s_last_sense[target] = 0;

    if (PX68K_TAB5_DIAG_VERBOSE)
        printf("PX68K_SCSI: mount target=%d blocks=%lu block_size=%u bytes=%llu ro=%d path=%s\n",
               target,
               (unsigned long)img->blocks,
               (unsigned)SCSI_BLOCK_SIZE,
               (unsigned long long)img->bytes,
               img->readonly,
               img->path);

    return img->mounted ? 1 : 0;
}

void SCSI_UnmountImage(int target)
{
    if (target < 0 || target >= SCSI_TARGETS)
        return;
    memset(&s_images[target], 0, sizeof(s_images[target]));
    s_last_sense[target] = 0;
}

int SCSI_ImageReady(int target)
{
    return scsi_target_ready(target);
}

uint32_t SCSI_ImageBlocks(int target)
{
    if (target < 0 || target >= SCSI_TARGETS || !s_images[target].mounted)
        return 0;
    return s_images[target].blocks;
}

int SCSI_ProbeLayout(int target, uint32_t *partition_count)
{
    return scsi_probe_hds_layout(target, partition_count);
}

int SCSI_ProbeFirstBlock(int target, uint32_t *hash_out)
{
    if (!scsi_target_ready(target))
        return 0;

    SCSIImage *img = &s_images[target];
    void *fp = file_open(img->path);
    if (!fp)
        return 0;

    if (file_seek(fp, 0, FSEEK_SET) != 0 ||
        file_lread(fp, s_io_buf, SCSI_BLOCK_SIZE) != SCSI_BLOCK_SIZE)
    {
        file_close(fp);
        return 0;
    }
    file_close(fp);

    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < SCSI_BLOCK_SIZE; ++i)
    {
        hash ^= s_io_buf[i];
        hash *= 16777619u;
    }
    if (hash_out)
        *hash_out = hash;
    return 1;
}

uint32_t SCSI_DebugIOCSCalls(void)
{
    return s_iocs_calls;
}

uint32_t SCSI_DebugReads(void)
{
    return s_reads;
}

uint32_t SCSI_DebugInstallerCalls(void)
{
    return s_installer_calls;
}

uint32_t SCSI_DebugInitCalls(void)
{
    return s_init_calls;
}

uint32_t SCSI_DebugDriverInstalls(void)
{
    return s_driver_installs;
}

uint32_t SCSI_DebugPartitionCount(void)
{
    return s_last_partition_count;
}

uint32_t SCSI_DebugIOCSVector(void)
{
    const uint32_t a = 0x000007d4u;
    if (!MEM || Config.ram_size <= (int)(a + 3u))
        return 0;
    return ((uint32_t)MEM[(a + 0u) ^ 1u] << 24) |
           ((uint32_t)MEM[(a + 1u) ^ 1u] << 16) |
           ((uint32_t)MEM[(a + 2u) ^ 1u] << 8) |
           ((uint32_t)MEM[(a + 3u) ^ 1u]);
}

void SCSI_InstallIOCSVector(void)
{
    /* Build 5.15d: install the actual SCSIEX IOCS bridge entry. */
    const uint32_t a = 0x000007d4u;
    const uint32_t v = 0x00ea00a0u;
    if (!MEM || Config.ram_size <= (int)(a + 3u))
        return;

    MEM[(a + 0u) ^ 1u] = (uint8_t)(v >> 24);
    MEM[(a + 1u) ^ 1u] = (uint8_t)(v >> 16);
    MEM[(a + 2u) ^ 1u] = (uint8_t)(v >> 8);
    MEM[(a + 3u) ^ 1u] = (uint8_t)v;
}

static void scsi_restore_one_shot_boot_priority(void)
{
    if (!s_direct_boot_armed)
        return;
    SRAM_SetBootPriorityOverride(0, 0);
}

int SCSI_ArmDirectBoot(int target)
{
    uint32_t partitions = 0;

    if (!scsi_target_ready(target) || !scsi_probe_hds_layout(target, &partitions))
        return 0;

    s_direct_boot_target = target;
    s_saved_boot_prio_hi = SRAM_Read(0x0018u);
    s_saved_boot_prio_lo = SRAM_Read(0x0019u);
    s_direct_boot_armed = 1u;

    /* X68000 SRAM $ED0018 word:
     *   $8?00 = HD? (HDD0 is $8000)
     *   $9?70 = 2HD?
     *   $A000 = ROM
     *   $B000 = RAM
     * Build 5.94 used $A000 here, which explicitly requested ROM boot and
     * allowed the normal STD fallback to reach FDD.  5.94a/5.94b select the real
     * IPL HD0 path with $8000.  This remains a transient read overlay only;
     * SWITCH.X bytes and sram.dat are never modified. */
    SRAM_SetBootPriorityOverride(1, 0x8000u);

    if (PX68K_TAB5_DIAG_VERBOSE)
        printf("PX68K_SCSI_BOOT: armed target=%d bootprio old=%02X%02X -> HD0=$8000 guestword=$%04lX partitions=%lu\n",
               target, s_saved_boot_prio_hi, s_saved_boot_prio_lo,
               (unsigned long)cpu_readmem24_word(0x00ed0018u),
               (unsigned long)partitions);
    return 1;
}

static void scsi_prepare_direct_boot(void)
{
    const int target = s_direct_boot_target;
    uint8_t first[16];

    /* Restore SWITCH.X-visible state before guest boot code takes control. */
    scsi_restore_one_shot_boot_priority();
    s_direct_boot_armed = 0u;

    if (target < 0 || !scsi_target_ready(target))
    {
        printf("PX68K_SCSI_BOOT: trap declined; target=%d not ready\n", target);
        scsi_set_d0(0xffffffffu);
        return;
    }

    if (!scsi_image_read_at(target, SCSI_BOOT_IMAGE_OFFSET, first, sizeof(first)) ||
        !scsi_image_copy_to_guest(target, SCSI_BOOT_IMAGE_OFFSET,
                                  SCSI_BOOT_GUEST_ADDR, SCSI_BOOT_BYTES))
    {
        printf("PX68K_SCSI_BOOT: load FAILED target=%d image_off=$%06lX bytes=%lu\n",
               target, (unsigned long)SCSI_BOOT_IMAGE_OFFSET,
               (unsigned long)SCSI_BOOT_BYTES);
        scsi_set_d0(0xffffffffu);
        return;
    }

    /* The disk IPL expects IOCS _SCSIDRV at $000007D4 before it starts.
     * Register/SR setup and JMP $002000 are deliberately executed by the
     * synthetic 68000 ROM stub after this host trap returns; changing PC/SR
     * from inside a Musashi memory callback would be unnecessarily fragile. */
    SCSI_InstallIOCSVector();
    s_direct_boot_target = -1;
    scsi_set_d0(0u);

    if (PX68K_TAB5_DIAG_VERBOSE)
        printf("PX68K_SCSI_BOOT: IPL prepared target=%d HDS+$%lX -> RAM $%06lX bytes=%lu first=%02X%02X%02X%02X vec=$%08lX\n",
               target, (unsigned long)SCSI_BOOT_IMAGE_OFFSET,
               (unsigned long)SCSI_BOOT_GUEST_ADDR, (unsigned long)SCSI_BOOT_BYTES,
               first[0], first[1], first[2], first[3],
               (unsigned long)SCSI_DebugIOCSVector());
}

void SCSI_Init(void)
{
    HostFS_Reset();

    /* A reset while an earlier one-shot boot is pending must not leave the
     * transient ROM-priority overlay active. */
    SRAM_SetBootPriorityOverride(0, 0);

    /*
     * Build 5.15c: expose a structurally-correct external SCSI ROM header.
     *
     * Real/XEiJ SCSIEX ROM layout (base $EA0000):
     *   +$00..+$1F  external SPC register window
     *   +$20..+$3F  eight ROM boot handles (SCSI ID 0..7)
     *   +$40         SCSI init handle
     *   +$44         "SCSIEX" magic
     *
     * Human68k 3.02 follows a boot handle to the boot routine and then
     * inspects boot-20/-16/-12/-8 for "SCSI", installer pointer,
     * IOCS parameter pointer and "Human68k" respectively.
     *
     * Build 5.15b incorrectly placed those -20..-8 fields directly in
     * the +$20 boot-handle table, so Human68k never discovered the
     * installer.  Keep the ROM tiny, but keep the ABI/layout exact.
     */
    int i;
    uint8_t tmp;
    const uint32_t boot = 0x00ea00c0u;
    const uint32_t init = 0x00ea0084u;
    const uint32_t installer = 0x00ea0090u;
    const uint32_t iocs = 0x00ea00a0u;

#define PUT_BE32(off, value) do { \
        uint32_t _v = (uint32_t)(value); \
        SCSIIPL[(off) + 0] = (uint8_t)(_v >> 24); \
        SCSIIPL[(off) + 1] = (uint8_t)(_v >> 16); \
        SCSIIPL[(off) + 2] = (uint8_t)(_v >> 8);  \
        SCSIIPL[(off) + 3] = (uint8_t)(_v);       \
    } while (0)

    memset(SCSIIPL, 0xff, sizeof(SCSIIPL));

    /* +$20..+$3F: ROM boot handle for SCSI IDs 0..7. */
    for (i = 0; i < 8; ++i)
        PUT_BE32(0x20 + i * 4, boot);

    /* +$40: init handle, +$44: external SCSI ROM magic, +$4A: BIOS level. */
    PUT_BE32(0x40, init);
    memcpy(&SCSIIPL[0x44], "SCSIEX", 6);
    SCSIIPL[0x4a] = 0x00;
    SCSIIPL[0x4b] = 0x10; /* level 16 semantics for this host bridge */
    memcpy(&SCSIIPL[0x4c], "PX68K SCSIEX HOST", 17);
    SCSIIPL[0x5d] = 0x00;

    /* Metadata at fixed offsets relative to boot=$EA00C0. */
    memcpy(&SCSIIPL[0xac], "SCSI", 4);      /* boot - 20 */
    PUT_BE32(0xb0, installer);               /* boot - 16 */
    PUT_BE32(0xb4, iocs);                    /* boot - 12 */
    memcpy(&SCSIIPL[0xb8], "Human68k", 8);  /* boot - 8  */

    /* $EA00C0 direct boot routine.
     *   move.b #$82,$E9F802  ; host prepares HDS IPL at $002000
     *   tst.l  d0
     *   bmi.s  fail
     *   moveq  #0,d0
     *   moveq  #$20,d1
     *   moveq  #2,d5         ; 1024-byte sector exponent
     *   move.w #$2000,sr
     *   jmp    $00002000.l
     * fail: rts
     *
     * Keeping the final register/SR/PC handoff in guest instructions avoids
     * mutating Musashi execution state from inside the memory-write callback. */
    {
        static const uint8_t code[] = {
            0x13, 0xfc, 0x00, 0x82, 0x00, 0xe9, 0xf8, 0x02,
            0x4a, 0x80,
            0x6b, 0x10,
            0x70, 0x00,
            0x72, 0x20,
            0x7a, 0x02,
            0x46, 0xfc, 0x20, 0x00,
            0x4e, 0xf9, 0x00, 0x00, 0x20, 0x00,
            0x4e, 0x75
        };
        memcpy(&SCSIIPL[0xc0], code, sizeof(code));
    }

    /* $EA0084 SCSI init routine -> host init trap ($E9F802, value $81). */
    {
        static const uint8_t code[] = {
            0x13, 0xfc, 0x00, 0x81, 0x00, 0xe9, 0xf8, 0x02,
            0x4e, 0x75
        };
        memcpy(&SCSIIPL[0x84], code, sizeof(code));
    }

    /* $EA0090 Human68k 3.02 device installer -> host trap value $80. */
    {
        static const uint8_t code[] = {
            0x13, 0xfc, 0x00, 0x80, 0x00, 0xe9, 0xf8, 0x02,
            0x4e, 0x75
        };
        memcpy(&SCSIIPL[0x90], code, sizeof(code));
    }

    /* $EA00A0 IOCS $F5 _SCSIDRV entry -> high-level host dispatcher. */
    {
        static const uint8_t code[] = {
            0x13, 0xc1, 0x00, 0xe9, 0xf8, 0x00,
            0x4e, 0x75
        };
        memcpy(&SCSIIPL[0xa0], code, sizeof(code));
    }

#undef PUT_BE32

    /* PX68K memory tables are word-swizzled on little-endian hosts. */
    for (i = 0; i < (int)sizeof(SCSIIPL); i += 2)
    {
        tmp = SCSIIPL[i];
        SCSIIPL[i] = SCSIIPL[i + 1];
        SCSIIPL[i + 1] = tmp;
    }

    /* Mount state deliberately survives reset/SCSI_Init once configured. */
    s_iocs_calls = 0;
    s_reads = 0;
    s_writes = 0;
    s_unknown = 0;
    s_installer_calls = 0;
    s_init_calls = 0;
    s_driver_installs = 0;
    s_last_partition_count = 0;
    s_direct_boot_target = -1;
    s_direct_boot_armed = 0u;

    if (PX68K_TAB5_DIAG_VERBOSE)
        printf("PX68K_SCSIROM: SCSIEX header OK boot_handles=$EA0020-$EA003F boot=$EA00C0 init=$EA0084 installer=$EA0090 iocs=$EA00A0 direct_boot=5.94b\n");
}

void SCSI_Cleanup(void)
{
    HostFS_Reset();
    scsi_restore_one_shot_boot_priority();
    s_direct_boot_armed = 0u;
    s_direct_boot_target = -1;
}

static void scsi_iocs_inquiry(int target)
{
    uint8_t reply[36];
    uint32_t count = scsi_reg(M68K_D3) & 0xffu;
    uint32_t addr = scsi_reg(M68K_A1);

    if (!scsi_target_ready(target))
    {
        scsi_set_d0(0xffffffffu);
        return;
    }

    memset(reply, 0, sizeof(reply));
    reply[0] = 0x00; /* direct-access device */
    reply[2] = 0x01; /* ANSI X3.131-1986 */
    reply[3] = 0x01;
    reply[4] = 31;
    memcpy(&reply[8],  "PX68K   ", 8);
    memcpy(&reply[16], "TAB5 HDS HDD    ", 16);
    memcpy(&reply[32], "15b ", 4);

    if (count > sizeof(reply)) count = sizeof(reply);
    guest_write_bytes(addr, reply, count);
    scsi_set_sense(target, 0);
    scsi_set_d0(0);
}

static void scsi_iocs_readcap(int target)
{
    uint8_t reply[8];
    uint32_t addr = scsi_reg(M68K_A1);

    if (!scsi_target_ready(target) || s_images[target].blocks == 0)
    {
        scsi_set_d0(0xffffffffu);
        return;
    }

    put_be32(&reply[0], s_images[target].blocks - 1u);
    put_be32(&reply[4], SCSI_BLOCK_SIZE);
    guest_write_bytes(addr, reply, sizeof(reply));
    scsi_set_sense(target, 0);
    scsi_set_d0(0);
}

static void scsi_iocs_modesense(int target)
{
    uint8_t reply[12];
    uint32_t count = scsi_reg(M68K_D3) & 0xffu;
    uint32_t addr = scsi_reg(M68K_A1);

    if (!scsi_target_ready(target))
    {
        scsi_set_d0(0xffffffffu);
        return;
    }

    memset(reply, 0, sizeof(reply));
    reply[0] = 11; /* bytes following this byte */
    reply[1] = 0;
    reply[2] = s_images[target].readonly ? 0x80 : 0x00;
    reply[3] = 8;  /* one block descriptor */
    reply[4] = 0;  /* density */

    uint32_t blocks24 = s_images[target].blocks;
    if (blocks24 > 0x00ffffffu) blocks24 = 0x00ffffffu;
    reply[5] = (uint8_t)(blocks24 >> 16);
    reply[6] = (uint8_t)(blocks24 >> 8);
    reply[7] = (uint8_t)blocks24;
    reply[8] = 0;
    reply[9] = (uint8_t)(SCSI_BLOCK_SIZE >> 16);
    reply[10] = (uint8_t)(SCSI_BLOCK_SIZE >> 8);
    reply[11] = (uint8_t)SCSI_BLOCK_SIZE;

    if (count > sizeof(reply)) count = sizeof(reply);
    guest_write_bytes(addr, reply, count);
    scsi_set_sense(target, 0);
    scsi_set_d0(0);
}

static void scsi_iocs_request(int target)
{
    uint8_t reply[18];
    uint32_t count = scsi_reg(M68K_D3) & 0xffu;
    uint32_t addr = scsi_reg(M68K_A1);

    if (target < 0 || target >= SCSI_TARGETS)
    {
        scsi_set_d0(0xffffffffu);
        return;
    }

    memset(reply, 0, sizeof(reply));
    /* Legacy 4-byte sense: zero means no outstanding error. */
    if (s_last_sense[target])
    {
        reply[0] = (uint8_t)(0x70u | (s_last_sense[target] & 0x0fu));
        reply[2] = s_last_sense[target];
    }
    if (count > sizeof(reply)) count = sizeof(reply);
    guest_write_bytes(addr, reply, count);
    s_last_sense[target] = 0;
    scsi_set_d0(0);
}

static void scsi_iocs_dispatch(uint8_t fn)
{
    ++s_iocs_calls;
    const int target = scsi_target_from_d4();

    if (PX68K_TAB5_DIAG_VERBOSE && s_iocs_calls <= 24u)
    {
        printf("PX68K_SCSI: IOCS #%lu fn=%02X target=%d D2=%08lX D3=%08lX D5=%08lX A1=%08lX ready=%d\n",
               (unsigned long)s_iocs_calls,
               fn,
               target,
               (unsigned long)scsi_reg(M68K_D2),
               (unsigned long)scsi_reg(M68K_D3),
               (unsigned long)scsi_reg(M68K_D5),
               (unsigned long)scsi_reg(M68K_A1),
               scsi_target_ready(target));
    }

    switch (fn)
    {
        case 0x00: /* _S_RESET */
            for (int i = 0; i < SCSI_TARGETS; ++i)
            {
                s_last_sense[i] = 0;
                if (s_images[i].mounted) s_images[i].started = 1;
            }
            scsi_set_d0(0);
            break;

        case 0x0a: /* _S_LEVEL */
            scsi_set_d0(3); /* XVI-style SCSI IOCS level */
            break;

        case 0x20: /* _S_INQUIRY */
            scsi_iocs_inquiry(target);
            break;

        case 0x21: /* _S_READ */
        case 0x26: /* _S_READEXT */
        case 0x2e: /* _S_READI */
        {
            const uint32_t bs = scsi_block_bytes_from_code(scsi_reg(M68K_D5));
            const int rc = scsi_rw_blocks(target, 0,
                                          scsi_reg(M68K_D2),
                                          scsi_reg(M68K_D3),
                                          scsi_reg(M68K_A1),
                                          bs);
            scsi_set_d0((uint32_t)rc);
            break;
        }

        case 0x22: /* _S_WRITE */
        case 0x27: /* _S_WRITEEXT */
        {
            const uint32_t bs = scsi_block_bytes_from_code(scsi_reg(M68K_D5));
            const int rc = scsi_rw_blocks(target, 1,
                                          scsi_reg(M68K_D2),
                                          scsi_reg(M68K_D3),
                                          scsi_reg(M68K_A1),
                                          bs);
            scsi_set_d0((uint32_t)rc);
            break;
        }

        case 0x24: /* _S_TESTUNIT */
            scsi_set_d0(scsi_target_ready(target) ? 0u : 0xffffffffu);
            break;

        case 0x25: /* _S_READCAP */
            scsi_iocs_readcap(target);
            break;

        case 0x28: /* _S_VERIFYEXT */
            scsi_set_d0(scsi_target_ready(target) ? 0u : 0xffffffffu);
            break;

        case 0x29: /* _S_MODESENSE */
            scsi_iocs_modesense(target);
            break;

        case 0x2a: /* _S_MODESELECT */
            scsi_set_d0(scsi_target_ready(target) ? 0u : 0xffffffffu);
            break;

        case 0x2b: /* _S_REZEROUNIT */
        case 0x2d: /* _S_SEEK */
            scsi_set_d0(scsi_target_ready(target) ? 0u : 0xffffffffu);
            break;

        case 0x2c: /* _S_REQUEST */
            scsi_iocs_request(target);
            break;

        case 0x2f: /* _S_STARTSTOP */
            if (target >= 0 && target < SCSI_TARGETS && s_images[target].mounted)
            {
                const uint32_t op = scsi_reg(M68K_D3) & 0xffu;
                s_images[target].started = op == 0 ? 0u : 1u;
                scsi_set_d0(0);
            }
            else
            {
                scsi_set_d0(0xffffffffu);
            }
            break;

        default:
            ++s_unknown;
            if (s_unknown <= 24u)
                printf("PX68K_SCSI: unsupported IOCS fn=%02X (count=%lu)\n",
                       fn, (unsigned long)s_unknown);
            scsi_set_d0(0xffffffffu);
            break;
    }
}

void FASTCALL SCSI_Write(uint32_t adr, uint8_t data)
{
    /*
     * $e9f800 is the software trap used by PX68K's fake SCSI BIOS.
     * Writes in the actual $ea0000 SCSI expansion ROM area remain ignored.
     */
    const uint32_t a = adr & 0x00ffffffu;
    if (a == SCSI_IOCS_TRAP_ADDR)
    {
        scsi_iocs_dispatch(data);
    }
    else if (a == SCSI_INSTALL_TRAP_ADDR && data == 0x80u)
    {
        scsi_device_installer();
    }
    else if (a == SCSI_INSTALL_TRAP_ADDR && data == 0x81u)
    {
        ++s_init_calls;
        SCSI_InstallIOCSVector();
        if (PX68K_TAB5_DIAG_VERBOSE && s_init_calls <= 8u)
            printf("PX68K_SCSIROM: init #%lu installed IOCS vector=$%08lX\n",
                   (unsigned long)s_init_calls,
                   (unsigned long)SCSI_DebugIOCSVector());
        scsi_set_d0(0);
    }
    else if (a == SCSI_INSTALL_TRAP_ADDR && data == 0x82u)
    {
        scsi_prepare_direct_boot();
    }
    else if (a == SCSI_INSTALL_TRAP_ADDR && data == 0x90u)
    {
        HostFS_StrategyTrap();
    }
    else if (a == SCSI_INSTALL_TRAP_ADDR && data == 0x91u)
    {
        HostFS_InterruptTrap();
    }
}

uint8_t FASTCALL SCSI_Read(uint32_t adr)
{
    /* External fake ROM mapped at $ea0000-$ea1fff. */
    if ((adr & 0x00ffe000u) == 0x00ea0000u)
        return SCSIIPL[(adr ^ 1u) & 0x1fffu];
    return 0;
}
