/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/power.c — PCF50605 deep-sleep standby.
 *
 * Cleanroom from core/docs/hw/06-power.md ("Standby / sleep"): standby is a
 * single write to the PMU's OOCC1 register over the shared I2C control bus
 * (same 7-bit device 0x08 the battery gauge reads). GOSTDBY latches the
 * transition; a wake source bit (EXTONWAK / CHGWAK) MUST accompany it or the
 * device can never be powered on again. On wake the SoC is fully re-initialised
 * (registers/GPIO/peripherals lost), so control returns via the boot path, not
 * from here.
 */

#include "power.h"
#include "i2c.h"

#define PMU_ADDR         0x08            /* PCF50605 control port (7-bit)        */
#define PMU_OOCC1        0x08            /* on/off control & config 1            */
#define OOCC1_GOSTDBY    0x01            /* trigger standby (latching)           */
#define OOCC1_CHGWAK     0x20            /* wake on charger insertion            */
#define OOCC1_EXTONWAK   0x40            /* wake on external (button / dock)     */

void power_standby(void)
{
    /* Write OOCC1 = GOSTDBY | wake-sources. EXTONWAK + CHGWAK are BOTH set so a
     * button OR a charger wakes it — never issue GOSTDBY without a wake bit. */
    uint8_t cmd[2] = {
        PMU_OOCC1,
        (uint8_t)(OOCC1_GOSTDBY | OOCC1_CHGWAK | OOCC1_EXTONWAK),
    };
    i2c_send(PMU_ADDR, cmd, 2);

    /* The PMU cuts power within a few milliseconds; spin so we never run past
     * the point of no return with a half-torn-down system. */
    for (;;) {
        /* sleeping */
    }
}
