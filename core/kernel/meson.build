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
# so no filesystem metadata is ever mutated). It is also the only caller of
# ata_write_sectors. Its record codec + the write path's register grammar are
# covered host-side by tests/kernel/config_test.c.
#
# cfg_commit.c is the commit GATE in front of config_save: the debounce, the
# parked-drive and battery refusals, the wake-before-write, and what becomes
# of a change whose write failed. EXTRACTED OUT OF main.c's settings_commit()
# so the host can test it (tests/kernel/cfg_commit_test.c); pure, no
# hardware — main.c hands it the clock, the drive, the player and the battery.

kernel_sources = files('main.c', 'sched.c', 'switch.S', 'irq.c', 'timer.c', 'clock.c', 'console.c', 'pcm_ring.c', 'cache.c', 'panic.c', 'config.c', 'cfg_commit.c')
