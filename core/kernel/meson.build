# Kernel entry point.
#
# main.c is the first C function after crt0.S. PR #5 adds the
# cooperative scheduler: sched.c (task table + round-robin policy) and
# switch.S (the ARM context switch + initial-frame trampoline). IRQ
# vectors and the timer tick land in subsequent PRs.

kernel_sources = files('main.c', 'sched.c', 'switch.S')
