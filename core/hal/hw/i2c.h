/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/i2c.h — PP502x on-SoC I2C master (codec control bus).
 *
 * See core/docs/hw/09-i2c.md. Polled, write-only path — all Phase 1
 * needs is pushing register writes to the WM8758 codec. Freestanding:
 * <stdint.h> only, and asm-free so the driver host-compiles for the
 * mock-bus trace tests.
 */
#ifndef CORE_HAL_HW_I2C_H
#define CORE_HAL_HW_I2C_H

#include <stdint.h>

/* One-time controller bring-up: clock-gate, reset pulse, clock poke. */
void i2c_init(void);

/*
 * Write up to 4 payload bytes to 7-bit device address `dev`. Blocks
 * (bounded) until the controller is idle before loading the transaction.
 * Returns 0 on success, -1 on a bad length, -2 on a BUSY timeout.
 */
int i2c_send(uint8_t dev, const uint8_t *bytes, int len);

/*
 * Register-pointer read: set the device's internal address pointer with a
 * 1-byte write of `reg`, then read `n` result bytes (1..4) from 7-bit
 * device address `dev` into `buf`. This is the shape the PCF50605 PMU
 * (battery ADC) needs; the codec never reads, so Phase 1 shipped without
 * it. Blocks (bounded) for the turn-around and for the read to complete
 * before latching DATA — unlike the lazily-polled write path, the result
 * bytes are only valid once BUSY clears. Returns 0 on success, -1 on a
 * bad length/NULL buffer, -2 on a BUSY timeout.
 */
int i2c_read(uint8_t dev, uint8_t reg, uint8_t *buf, int n);

#endif /* CORE_HAL_HW_I2C_H */
