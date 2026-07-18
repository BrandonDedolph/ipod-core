/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/ata.h — minimal PIO-polled ATA sector reader (PP5022).
 *
 * Read-only, 512-byte LBA28 sectors. Reuses the drive state the
 * chainloading bootloader left behind (powered, spun, PIO-timed), so init
 * is just "select master, wait ready" — see core/docs/hw/04-ata.md. The
 * 80 GB 5.5G is addressed in plain 512-byte sectors here; the "2048-byte
 * sector" is a FAT-layer virtual-sector detail handled above this driver.
 * Asm-free (host-trace-testable).
 */
#ifndef CORE_HAL_HW_ATA_H
#define CORE_HAL_HW_ATA_H

#include <stdint.h>

/* Minimal bring-up: mask the ATA IRQ, select the master device, wait for
 * the drive to report ready. Returns 0 on success, -1 on timeout. */
int ata_init(void);

/*
 * Read `count` (1..256) 512-byte sectors starting at LBA `lba` into `buf`
 * (must be 16-bit aligned; needs count*512 bytes). Returns 0 on success,
 * negative on a bad argument, a not-ready/DRQ timeout, or a drive error.
 */
int ata_read_sectors(uint32_t lba, uint32_t count, void *buf);

/*
 * IDENTIFY DEVICE: read the drive's 256-word (512-byte) identify block
 * into `buf` (16-bit aligned). Takes no LBA, so it works even when sector
 * reads fail — used to learn the logical sector size (word 106 bit 12;
 * words 117/118 = size in 16-bit words). Returns 0 on success, negative on
 * a not-ready/DRQ timeout or drive error.
 */
int ata_identify(void *buf);

#endif /* CORE_HAL_HW_ATA_H */
