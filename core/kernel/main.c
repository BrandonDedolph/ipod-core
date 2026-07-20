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
#include "../fs/fat32.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "console.h"
#include "pcm_ring.h"

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
 * Streaming WAV playback. Instead of preloading the whole file, a small ring
 * buffer decouples the foreground disk pump (producer) from the DMA-completion
 * ISR (consumer): the pump reads PCM off the disk into the ring, the ISR
 * drains it into the DMA buffer without ever touching the disk. The ring holds
 * ~1.5 s of headroom — far more than the pump's read latency — and disk PIO
 * (~512 KB/s) out-reads playback (~172 KB/s) by ~3x, so a full-length song
 * streams without underrunning. See kernel/pcm_ring.h + fs/fat32.h.
 */
#define RING_FRAMES  (1u << 16)          /* 65536 frames = 256 KB ~ 1.49 s     */
#define PUMP_BYTES   (16u * 1024u)       /* per-pump disk read chunk            */
#define HDR_BYTES    4096u               /* enough to hold any real WAV header  */

static int16_t  ring_storage[RING_FRAMES * 2];
static uint8_t  pump_buf[PUMP_BYTES] __attribute__((aligned(4)));
static uint8_t  hdr_buf[HDR_BYTES]   __attribute__((aligned(4)));

static pcm_ring_t     g_ring;
static fat32_stream_t g_pcm_stream;      /* positioned at the PCM data start    */
static uint32_t       g_pcm_remaining;   /* PCM data bytes not yet pumped in     */

/* hal_audio source (runs in the DMA ISR): drain the ring. A short return on
 * underrun makes the HAL zero-pad the rest — a glitch, never a stall. */
static int ring_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    return (int)pcm_ring_read(&g_ring, buf, (uint32_t)frames);
}

/* Producer: top the ring up from disk. Reads whole frames (4 bytes) in
 * PUMP_BYTES chunks until the ring is full or the file's PCM is exhausted.
 * Foreground only — never the ISR. */
static void pump_ring(void)
{
    uint32_t freef = pcm_ring_free(&g_ring);
    while (freef > 0 && g_pcm_remaining >= 4u) {
        uint32_t want = freef * 4u;                  /* free space, in bytes  */
        if (want > sizeof pump_buf) {
            want = sizeof pump_buf;
        }
        if (want > g_pcm_remaining) {
            want = g_pcm_remaining;
        }
        want &= ~3u;                                 /* whole frames only     */
        if (want == 0u) {
            break;
        }
        int32_t got = fat32_stream_read(&g_pcm_stream, pump_buf, want);
        if (got <= 0) {
            g_pcm_remaining = 0;                     /* EOF or read error     */
            break;
        }
        uint32_t nframes = (uint32_t)got / 4u;
        pcm_ring_write(&g_ring, (const int16_t *)pump_buf, nframes);
        g_pcm_remaining -= nframes * 4u;
        freef           -= nframes;
    }
}

/* FAT32 block callback: read absolute 512-byte LBAs off the disk. */
static int disk_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    return ata_read_sectors(lba, count, buf);
}

/*
 * Locate the PCM 'data' chunk of a RIFF/WAVE file. `buflen` is how many
 * header bytes are actually present in `wav[]` (all chunk parsing/bounds use
 * it); `filelen` is the true on-disk file size, used only to clamp the
 * reported PCM length. In the streaming path only the ~4 KB header is in the
 * buffer while filelen is the whole multi-MB file — so the two must be kept
 * distinct (passing buflen for both would clamp data_len to the buffer). On
 * success sets *data_off / *data_len / *rate; returns 0 only for PCM (format
 * tag 1), 16-bit, stereo.
 */
static int wav_parse(const uint8_t *wav, uint32_t buflen, uint32_t filelen,
                     uint32_t *data_off, uint32_t *data_len, uint32_t *rate)
{
    if (buflen < 12 ||
        wav[0] != 'R' || wav[1] != 'I' || wav[2] != 'F' || wav[3] != 'F' ||
        wav[8] != 'W' || wav[9] != 'A' || wav[10] != 'V' || wav[11] != 'E') {
        return -1;
    }
    uint32_t fmt_tag = 0, fmt_ch = 0, fmt_rate = 0, fmt_bits = 0;
    int have_fmt = 0;
    uint32_t off = 12;
    while (off + 8u <= buflen) {
        const uint8_t *id = &wav[off];
        uint32_t sz = (uint32_t)wav[off + 4] | ((uint32_t)wav[off + 5] << 8) |
                      ((uint32_t)wav[off + 6] << 16) |
                      ((uint32_t)wav[off + 7] << 24);
        const uint8_t *body = &wav[off + 8];
        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ' &&
            sz >= 16u && off + 8u + 16u <= buflen) {
            fmt_tag  = (uint32_t)body[0]  | ((uint32_t)body[1] << 8);
            fmt_ch   = (uint32_t)body[2]  | ((uint32_t)body[3] << 8);
            fmt_rate = (uint32_t)body[4]  | ((uint32_t)body[5] << 8) |
                       ((uint32_t)body[6] << 16) | ((uint32_t)body[7] << 24);
            fmt_bits = (uint32_t)body[14] | ((uint32_t)body[15] << 8);
            have_fmt = 1;
        } else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            uint32_t avail = filelen - (off + 8u);   /* PCM bytes on disk     */
            *data_off = off + 8u;
            *data_len = sz <= avail ? sz : avail;
            *rate     = fmt_rate;
            return (have_fmt && fmt_tag == 1u && fmt_ch == 2u && fmt_bits == 16u)
                       ? 0 : -1;
        }
        off += 8u + sz + (sz & 1u);
    }
    return -1;
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

        /* Read the MBR, find the FAT32 data partition (type 0B/0C), mount
         * it, open TEST.WAV and preload it to RAM — then play it through
         * the DMA-fed hal_audio backend. Audio and the disk reader are each
         * proven separately; this is the payoff path. */
        uint8_t *mbr = (uint8_t *)mbr_sector;
        int      rd_rc = -1;
        uint32_t sig = 0, fat_lba = 0;
        if (ata_init() == 0) {
            rd_rc = ata_read_sectors(0, 1, mbr);
            if (rd_rc == 0) {
                sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
                for (int p = 0; p < 4; p++) {
                    const uint8_t *e = &mbr[0x1BE + 16 * p];
                    if (e[4] == 0x0B || e[4] == 0x0C) {
                        fat_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                                  ((uint32_t)e[10] << 16) |
                                  ((uint32_t)e[11] << 24);
                        break;
                    }
                }
            }
        }

        fat32_t  fs;
        int      mnt = -1, op = -1;
        uint32_t fclus = 0, fsize = 0;
        uint32_t data_off = 0, data_len = 0, wrate = 0;
        int      hdr_ok = -1;
        if (sig == 0xAA55u && fat_lba != 0) {
            uart_puts("core: [fat32] mount + open TEST.WAV\n");
            /* The MBR partition-start LBA may be in the disk's native
             * 512-byte units OR (on the stock 80 GB, whose FAT uses
             * 2048-byte sectors) in 2048-byte units. Try as-is first, then
             * x4; fat32_mount validates the boot signature + BPB so a wrong
             * base just fails cleanly. */
            mnt = fat32_mount(&fs, disk_read, 0, fat_lba);
            if (mnt != 0) {
                mnt = fat32_mount(&fs, disk_read, 0, fat_lba * 4u);
            }
            if (mnt == 0) {
                op = fat32_open(&fs, "TEST.WAV", &fclus, &fsize);
            }
            if (op == 0) {
                /* Read just the header region (not the whole file) and parse
                 * it: wav_parse scans for the 'data' chunk and reports where
                 * the PCM starts + how long it is. Streaming begins there. */
                fat32_stream_t hs;
                fat32_stream_open(&hs, &fs, fclus, fsize);
                int32_t hn = fat32_stream_read(&hs, hdr_buf, sizeof hdr_buf);
                if (hn > 0) {
                    hdr_ok = wav_parse(hdr_buf, (uint32_t)hn, fsize,
                                       &data_off, &data_len, &wrate);
                }
            }
        }

        /* One diagnostic screen (single silicon-proven present):
         *   SIG  = MBR signature (AA55).  MNT/OP = fat32 mount/open rc (0).
         *   DOFF = byte offset of the PCM 'data' chunk in the file.
         *   RATE = WAV sample rate (0000AC44 = 44100).
         *   DLEN = PCM data length in bytes (should be ~the whole file, NOT
         *          clamped to the 4 KB header — a small DLEN means the
         *          buflen/filelen split regressed). */
        console_clear(bg);
        console_str  (2, 3, "SIG",  CON_WHITE, bg);
        console_hex32(8, 3, sig,               CON_WHITE, bg);
        console_str  (2, 5, "MNT",  CON_WHITE, bg);
        console_hex32(8, 5, (uint32_t)mnt,     CON_WHITE, bg);
        console_str  (2, 7, "OP",   CON_WHITE, bg);
        console_hex32(8, 7, (uint32_t)op,      CON_WHITE, bg);
        console_str  (2, 9, "DOFF", CON_WHITE, bg);
        console_hex32(8, 9, data_off,          CON_WHITE, bg);
        console_str  (2, 11, "RATE", CON_WHITE, bg);
        console_hex32(8, 11, wrate,            CON_WHITE, bg);
        console_str  (2, 13, "DLEN", CON_WHITE, bg);
        console_hex32(8, 13, data_len,         CON_WHITE, bg);
        lcd_present_fb(console_framebuffer());
        uart_puts("core: diagnostic screen presented\n");

        /* Stream the track: position a fresh cursor at the PCM data start,
         * prime the ring, then let the DMA-fed backend drain it while the
         * foreground loop keeps refilling from disk. Plays a whole song —
         * no 4 MB preload cap. */
        if (hdr_ok == 0 && wrate == 44100u) {
            uart_puts("core: streaming TEST.WAV\n");

            /* Position at PCM start: skip data_off header bytes. */
            fat32_stream_open(&g_pcm_stream, &fs, fclus, fsize);
            uint32_t to_skip = data_off;
            while (to_skip > 0) {
                uint32_t chunk = to_skip < sizeof pump_buf ? to_skip
                                                           : sizeof pump_buf;
                int32_t got = fat32_stream_read(&g_pcm_stream, pump_buf, chunk);
                if (got <= 0) {
                    break;
                }
                to_skip -= (uint32_t)got;
            }

            /* Whole frames only: a sub-frame tail (data_len not a multiple
             * of 4) would otherwise strand 1-3 bytes and spin the drain loop
             * forever, since pump_ring stops below one frame. */
            g_pcm_remaining = data_len & ~3u;
            pcm_ring_init(&g_ring, ring_storage, RING_FRAMES);
            pump_ring();                 /* prime before the DAC starts pulling */

            if (hal_audio_init(44100u, 2u) == 0) {
                hal_audio_set_source(ring_source, 0);
                hal_audio_start();
                /* Refill from disk until all PCM is pumped AND the ring has
                 * drained. sleep_ms yields the CPU between top-ups; the DMA
                 * ISR is what actually clocks samples out meanwhile. */
                while (g_pcm_remaining > 0u || pcm_ring_fill(&g_ring) > 0u) {
                    pump_ring();
                    sleep_ms(20);
                }
                sleep_ms(200);           /* let the FIFO/DMA drain the tail */
                hal_audio_stop();
            }
            uart_puts("core: streaming done\n");
        }
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
