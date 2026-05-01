/*
 * core/sim/main.c — entry point for the host simulator (core-sim).
 *
 * Initializes the HAL, draws a test pattern, and runs an event loop.
 * Press SPACE to toggle a 440 Hz sine wave through the audio HAL —
 * a smoke test that the audio pipeline is wired end-to-end. Real
 * Cabinet UI + decoder integration lands in later PRs.
 */

#include "../hal/hal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Linen palette colors (subset). Hex codes from the original mockups. */
static lcd_pixel_t g_ink, g_cream, g_accent;

/* ---------- Audio: sine-wave demo source --------------------------- */

#define DEMO_SAMPLE_RATE 44100
#define DEMO_CHANNELS    2
#define DEMO_FREQ_HZ     440.0f
#define DEMO_AMPLITUDE   8000     /* ~25% scale; quieter than fixture */

typedef struct {
    /* Phase accumulator in turns (0..1), to keep precision and avoid
     * the 'sin(big number)' libm slowdown. */
    float phase;
    float phase_inc;       /* turns per frame */
} sine_demo_t;

static int sine_source_cb(void *user, int16_t *buf, int frames) {
    sine_demo_t *s = (sine_demo_t *)user;
    for (int i = 0; i < frames; i++) {
        float v = sinf(2.0f * (float)M_PI * s->phase);
        int16_t sample = (int16_t)(DEMO_AMPLITUDE * v);
        for (int c = 0; c < DEMO_CHANNELS; c++) {
            buf[i * DEMO_CHANNELS + c] = sample;
        }
        s->phase += s->phase_inc;
        if (s->phase >= 1.0f) s->phase -= 1.0f;
    }
    return frames;
}

/* ---------- LCD: test pattern -------------------------------------- */

static void draw_test_pattern(uint32_t frame, bool audio_on) {
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

    /* "Selector" rectangle — color signals audio on/off so the demo is
     * visible without sound (helpful when running headless). */
    lcd_pixel_t sel = audio_on ? g_accent : g_ink;
    for (int y = 140; y < 165; y++) {
        for (int x = 16; x < LCD_WIDTH - 16; x++) {
            int cy = (y == 140 || y == 164) ? 1 : 0;
            int cx = (x < 18 || x >= LCD_WIDTH - 18) ? 1 : 0;
            if (cy && cx) continue;
            fb[y * LCD_WIDTH + x] = sel;
        }
    }
}

/* ---------- main --------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (hal_init() != 0) {
        return EXIT_FAILURE;
    }

    g_ink    = lcd_rgb(0x1A, 0x17, 0x14);
    g_cream  = lcd_rgb(0xF4, 0xF1, 0xEC);
    g_accent = lcd_rgb(0xC4, 0x5A, 0x3A);

    /* Set up the audio sine generator but don't start it yet. */
    sine_demo_t sine = {
        .phase     = 0.0f,
        .phase_inc = DEMO_FREQ_HZ / (float)DEMO_SAMPLE_RATE,
    };

    bool audio_ok = false;
    if (hal_audio_init(DEMO_SAMPLE_RATE, DEMO_CHANNELS) != 0) {
        log_printf("hal_audio_init failed; sim runs without audio");
    } else {
        hal_audio_set_source(sine_source_cb, &sine);
        audio_ok = true;
    }

    log_printf("core-sim starting; q/Esc exits, SPACE toggles 440 Hz sine");

    uint32_t frame = 0;
    bool running   = true;
    bool audio_on  = false;
    while (running) {
        draw_test_pattern(frame, audio_on);
        lcd_present();

        button_t b = button_get(16);
        switch (b) {
            case BUTTON_QUIT:
                running = false;
                break;
            case BUTTON_PLAY:
                if (!audio_ok) {
                    log_printf("frame %u: audio unavailable; SPACE ignored", frame);
                } else if (audio_on) {
                    hal_audio_stop();
                    audio_on = false;
                    log_printf("frame %u: audio off", frame);
                } else {
                    hal_audio_start();
                    audio_on = true;
                    log_printf("frame %u: audio on (440 Hz sine)", frame);
                }
                break;
            case BUTTON_SELECT:
                log_printf("frame %u: SELECT pressed", frame);
                break;
            case BUTTON_MENU:
                log_printf("frame %u: MENU pressed", frame);
                break;
            case BUTTON_SCROLL_FWD:
                log_printf("frame %u: scroll forward", frame);
                break;
            case BUTTON_SCROLL_BACK:
                log_printf("frame %u: scroll back", frame);
                break;
            case BUTTON_LEFT:
                log_printf("frame %u: prev (stub)", frame);
                break;
            case BUTTON_RIGHT:
                log_printf("frame %u: next (stub)", frame);
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
