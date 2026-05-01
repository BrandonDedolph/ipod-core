/*
 * core/sim/main.c — entry point for the host simulator (core-sim).
 *
 * Initializes the HAL, draws a test pattern, and runs an event loop.
 * Real Cabinet UI lands in a later PR; this is the "sim is wired up
 * end-to-end" smoke test for phase 1.
 */

#include "../hal/hal.h"

#include <stdint.h>
#include <stdlib.h>

/* Linen palette colors (subset). Hex codes from the original mockups. */
static const lcd_pixel_t COLOR_INK     = 0;          /* set in main */
static lcd_pixel_t       g_ink, g_cream, g_accent;

static void draw_test_pattern(uint32_t frame) {
    lcd_pixel_t *fb = lcd_framebuffer();

    /* Cream background. */
    lcd_fill(g_cream);

    /* Status bar — top 16 px in ink. */
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            fb[y * LCD_WIDTH + x] = g_ink;
        }
    }

    /* Animated stripe — moves with the frame counter. */
    int stripe_x = (int)((frame * 2u) % (LCD_WIDTH - 32));
    for (int y = 60; y < 90; y++) {
        for (int x = stripe_x; x < stripe_x + 32; x++) {
            fb[y * LCD_WIDTH + x] = g_accent;
        }
    }

    /* Static "selector" rectangle — soft-rounded look approximated. */
    for (int y = 140; y < 165; y++) {
        for (int x = 16; x < LCD_WIDTH - 16; x++) {
            /* clip the four corner pixels for a faux-rounded look */
            int cy = (y == 140 || y == 164) ? 1 : 0;
            int cx = (x < 18 || x >= LCD_WIDTH - 18) ? 1 : 0;
            if (cy && cx) continue;
            fb[y * LCD_WIDTH + x] = g_ink;
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (hal_init() != 0) {
        return EXIT_FAILURE;
    }

    g_ink    = lcd_rgb(0x1A, 0x17, 0x14);   /* near-black ink */
    g_cream  = lcd_rgb(0xF4, 0xF1, 0xEC);   /* warm cream */
    g_accent = lcd_rgb(0xC4, 0x5A, 0x3A);   /* terracotta */
    (void)COLOR_INK;

    log_printf("core-sim starting; press q or Esc to exit");

    uint32_t frame = 0;
    bool running   = true;
    while (running) {
        draw_test_pattern(frame);
        lcd_present();

        /* Block up to ~16 ms — sync to the SDL vsync; gives ~60 fps. */
        button_t b = button_get(16);
        switch (b) {
            case BUTTON_QUIT:
                running = false;
                break;
            case BUTTON_SELECT:
                log_printf("frame %u: SELECT pressed", frame);
                break;
            case BUTTON_MENU:
                log_printf("frame %u: MENU pressed (back to top — stub)", frame);
                break;
            case BUTTON_SCROLL_FWD:
                log_printf("frame %u: scroll forward", frame);
                break;
            case BUTTON_SCROLL_BACK:
                log_printf("frame %u: scroll back", frame);
                break;
            case BUTTON_LEFT:
                log_printf("frame %u: prev track (stub)", frame);
                break;
            case BUTTON_RIGHT:
                log_printf("frame %u: next track (stub)", frame);
                break;
            case BUTTON_PLAY:
                log_printf("frame %u: play/pause (stub)", frame);
                break;
            default:
                break;
        }

        frame++;
    }

    log_printf("core-sim shutting down (ran %u frames, %u ms)",
               frame, clock_ms());
    hal_shutdown();
    return EXIT_SUCCESS;
}
