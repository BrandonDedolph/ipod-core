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
#define BCM_SPIN_LIMIT       (1u << 16)

/*
 * Upper bound on the wait-for-idle poll before issuing a new frame.
 * Each trip is a full bcm_read32 handshake (several bus accesses), and
 * Rockbox's own re-kick timeout for a stalled update is 50 ms, so
 * 1<<16 read attempts is far beyond any healthy update. On timeout we
 * just proceed — issuing over a busy BCM beats hanging.
 */
#define BCM_IDLE_SPIN_LIMIT  (1u << 16)

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

void lcd_fill(uint16_t rgb565)
{
    /* Solid color: both packed halves are the same pixel, so the
     * high/low ordering (see lcd_present) is irrelevant here. */
    const uint32_t pair = ((uint32_t)rgb565 << 16) | rgb565;
    uint32_t n = BCM_FRAME_WORDS;

    /* The BCM frame stream must not be interrupted: an ISR (e.g. the 46 ms
     * audio DMA completion, which flushes the cache) stalling the pixel push
     * long enough makes the BCM abort the frame — the "2nd present stalls
     * once audio is running" bug. Present with IRQs masked; the whole frame
     * is a few ms, well under the DMA buffer's 46 ms, so audio can't underrun. */
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

    bcm_frame_commit();
    LCD_IRQ_EXIT();
}

void lcd_present_fb(const uint16_t *fb)
{
    uint32_t i = 0;

    /* Uninterrupted frame stream — see lcd_fill's LCD_IRQ_ENTER note. */
    LCD_IRQ_ENTER();
    bcm_frame_begin();

    /* (3) Stream the caller's framebuffer as 32-bit stores, two RGB565
     * pixels per store — same fast path as lcd_fill, only the pixel
     * source differs.
     *
     * Packing / half order (matching lcd_fill's (rgb<<16)|rgb): the BCM
     * consumes each 32-bit store as two consecutive 16-bit pushes to
     * ascending BCM-internal framebuffer addresses (02-lcd.md,
     * "Memory-mapped BCM interface"), and on this little-endian bus the
     * store's low 16 bits are the first push. Framebuffer offset
     * ascends with x (02-lcd.md: pixel (x,y) at (LCD_WIDTH*2)*y+(x*2),
     * RGB565 little-endian), so the earlier pixel fb[2*i] (even x) is
     * the first push and MUST go in the LOW half, and the later pixel
     * fb[2*i+1] (odd x) is the second push and goes in the HIGH half.
     * For a solid frame fb[2*i]==fb[2*i+1], which collapses to exactly
     * lcd_fill's (rgb<<16)|rgb — so a constant framebuffer streamed
     * here is byte-identical to lcd_fill's output. */
    while (i < BCM_FRAME_WORDS) {
        const uint32_t pair = ((uint32_t)fb[2u * i + 1u] << 16) |
                              fb[2u * i];
        mmio_write32(BCM_DATA_ADDR, pair);
        i++;
    }

    bcm_frame_commit();
    LCD_IRQ_EXIT();
}
