/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Phase 1 PR #3 brought up the SER0 debug UART (boot banner — the
 * out-of-band channel every later bring-up step reports through).
 * PR #4 added the first visible sign of life: BCM LCD init plus a
 * red → green → blue solid-fill cycle, narrated over the UART.
 * PR #5 (this one) stands up the cooperative scheduler: after the
 * bring-up narration, kernel_main hands control to a two-task set (a
 * one-shot demo task + an idle task) and never returns. The idle task
 * low-power sleeps the CPU via the per-core countdown between yields.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"

/*
 * Crude bounded busy-delay between LCD fill colors so a human can see
 * the cycle — same volatile-loop idiom as uart.c's reset hold, just
 * bigger. Replaced by a real timer-based delay in a later PR. (By the
 * time this runs the core is at CPUFREQ_NORMAL = 30 MHz — clock_init()
 * has already run — so the cycle is faster than at the 24 MHz boot
 * clock, but it's still not tuned precisely.)
 */
static void delay_eyeball(void) {
    for (volatile uint32_t i = 0; i < (1u << 24); i++) {
        /* spin */
    }
}

/*
 * Idle-task CPU sleep. Program the per-core countdown to wake this core
 * after ~`ms` milliseconds and halt until then (01-soc-pp5022.md, "Sleep
 * / wake"). We use PROC_WAIT_CNT (self-wakes on the countdown), NOT
 * PROC_SLEEP — with no interrupt controller installed yet, sleep-until-
 * interrupt would never wake. Only the CPU runs the kernel in Phase 1,
 * so CPU_CTL is correct. Three NOPs after the write per the doc's
 * pipeline rule.
 */
static void cpu_wait_ms(uint8_t ms) {
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_MSEC | ms);
    __asm__ volatile("nop\n\tnop\n\tnop");
}

/* Per-task stacks (carved from .bss; the scheduler builds each task's
 * initial context frame at the top). 1 KB is ample for these leaf
 * narrators; real subsystem tasks size their own later. */
#define TASK_STACK_SIZE 1024
static uint8_t demo_stack[TASK_STACK_SIZE];
static uint8_t idle_stack[TASK_STACK_SIZE];

/*
 * Idle task: never exits. Sleep the CPU for a short countdown, then
 * yield. Once the demo task has finished, this is the only runnable
 * task, so sched_yield() is a no-op and the loop just idles the core.
 */
_Noreturn static void idle_task(void) {
    uart_puts("core: idle task entered\n");
    for (;;) {
        cpu_wait_ms(10);
        sched_yield();
    }
}

/*
 * Demo task: proves the context switch actually runs a task, that a
 * task can yield and be resumed, and that a task which RETURNS from its
 * entry function lands cleanly in sched_task_exit (via the trampoline)
 * and is dropped from the round-robin. Runs once, yields to let idle
 * start, resumes, then returns.
 */
static void demo_task(void) {
    uart_puts("core: task A running\n");
    uart_puts("core: task A yielding to idle\n");
    sched_yield();
    uart_puts("core: task A resumed, exiting\n");
    /* falls off the end → task_trampoline's lr → sched_task_exit */
}

_Noreturn void kernel_main(void) {
    uart_init();

    uart_puts("core: kernel alive (iPod 5G/5.5G, PP5022)\n");

    /* Hex-path self-test: a fixed pattern exercising every nibble
     * position plus digits and letters. If the line below doesn't
     * read 1234ABCD on the terminal, distrust every register dump
     * that follows. */
    uart_puts("core: uart self-test ");
    uart_put_hex32(0x1234ABCD);
    uart_putc('\n');

    /* First real register dump: which core are we? Expect the low
     * byte to read PROC_ID_CPU = 0x55 (core/docs/hw/01-soc-pp5022.md,
     * "Dual core: CPU and COP"). */
    uart_puts("core: PROCESSOR_ID ");
    uart_put_hex32(PROCESSOR_ID);
    uart_putc('\n');

    /* Come off the 24 MHz boot clock up to CPUFREQ_NORMAL (30 MHz)
     * before the rest of bring-up, so it runs at a sane speed (the
     * boot-clock crawl is what made the LCD cycle take ~1 min/frame on
     * the first hardware boot). Codec-heavy work later requests a
     * further cpu_boost() to 80 MHz. */
    uart_puts("core: clock init -> 30 MHz\n");
    clock_init();
    uart_puts("core: cpu freq ");
    uart_put_hex32(cpu_frequency());
    uart_putc('\n');

    /* LCD bring-up: host-side port init, then probe the BCM power
     * rail. After a chainload the BCM is already powered and idle
     * (core/docs/hw/02-lcd.md, "Chainload handoff state"). If the
     * probe reads unpowered we skip the fills entirely: Rockbox never
     * touches the BCM ports of an unpowered BCM (it bootstraps
     * first), and the clicky emulator (4G model, no BCM) raises a
     * FatalMemException on any 0x3xxxxxxx access — gating on the
     * probe keeps both the dead-BCM hardware case and the emulator
     * smoke well-defined. */
    if (lcd_init()) {
        uart_puts("core: lcd bcm powered\n");

        /* Solid-fill cycle: red -> green -> blue, ~1 s apart, ending
         * on blue. One narration line per fill so the serial log
         * pinpoints exactly which BCM transaction wedged if the
         * panel stays dark. */
        uart_puts("core: lcd fill red\n");
        lcd_fill(0xF800);
        delay_eyeball();

        uart_puts("core: lcd fill green\n");
        lcd_fill(0x07E0);
        delay_eyeball();

        uart_puts("core: lcd fill blue\n");
        lcd_fill(0x001F);
        uart_puts("core: lcd cycle done (panel should be blue)\n");
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping fills (no bootstrap yet)\n");
    }

    /* Bring up the 100 Hz system tick: install the timer, then unmask
     * IRQs at the core. Prove the interrupt path end-to-end before the
     * scheduler starts — sleep_ms yields cooperatively, and with no
     * scheduler running yet sched_yield is a no-op, so this simply spins
     * on the IRQ-driven tick counter. If the two tick readings differ by
     * ~10 (100 Hz x 0.1 s), the timer fired, the controller delivered,
     * irq_vector_entry -> irq_dispatch -> timer_tick_isr ran, and
     * sleep_ms observed the counter. */
    uart_puts("core: timer init @ 100 Hz, enabling IRQs\n");
    timer_init();
    arch_irq_enable();

    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (pre-sleep)\n");
    sleep_ms(100);
    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (post-sleep, ~+10)\n");

    /* Hand off to the cooperative scheduler. Demo task is added first
     * so it runs first; it finishes and drops out, leaving the idle
     * task to low-power the core. sched_start never returns. */
    uart_puts("core: sched init\n");
    sched_init();
    if (sched_add_task(demo_task, demo_stack, sizeof demo_stack, "demoA") < 0 ||
        sched_add_task(idle_task, idle_stack, sizeof idle_stack, "idle") < 0) {
        uart_puts("core: FATAL sched_add_task failed\n");
        for (;;) {
        }
    }
    sched_start();
}
