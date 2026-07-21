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
#include "cache.h"
#include "console.h"
#include "pcm_ring.h"
#include "../codecs/decoder.h"
#include "../codecs/arena.h"
#include "../codecs/dr_flac/flac.h"
#include "../codecs/dr_mp3/mp3.h"

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
 * Streaming FLAC playback. The drive can't sustain uncompressed PCM over PIO
 * (~172 KB/s needed, ~173 KB/s ceiling), so we stream the COMPRESSED FLAC
 * (~40 KB/s) and decode on the fly. The same SPSC ring decouples producer from
 * the DMA ISR — but the producer is now the decoder: flac_pump() decodes PCM
 * frames into the ring (dr_flac's own read callback pulls compressed bytes off
 * the disk via fat32_stream), and ring_source() (ISR) drains the ring to DMA.
 * dr_flac runs freestanding on a static arena — no libc. See codecs/.
 */
#define RING_FRAMES   (1u << 16)         /* 65536 frames = 256 KB ~ 1.49 s      */
#define DECODE_FRAMES 4096u              /* frames per decode() call            */
#define ARENA_BYTES   (128u * 1024u)     /* MP3 arena high-water ~96 KB (measured);
                                            FLAC ~40 KB. Sized for the larger. This
                                            raises .bss ~64 KB — fine on 64 MB SDRAM.*/

static int16_t    ring_storage[RING_FRAMES * 2];
static int16_t    decode_buf[DECODE_FRAMES * 2];        /* decoder output stage */
static uint8_t    arena_buf[ARENA_BYTES] __attribute__((aligned(8)));

static pcm_ring_t g_ring;
static decoder_t  g_dec;
static int        g_eos;                 /* decoder hit end of stream           */

/*
 * fat32-backed decoder_source_t: the decoder pulls its compressed input from a
 * file through this. fat32_stream is forward-only, so a backward seek re-opens
 * from the first cluster and skips forward — fine because dr_flac only seeks
 * during open() (to size the file / skip metadata), never in the decode loop.
 */
typedef struct {
    fat32_t       *fs;
    uint32_t       first_clus;
    uint32_t       fsize;
    fat32_stream_t st;
    uint32_t       phys;                 /* where the fat32 stream physically is */
    uint32_t       pos;                  /* logical position; a seek moves ONLY  */
                                         /* this — the physical walk is deferred */
} fat_src_t;

static fat_src_t g_fsrc;

static void fat_src_open(fat_src_t *s, fat32_t *fs, uint32_t clus, uint32_t sz)
{
    s->fs = fs;
    s->first_clus = clus;
    s->fsize = sz;
    s->phys = 0;
    s->pos = 0;
    fat32_stream_open(&s->st, fs, clus, sz);
}

/* Bring the physical stream up to the logical position — only when a read
 * actually needs data there. Forward is a cheap FAT-walk skip; backward
 * re-opens from the start then skips. This is what makes dr_flac's open-time
 * seek-to-EOF (to size the file) free: it never reads at EOF, so we never
 * physically walk there — vs the old seek that read+discarded the whole file. */
static void fat_src_sync(fat_src_t *s)
{
    if (s->pos < s->phys) {              /* rewind: re-open at 0                 */
        fat32_stream_open(&s->st, s->fs, s->first_clus, s->fsize);
        s->phys = 0;
    }
    while (s->phys < s->pos) {           /* skip forward via the FAT chain       */
        uint32_t got = fat32_stream_skip(&s->st, s->pos - s->phys);
        if (got == 0) {
            break;
        }
        s->phys += got;
    }
}

static size_t fat_src_read(void *ud, void *buf, size_t bytes)
{
    fat_src_t *s = (fat_src_t *)ud;
    fat_src_sync(s);
    int32_t got = fat32_stream_read(&s->st, buf, (uint32_t)bytes);
    if (got <= 0) {
        return 0;
    }
    s->phys += (uint32_t)got;
    s->pos  += (uint32_t)got;
    return (size_t)got;
}
static int fat_src_seek(void *ud, int offset, int origin)
{
    fat_src_t *s = (fat_src_t *)ud;
    long target = (origin == DECODER_SEEK_SET) ? (long)offset
                : (origin == DECODER_SEEK_END) ? (long)s->fsize + offset
                                               : (long)s->pos + offset;
    if (target < 0 || (uint32_t)target > s->fsize) {
        return 0;
    }
    s->pos = (uint32_t)target;           /* lazy: physical move deferred to read */
    return 1;
}
static int64_t fat_src_tell(void *ud)
{
    return (int64_t)((fat_src_t *)ud)->pos;
}

/* hal_audio source (runs in the DMA ISR): drain the ring. A short return on
 * underrun makes the HAL zero-pad the rest — a glitch, never a stall. */
static int ring_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    return (int)pcm_ring_read(&g_ring, buf, (uint32_t)frames);
}

/* Producer: decode (FLAC or MP3) into the ring until it's full or end-of-stream.
 * Codec-agnostic — it only calls g_dec.ops->decode, so the same loop drives
 * whichever decoder open() installed. Foreground only. decode() pulls compressed
 * bytes off the disk internally, so this both reads and decodes. */
static void decode_pump(void)
{
    while (!g_eos && pcm_ring_free(&g_ring) >= DECODE_FRAMES) {
        int got = g_dec.ops->decode(&g_dec, decode_buf, (int)DECODE_FRAMES);
        if (got <= 0) {
            g_eos = 1;
            break;
        }
        pcm_ring_write(&g_ring, decode_buf, (uint32_t)got);
    }
}

/* FAT32 block callback: read absolute 512-byte LBAs off the disk. */
static int disk_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    return ata_read_sectors(lba, count, buf);
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
    /* Free-running 1 MHz microsecond counter (USEC_TIMER, 01-soc-pp5022.md
     * "Timers"). Sampled here at the very top so every later milestone can
     * report elapsed-since-boot in real time, INDEPENDENT of the 100 Hz tick
     * (which isn't armed until timer_init below). Powers the on-screen BTUS
     * (boot -> diagnostic screen) and TFUS (boot -> first sound) counters,
     * and their UART echoes. Wraps every ~71 min — boot is seconds, so no
     * wrap concern; unsigned subtraction is wrap-safe regardless. */
    uint32_t boot_us0 = mmio_read32(USEC_TIMER_ADDR);

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

    /* Turn on the unified cache now — decode is far too slow with it off.
     * Write-back, so the audio DMA path flushes (cache_commit) before the
     * DMA reads a freshly-filled buffer. */
    uart_puts("core: cache init\n");
    cache_init();

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
        /* Capture the GREEN/RED background BEFORE boosting: green means
         * clock_init's 30 MHz PLL locked (cpu_frequency() still reads
         * CPUFREQ_NORMAL here). cpu_boost() below moves us to 80 MHz, so this
         * must be sampled first or the "PLL locked" proof would always read
         * red. */
        uint16_t bg = (cpu_frequency() == CPUFREQ_NORMAL) ? CON_GREEN
                                                          : CON_RED;

        /* Boost to 80 MHz up front, bracketing the ENTIRE open path (ATA
         * init, MBR read, FAT mount, TEST.FLAC open + album-art skip) and the
         * decode prime — not just playback. Song-open is partly CPU-bound (FAT
         * chain walks, the dr_flac header parse), so the higher clock cuts
         * time-to-first-sound, not only decode headroom. Correctness is
         * unaffected: clock rate never changes the decoded PCM. Tradeoff: the
         * sub-second open path now draws 80 MHz power instead of 30 MHz —
         * negligible battery cost, and cpu_unboost() at the end of this block
         * drops back to 30 MHz on every exit path (playable or not). The timer
         * is a fixed 1 MHz source, so BTUS/TFUS/DTKS stay comparable across
         * clocks. */
        cpu_boost();

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

        /* These outlive the decode loop: dr_flac keeps pointers to the byte
         * source and the allocation callbacks for the decoder's whole life. */
        fat32_t          fs;
        decoder_source_t src;
        decoder_arena_t  arena;
        decoder_alloc_t  alloc;
        int      mnt = -1, op = -1, oc = -1;
        int      fmt = -1;               /* which codec opened: 0=FLAC, 1=MP3    */
        uint32_t fclus = 0, fsize = 0, arate = 0, achan = 0;
        uint32_t awater = 0, dtks = 0;
        if (sig == 0xAA55u && fat_lba != 0) {
            uart_puts("core: [fat32] mount + probe TEST.FLA / TEST.MP3\n");
            /* The MBR partition-start LBA may be in the disk's native
             * 512-byte units OR (on the stock 80 GB, whose FAT uses
             * 2048-byte sectors) in 2048-byte units. Try as-is first, then
             * x4; fat32_mount validates the boot signature + BPB. */
            mnt = fat32_mount(&fs, disk_read, 0, fat_lba);
            if (mnt != 0) {
                mnt = fat32_mount(&fs, disk_read, 0, fat_lba * 4u);
            }
            if (mnt == 0) {
                /* Format probe: TEST.FLA (FLAC) wins if present, else TEST.MP3
                 * (MP3). fat32_open now matches VFAT long names too (LFN landed),
                 * but we keep the simple fixed-name probe for bring-up. NOTE on
                 * 8.3: a 4-char extension like ".FLAC" has no valid short name and
                 * is mangled on disk (TEST~1.FLA); the 3-char TEST.FLA/TEST.MP3
                 * names always resolve, so we stick with those here. */
                op = fat32_open(&fs, "TEST.FLA", &fclus, &fsize);
                if (op == 0) {
                    fmt = 0;             /* FLAC */
                } else {
                    op = fat32_open(&fs, "TEST.MP3", &fclus, &fsize);
                    if (op == 0) {
                        fmt = 1;         /* MP3 */
                    }
                }
            }
            if (op == 0) {
                /* Open as a stream: the decoder pulls compressed bytes off the
                 * disk through fat_src_*, decoding on a static arena (no libc).
                 * Only the header is read here. The fat_src source, arena, ring,
                 * pump loop, ISR and DMA are all codec-agnostic — only THIS
                 * open() call differs between FLAC and MP3. */
                fat_src_open(&g_fsrc, &fs, fclus, fsize);
                src.read = fat_src_read;
                src.seek = fat_src_seek;
                src.tell = fat_src_tell;
                src.userdata = &g_fsrc;
                decoder_arena_init(&arena, arena_buf, sizeof arena_buf);
                alloc = decoder_arena_allocator(&arena);
                oc = (fmt == 1) ? mp3_open_stream(&g_dec, &src, &alloc)
                                : flac_open_stream(&g_dec, &src, &alloc);
                if (oc == 0) {
                    arate = g_dec.sample_rate;
                    achan = g_dec.channels;
                }
                awater = (uint32_t)arena.high_water;
            }
        }

        /* If it opened at 44.1k stereo, prime the ring by DECODING and time
         * it: DTKS = ticks to decode ~1.49 s of audio (the whole ring). If
         * DTKS < ~0x95 (149 ticks = 1.49 s) the decoder beats real time at
         * this CPU clock — playback will be clean; larger means too slow
         * (a case for the 80 MHz boost). */
        int      playable = (oc == 0 && arate == 44100u && achan == 2u);
        int      started  = 0;   /* audio DMA actually running               */
        uint32_t tfus     = 0;   /* boot -> first sound, microseconds        */
        if (playable) {
            /* Already at 80 MHz (boosted up front). Prime the ring and time
             * it: DTKS = ticks to decode ~1.49 s of audio at 80 MHz, cache on.
             * The timer runs off a fixed 1 MHz source, so DTKS stays
             * comparable across CPU clocks. */
            pcm_ring_init(&g_ring, ring_storage, RING_FRAMES);
            g_eos = 0;
            uint32_t t0 = current_tick();
            decode_pump();               /* decode-prime the ring */
            dtks   = current_tick() - t0;
            awater = (uint32_t)arena.high_water;

            /* Start audio BEFORE the diagnostic present so the on-screen TFUS
             * reflects the true time-to-first-sound. hal_audio_start() primes
             * both DMA buffers and kicks channel 0, then returns (non-
             * blocking) — sound is live from here. The single BCM present that
             * follows streams in a few ms while the DMA drains the 1.49 s
             * ring, so the ring cannot underrun before the play loop below
             * resumes refilling. */
            uart_puts(fmt == 1 ? "core: playing TEST.MP3\n"
                               : "core: playing TEST.FLAC\n");
            if (hal_audio_init(44100u, 2u) == 0) {
                hal_audio_set_source(ring_source, 0);
                hal_audio_start();
                tfus    = mmio_read32(USEC_TIMER_ADDR) - boot_us0;
                started = 1;
            }
        }

        /* Boot -> here, microseconds. Sampled just before the present, so it
         * measures our whole boot + open + prime path (excludes the present
         * itself). With audio already started above, TFUS <= BTUS + a few ms. */
        uint32_t btus = mmio_read32(USEC_TIMER_ADDR) - boot_us0;

        /* One diagnostic screen (single silicon-proven present):
         *   SIG=MBR sig(AA55). MNT/OP=mount/open rc(0). OPN=flac open rc(0).
         *   RATE=sample rate(AC44). AWTR=arena bytes used(<0x10000).
         *   DTKS=decode-prime ticks — <~0x95 => real-time.
         *   BTUS=boot->screen microseconds. TFUS=boot->first-sound us.
         *   FMT=active codec: 0=FLAC, 1=MP3 (FFFFFFFF => neither opened). */
        console_clear(bg);
        console_str  (2, 3, "SIG",  CON_WHITE, bg);
        console_hex32(8, 3, sig,               CON_WHITE, bg);
        console_str  (2, 5, "MNT",  CON_WHITE, bg);
        console_hex32(8, 5, (uint32_t)mnt,     CON_WHITE, bg);
        console_str  (2, 7, "OP",   CON_WHITE, bg);
        console_hex32(8, 7, (uint32_t)op,      CON_WHITE, bg);
        console_str  (2, 9, "OPN",  CON_WHITE, bg);
        console_hex32(8, 9, (uint32_t)oc,      CON_WHITE, bg);
        console_str  (2, 11, "RATE", CON_WHITE, bg);
        console_hex32(8, 11, arate,            CON_WHITE, bg);
        console_str  (2, 13, "AWTR", CON_WHITE, bg);
        console_hex32(8, 13, awater,           CON_WHITE, bg);
        console_str  (2, 15, "DTKS", CON_WHITE, bg);
        console_hex32(8, 15, dtks,             CON_WHITE, bg);
        console_str  (2, 17, "BTUS", CON_WHITE, bg);
        console_hex32(8, 17, btus,             CON_WHITE, bg);
        console_str  (2, 19, "TFUS", CON_WHITE, bg);
        console_hex32(8, 19, tfus,             CON_WHITE, bg);
        console_str  (2, 21, "FMT",  CON_WHITE, bg);
        console_hex32(8, 21, (uint32_t)fmt,    CON_WHITE, bg);
        lcd_present_fb(console_framebuffer());
        uart_puts("core: diagnostic screen presented BTUS ");
        uart_put_hex32(btus);
        uart_puts(" TFUS ");
        uart_put_hex32(tfus);
        uart_putc('\n');

        /* Play: decode into the ring while the DMA ISR drains it. The ring is
         * already primed and audio is running; keep decoding until end-of-
         * stream AND the ring is empty. */
        if (started) {
            while (!g_eos || pcm_ring_fill(&g_ring) > 0u) {
                decode_pump();
                sleep_ms(20);
            }
            sleep_ms(200);               /* let the FIFO/DMA drain the tail */
            hal_audio_stop();
        }
        if (playable) {
            g_dec.ops->close(&g_dec);
            uart_puts("core: playback done\n");
        }
        /* Drop back to 30 MHz on EVERY exit of the boosted block (matches the
         * single cpu_boost() up front), so we never idle the core at 80 MHz. */
        cpu_unboost();
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
