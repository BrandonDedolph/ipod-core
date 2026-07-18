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
    /* Mask the ATA interrupt (we poll), select the master device, and wait
     * for the drive — already spun up by the bootloader — to be ready. */
    mmio_write8(ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
    mmio_write8(ATA_SELECT_ADDR, 0);
    return ata_wait_ready();
}

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf)
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
                (uint8_t)(ATA_SELECT_LBA | ((lba >> 24) & 0x0F)));
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
