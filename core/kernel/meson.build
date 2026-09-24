# Kernel entry point.
#
# main.c is the first C function after crt0.S. PR #5 adds the
# cooperative scheduler: sched.c (task table + round-robin policy) and
# switch.S (the ARM context switch + initial-frame trampoline). IRQ
# vectors and the timer tick land in subsequent PRs.
#
# panic.c is the other end of boot/crt0.S: the fault stubs there capture
# the faulting register state and call into it. hw-only (it talks to the
# SER0 UART and the BCM panel directly), which is why it lives in
# kernel_sources rather than in a portable library.

# config.c is the ONLY code in the firmware that writes to the user's disk
# (settings persistence into a pre-allocated CORECFG.DAT, overwritten in place
# so no filesystem metadata is ever mutated). It was the only caller of
# ata_write_sectors until evlog.c (below). Its record codec + the write
# path's register grammar are covered host-side by tests/kernel/config_test.c.
#
# cfg_commit.c is the commit GATE in front of config_save: the debounce, the
# parked-drive and battery refusals, the wake-before-write, and what becomes
# of a change whose write failed. EXTRACTED OUT OF main.c's settings_commit()
# so the host can test it (tests/kernel/cfg_commit_test.c); pure, no
# hardware — main.c hands it the clock, the drive, the player and the battery.

#
# evlog.c is the SECOND writer: the on-disk event log (a RAM tap on the UART,
# flushed one 2048 B block at a time into the pre-allocated CORELOG.BIN
# through the same gate as config_save, every block's LBA re-resolved
# through the file's cluster chain). Hardware-free like cfg_commit.c — the
# write and wake are injected from main.c — so tests/kernel/evlog_test.c
# drives the whole ring, the boot scan and the flush policy on the host.

#
# datetime.c, timesync.c and wallclock.c are the CLOCK's portable half, and
# none of them touch hardware. datetime.c is the integer civil<->epoch calendar
# for 2000..2099 that hal/hw/rtc.c decodes its BCD into and every screen
# formats from — the one piece of this feature that can be proved exhaustively,
# which tests/kernel/datetime_test.c does day by day across the whole century.
# timesync.c is the boot decision about the clock stamp the host leaves in
# CORECFG.DAT (apply it once, or judge it stale — never on a loop), and
# wallclock.c carries the time between the rare RTC reads over the same
# wrapping microsecond counter ui/sleeptimer.c counts minutes on. All three are
# host-built by tests/ rather than being reachable only through main.c.
#
# otg_store.c is the THIRD writer: the On-The-Go live list (512 locator pairs)
# in two CRC-32'd 5120-byte slots inside the pre-allocated COREOTG.DAT, through
# the same gate as config_save and with every slot's cluster runs re-resolved
# per write (a slot is bigger than a small cluster, so it can straddle one).
# Hardware-free like evlog.c — the write and the wake are injected — so
# tests/kernel/otg_store_test.c drives the codec, the addressing, the mount
# and the commit policy on the host.
kernel_sources = files('main.c', 'sched.c', 'switch.S', 'irq.c', 'timer.c', 'clock.c', 'console.c', 'pcm_ring.c', 'cache.c', 'panic.c', 'config.c', 'cfg_commit.c', 'evlog.c', 'otg_store.c', 'datetime.c', 'timesync.c', 'wallclock.c', 'chargestat.c')
