/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/i2c.c — PP502x on-SoC I2C master, polled write + read.
 *
 * Implements core/docs/hw/09-i2c.md. Byte-wide register file at
 * 0x7000C000; a transaction is "load address, load <=4 data bytes, set
 * count, strobe". The original Phase 1 consumer is the WM8758 codec
 * control port (see wm8758.c), which is write-only. The register-pointer
 * READ path (i2c_read) was added for the PCF50605 PMU battery gauge
 * (see battery.c) — write the register address, turn the bus around,
 * read N result bytes.
 */

#include "pp5022.h"
#include "mmio.h"
#include "irqlock.h"   /* DEV_EN/DEV_RS RMW vs the timer ISR (see irqlock.h) */
#include "i2c.h"

#define I2C_MAX_BYTES 4

/*
 * Bounded BUSY-wait so a dead/unclocked bus can't hang the core (same
 * posture as UART_TX_SPIN_LIMIT / the PLL-lock spin). A codec register
 * write clocks out in tens of microseconds; 1<<16 trips is far past
 * working hardware. On timeout the caller gets an error rather than a
 * hang.
 */
#define I2C_BUSY_SPIN_LIMIT (1u << 16)

/* Reset-hold spin; matches uart.c's conservative bounded hold. */
#define I2C_RESET_HOLD_SPIN (1u << 16)

static int i2c_wait_idle(void)
{
    uint32_t spin = I2C_BUSY_SPIN_LIMIT;
    while ((mmio_read8(I2C_STATUS_ADDR) & I2C_BUSY) && --spin != 0) {
        /* poll */
    }
    return spin != 0 ? 0 : -1;
}

/*
 * Set once the reset pulse + clock poke have run. i2c_init is NOT called once:
 * main.c brings the bus up at boot, and hal_audio_init calls it again for
 * every track and every seek (audio.c), immediately after wm8758_mute's
 * register write — which, because i2c_send returns before its transaction
 * completes (09-i2c.md, "Write transaction"), may still be on the wire. The
 * old init asserted DEV_RS unconditionally with the idle wait at the END, so
 * every track change could truncate that write mid-byte. A slave left
 * mid-byte can hold SDA low; this controller exposes no SCL bit-bang, so
 * there is no recovery, and every later write (including power_standby's,
 * i.e. the power-off itself) burns its 65536 polls and fails.
 *
 * So the reset is one-shot. A re-init only drains the bus: it waits for the
 * in-flight transaction to complete and returns, leaving the controller's
 * state exactly as the first init left it.
 */
static int g_inited;

/*
 * Suspend gating of the I2C clock. The bus has two idle-time users — the
 * codec's power-down write, which player_pump issues once early in a
 * suspend, and the PMU battery ADC every 5 s (and the PMU standby command
 * itself, which is the last thing a suspend does). Rather than track which
 * of those is next, the gate is self-restoring: i2c_clock_suspend() clears
 * DEV_I2C (recording that it was on), and the next transaction or init
 * re-gates first. What a transaction against an unclocked controller would
 * do is not documented (09-i2c.md) — BUSY might read clear and the write
 * be silently lost, which for power_standby's GOSTDBY is "the power-off
 * that did nothing" — so the restore is unconditional on every entry, not
 * an optimisation.
 */
static int g_gated;

void i2c_clock_suspend(void)
{
    uint32_t f = hw_irq_save();
    uint32_t en = mmio_read32(DEV_EN_ADDR);
    if (en & DEV_I2C) {
        mmio_write32(DEV_EN_ADDR, en & ~DEV_I2C);
        g_gated = 1;
    }
    hw_irq_restore(f);
}

/*
 * Undo the gate: clock on, then the reset pulse and the clock/config poke
 * — the whole "Controller init" sequence (09-i2c.md), not just the enable
 * bit. Nothing documents what a DEV_EN gate does to the controller's
 * configuration, and nobody (Rockbox included) has gated this block at
 * runtime before; if the config is lost, an enable-only restore hands the
 * codec power-down, the 5 s battery ADC read and — worst — the PMU
 * GOSTDBY command to an unconfigured controller. The three extra writes
 * cost microseconds and need no new hardware facts. No idle drain before
 * the reset: the block was unclocked, nothing can be in flight.
 */
void i2c_clock_resume(void)
{
    if (!g_gated) {
        return;
    }
    uint32_t f = hw_irq_save();
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_I2C);
    hw_irq_restore(f);
    g_gated = 0;

    f = hw_irq_save();
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) | DEV_I2C);
    hw_irq_restore(f);
    for (volatile uint32_t i = 0; i < I2C_RESET_HOLD_SPIN; i++) {
        /* hold reset */
    }
    f = hw_irq_save();
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) & ~DEV_I2C);
    hw_irq_restore(f);
    mmio_write32(I2C_CLKCFG_ADDR, 0x00000000);
    mmio_write32(I2C_CLKCFG_ADDR, 0x00000080);
}

void i2c_init(void)
{
    if (g_gated) {
        i2c_clock_resume();      /* the drain below needs a clocked STATUS */
    }
    if (g_inited) {
        /* Idempotent re-init: let whatever is on the wire finish. If it never
         * does, the CONTROLLER is wedged (BUSY stuck) — nothing good is in
         * flight to truncate, and without a reset every later write would
         * time out for the rest of the boot. Fall through to the reset. */
        if (i2c_wait_idle() == 0) {
            return;
        }
    }

    /* Clock-gate the I2C block on, then pulse its reset (09-i2c.md,
     * "Controller init"). DEV_EN/DEV_RS live in the 0x60006000 block and are
     * read-modify-written from the timer ISR too (clickwheel_service gates the
     * OPTO block there), so each RMW pair is IRQ-masked — see irqlock.h. The
     * reset HOLD sits outside the mask: it is a plain busy-wait with no shared
     * state, and masking it would add a multi-thousand-cycle IRQ blackout. */
    uint32_t f = hw_irq_save();
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_I2C);
    hw_irq_restore(f);
    g_gated = 0;

    /* The block is clocked now: let any transaction the boot ROM (or an
     * earlier image) left in flight complete BEFORE the reset yanks the
     * controller out from under it — see g_inited. Bounded; a dead bus just
     * falls through to the reset that would fix it. */
    (void)i2c_wait_idle();

    f = hw_irq_save();
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) | DEV_I2C);
    hw_irq_restore(f);
    for (volatile uint32_t i = 0; i < I2C_RESET_HOLD_SPIN; i++) {
        /* hold reset */
    }
    f = hw_irq_save();
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) & ~DEV_I2C);
    hw_irq_restore(f);

    /* Undocumented clock/config poke the iPod path performs: write 0
     * then 0x80 to 0x600060A4 (09-i2c.md). Required magic; no symbolic
     * meaning in the reference. */
    mmio_write32(I2C_CLKCFG_ADDR, 0x00000000);
    mmio_write32(I2C_CLKCFG_ADDR, 0x00000080);

    /* Rockbox primes the controller with a throwaway read from device
     * 0x08. Our codec use is write-only and we build no read path, so we
     * skip the prime and simply let the controller settle to idle; the
     * first real transaction's leading BUSY-wait covers the rest. */
    (void)i2c_wait_idle();
    g_inited = 1;
}

#ifdef MMIO_MOCK
/*
 * Host-test-only hook: forget that the one-shot init has run, so a trace
 * test can assert the first-init grammar from a known state. Compiled out
 * of the freestanding image — the arm-none-eabi build never defines
 * MMIO_MOCK.
 */
void i2c_test_reset(void)
{
    g_inited = 0;
    g_gated  = 0;
}
#endif

int i2c_send(uint8_t dev, const uint8_t *bytes, int len)
{
    if (len < 1 || len > I2C_MAX_BYTES) {
        return -1;
    }
    if (g_gated) {
        i2c_clock_resume();      /* suspend gated the block; see g_gated */
    }
    if (i2c_wait_idle() != 0) {
        return -2;
    }

    /*
     * NOTE: Rockbox brackets the register loads below in an IRQs-off
     * critical section because it shares this bus across threads
     * (codec / PMIC / accelerometer). In Phase 1 the codec bring-up is
     * the ONLY consumer and runs from a single context, so there is no
     * interleaving to guard against — and keeping this function free of
     * core-mask asm lets it host-compile for the trace tests. A future
     * multi-consumer bus must add a (host-portable) critical section.
     */

    /* Device address in bits 7:1, R/W bit clear = write (09-i2c.md). */
    mmio_write8(I2C_ADDR_ADDR, (uint8_t)((dev & 0x7F) << 1));

    /* Select write mode (clear the read-enable bit). */
    mmio_write8(I2C_CTRL_ADDR,
                (uint8_t)(mmio_read8(I2C_CTRL_ADDR) & ~I2C_READ));

    /* Load payload into DATA0..DATA(len-1). */
    for (int i = 0; i < len; i++) {
        mmio_write8(I2C_DATA_ADDR(i), bytes[i]);
    }

    /* Byte count = (len - 1) in CTRL bits 2:1. */
    mmio_write8(I2C_CTRL_ADDR,
                (uint8_t)((mmio_read8(I2C_CTRL_ADDR) & ~I2C_COUNT_MASK)
                          | (uint8_t)((len - 1) << 1)));

    /* Strobe: begin the transaction. Completion is polled lazily by the
     * next call's leading i2c_wait_idle() (09-i2c.md, "Write
     * transaction"). */
    mmio_write8(I2C_CTRL_ADDR,
                (uint8_t)(mmio_read8(I2C_CTRL_ADDR) | I2C_SEND));
    return 0;
}

int i2c_read(uint8_t dev, uint8_t reg, uint8_t *buf, int n)
{
    if (n < 1 || n > I2C_MAX_BYTES || buf == 0) {
        return -1;
    }

    /*
     * Phase 1 — set the register pointer. The PMU is a register-pointer
     * device: a 1-byte write of the target register loads its internal
     * address pointer (which then auto-increments across the read). This
     * is exactly a 1-byte i2c_send, so reuse the audited write path
     * rather than re-open-code the addr/count/strobe dance.
     */
    int rc = i2c_send(dev, &reg, 1);
    if (rc != 0) {
        return rc;
    }

    /*
     * The write path polls completion lazily (it lets the NEXT
     * transaction's leading wait cover it). A read must not — we are
     * about to turn the bus around, so the pointer write has to have
     * actually landed first. Block for it here.
     */
    if (i2c_wait_idle() != 0) {
        return -2;
    }

    /* Phase 2 — read transaction. Device address with the R/W bit SET. */
    mmio_write8(I2C_ADDR_ADDR,
                (uint8_t)(((dev & 0x7F) << 1) | I2C_ADDR_RW));

    /*
     * Select read mode AND set the byte count (n-1) in CTRL bits 2:1 in
     * one write. (The write path clears I2C_READ at its own start, so we
     * needn't restore write-mode here for the next caller.)
     */
    mmio_write8(I2C_CTRL_ADDR,
                (uint8_t)((mmio_read8(I2C_CTRL_ADDR) & ~I2C_COUNT_MASK)
                          | I2C_READ
                          | (uint8_t)((n - 1) << 1)));

    /* Strobe: begin the read. */
    mmio_write8(I2C_CTRL_ADDR,
                (uint8_t)(mmio_read8(I2C_CTRL_ADDR) | I2C_SEND));

    /*
     * Unlike a write, the result registers are NOT valid until the
     * transaction completes — block for BUSY to clear before latching.
     */
    if (i2c_wait_idle() != 0) {
        return -2;
    }

    /* Latch the result bytes DATA0..DATA(n-1). */
    for (int i = 0; i < n; i++) {
        buf[i] = mmio_read8(I2C_DATA_ADDR(i));
    }
    return 0;
}
