/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/ata.c — minimal PIO-polled ATA sector reader (PP5022).
 *
 * Implements core/docs/hw/04-ata.md's PIO LBA28 read path, trimmed to the
 * minimum. We boot DIRECTLY as the OSOS image (docs/hw/README.md, "How we
 * actually boot"): there is no chainloader, so the drive state we inherit is
 * whatever the Apple boot ROM left behind after it read our image off the
 * firmware partition — powered, spun up, and with IDE0_PRI_TIMING programmed
 * to values of the ROM's choosing at the ROM's clock. That is enough for us
 * to skip power, IDENTIFY and SET FEATURES, and we deliberately do NOT
 * rewrite IDE0_PRI_TIMING: the ROM's strobes read the disk fine today, and a
 * wrong value there is a silent boot hang with no UART to explain it (see
 * ata_clock_hold for how we keep the bus clock where the ROM left it
 * instead). PP502x task registers are plain accesses — no PP5002 IDE_CFG
 * write handshake. Control registers 8-bit; data port 16-bit, 256 halfwords
 * per 512-byte sector, little-endian, no byte swap.
 */

#include "pp5022.h"
#include "mmio.h"
#include "ata.h"
#include "uart.h"                 /* uart_puts: the refused-boost report     */
#include "../../kernel/clock.h"   /* CPUFREQ_MAX: the clock the strobes want */

/*
 * Poll ceilings, in microseconds on the free-running USEC_TIMER (04-ata.md,
 * "Timeouts": wait_for_rdy 10 s, and the ATA spec's 31 s spin-up allowance
 * after a soft reset). These used to be ITERATION counts (1<<20 polls, i.e.
 * ~0.2-0.5 s at 80 MHz), which is fine for a drive that is already spinning
 * — the boot ROM just read our image off it — and wrong for every case where
 * the drive is legitimately busy for seconds: BSY held after STANDBY
 * IMMEDIATE while the drive flushes and parks (the wait returned -1, so
 * g_ata_parked stayed 0 and the main loop re-issued STANDBY every pass), BSY
 * after the SRST in ata_recover while the drive re-spins, and a slow spin-up
 * on a low battery. A wedged or absent drive still cannot hang the kernel:
 * the ceiling is just measured in the unit the drive's behaviour is
 * specified in.
 */
#define ATA_READY_US        10000000u   /* !BSY / RDY: wait_for_rdy, 10 s   */
#define ATA_SRST_READY_US   31000000u   /* ...after a soft reset: 31 s spin-up */
/* Data-phase (DRQ) wait ceiling. Gives a spun-DOWN drive (parked by
 * ata_standby during playback/suspend) room to spin back up on the next read —
 * ~1-3 s typical, more on a low battery, where the ~4 s the old value allowed
 * was seen to expire mid-spin-up and issue an SRST into it. Normal reads set
 * DRQ in microseconds, so this never bites on a spinning drive. */
#define ATA_SPINUP_US        8000000u

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
 *
 * ATA_PHYS_LOG / ATA_SECTOR_SZ now live in ata.h: the write path's caller
 * (kernel/config.c) has to size its record in whole PHYSICAL sectors, and a
 * second private copy of the quantum is exactly the drift that leaves a
 * misaligned write behind after a future bump.
 */

/* One physical sector's worth of scratch for the alignment bounce. */
static uint16_t ata_bounce[ATA_SECTOR_SZ * ATA_PHYS_LOG / 2u];

/*
 * Wait for BSY to clear within `limit_us`. Returns the status byte that
 * satisfied the wait (so a caller can test ERR/DF on a status that is
 * actually valid — every other bit is undefined while BSY is set), or -1 on
 * timeout.
 */
static int ata_wait_not_busy_us(uint32_t limit_us)
{
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    for (;;) {
        uint8_t s = mmio_read8(ATA_ALT_STATUS_ADDR);
        if (!(s & ATA_STATUS_BSY)) {
            return s;
        }
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > limit_us) {
            return -1;
        }
    }
}

/* Wait for BSY to clear (ATA_READY_US). 0 on success, -1 on timeout. */
static int ata_wait_not_busy(void)
{
    return ata_wait_not_busy_us(ATA_READY_US) < 0 ? -1 : 0;
}

/* Wait for BSY clear AND RDY set within `limit_us`. 0 on success, -1 on
 * timeout. One poll loop rather than two chained ones: the deadline is on
 * the whole wait, which is what the doc's timeout table specifies. */
static int ata_wait_ready_us(uint32_t limit_us)
{
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    for (;;) {
        uint8_t s = mmio_read8(ATA_ALT_STATUS_ADDR);
        if ((s & (ATA_STATUS_BSY | ATA_STATUS_RDY)) == ATA_STATUS_RDY) {
            return 0;
        }
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > limit_us) {
            return -1;
        }
    }
}

/* Wait for BSY clear, then RDY set (ATA_READY_US). */
static int ata_wait_ready(void)
{
    return ata_wait_ready_us(ATA_READY_US);
}

/*
 * CPU-frequency bracket for a transfer (04-ata.md, "PIO timing values" — the
 * table is qualified "At 80 MHz operation"). We never rewrite
 * IDE0_PRI_TIMING0, so the PIO strobe widths are frozen at whatever the Apple
 * boot ROM programmed before it handed us control (there is no chainloader
 * to have re-timed them — see the file banner), while kernel/clock.c moves
 * the core (and therefore the IDE bus clock) between 30 and 80 MHz. Issuing a
 * transfer at a DIFFERENT clock than the strobes were programmed for is at
 * best slow and at worst marginal, so every command is bracketed in the
 * boost: one fixed operating point for all ATA traffic, the boosted one the
 * ROM's values are known to work at (every sector this firmware has ever read
 * on the device went through it).
 *
 * cpu_boost/cpu_unboost are refcounted, so when the UI or the player already
 * holds a boost (the common case — main.c boosts whenever the backlight is on
 * or audio is playing) this is a pure counter bump with NO PLL work. Only a
 * read issued from deep idle pays a frequency switch, and that path is already
 * dominated by the multi-second platter spin-up.
 *
 * THE BOOST CAN BE REFUSED. kernel/clock.c will not run the frequency
 * switch while the audio DMA is streaming out of SDRAM (it reprograms the
 * SDRAM timing the DMA master is reading through); the request is deferred
 * until the stream stops and cpu_boost() returns having changed nothing. So
 * a transfer issued from a 30 MHz core under live audio — the player's own
 * refill, unless something else already holds the boost — goes out at
 * 30 MHz against strobes programmed for 80. That refusal policy is right
 * and is NOT changed here; what was wrong is that it was silent. After
 * boosting, the bracket now checks the clock actually in effect and reports
 * a mismatch on the UART, so a marginal-timing read error seen on device
 * can be correlated with the clock it happened at instead of being a
 * mystery.
 *
 * Declared weak so this driver still links in the host golden-trace test,
 * which compiles ata.c alone with no kernel clock driver or UART; a test
 * that defines them becomes the kernel's stand-in and can observe the
 * bracket.
 */
__attribute__((weak)) void cpu_boost(void);
__attribute__((weak)) void cpu_unboost(void);
__attribute__((weak)) uint32_t cpu_frequency(void);
__attribute__((weak)) void uart_puts(const char *s);

static void ata_clock_hold(void)
{
    if (cpu_boost) {
        cpu_boost();
    }
    if (cpu_frequency && uart_puts && cpu_frequency() != CPUFREQ_MAX) {
        uart_puts("core: ata: boost refused, transfer at slow clock\n");
    }
}

static void ata_clock_release(void)
{
    if (cpu_unboost) {
        cpu_unboost();
    }
}

/* Wait for start-of-transfer: BSY clear and DRQ set. Returns -2 on a drive
 * error (ERR/DF) surfaced while waiting, -1 on timeout. */
static int ata_wait_drq(void)
{
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    for (;;) {
        uint8_t s = mmio_read8(ATA_ALT_STATUS_ADDR);
        if (!(s & ATA_STATUS_BSY)) {
            if (s & ATA_STATUS_DRQ) {
                return 0;
            }
            if (s & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
                return -2;
            }
        }
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > ATA_SPINUP_US) {
            return -1;      /* spin-up / transfer never came */
        }
    }
}

/*
 * Soft-reset the ATA channel. SRST+nIEN asserts reset, then nIEN alone
 * releases it (04-ata.md, "Soft reset"); the drive stays spun up throughout.
 * Delays are bounded busy-waits (this also runs at bring-up, before any timer
 * exists): ~tens of us after asserting, a few ms after releasing, per the ATA
 * reset timing. Factored out of ata_init() so the per-sector error recovery
 * (04-ata.md, "Per-sector error handling") can reuse the exact same sequence.
 */
static void ata_bus_reset(void)
{
    mmio_write8(ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
    for (volatile uint32_t d = 0; d < (1u << 10); d++) {
        /* hold reset asserted (>= ~5 us) */
    }
    mmio_write8(ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
    for (volatile uint32_t d = 0; d < (1u << 17); d++) {
        /* post-reset recovery (> ~2 ms) */
    }
}

int ata_init(void)
{
    /* Soft-reset the channel before use. The minimal "just reuse the
     * bootloader handoff state" init (select + wait-ready) reached a ready
     * drive on device but READ SECTORS returned a drive error — the drive
     * needs a clean reset to accept fresh commands. */
    ata_bus_reset();

    /* Select the master device (with the obsolete must-be-1 bits) and wait
     * for it to come ready — with the post-reset allowance, since the drive
     * may re-spin after the SRST. */
    mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);
    return ata_wait_ready_us(ATA_SRST_READY_US);
}

/*
 * MID-TRANSFER ERROR RECOVERY (04-ata.md, "Per-sector error handling").
 *
 * A multi-sector READ SECTORS that fails partway through leaves the drive
 * mid-command: it still has the remaining sectors queued and may still be
 * asserting DRQ with unread data in its buffer. Walking away from that state
 * is a DATA-INTEGRITY bug, not just an error-reporting one — the caller's
 * retry re-issues READ SECTORS on top of the residue, and the drive answers
 * with data shifted by however many halfwords were left behind, which the
 * retry then reports as SUCCESS. That silently corrupts FLAC frames and, far
 * worse, FAT/directory sectors.
 *
 * So on any error inside the transfer loop: latch the ERROR register (the
 * only place IDNF is visible), drain whatever DRQ still holds, then soft-reset
 * the channel so the next command starts from a clean drive state.
 *
 * Returns the classified result code for the caller.
 */
#define ATA_DRAIN_LIMIT  (256u * 256u)   /* bounded: 256 sectors' worth of words */

static int ata_recover(int cause)
{
    /* 1. Error register FIRST — a reset clears it. */
    uint8_t err = mmio_read8(ATA_ERROR_ADDR);

    /* 2. Drain any data the drive is still offering, bounded. Reading the
     *    data port is what retires a DRQ block; without this the drive can sit
     *    with a full buffer that the reset then has to discard mid-handshake. */
    uint32_t drain = ATA_DRAIN_LIMIT;
    while ((mmio_read8(ATA_ALT_STATUS_ADDR) & ATA_STATUS_DRQ) && --drain != 0) {
        (void)mmio_read16(ATA_DATA_ADDR);
    }

    /* 3. Clean slate for the next command. The drive may re-spin after the
     *    reset, so the wait carries the spec's 31 s spin-up allowance — and
     *    its result is NOT discarded: a drive that never comes back from the
     *    reset is not "recovered", whatever the original cause was, and the
     *    caller must hear "never came ready" (-1) rather than a retryable
     *    error that sends it straight back into the same wedged drive. */
    ata_bus_reset();
    mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);
    if (ata_wait_ready_us(ATA_SRST_READY_US) != 0) {
        return -1;
    }

    /* 4. IDNF means the LBA does not exist on this drive (or violates its
     *    physical-sector alignment rule) — re-issuing the identical command
     *    can only fail identically, so report it distinctly and let the caller
     *    fail fast instead of burning its whole retry budget. */
    if (err & ATA_ERROR_IDNF) {
        return ATA_ERR_IDNF;
    }
    return cause;
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
    /* Same rule as the sector paths: the drive raises BSY after the last
     * word while it completes the command, and every other status bit is
     * undefined while BSY is set. Testing ERR/DF on the first read after the
     * data could fail a good IDENTIFY on a transient byte (and this runs
     * once, at boot, with no retry behind it) or pass a bad one. Wait for
     * BSY to clear and test the status that cleared it. */
    int st = ata_wait_not_busy_us(ATA_SPINUP_US);
    if (st < 0) {
        return -2;
    }
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

/* Authoritative "platters spun down" state for the whole system. Set by
 * ata_standby(), cleared the moment any READ command is issued (a read
 * transparently spins the drive back up) or by an explicit ata_wakeup().
 * Read via ata_is_parked() so the UI idle-timer and the player's burst-park
 * logic share ONE truth about the drive instead of each guessing. */
static int g_ata_parked;

/* 1 after ata_sleep() has issued SLEEP: the drive's interface is INACTIVE and
 * ignores commands until a reset. Every command path checks it and runs
 * ata_leave_sleep() first, so a caller that skips ata_wakeup() still gets a
 * working drive — the same "any access wakes it" contract STANDBY has. */
static int g_ata_slept;

int ata_is_parked(void) { return g_ata_parked; }
int ata_is_slept(void)  { return g_ata_slept; }

/* Defined with the write path below; SLEEP needs the flush first. */
static int ata_flush_cache(void);
static int ata_wait_not_busy_timed(void);

/*
 * Bring the drive out of SLEEP. Unlike STANDBY, SLEEP shuts the interface
 * down and the drive answers nothing — a READ issued at it just times out.
 * The ATA-defined way back is a reset (software here, on the control
 * register: the same SRST pulse ata_init and the error recovery use), after
 * which the drive spins up and holds BSY for the duration, so the wait is
 * the TIMED one, not the poll-count one. Clears the slept flag first so a
 * failure here cannot make the next access reset again forever. The budget
 * is the post-SRST one (ATA_SRST_READY_US), the same wait ata_recover uses.
 */
static int ata_leave_sleep(void)
{
    g_ata_slept = 0;
    ata_bus_reset();
    mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);
    return ata_wait_ready_us(ATA_SRST_READY_US);   /* post-reset spin-up budget */
}

/*
 * LBA28 address-space guard. The register programming below carries only 28
 * address bits — SECTOR/LCYL/HCYL plus the low nibble of SELECT — so an LBA
 * at or past 1<<28 does not fail: its top bits are masked off, the drive
 * quietly serves (or, on a write, OVERWRITES) the sector 2^28 lower, and the
 * command reports success. Nothing on the fitted 80 GB drive lives above
 * 2^28 sectors (128 GiB), and the FAT layer above us never asks; this exists
 * so a future larger disk or a corrupt partition table fails loudly at the
 * driver instead of silently aliasing the wrong sector. Written to avoid the
 * uint32 wrap that `lba + count` would suffer near the top of the range.
 */
#define ATA_LBA28_SECTORS (1u << 28)

static int ata_lba28_in_range(uint32_t lba, uint32_t count)
{
    return lba < ATA_LBA28_SECTORS && count <= ATA_LBA28_SECTORS - lba;
}

static int ata_read_raw_locked(uint32_t lba, uint32_t count, void *buf)
{
    if (count == 0 || count > 256) {
        return -1;
    }
    if (!ata_lba28_in_range(lba, count)) {
        return -1;
    }
    if (g_ata_slept && ata_leave_sleep() != 0) {
        return -1;                      /* a READ cannot wake a SLEEPing drive, and
                                         * the reset that can did not bring it back:
                                         * do not stack the ready + DRQ budgets on top */
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
    g_ata_parked = 0;               /* a READ spins the drive up (see ata_wait_drq) */

    /* Command-to-status pipeline guard (~sub-microsecond). A short bounded
     * spin rather than asm nops so this stays host-compilable. */
    for (volatile uint32_t g = 0; g < 64; g++) {
        /* settle */
    }

    uint16_t *out = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        int rc = ata_wait_drq();
        if (rc != 0) {
            /* -3 drive error, -2 timeout; ata_recover may upgrade to IDNF. */
            return ata_recover(rc == -2 ? -3 : -2);
        }
        /* Read the primary status once to acknowledge, then stream the
         * sector: 256 little-endian halfwords straight into the buffer. */
        (void)mmio_read8(ATA_COMMAND_ADDR);
        for (int w = 0; w < 256; w++) {
            *out++ = mmio_read16(ATA_DATA_ADDR);
        }
        /* The drive raises BSY after the last word of a DRQ block while it
         * fetches the next one (or completes the command), and every other
         * status bit is undefined while BSY is set — so testing ERR/DF on
         * the first read after the data can see a stale or transient byte
         * and run a needless recovery, or miss a real one. Wait for BSY to
         * clear and test the status that cleared it. */
        int st = ata_wait_not_busy_us(ATA_SPINUP_US);
        if (st < 0) {
            return ata_recover(-2);
        }
        if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            return ata_recover(-3);
        }
    }
    return 0;
}

/* Clock-bracketed wrapper: every command issued by this driver runs at one
 * fixed CPU/IDE clock (see ata_clock_hold). */
static int ata_read_raw(uint32_t lba, uint32_t count, void *buf)
{
    ata_clock_hold();
    int rc = ata_read_raw_locked(lba, count, buf);
    ata_clock_release();
    return rc;
}

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf)
{
    uint8_t *out = (uint8_t *)buf;

    while (count > 0) {
        uint32_t off = lba & (ATA_PHYS_LOG - 1u);      /* 0..ATA_PHYS_LOG-1 */

        /* FAST PATH: physically aligned start, at least one whole physical unit
         * to read, and a 2-byte-aligned destination — stream straight into the
         * caller's buffer in ONE large multi-sector command (up to 256 sectors),
         * with no bounce buffer and no byte copy. This turns a 128 KB read from
         * ~128 READ SECTORS commands + a 128 KB byte-copy into ~1 command. */
        if (off == 0 && count >= ATA_PHYS_LOG &&
            ((uintptr_t)out & 1u) == 0) {
            uint32_t bulk = count & ~(ATA_PHYS_LOG - 1u);  /* whole phys units    */
            if (bulk > 256u) bulk = 256u;                  /* per-command cap      */
            int rc = ata_read_raw(lba, bulk, out);
            if (rc != 0) {
                return rc;
            }
            out   += bulk * ATA_SECTOR_SZ;
            lba   += bulk;
            count -= bulk;
            continue;
        }

        /* SLOW PATH (unchanged, byte-identical): an unaligned head, a trailing
         * partial physical unit, or an odd destination — read the whole physical
         * unit into the bounce and copy out only the logical sectors asked for,
         * hiding the drive's physical-alignment requirement from callers. */
        uint32_t phys = lba - off;                    /* physical boundary */
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

/* ---------------------------------------------------------------------------
 * Drive power management. Two depths:
 *
 *   ata_standby()  STANDBY IMMEDIATE (0xE0): platters down, interface up.
 *                  The next media access auto-spins it back up, holding BSY
 *                  for the (multi-second) spin-up. The idle-timer park.
 *   ata_sleep()    FLUSH CACHE, STANDBY IMMEDIATE, then SLEEP (0xE6): the
 *                  drive's lowest state — the interface logic is powered
 *                  down too, and the drive answers NOTHING until a reset.
 *                  For suspend and power-off, where the drive will not be
 *                  touched for minutes-to-hours and every mA counts.
 *
 * ata_wakeup() undoes either: a reset first if the drive was put to SLEEP
 * (nothing else reaches it), then a whole-physical-sector throwaway read
 * that pre-pays the spin-up. 04-ata.md, "Spin-up / spin-down" and "Power-
 * management commands".
 *
 * UNVERIFIED ON THE DEVICE: the STANDBY path has run on hardware, the
 * SLEEP + reset-to-wake path has not (2026-09-13). The MK8010GAH lists
 * SLEEP as mandatory (it is, for every ATA-6 drive); what is not known is
 * how long its post-reset spin-up takes, which is why the wake wait is
 * the post-reset timed one (ATA_SRST_READY_US) rather than a poll count.
 * ------------------------------------------------------------------------- */
#define ATA_CMD_STANDBY_IMM   0xE0
#define ATA_CMD_SLEEP         0xE6

/* STANDBY IMMEDIATE, clock already held. */
static int ata_standby_locked(void)
{
    if (g_ata_slept) {
        return 0;                       /* already below STANDBY; it would not hear us */
    }
    if (ata_wait_ready() != 0) {
        return -1;
    }
    mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);          /* master */
    mmio_write8(ATA_COMMAND_ADDR, ATA_CMD_STANDBY_IMM);
    for (volatile uint32_t g = 0; g < 64; g++) {
        /* command-to-status settle */
    }
    int rc = ata_wait_not_busy();   /* accepted; platters coast down on their own */
    if (rc == 0) {
        g_ata_parked = 1;
    }
    return rc;
}

int ata_standby(void)
{
    ata_clock_hold();
    int rc = ata_standby_locked();
    ata_clock_release();
    return rc;
}

int ata_sleep(void)
{
    if (g_ata_slept) {
        return 0;                       /* already there; it would not hear us */
    }
    ata_clock_hold();

    /*
     * FLUSH CACHE first. STANDBY IMMEDIATE is documented to flush the drive's
     * own cache on the way down, but the explicit flush is what the write
     * path's durability rests on and it costs nothing when the cache is
     * clean. -1 means the drive never came READY: nothing after it can be
     * issued either, so stop. Any other failure has already been RECOVERED
     * (reset, reselect, ready) by ata_flush_cache, which leaves the drive in
     * a known state — so carry on and park it anyway: power is about to be
     * cut, and parked heads matter more than the code, which is still
     * reported.
     */
    /* A drive that is already parked was flushed by its STANDBY IMMEDIATE and
     * every write path flushes itself, so its cache is clean by construction.
     * FLUSH CACHE would only spin it up to do nothing (04-ata.md's
     * ata_sleepnow flushes only when the drive is ON, for this reason). */
    int rc = g_ata_parked ? 0 : ata_flush_cache();
    if (rc == -1) {
        ata_clock_release();
        return -1;
    }

    int rcs = ata_standby_locked();     /* heads parked, platters down */
    if (rcs != 0) {
        ata_clock_release();
        return rc != 0 ? rc : rcs;
    }

    /* SLEEP. The command completes (BSY clears) BEFORE the drive enters
     * Sleep mode, so the ordinary bounded wait reads its completion; after
     * that the interface goes quiet and the flag below is the only truth. */
    if (ata_wait_ready() == 0) {
        mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);          /* master */
        mmio_write8(ATA_COMMAND_ADDR, ATA_CMD_SLEEP);
        for (volatile uint32_t g = 0; g < 64; g++) {
            /* command-to-status settle */
        }
        rcs = ata_wait_not_busy();
        g_ata_slept = 1;                /* issued: only a reset reaches it now */
    } else {
        rcs = -1;
    }

    ata_clock_release();
    return rc != 0 ? rc : rcs;
}

int ata_wakeup(void)
{
    ata_clock_hold();
    if (g_ata_slept && ata_leave_sleep() != 0) {
        /* A READ cannot wake a SLEEPing drive — only a reset can (above) —
         * and the reset's own 31 s budget just ran out. Issuing the probe
         * anyway would stack another ready + DRQ wait on a drive that is
         * not answering; report it now and let the caller decide. */
        ata_clock_release();
        g_ata_parked = 0;
        return -1;
    }
    /*
     * The drive reports "ready" in standby (BSY clear, RDY set) but is spun
     * down; a READ is what triggers spin-up. The throwaway read MUST cover a
     * whole PHYSICAL sector — this drive rejects sub-physical-sector access
     * with IDNF (see ATA_PHYS_LOG above; verified on device 2026-07-18:
     * count=1 IDNFs, count=2 at an even LBA succeeds). The old count=1 read
     * therefore ALWAYS errored, which meant the spin-up was never pre-paid,
     * an ERR stayed latched on the drive, and — because the function returned
     * before clearing it — g_ata_parked stayed set forever, so ata_is_parked()
     * lied to every caller for the rest of the boot.
     *
     * ata_read_raw drains the full transfer, runs the documented error
     * recovery on failure, and clears g_ata_parked the moment the command is
     * issued; the extended (time-based) DRQ wait inside it tolerates the
     * multi-second spin-up.
     */
    int rc = ata_read_raw_locked(0, ATA_PHYS_LOG, ata_bounce);
    ata_clock_release();

    /*
     * Reconcile on EVERY exit path. Even a failed wake means we can no longer
     * claim the platters are parked (the READ either spun them up or the drive
     * is in a state we cannot characterise). Reporting "not parked" makes the
     * idle timer re-issue STANDBY later — harmless; reporting "parked" forever
     * suppresses spin-up pre-payment for the rest of the session.
     */
    g_ata_parked = 0;
    return rc;
}

/* ---------------------------------------------------------------------------
 * PIO WRITE path (LBA28). 04-ata.md, "Read / write paths" -> "Write
 * differences" + "Power-management commands" -> "Flush cache".
 *
 * PROVEN ON HARDWARE 2026-07-27 with exactly ONE caller: the settings save
 * in kernel/config.c, which resolves its target LBA three independent ways
 * before letting a byte leave the CPU and whose banner documents the
 * re-qualification procedure (write a scratch LBA, read back, compare,
 * fsck). Anyone adding a caller owes that same procedure, because a wrong
 * LBA here does not fail loudly the way a bad read does: it destroys data.
 * The SECOND caller — the event log, kernel/evlog.c — is wired and
 * unqualified until STATUS.md's first-flash item 9 has been run.
 *
 * ALIGNMENT IS NOT OPTIONAL. This drive reports 2 logical sectors per
 * physical sector and REJECTS sub-physical-sector access with IDNF (see the
 * ATA_PHYS_LOG comment at the top of this file — verified on device
 * 2026-07-18: count=1 IDNFs, count=2 at an even LBA succeeds). The read
 * wrapper hides that with a read-modify-nothing bounce; a write wrapper
 * cannot do the equivalent safely — a read-modify-write of a physical sector
 * turns "write these 512 bytes" into "re-write 1024 bytes, half of them from
 * a stale copy", and a power loss between the read and the write loses data
 * the caller never asked to touch. So misalignment is REJECTED, loudly, and
 * the calling layer is expected to deal in whole physical sectors.
 *
 * The FLUSH CACHE at the end is load-bearing, not decorative: the drive's
 * write cache is on by default, so WRITE SECTORS returning success only means
 * the data reached the drive's DRAM. Without the flush a battery pull (or a
 * MENU+SELECT reset, or the idle STANDBY path) between the write and the
 * drive's own writeback loses it silently. 0xE7 is the non-EXT form, which is
 * the correct choice for this ATA-6-era drive (04-ata.md flush selection
 * logic step 3: word 83 bit 12).
 * ------------------------------------------------------------------------- */

#define ATA_CMD_WRITE_SECTORS 0x30   /* LBA28 PIO write (one DRQ per sector) */
#define ATA_CMD_FLUSH_CACHE   0xE7   /* non-EXT flush (ATA-6+)               */

/* Wait for BSY to clear with the data-phase deadline. FLUSH CACHE on a
 * spun-down or busy drive can hold BSY for seconds, and a write's commit
 * after the last sector likewise. Returns 0 on success, -1 on timeout. */
static int ata_wait_not_busy_timed(void)
{
    return ata_wait_not_busy_us(ATA_SPINUP_US) < 0 ? -1 : 0;
}

/*
 * Raw PIO WRITE SECTORS: `count` logical sectors at `lba` from `buf`. Both
 * must already be physical-sector-aligned (the wrapper guarantees it). The
 * shape mirrors ata_read_raw exactly — same register programming, same
 * command-to-status settle, same per-sector DRQ handshake, SAME MID-TRANSFER
 * RECOVERY — with the data direction reversed and a completion wait after the
 * final sector, because on a write the drive asserts BSY after the last word
 * while it commits.
 *
 * The recovery is not optional on the write side either. The first version of
 * this function returned straight out of the loop on a DRQ timeout or an
 * ERR/DF status, leaving the drive mid-WRITE with sectors still queued and a
 * possibly half-filled buffer — exactly the residue the read path's
 * ata_recover() banner warns about, except that the very next command after a
 * settings save is typically the PLAYER'S REFILL READ, which would then be
 * issued on top of that residue and could return shifted data reported as
 * success. Every failure exit after the command byte is written therefore
 * goes through ata_recover(), which also lets a bad LBA surface as
 * ATA_ERR_IDNF instead of a retryable -3.
 */
static int ata_write_raw(uint32_t lba, uint32_t count, const void *buf)
{
    if (count == 0 || count > 256) {
        return -1;
    }
    if (!ata_lba28_in_range(lba, count)) {
        return -1;
    }
    if (g_ata_slept && ata_leave_sleep() != 0) {
        return -1;                      /* as the read path: a failed reset is final */
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
    mmio_write8(ATA_COMMAND_ADDR, ATA_CMD_WRITE_SECTORS);
    g_ata_parked = 0;               /* a WRITE spins the drive up just as a READ does */

    /* Command-to-status pipeline guard (~sub-microsecond), as on the read. */
    for (volatile uint32_t g = 0; g < 64; g++) {
        /* settle */
    }

    const uint16_t *in = (const uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        int rc = ata_wait_drq();
        if (rc != 0) {
            /* -3 drive error, -2 timeout; ata_recover may upgrade to IDNF. */
            return ata_recover(rc == -2 ? -3 : -2);
        }
        /* Read the primary status once to acknowledge, then stream the
         * sector out: 256 little-endian halfwords, no byte swap. */
        (void)mmio_read8(ATA_COMMAND_ADDR);
        for (int w = 0; w < 256; w++) {
            mmio_write16(ATA_DATA_ADDR, *in++);
        }
        /* As on the read: BSY follows the last word while the drive takes
         * the block, and ERR/DF are only meaningful once it clears. */
        int st = ata_wait_not_busy_us(ATA_SPINUP_US);
        if (st < 0) {
            return ata_recover(-2);
        }
        if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            return ata_recover(-3);
        }
    }

    /* The drive holds BSY after the final data word while it commits the
     * transfer; only then is the status meaningful. (The read path has no
     * equivalent step — its last DRQ IS the last data.) A drive that never
     * drops BSY, or reports an error once it does, is still mid-command from
     * our point of view: reset it before anyone issues the next one. */
    if (ata_wait_not_busy_timed() != 0) {
        return ata_recover(-2);
    }
    if (mmio_read8(ATA_ALT_STATUS_ADDR) & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        return ata_recover(-3);
    }
    return 0;
}

/* Push the drive's write cache to the platters. See the banner above: this is
 * what makes a returned write durable across a power cut. A flush that times
 * out or errors leaves the drive in a state we cannot characterise — and the
 * next command is the player's refill — so it takes the same recovery as a
 * failed transfer rather than handing a wedged channel to the reader. */
static int ata_flush_cache(void)
{
    if (ata_wait_ready() != 0) {
        return -1;
    }
    mmio_write8(ATA_SELECT_ADDR, ATA_SELECT_OBS);          /* master */
    mmio_write8(ATA_COMMAND_ADDR, ATA_CMD_FLUSH_CACHE);
    for (volatile uint32_t g = 0; g < 64; g++) {
        /* command-to-status settle */
    }
    if (ata_wait_not_busy_timed() != 0) {
        return ata_recover(-2);
    }
    if (mmio_read8(ATA_ALT_STATUS_ADDR) & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        return ata_recover(-3);
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf)
{
    /* Reject rather than bounce: see the banner. `lba` and `count` must both
     * be multiples of ATA_PHYS_LOG, and the source must be 16-bit aligned for
     * the halfword data port. Argument checks come BEFORE the clock hold so a
     * rejected call leaves the refcount balanced. */
    if (count == 0) {
        return -1;
    }
    if ((lba % ATA_PHYS_LOG) != 0 || (count % ATA_PHYS_LOG) != 0) {
        return -1;
    }
    if (((uintptr_t)buf & 1u) != 0) {
        return -1;
    }

    /*
     * Hold the CPU/IDE clock for the WHOLE request — data phase and flush
     * alike — exactly as ata_read_raw does per command (see ata_clock_hold).
     * We never rewrite IDE0_PRI_TIMING0, so the PIO strobe widths are frozen
     * at whatever the boot ROM programmed, while kernel/clock.c moves the
     * core between 30 and 80 MHz. A read issued at the wrong clock is
     * slow or marginal and fails loudly; a WRITE issued at the wrong clock
     * can land corrupt bytes on the platter, which does not fail at all. The
     * debounced settings save runs from the idle main loop, which is
     * precisely where the core may be sitting at 30 MHz — so this is the
     * common case, not the corner one. Refcounted, so it is a counter bump
     * whenever the UI or player already holds a boost.
     */
    ata_clock_hold();

    const uint8_t *in = (const uint8_t *)buf;
    int rc = 0;
    while (count > 0) {
        uint32_t chunk = count;
        if (chunk > 256u) {
            chunk = 256u;                       /* per-command cap (NSECTOR) */
        }
        chunk &= ~(ATA_PHYS_LOG - 1u);          /* keep each command aligned */
        rc = ata_write_raw(lba, chunk, in);
        if (rc != 0) {
            ata_clock_release();
            return rc;
        }
        in    += chunk * ATA_SECTOR_SZ;
        lba   += chunk;
        count -= chunk;
    }

    /* One flush for the whole request, not one per command: the flush is the
     * expensive part and the caller's durability point is "ata_write_sectors
     * returned", not "each 256-sector chunk landed". */
    rc = ata_flush_cache();
    ata_clock_release();
    return rc;
}
