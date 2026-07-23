/*
 * core/hal/hw/lcd.c — BCM video coprocessor LCD driver (PP5022,
 * full-frame solid fill).
 *
 * Implements the host-side port init and the full-frame LCD_UPDATE
 * sequence from core/docs/hw/02-lcd.md ("Host-side port init",
 * "Memory-mapped BCM interface", "LCD update protocol"), verified
 * against Rockbox lcd-video.c / ipodloader2 fb.c (2026-06-11).
 *
 * No BCM bootstrap here: the chainload handoff (02-lcd.md, "Chainload
 * handoff state") guarantees the Apple flash ROM already powered the
 * BCM and uploaded its firmware, and ipodloader2 finished its last
 * frame synchronously — the BCM arrives powered, awake and idle. We
 * use the Rockbox-BOOTLOADER update variant: point the write port at
 * the framebuffer, stream params/data, THEN wait for the previous
 * update to retire (skipped on the first frame), write the command,
 * strobe, and return without a completion wait. Waiting AFTER the
 * ~150 KB pixel stream — not before it — matches Rockbox's ordering
 * (the stream is the completion delay) and is what lets repeated
 * presents work: polling right after the previous strobe hits the BCM
 * at peak-busy and spins its whole budget (the "second present stalls"
 * bug). A re-kick re-issues the update if the BCM latches busy past a
 * bounded poll budget (analog of Rockbox's 50 ms BCM_UPDATE_TIMEOUT).
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only
 * (via pp5022.h); LCD_WIDTH/LCD_HEIGHT from the portable hal.h
 * contract.
 */

#include "pp5022.h"
#include "mmio.h"
#include "lcd.h"
#include "hal.h"
#ifndef MMIO_MOCK
#include "../../kernel/irq.h"   /* arch_irq_save/restore: present is a critical
                                 * section (real build only; the host trace
                                 * tests compile this file with -DMMIO_MOCK and
                                 * the ARM CPSR asm can't build there). */
#define LCD_IRQ_ENTER()  uint32_t lcd_irq_saved_ = arch_irq_save()
#define LCD_IRQ_EXIT()   arch_irq_restore(lcd_irq_saved_)
#else
#define LCD_IRQ_ENTER()  ((void)0)
#define LCD_IRQ_EXIT()   ((void)0)
#endif

/*
 * Upper bounds on the BCM handshake polls so a wedged or absent BCM
 * can't hang the kernel (same degrade-gracefully pattern as uart.c's
 * TX spin). The write/read handshakes normally complete in a few bus
 * cycles, so 1<<16 trips is orders of magnitude past "working
 * hardware". On timeout the access is attempted anyway: worst case
 * the frame is corrupt or dropped, the firmware keeps running and the
 * UART narration tells us what happened.
 */
#define BCM_SPIN_LIMIT       (1u << 13)   /* ~8k; a healthy handshake takes a few
                                          * cycles, so this is still orders past
                                          * working hw but bounds a wedged BCM to
                                          * sub-ms instead of the ~1ms of 1<<16. */

/*
 * Upper bound on the wait-for-idle poll before issuing a new frame.
 * Each trip is a full bcm_read32 handshake (several bus accesses), and
 * Rockbox's own re-kick timeout for a stalled update is 50 ms, so
 * 1<<16 read attempts is far beyond any healthy update. On timeout we
 * just proceed — issuing over a busy BCM beats hanging.
 */
#define BCM_IDLE_SPIN_LIMIT  (1u << 9)    /* Bound the wait-for-idle OUTER loop:
                                          * each trip is a bcm_read32 (itself
                                          * bounded), so this caps a wedged BCM to
                                          * ~tens of ms and issues the update
                                          * anyway, instead of the minutes-long
                                          * nested spin (65536 x 65536) that hard-
                                          * froze the device on a present after a
                                          * scroll wedged the BCM. Healthy path:
                                          * the pixel stream already retired the
                                          * previous update, so idle reads on the
                                          * first trip. */

/*
 * Poll-count budget before we re-kick a BCM that is still reading the
 * busy pattern. Rockbox re-issues LCD_UPDATE + strobe if an update
 * hasn't retired within HZ/20 (~50 ms) to unstick a latched-busy BCM;
 * lacking a tick clock here we approximate that timeout as a fraction
 * of the idle-spin budget. In the healthy case the ~150 KB pixel
 * stream has already given the previous update ample time to retire, so
 * the idle poll returns on its first read and this budget is never hit.
 */
#define BCM_REKICK_TRIPS     (BCM_IDLE_SPIN_LIMIT >> 4)

/* First-frame flag: the chainload handoff guarantees an idle BCM, so
 * the initial lcd_fill skips the wait-for-idle read entirely. */
static int lcd_first_frame = 1;

/* Set the BCM-internal write destination: 32-bit address store to the
 * write-address port, then poll write-ready (02-lcd.md, write
 * handshake; verified against Rockbox lcd-video.c, 2026-06-11). */
static void bcm_write_addr(uint32_t addr)
{
    uint32_t spin = BCM_SPIN_LIMIT;

    mmio_write32(BCM_WR_ADDR_ADDR, addr);
    while (!(mmio_read16(BCM_CONTROL_ADDR) & BCM_CONTROL_WR_READY) &&
           --spin != 0) {
        /* poll */
    }
}

/* Write one 32-bit word to a BCM-internal address. */
static void bcm_write32(uint32_t addr, uint32_t value)
{
    bcm_write_addr(addr);
    mmio_write32(BCM_DATA_ADDR, value);
}

/* Read one 32-bit word from a BCM-internal address. Handshake order
 * matters (02-lcd.md, read handshake; verified against Rockbox
 * lcd-video.c, 2026-06-11): poll the read port ready FIRST, then
 * write the address, then poll data-ready, then read the data port. */
static uint32_t bcm_read32(uint32_t addr)
{
    uint32_t spin = BCM_SPIN_LIMIT;

    while (!(mmio_read16(BCM_RD_ADDR_ADDR) & BCM_RD_ADDR_READY) &&
           --spin != 0) {
        /* poll */
    }
    mmio_write32(BCM_RD_ADDR_ADDR, addr);

    spin = BCM_SPIN_LIMIT;
    while (!(mmio_read16(BCM_CONTROL_ADDR) & BCM_CONTROL_RD_READY) &&
           --spin != 0) {
        /* poll */
    }
    return mmio_read32(BCM_DATA_ADDR);
}

int lcd_init(void)
{
    /* Host-side port init (02-lcd.md, "Host-side port init"; from
     * Rockbox lcd_init_device, verified 2026-06-11 — runs even when
     * the BCM is already alive): BCM power rail + companion bit as
     * GPO, GPIOC bit 6 = BCM interrupt pin as a GPIO input, bit 7
     * released to its alternate function, GPO32 bit 0 released. */
    mmio_write32(GPO32_ENABLE_ADDR, mmio_read32(GPO32_ENABLE_ADDR) | 0xC000);
    mmio_write32(GPIOC_ENABLE_ADDR, mmio_read32(GPIOC_ENABLE_ADDR) & ~0x80);
    mmio_write32(GPIOC_ENABLE_ADDR, mmio_read32(GPIOC_ENABLE_ADDR) | 0x40);
    mmio_write32(GPIOC_OUTPUT_EN_ADDR,
                 mmio_read32(GPIOC_OUTPUT_EN_ADDR) & ~0x40);
    mmio_write32(GPO32_ENABLE_ADDR, mmio_read32(GPO32_ENABLE_ADDR) & ~1u);

    /* Probe: nonzero means the BCM is powered (and, post-chainload,
     * initialized). We do not bootstrap a dead BCM in this PR. */
    return (mmio_read32(GPO32_VAL_ADDR) & GPO32_BCM_POWER) != 0;
}

/* Number of 32-bit stores that carry one full 320x240 frame: two
 * RGB565 pixels are packed per store. */
#define BCM_FRAME_WORDS  ((LCD_WIDTH * LCD_HEIGHT) / 2u)

/*
 * Shared full-frame update preamble (step 2, identical for lcd_fill and
 * lcd_present): point the BCM write port at the framebuffer / command
 * parameter region so the caller can stream pixel data.
 *
 * The wait-for-idle deliberately does NOT live here. Polling right
 * after the previous frame's strobe — before any pixels stream — hits
 * the BCM at peak busy and never sees idle within budget (the "second
 * present stalls" hang). It runs in bcm_frame_commit instead, after the
 * full pixel stream has given the previous update time to retire.
 */
static void bcm_frame_begin(void)
{
    bcm_write_addr(BCMA_CMDPARAM);
}

/*
 * Shared full-frame update commit (steps 1, 4-5, identical for lcd_fill
 * and lcd_present):
 *
 *  (1) Wait for the previous update to retire, unless this is the first
 *      frame after the chainload handoff (guaranteed idle). The BCM is
 *      busy while BCMA_COMMAND still reads the in-flight LCD_UPDATE code
 *      (0xFFFF0000) or its half-consumed remnant 0xFFFF (02-lcd.md,
 *      "LCD update protocol"; verified against Rockbox lcd-video.c,
 *      2026-06-11). By the time we get here the caller has streamed the
 *      whole frame, so a healthy BCM has long since gone idle and the
 *      poll returns on its first read. If it is still busy past
 *      BCM_REKICK_TRIPS polls it may have latched — re-issue the command
 *      and re-strobe to unstick it (Rockbox's 50 ms re-kick analog). On
 *      exhausting the full spin budget, proceed anyway.
 *  (4) Queue the full-frame update command.
 *  (5) Strobe execute. Bootloader variant: return without a completion
 *      wait; the next frame's pre-stream wait catches any overrun.
 */
static void bcm_frame_commit(void)
{
    if (!lcd_first_frame) {
        uint32_t spin = BCM_IDLE_SPIN_LIMIT;
        uint32_t kick = BCM_REKICK_TRIPS;
        uint32_t stat = bcm_read32(BCMA_COMMAND);

        while ((stat == BCMCMD_LCD_UPDATE || stat == 0xFFFF) &&
               --spin != 0) {
            if (--kick == 0) {
                /* Still busy far too long: the update likely latched.
                 * Re-issue LCD_UPDATE + strobe to unstick the BCM. */
                bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);
                mmio_write16(BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
                kick = BCM_REKICK_TRIPS;
            }
            stat = bcm_read32(BCMA_COMMAND);
        }
    }
    lcd_first_frame = 0;

    bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);
    mmio_write16(BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
}

/* Byte stride of one framebuffer row in BCM-internal memory: LCD_WIDTH
 * RGB565 pixels, 2 bytes each (02-lcd.md, "Internal BCM addresses":
 * pixel (x,y) at offset (LCD_WIDTH*2)*y + x*2 from BCMA_CMDPARAM). */
#define BCM_ROW_STRIDE_BYTES  ((uint32_t)LCD_WIDTH * 2u)

/*
 * Stream a contiguous run of `pixels` RGB565 values from `src` to the
 * BCM data port as packed 32-bit stores — two pixels per store, exactly
 * the packing lcd_present_fb proved on real 5.5G silicon: the earlier
 * pixel src[2k] (even, first 16-bit push) in the LOW half, the later
 * src[2k+1] (odd, second push) in the HIGH half (02-lcd.md,
 * "Memory-mapped BCM interface" — a 32-bit store is consumed as two
 * consecutive 16-bit pushes to ascending BCM addresses, low half first
 * on this little-endian bus). The caller must have already pointed the
 * write port at the run's start with bcm_write_addr, and `pixels` MUST
 * be even so the run is a whole number of 32-bit stores (guaranteed by
 * the even-width rounding in lcd_present_rect and by W*H being even for
 * a full frame). */
static void bcm_stream_pixels(const uint16_t *src, uint32_t pixels)
{
    uint32_t words = pixels >> 1;
    uint32_t k = 0;

    while (k < words) {
        const uint32_t pair = ((uint32_t)src[2u * k + 1u] << 16) |
                              src[2u * k];
        mmio_write32(BCM_DATA_ADDR, pair);
        k++;
    }
}

void lcd_fill(uint16_t rgb565)
{
    /* Solid color: both packed halves are the same pixel, so the
     * high/low ordering (see lcd_present) is irrelevant here. */
    const uint32_t pair = ((uint32_t)rgb565 << 16) | rgb565;
    uint32_t n = BCM_FRAME_WORDS;

    /* ONLY the pixel stream must be uninterrupted: an ISR stalling the push
     * mid-stream makes the BCM abort the frame. So mask just the stream — NOT
     * bcm_frame_commit()'s wait-for-idle spin, which polls the BCM and touches
     * no pixels. Masking the spin (as the code once did) let a long spin during
     * back-to-back presents starve the audio DMA ISR (glitches) and, worst case,
     * stall the tick + audio long enough to hard-freeze mid-present. The audio
     * ISR touches no BCM state, so it is safe to fire during the spin/commit. */
    LCD_IRQ_ENTER();
    bcm_frame_begin();

    /* (3) Stream the full 320x240 frame as 32-bit stores, two RGB565
     * pixels per store, no per-store handshake — the BCM's undecoded
     * low address bits consume each word as two consecutive 16-bit
     * pushes (02-lcd.md, "Memory-mapped BCM interface"; verified
     * against Rockbox lcd-video.c / ipodloader2 fb.c, 2026-06-11). */
    while (n-- != 0) {
        mmio_write32(BCM_DATA_ADDR, pair);
    }

    LCD_IRQ_EXIT();                       /* unmask before the idle-wait spin */
    bcm_frame_commit();
}

/*
 * Present a sub-rectangle of a full-frame (LCD_WIDTH x LCD_HEIGHT,
 * row-major RGB565) buffer `fb`, streaming only the rect's w*h pixels to
 * the panel instead of the whole ~150 KB frame — the win for our
 * row-based UI, which usually redraws one horizontal band.
 *
 * Mechanism (cleanroom from 02-lcd.md, "LCD update protocol" — the
 * documented Rockbox lcd_update_rect, NOT the ipodloader2
 * BCMCMD_LCD_UPDATERECT / BCM_CMD(5) header path): the BCM keeps a
 * persistent 320x240 framebuffer in its own SDRAM at BCMA_CMDPARAM. We
 * overwrite ONLY the changed pixels there — at their normal full-frame
 * stride offsets — then issue the ordinary full-frame BCMCMD_LCD_UPDATE.
 * The BCM scans out its whole framebuffer; the untouched regions keep
 * last frame's pixels. This deliberately reuses the device-proven
 * bcm_frame_commit() handshake verbatim (idle-wait AFTER the stream,
 * re-kick, LCD_UPDATE, 0x31 strobe) — the ONLY things that differ from
 * lcd_present_fb are the write-port start offset and, for a
 * narrower-than-full rect, a per-row re-address to skip the gap between
 * rows. We chose this over the UPDATERECT command because it (a) reuses
 * the crown-jewel commit unchanged and (b) makes a full-frame rect
 * byte-identical to the proven path, so lcd_present_fb can delegate.
 *
 * A fully out-of-bounds or zero-area rect is a safe no-op. x and width
 * are rounded to even (BCM bus alignment: pixels stream two-per-32-bit-
 * store, so a row must start on an even column and span an even count;
 * 02-lcd.md, "LCD update protocol / Constraints" — x down, width up to
 * still cover the requested region).
 */
void lcd_present_rect(const uint16_t *fb, int x, int y, int w, int h)
{
    /* Validate + clamp to the panel. Trim a partially off-screen rect;
     * bail on anything with no on-screen area. */
    if (w <= 0 || h <= 0) {
        return;
    }
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || x + w <= 0 || y + h <= 0) {
        return;
    }
    if (x < 0) { w += x; x = 0; }           /* trim off-screen-left  */
    if (y < 0) { h += y; y = 0; }           /* trim off-screen-above */
    if (x + w > LCD_WIDTH)  { w = LCD_WIDTH  - x; }
    if (y + h > LCD_HEIGHT) { h = LCD_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    /* Even alignment (BCM streams two pixels per 32-bit store): snap the
     * left edge down to an even column and the right edge up to an even
     * column, then take the width between them — so the aligned window
     * still fully covers the requested [x, x+w). Both endpoints even ->
     * width even. */
    int left  = x & ~1;                     /* floor x to even            */
    int right = (x + w + 1) & ~1;           /* ceil (x+w) to even         */
    x = left;
    w = right - left;
    if (x + w > LCD_WIDTH) { w = LCD_WIDTH - x; }
    if (w <= 0 || h <= 0) {
        return;
    }

    /* Mask ONLY the pixel stream (see lcd_fill's note); the wait-for-idle in
     * bcm_frame_commit runs unmasked so the audio DMA ISR can preempt it. */
    LCD_IRQ_ENTER();

    if (w == LCD_WIDTH) {
        /* Full-width band: the rect's rows are contiguous in BCM memory
         * (no inter-row gap), so a single write-addr + one contiguous
         * stream covers them — exactly like lcd_present_fb. For a full
         * frame (x=0,y=0,w=W,h=H) the offset is 0 and the pixel count is
         * W*H, so this reproduces the proven path byte-for-byte. */
        bcm_write_addr(BCMA_CMDPARAM + (uint32_t)y * BCM_ROW_STRIDE_BYTES);
        bcm_stream_pixels(fb + (uint32_t)y * LCD_WIDTH,
                          (uint32_t)w * (uint32_t)h);
    } else {
        /* Narrower rect: each destination row is separated by a
         * full-width gap in BCM memory, so re-point the write port at the
         * start of every row before streaming that row's w pixels. Source
         * stride is the full framebuffer width; dest stride is one BCM
         * row (02-lcd.md, "LCD update protocol", partial-width branch). */
        for (int r = 0; r < h; r++) {
            uint32_t row = (uint32_t)(y + r);
            bcm_write_addr(BCMA_CMDPARAM + row * BCM_ROW_STRIDE_BYTES +
                           (uint32_t)x * 2u);
            bcm_stream_pixels(fb + row * (uint32_t)LCD_WIDTH + (uint32_t)x,
                              (uint32_t)w);
        }
    }

    LCD_IRQ_EXIT();                       /* unmask before the idle-wait spin */
    bcm_frame_commit();
}

void lcd_present_fb(const uint16_t *fb)
{
    /* A full-frame present is just the whole-panel rectangle. This
     * delegation is byte-identical to the former hand-written full-frame
     * loop: lcd_present_rect's w==LCD_WIDTH branch with x=y=0 points the
     * write port at BCMA_CMDPARAM, streams exactly BCM_FRAME_WORDS packed
     * words in the same low=even/high=odd order, and commits through the
     * same bcm_frame_commit() — verified by the hw-lcd-present /
     * hw-lcd-trace golden traces, which are unchanged. */
    lcd_present_rect(fb, 0, 0, LCD_WIDTH, LCD_HEIGHT);
}

/* ---- Panel sleep/wake (suspend) -------------------------------------------
 *
 * EXPERIMENTAL. Rockbox's iPod-Video panel-off also POWER-GATES the BCM
 * (GPO32 &= ~0x4000) and re-bootstraps + re-uploads its firmware on wake — code
 * we don't have (we rely on the boot handoff for BCM bringup). So this is the
 * RECOVERABLE subset: clear the panel-enable bits and issue the LCD_SLEEP
 * command, but leave the BCM powered and bootstrapped. lcd_wake() restores the
 * bits; the caller then presents a frame to re-light + repaint. If the panel
 * doesn't come back, the BCM is still alive so a present (or worst case a
 * reset) recovers — nothing is permanently off.
 *
 * Register/command facts (Rockbox iPod-Video bcm_powerdown): panel-enable bits
 * 0xF0 in BCM reg 0x10001400; LCD_SLEEP is BCM command 8. */
#define BCM_PANEL_CTL_ADDR    0x10001400u
#define BCM_PANEL_CTL_ENABLE  0x000000F0u
#define BCMCMD_LCD_SLEEP      BCM_CMD(8)

void lcd_sleep(void)
{
    LCD_IRQ_ENTER();
    bcm_write32(BCM_PANEL_CTL_ADDR,
                bcm_read32(BCM_PANEL_CTL_ADDR) & ~BCM_PANEL_CTL_ENABLE);
    bcm_write32(BCMA_COMMAND, BCMCMD_LCD_SLEEP);
    mmio_write16(BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
    LCD_IRQ_EXIT();
}

void lcd_wake(void)
{
    LCD_IRQ_ENTER();
    bcm_write32(BCM_PANEL_CTL_ADDR,
                bcm_read32(BCM_PANEL_CTL_ADDR) | BCM_PANEL_CTL_ENABLE);
    LCD_IRQ_EXIT();
}
