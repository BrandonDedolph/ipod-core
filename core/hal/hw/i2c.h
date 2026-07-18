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

#endif /* CORE_HAL_HW_I2C_H */
