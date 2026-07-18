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
#include "hw/audio.h"
#include "hw/ata.h"
#include "hal.h"
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
 * Stream up to `frames` stereo sine frames through the polled I2S FIFO
 * and return the number that were actually ACCEPTED by the FIFO. The
 * write self-paces on FIFO space, so a full run takes ~frames/44100 s of
 * wall-clock on working hardware. If the TX FIFO never drains (codec not
 * clocking) we bail after 64 consecutive write timeouts and return the
 * count so far — so the returned value is the headline diagnostic:
 *   ~frames  => the FIFO drained, i.e. the codec IS clocking the data
 *              out (a silent output is then an output-stage problem);
 *   ~0       => the FIFO never drained, i.e. no codec clock (I2S
 *              pad-mux / MCLK / config).
 */
static uint32_t audio_play_tone(uint32_t frames)
{
    uint32_t phase = 0;
    uint32_t misses = 0;
    uint32_t wrote = 0;
    for (uint32_t n = 0; n < frames; n++) {
        int16_t s = sine64[phase & 63];
        if (i2s_write_stereo(s, s) != 0) {
            if (++misses >= 64) {
                break;
            }
            continue;
        }
        misses = 0;
        wrote++;
        phase++;
    }
    return wrote;
}

/*
 * DMA-path source callback (hal_audio_set_source): fill `frames` stereo
 * frames of the same 689 Hz sine into the interleaved [L,R,...] buffer.
 * Phase persists across calls so the tone is continuous across the
 * ping-pong buffer boundary. Runs in IRQ context (the DMA-completion ISR)
 * on hw — leaf, no allocation, no blocking. Always fills the full request.
 */
static uint32_t g_sine_phase;
static int sine_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    for (int f = 0; f < frames; f++) {
        int16_t s = sine64[g_sine_phase & 63];
        buf[2 * f]     = s;   /* L */
        buf[2 * f + 1] = s;   /* R */
        g_sine_phase++;
    }
    return frames;
}

/* Per-task stacks (carved from .bss; the scheduler builds each task's
 * initial context frame at the top). 1 KB is ample for these leaf
 * narrators; real subsystem tasks size their own later. */
#define TASK_STACK_SIZE 1024
static uint8_t demo_stack[TASK_STACK_SIZE];
static uint8_t idle_stack[TASK_STACK_SIZE];

/* Disk scratch (uint16_t for the 2-byte alignment ata_read_sectors needs).
 * 1024 words = 2048 bytes = up to 4 logical sectors, enough to read a
 * whole physical sector at once. */
static uint16_t mbr_sector[1024];

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
    /* Bring up the 100 Hz system tick and unmask IRQs at the core BEFORE
     * the audio bring-up: continuous playback is DMA-driven and needs its
     * completion interrupt (source 26) delivered, and sleep_ms below spins
     * on the IRQ-fed tick. Prove the IRQ path first with the tick counter
     * (with no scheduler yet sched_yield is a no-op): if the two readings
     * differ by ~10 (100 Hz x 0.1 s), timer -> controller ->
     * irq_vector_entry -> irq_dispatch -> timer_tick_isr -> sleep_ms all
     * work. */
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

    /* LCD + AUDIO. Probe the BCM power rail: nonzero => real hardware. The
     * clicky emulator has no BCM (lcd_init() false there), so this whole
     * block — every I2S/DAC/DMA access — is skipped, keeping the emulator
     * smoke green; the audio register grammar is proven host-side by the
     * mock-bus trace tests. We run the audio chain first, then report the
     * result in a SINGLE framebuffer present (lcd_present_fb is only
     * silicon-proven for the first frame; a second present stalls in the
     * BCM wait-for-idle poll). GREEN bg = PLL locked. */
    if (lcd_init()) {
        uint16_t bg = (cpu_frequency() == CPUFREQ_NORMAL) ? CON_GREEN
                                                          : CON_RED;

        /* (a) Polled first-sound: a known-good ~1 s tone straight to the
         * I2S FIFO, proving the codec/output path independently of DMA. */
        uart_puts("core: [polled] bring-up + ~1s tone\n");
        i2c_init();
        i2s_init();
        wm8758_init();
        audio_settle();
        i2s_tx_enable();
        uint32_t wrote = audio_play_tone(44100u);

        /* (b) DMA continuous playback via the hal_audio backend: re-init
         * through hal_audio, register the sine source, start, and let it
         * run ~3 s. DMA feeds the FIFO autonomously (paced by the I2S
         * request) while the CPU just waits on the tick; each completion
         * IRQ refills a buffer. The completion count proves the DMA + IRQ
         * path end to end. */
        uart_puts("core: [dma] hal_audio continuous ~3s tone\n");
        uint32_t dmac = 0;
        if (hal_audio_init(44100u, 2u) == 0) {
            g_sine_phase = 0;
            hal_audio_set_source(sine_source, 0);
            hal_audio_start();
            sleep_ms(3000);
            hal_audio_stop();
            dmac = audio_dma_completions();
        }
        uart_puts("core: dma completions ");
        uart_put_hex32(dmac);
        uart_putc('\n');

        /* (c) ATA probe. READ SECTORS to LBA 0 keeps returning IDNF on
         * this drive (the MK8010GAH with 2048-byte sectors), so before
         * building FAT32 we get GROUND TRUTH: IDENTIFY DEVICE takes no LBA
         * (can't IDNF), which both proves the read/DRQ path works and
         * reports the true logical sector size (word 106 bit 12 => >512;
         * words 117/118 = size in 16-bit words). */
        uart_puts("core: [ata] init + read MBR + find FAT32\n");
        uint8_t *mbr = (uint8_t *)mbr_sector;
        int      ata_rc = ata_init();
        int      rd_rc  = -1;
        uint32_t sig = 0, fat_lba = 0;
        uint8_t  fat_type = 0;
        if (ata_rc == 0) {
            /* The wrapper now reads whole physical sectors internally, so a
             * plain single-sector MBR read works. */
            rd_rc = ata_read_sectors(0, 1, mbr);
            if (rd_rc == 0) {
                sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
                for (int p = 0; p < 4; p++) {
                    const uint8_t *e = &mbr[0x1BE + 16 * p];
                    if (e[4] == 0x0B || e[4] == 0x0C) {
                        fat_type = e[4];
                        fat_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                                  ((uint32_t)e[10] << 16) |
                                  ((uint32_t)e[11] << 24);
                        break;
                    }
                }
            }
        }
        uart_puts("core: rd ");
        uart_put_hex32((uint32_t)rd_rc);
        uart_puts(" sig ");
        uart_put_hex32(sig);
        uart_puts(" fat_lba ");
        uart_put_hex32(fat_lba);
        uart_putc('\n');
        (void)wrote;

        /* Diagnostic screen (single silicon-proven present):
         *   ID   = ata_identify rc (0 => read/DRQ path works, so the READ
         *          SECTORS IDNF is purely an addressing issue).
         *   W106 = identify word 106 (bit 12 / 0x1000 set => logical
         *          sector > 512 bytes).
         *   W117/W118 = logical sector size in 16-bit words (x2 = bytes);
         *          0400/0000 => 2048-byte sectors.
         *   RD   = ata_read_sectors rc; SIG = MBR signature (AA55 if OK). */
        console_clear(bg);
        console_str  (2, 3, "FREQ", CON_WHITE, bg);
        console_hex32(8, 3, cpu_frequency(),  CON_WHITE, bg);
        console_str  (2, 5, "RD",   CON_WHITE, bg);   /* MBR read rc (0 = OK) */
        console_hex32(8, 5, (uint32_t)rd_rc,  CON_WHITE, bg);
        console_str  (2, 7, "SIG",  CON_WHITE, bg);   /* AA55 = real MBR */
        console_hex32(8, 7, sig,              CON_WHITE, bg);
        console_str  (2, 9, "PART", CON_WHITE, bg);   /* FAT32 type 0B/0C */
        console_hex32(8, 9, fat_type,         CON_WHITE, bg);
        console_str  (2, 11, "PLBA", CON_WHITE, bg);  /* FAT32 partition start LBA */
        console_hex32(8, 11, fat_lba,         CON_WHITE, bg);
        lcd_present_fb(console_framebuffer());
        uart_puts("core: diagnostic screen presented\n");
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping audio + screen\n");
    }

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
