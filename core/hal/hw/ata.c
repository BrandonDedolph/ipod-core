/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/ata.c — minimal PIO-polled ATA sector reader (PP5022).
 *
 * Implements core/docs/hw/04-ata.md's PIO LBA28 read path, trimmed to the
 * minimum: the chainloading bootloader already powered/spun/PIO-timed the
 * drive, so we skip power, reset, IDENTIFY and SET FEATURES, and we do NOT
 * touch IDE0_PRI_TIMING (its values depend on the CPU clock the bootloader
 * ran at). PP502x task registers are plain accesses — no PP5002 IDE_CFG
 * write handshake. Control registers 8-bit; data port 16-bit, 256
 * halfwords per 512-byte sector, little-endian, no byte swap.
 */

#include "pp5022.h"
#include "mmio.h"
#include "ata.h"

/*
 * Bounded polls so a wedged/absent drive can't hang the kernel. The disk
 * is already spun up (bootloader used it) so waits are short in practice;
 * 1<<20 trips is generously past a healthy PIO sector.
 */
#define ATA_BSY_SPIN_LIMIT (1u << 20)
#define ATA_DRQ_SPIN_LIMIT (1u << 20)

/*
 * Logical (512-byte) sectors per PHYSICAL sector. The stock 80 GB 5.5G
 * drive (MK8010GAH) reports 2 logical per physical (IDENTIFY word 106 =
 * 0x6001: bit 13 "multiple logical per physical", low nibble 1 => 2^1) and
 * — unlike a normal 512e drive — REJECTS sub-physical-sector reads with
 * IDNF. So every read must cover whole physical sectors, aligned to a
 * physical boundary. Reading in 2-sector units is also accepted by plain
 * 512-byte drives, so this value is safe across drives (verified on device
 * 2026-07-18: count=1 reads IDNF, count=2 at even LBA succeeds).
 * TODO: auto-detect from IDENTIFY word 106 to also cover 4-logical (2048 B
 * physical) drives.
 */
#define ATA_PHYS_LOG   2u
#define ATA_SECTOR_SZ  512u

/* One physical sector's worth of scratch for the alignment bounce. */
static uint16_t ata_bounce[ATA_SECTOR_SZ * ATA_PHYS_LOG / 2u];

/* Wait for BSY to clear (bounded). */
static int ata_wait_not_busy(void)
{
    uint32_t spin = ATA_BSY_SPIN_LIMIT;
    while ((mmio_read8(ATA_ALT_STATUS_ADDR) & ATA_STATUS_BSY) && --spin != 0) {
        /* poll */
    }
    return spin != 0 ? 0 : -1;
}

/* Wait for BSY clear, then RDY set (bounded). */
static int ata_wait_ready(void)
{
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    uint32_t spin = ATA_BSY_SPIN_LIMIT;
    while (!(mmio_read8(ATA_ALT_STATUS_ADDR) & ATA_STATUS_RDY) && --spin != 0) {
        /* poll */
    }
    return spin != 0 ? 0 : -1;
}

/* Wait for start-of-transfer: BSY clear and DRQ set. Returns -2 on a drive
 * error (ERR/DF) surfaced while waiting, -1 on timeout. */
static int ata_wait_drq(void)
{
    uint32_t spin = ATA_DRQ_SPIN_LIMIT;
    while (--spin != 0) {
        uint8_t s = mmio_read8(ATA_ALT_STATUS_ADDR);
        if (!(s & ATA_STATUS_BSY)) {
            if (s & ATA_STATUS_DRQ) {
                return 0;
            }
            if (s & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
                return -2;
            }
        }
    }
    return -1;
}

int ata_init(void)
{
    /* Soft-reset the ATA channel before use. The minimal "just reuse the
     * bootloader handoff state" init (select + wait-ready) reached a ready
     * drive on device but READ SECTORS returned a drive error — the drive
     * needs a clean reset to accept fresh commands. SRST+nIEN asserts
     * reset, then nIEN alone releases it (04-ata.md, "Soft reset"); the
     * drive stays spun up throughout. Delays are bounded busy-waits (no
     * timer yet at bring-up): ~tens of us after asserting, a few ms after
     * releasing, per the ATA reset timing. */
    mmio_write8(ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
    for (volatile uint32_t d = 0; d < (1u << 10); d++) {
        /* hold reset asserted (>= ~5 us) */
    }
    mmio_write8(ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
    for (volatile uint32_t d = 0; d < (1u << 17); d++) {
        /* post-reset recovery (> ~2 ms) */
    }

    /* Select the master device (with the obsolete must-be-1 bits) and wait
     * for it to come ready. */
    mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);
    return ata_wait_ready();
}

int ata_identify(void *buf)
{
    if (ata_wait_ready() != 0) {
        return -1;
    }
    mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);   /* master */
    if (ata_wait_ready() != 0) {
        return -1;
    }
    mmio_write8(ATA_COMMAND_ADDR, ATA_CMD_IDENTIFY);
    for (volatile uint32_t g = 0; g < 64; g++) {
        /* command-to-status settle */
    }
    int rc = ata_wait_drq();
    if (rc != 0) {
        return rc == -2 ? -3 : -2;
    }
    (void)mmio_read8(ATA_COMMAND_ADDR);
    uint16_t *out = (uint16_t *)buf;
    for (int w = 0; w < 256; w++) {
        *out++ = mmio_read16(ATA_DATA_ADDR);
    }
    uint8_t st = mmio_read8(ATA_ALT_STATUS_ADDR);
    if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        return -3;
    }
    return 0;
}

/*
 * Raw PIO READ SECTORS: `count` logical sectors at `lba`. On this drive
 * `lba` and `count` must be physical-sector-aligned (multiples of
 * ATA_PHYS_LOG) or the drive returns IDNF — the ata_read_sectors wrapper
 * guarantees that.
 */
static int ata_read_raw(uint32_t lba, uint32_t count, void *buf)
{
    if (count == 0 || count > 256) {
        return -1;
    }
    if (ata_wait_ready() != 0) {
        return -1;
    }

    /* Program the LBA28 transfer. NSECTOR = count (256 wraps to 0). */
    mmio_write8(ATA_NSECTOR_ADDR, (uint8_t)count);
    mmio_write8(ATA_SECTOR_ADDR,  (uint8_t)(lba & 0xFF));
    mmio_write8(ATA_LCYL_ADDR,    (uint8_t)((lba >> 8)  & 0xFF));
    mmio_write8(ATA_HCYL_ADDR,    (uint8_t)((lba >> 16) & 0xFF));
    mmio_write8(ATA_SELECT_ADDR,
                (uint8_t)(ATA_SELECT_OBS | ATA_SELECT_LBA |
                          ((lba >> 24) & 0x0F)));
    mmio_write8(ATA_COMMAND_ADDR, ATA_CMD_READ_SECTORS);

    /* Command-to-status pipeline guard (~sub-microsecond). A short bounded
     * spin rather than asm nops so this stays host-compilable. */
    for (volatile uint32_t g = 0; g < 64; g++) {
        /* settle */
    }

    uint16_t *out = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        int rc = ata_wait_drq();
        if (rc != 0) {
            return rc == -2 ? -3 : -2;   /* -3 drive error, -2 timeout */
        }
        /* Read the primary status once to acknowledge, then stream the
         * sector: 256 little-endian halfwords straight into the buffer. */
        (void)mmio_read8(ATA_COMMAND_ADDR);
        for (int w = 0; w < 256; w++) {
            *out++ = mmio_read16(ATA_DATA_ADDR);
        }
        uint8_t st = mmio_read8(ATA_ALT_STATUS_ADDR);
        if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            return -3;
        }
    }
    return 0;
}

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf)
{
    uint8_t *out = (uint8_t *)buf;

    /* Serve the request one physical sector at a time: round the current
     * LBA down to a physical boundary, read the whole physical sector into
     * the bounce, then copy out only the logical sectors the caller asked
     * for. This makes the drive's physical-alignment requirement invisible
     * to callers, who keep addressing plain 512-byte LBAs. */
    while (count > 0) {
        uint32_t phys = lba & ~(ATA_PHYS_LOG - 1u);   /* physical boundary */
        uint32_t off  = lba - phys;                   /* 0..ATA_PHYS_LOG-1 */

        int rc = ata_read_raw(phys, ATA_PHYS_LOG, ata_bounce);
        if (rc != 0) {
            return rc;
        }

        uint32_t avail = ATA_PHYS_LOG - off;
        uint32_t take  = count < avail ? count : avail;

        const uint8_t *src = (const uint8_t *)ata_bounce + off * ATA_SECTOR_SZ;
        for (uint32_t i = 0; i < take * ATA_SECTOR_SZ; i++) {
            out[i] = src[i];
        }

        out   += take * ATA_SECTOR_SZ;
        lba   += take;
        count -= take;
    }
    return 0;
}
