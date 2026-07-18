/*
 * tests/hw_mmio/lcd_present_test.c — golden-trace test for
 * lcd_present_fb() in hal/hw/lcd.c, compiled host-side against the
 * recording mock bus (-DMMIO_MOCK). Mirrors lcd_trace_test.c.
 *
 * lcd_present_fb streams an arbitrary host framebuffer through the same
 * BCM full-frame fast path lcd_fill uses. This test proves:
 *   - the exact BCM grammar: point the write port at BCMA_CMDPARAM
 *     (write-addr + WR_READY poll), stream EXACTLY (W*H)/2 = 38400
 *     32-bit data words, then the LCD_UPDATE command (0xFFFF0000) and
 *     the 0x31 strobe;
 *   - the two-pixel packing: each 32-bit word carries two consecutive
 *     framebuffer pixels with fb[2*i] (even x, first push) in the LOW
 *     half and fb[2*i+1] (odd x, second push) in the HIGH half — the
 *     ordering that collapses to lcd_fill's (rgb<<16)|rgb for a solid
 *     frame. A distinct per-pixel pattern (fb[i] = i & 0xFFFF) makes
 *     the two halves and their order observable.
 *
 * lcd_first_frame is a file-static shared with lcd_fill; this binary
 * calls lcd_present_fb first, so the first case hits the first-frame
 * (no wait-for-idle) path, exactly like lcd_trace_test orders its
 * fill cases.
 */

#include "pp5022.h"
#include "hal.h"
#include "lcd.h"
#include "mmio_mock.h"
#include "trace_expect.h"

#define FRAME_PIXELS  (LCD_WIDTH * LCD_HEIGHT)
#define FRAME_WORDS   ((LCD_WIDTH * LCD_HEIGHT) / 2u)   /* 38400 */

/* Large enough to keep off the stack (153,600 bytes). */
static uint16_t g_fb[FRAME_PIXELS];

/* Distinct per-pixel pattern so the packing/half-order is observable:
 * every pixel differs from its neighbour and from the other half. */
static void fill_pattern(void)
{
    for (unsigned i = 0; i < FRAME_PIXELS; i++) {
        g_fb[i] = (uint16_t)(i & 0xFFFF);
    }
}

/* The word lcd_present_fb must emit for pixel-pair i: high half = odd
 * pixel fb[2*i+1], low half = even pixel fb[2*i]. */
static uint32_t expected_pair(unsigned i)
{
    return ((uint32_t)g_fb[2u * i + 1u] << 16) | g_fb[2u * i];
}

/* Expect one bcm_write_addr: 32-bit address store then a single
 * WR_READY poll (BCM_CONTROL programmed ready). */
static void expect_write_addr(trace_cursor *tc, uint32_t addr)
{
    expect_w(tc, 32, BCM_WR_ADDR_ADDR, addr);
    expect_r(tc, 16, BCM_CONTROL_ADDR);
}

/* First frame after handoff: skips wait-for-idle, streams the frame. */
static int test_lcd_present_first_frame(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(BCM_CONTROL_ADDR, BCM_CONTROL_WR_READY);

    fill_pattern();
    lcd_present_fb(g_fb);

    /* Golden trace: this asserts the exact grammar AND the exact count
     * (trace_expect_end fails on any missing/extra event). */
    trace_cursor tc = trace_begin("lcd_present_first");
    expect_write_addr(&tc, BCMA_CMDPARAM);
    for (unsigned i = 0; i < FRAME_WORDS; i++) {
        expect_w(&tc, 32, BCM_DATA_ADDR, expected_pair(i));
    }
    expect_write_addr(&tc, BCMA_COMMAND);
    expect_w(&tc, 32, BCM_DATA_ADDR, BCMCMD_LCD_UPDATE);      /* 0xFFFF0000 */
    expect_w(&tc, 16, BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);  /* 0x31 */
    trace_expect_end(&tc);

    int fails = trace_done(&tc);

    /* Explicit exact-count check: writes to BCM_DATA are the 38400
     * pixel words plus the single BCMA_COMMAND word (bcm_write32). */
    size_t data_writes = mmio_mock_count(MMIO_OP_WRITE, BCM_DATA_ADDR);
    size_t pixel_writes = data_writes - 1u;   /* minus the command word */
    if (pixel_writes != FRAME_WORDS) {
        fprintf(stderr,
                "[lcd_present_first] FAIL: %zu pixel data writes, "
                "expected %u\n", pixel_writes, FRAME_WORDS);
        fails++;
    } else {
        printf("[lcd_present_first] pixel data writes = %zu (== %u)\n",
               pixel_writes, FRAME_WORDS);
    }

    /* Explicit packing check on the very first streamed word: it must
     * be (fb[1] << 16) | fb[0] = 0x00010000 for this pattern — proving
     * fb[0] (even) landed in the LOW half and fb[1] (odd) in the HIGH
     * half, matching how lcd_fill would place two identical pixels. */
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    uint32_t first_data = 0;
    int found = 0;
    for (size_t k = 0; k < len; k++) {
        if (log[k].op == MMIO_OP_WRITE && log[k].width == 32 &&
            log[k].addr == BCM_DATA_ADDR) {
            first_data = log[k].value;
            found = 1;
            break;
        }
    }
    uint32_t want = ((uint32_t)g_fb[1] << 16) | g_fb[0];   /* 0x00010000 */
    if (!found || first_data != want) {
        fprintf(stderr,
                "[lcd_present_first] FAIL: first data word = %08X, "
                "expected %08X (fb[1]<<16 | fb[0])\n",
                found ? first_data : 0u, want);
        fails++;
    } else {
        printf("[lcd_present_first] first word %08X = fb[1]<<16 | fb[0] "
               "(low=even, high=odd)\n", first_data);
    }

    return fails;
}

/* Non-first frame: wait-for-idle polls BCMA_COMMAND via bcm_read32
 * until it reads idle (programmed busy, busy-remnant, then idle), then
 * the identical stream follows. */
static int test_lcd_present_subsequent(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(BCM_CONTROL_ADDR,
                       BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY);
    mmio_mock_set_read(BCM_RD_ADDR_ADDR, BCM_RD_ADDR_READY);
    const uint32_t idle_seq[] = { BCMCMD_LCD_UPDATE, 0xFFFF, 0x00000000 };
    mmio_mock_queue_read(BCM_DATA_ADDR, idle_seq, 3);

    fill_pattern();
    lcd_present_fb(g_fb);

    trace_cursor tc = trace_begin("lcd_present_subsequent");
    /* three wait-for-idle bcm_read32(BCMA_COMMAND) iterations */
    for (int it = 0; it < 3; it++) {
        expect_r(&tc, 16, BCM_RD_ADDR_ADDR);           /* RD_ADDR_READY poll */
        expect_w(&tc, 32, BCM_RD_ADDR_ADDR, BCMA_COMMAND);
        expect_r(&tc, 16, BCM_CONTROL_ADDR);           /* RD_READY poll */
        expect_r(&tc, 32, BCM_DATA_ADDR);              /* status word */
    }
    /* then the identical stream as the first-frame path */
    expect_write_addr(&tc, BCMA_CMDPARAM);
    for (unsigned i = 0; i < FRAME_WORDS; i++) {
        expect_w(&tc, 32, BCM_DATA_ADDR, expected_pair(i));
    }
    expect_write_addr(&tc, BCMA_COMMAND);
    expect_w(&tc, 32, BCM_DATA_ADDR, BCMCMD_LCD_UPDATE);
    expect_w(&tc, 16, BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    /* order matters: the first present flips the file-static
     * lcd_first_frame flag, so the first-frame case must run first. */
    fails += test_lcd_present_first_frame();
    fails += test_lcd_present_subsequent();
    return fails == 0 ? 0 : 1;
}
