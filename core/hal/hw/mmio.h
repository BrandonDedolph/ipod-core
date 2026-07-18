/*
 * core/hal/hw/mmio.h — MMIO accessor seam.
 *
 * The freestanding drivers (uart.c, lcd.c) touch hardware only through
 * these six accessors. Two build modes:
 *
 *   - Firmware (default): each accessor is a static-inline volatile
 *     dereference — byte-for-byte the same codegen as the raw
 *     PP_REG{8,16,32} lvalue macros it replaces, so the hw image is
 *     unchanged.
 *
 *   - Host test (-DMMIO_MOCK): the accessors are plain extern functions
 *     supplied by a recording fake bus (tests/hw_mmio/mmio_mock.c), so
 *     the drivers compile and run on the host and their exact ordered
 *     register-access grammar can be asserted. This is the only place
 *     the BCM/LCD transaction stream gets checked — the clicky emulator
 *     models no BCM and the device would need a logic analyzer.
 *
 * Addresses and bit layouts still come exclusively from pp5022.h (which
 * transcribes core/docs/hw/); this header adds no hardware facts, only
 * the read/write mechanism. Widths are load-bearing: the BCM decodes
 * only PP address bits 16..18, and a 32-bit data store is consumed as
 * two 16-bit pushes while status ports are polled 16-bit — so a driver
 * that uses the wrong width is a real bug the trace tests must catch.
 */

#ifndef CORE_HAL_HW_MMIO_H
#define CORE_HAL_HW_MMIO_H

#include <stdint.h>

#ifdef MMIO_MOCK

uint32_t mmio_read32(uintptr_t addr);
uint16_t mmio_read16(uintptr_t addr);
uint8_t  mmio_read8 (uintptr_t addr);
void     mmio_write32(uintptr_t addr, uint32_t value);
void     mmio_write16(uintptr_t addr, uint16_t value);
void     mmio_write8 (uintptr_t addr, uint8_t  value);

#else

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}
static inline uint16_t mmio_read16(uintptr_t addr)
{
    return *(volatile uint16_t *)addr;
}
static inline uint8_t mmio_read8(uintptr_t addr)
{
    return *(volatile uint8_t *)addr;
}
static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}
static inline void mmio_write16(uintptr_t addr, uint16_t value)
{
    *(volatile uint16_t *)addr = value;
}
static inline void mmio_write8(uintptr_t addr, uint8_t value)
{
    *(volatile uint8_t *)addr = value;
}

#endif /* MMIO_MOCK */

#endif /* CORE_HAL_HW_MMIO_H */
