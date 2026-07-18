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
#include "hw/i2c.h"
#include "hw/wm8758.h"
#include "hw/i2s.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "console.h"

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

/*
 * First-sound tone. A single-cycle 64-sample sine (~689 Hz when streamed
 * one sample per frame at 44.1 kHz), amplitude 12000 ≈ -8.7 dBFS so a
 * 0 dB headphone gain lands at a reasonable listening level. Generated
 * offline (round(12000·sin(2π·i/64))).
 */
static const int16_t sine64[64] = {
        0,   1176,   2341,   3483,   4592,   5657,   6667,   7613,
     8485,   9276,   9978,  10583,  11087,  11483,  11769,  11942,
    12000,  11942,  11769,  11483,  11087,  10583,   9978,   9276,
     8485,   7613,   6667,   5657,   4592,   3483,   2341,   1176,
        0,  -1176,  -2341,  -3483,  -4592,  -5657,  -6667,  -7613,
    -8485,  -9276,  -9978, -10583, -11087, -11483, -11769, -11942,
   -12000, -11942, -11769, -11483, -11087, -10583,  -9978,  -9276,
    -8485,  -7613,  -6667,  -5657,  -4592,  -3483,  -2341,  -1176,
};

/* Crude bounded settle delay for the codec VMID ramp, so the first
 * samples don't land on a still-charging bias rail and pop. This path
 * runs at CPUFREQ_NORMAL = 30 MHz (kernel_main calls clock_init only,
 * never cpu_boost), so 1<<21 volatile iterations is ~100 ms — roughly a
 * VMID_10K settle. No timer needed; this runs before the scheduler.
 * Revisit the loop count if this ever moves onto the boosted clock. */
static void audio_settle(void)
{
    for (volatile uint32_t i = 0; i < (1u << 21); i++) {
        /* spin */
    }
}

/*
 * Stream `frames` stereo sine frames through the polled I2S FIFO. The
 * write self-paces on FIFO space, so this takes ~frames/44100 seconds of
 * wall-clock on working hardware. Returns 0 if it streamed, -1 if the
 * TX FIFO never drained (codec not clocking — 64 consecutive write
 * timeouts) so we bail instead of grinding for minutes.
 */
static int audio_play_tone(uint32_t frames)
{
    uint32_t phase = 0;
    uint32_t misses = 0;
    for (uint32_t n = 0; n < frames; n++) {
        int16_t s = sine64[phase & 63];
        if (i2s_write_stereo(s, s) != 0) {
            if (++misses >= 64) {
                return -1;
            }
            continue;
        }
        misses = 0;
        phase++;
    }
    return 0;
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
        /* On-screen register readout — the panel reports the boot state
         * with no serial cable. The whole screen is GREEN if the PLL
         * locked (core reached CPUFREQ_NORMAL) or RED if clock_init
         * degraded to the crystal, with the raw clock registers on top:
         * FREQ = cpu_frequency(), STAT = PLL_STATUS (bit 31 = lock),
         * CTRL = PLL_CONTROL readback. */
        uint16_t bg = (cpu_frequency() == CPUFREQ_NORMAL) ? CON_GREEN
                                                          : CON_RED;
        uart_puts("core: lcd bcm powered - rendering debug screen\n");
        console_clear(bg);
        console_str  (2, 3, "FREQ", CON_WHITE, bg);
        console_hex32(8, 3, cpu_frequency(),               CON_WHITE, bg);
        console_str  (2, 5, "STAT", CON_WHITE, bg);
        console_hex32(8, 5, mmio_read32(PLL_STATUS_ADDR),  CON_WHITE, bg);
        console_str  (2, 7, "CTRL", CON_WHITE, bg);
        console_hex32(8, 7, mmio_read32(PLL_CONTROL_ADDR), CON_WHITE, bg);
        lcd_present_fb(console_framebuffer());
        uart_puts("core: debug screen presented\n");

        /* FIRST SOUND. Gated behind the same BCM-powered probe as the
         * debug screen: it means we are on real hardware, not the clicky
         * emulator (which models no BCM — and no I2S/DAC either, so sound
         * can only be proven on device; the register grammar is checked
         * host-side by the mock-bus trace tests). Chain: I2C controller ->
         * WM8758 codec -> I2S serializer -> a polled 689 Hz sine. i2s_init
         * runs before wm8758_init because the codec's MCLK reference must
         * be alive before the codec is told to master the bus clocks
         * (05-audio.md). Blocks ~2 s, then continues to the scheduler. */
        uart_puts("core: audio bring-up (I2C -> WM8758 -> I2S)\n");
        console_str(2, 9, "SND INIT", CON_WHITE, bg);
        lcd_present_fb(console_framebuffer());
        i2c_init();
        i2s_init();
        wm8758_init();
        audio_settle();
        i2s_tx_enable();

        console_str(2, 9, "SND PLAY", CON_WHITE, bg);
        lcd_present_fb(console_framebuffer());
        uart_puts("core: playing ~2s 689 Hz tone\n");
        int tone_rc = audio_play_tone(44100u * 2u);

        console_str(2, 9, tone_rc == 0 ? "SND DONE " : "SND NOCLK",
                    CON_WHITE, bg);
        lcd_present_fb(console_framebuffer());
        uart_puts(tone_rc == 0
                      ? "core: tone done\n"
                      : "core: I2S FIFO never drained (no codec clock)\n");
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping debug screen\n");
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
