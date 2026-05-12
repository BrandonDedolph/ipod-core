/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Phase 1 PR #1 deliverable: the cross-build pipeline produces a valid
 * ARM ELF whose entry point is _start (boot/crt0.S) and whose first C
 * function is kernel_main below. For now this is a spin loop — UART,
 * LCD, cache init, COP wake, and the cooperative scheduler land in
 * subsequent PRs.
 */

_Noreturn void kernel_main(void) {
    for (;;) {
        __asm__ volatile ("nop");
    }
}
