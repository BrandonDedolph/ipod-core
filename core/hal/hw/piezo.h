/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/piezo.h — menu-click piezo (PP5022 hardware PWM).
 *
 * The iPod Video's navigation "click" is produced by the SoC's PWM0 unit
 * driving the piezo transducer — NOT a GPIO toggled by software. We enable the
 * PWM device clock once, then a click is a single short PWM burst: write PWM0
 * on, and stop it a couple of milliseconds later. Stopping is done from the
 * main loop (piezo_service), so a click never blocks decode.
 *
 * Register facts (addresses/bit values, PP5020/5022 — extracted as hardware
 * facts, reimplemented cleanroom): DEV_INIT1 @0x70000010 (clear 0xc to route
 * PWM to the piezo), DEV_EN @0x6000600C bit 0x00020000 (PWM clock), PWM0_CTRL
 * @0x7000A000 (0x80000000 | form<<16 | inv_freq; inv_freq = 91225/hz; 0 = off).
 */
#ifndef CORE_HAL_HW_PIEZO_H
#define CORE_HAL_HW_PIEZO_H

#include <stdint.h>

/* Enable the PWM peripheral and route it to the piezo. Idempotent. */
void piezo_init(void);

/* Fire one short, self-contained navigation click (drives the PWM burst and
 * stops it before returning; ~1.5 ms). Call on each wheel detent / button
 * press. Self-contained so a blocking op right after (e.g. a library load)
 * can't leave the piezo buzzing. */
void piezo_click(void);

/* Fire a click at an explicit tone frequency (Hz) and burst length (µs) — used
 * for the selectable clicker sound profiles. */
void piezo_click_ex(uint32_t hz, uint32_t us);

#endif /* CORE_HAL_HW_PIEZO_H */
