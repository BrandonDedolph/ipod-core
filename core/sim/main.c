/*
 * core/sim/main.c — entry point for the host simulator (core-sim).
 *
 * Initializes the HAL, draws a Linen-palette test pattern, and runs an
 * event loop. SPACE plays the FLAC fixture through the full audio
 * pipeline (file -> dr_flac -> ring -> hal_audio -> SDL2 -> speakers).
 * SPACE again pauses; pressing SPACE after EOS replays from the start.
 *
 * Real Cabinet UI port lands in a later PR; this is a smoke test that
 * the audio engine works end-to-end with real codec input.
 */

#include "../apps/audio/engine.h"
#include "../codecs/dr_flac/flac.h"
#include "../hal/hal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE_PATH "tests/codec-vectors/sine_440hz_1s_44k_s16_stereo.flac"

/* Linen palette colors (subset). Hex codes from the original mockups. */
static lcd_pixel_t g_ink, g_cream, g_accent;

/* ---------- LCD: test pattern -------------------------------------- */

static void draw_test_pattern(uint32_t frame,
                              bool audio_playing,
                              uint32_t ring_fill) {
    lcd_pixel_t *fb = lcd_framebuffer();
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

    /* Ring-fill bar — width proportional to how full the engine's
     * audio ring is. Lights up only while audio is playing. */
    int bar_w = (int)((ring_fill * (LCD_WIDTH - 32)) / AUDIO_RING_FRAMES);
    if (bar_w > LCD_WIDTH - 32) bar_w = LCD_WIDTH - 32;
    for (int y = 110; y < 120; y++) {
        for (int x = 16; x < 16 + bar_w; x++) {
            fb[y * LCD_WIDTH + x] = g_accent;
        }
    }

    /* "Selector" rectangle — color signals audio on/off. */
    lcd_pixel_t sel = audio_playing ? g_accent : g_ink;
    for (int y = 140; y < 165; y++) {
        for (int x = 16; x < LCD_WIDTH - 16; x++) {
            int cy = (y == 140 || y == 164) ? 1 : 0;
            int cx = (x < 18 || x >= LCD_WIDTH - 18) ? 1 : 0;
            if (cy && cx) continue;
            fb[y * LCD_WIDTH + x] = sel;
        }
    }
}

/* ---------- Fixture loading --------------------------------------- */

static long load_fixture(const char *path, void **out_buf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void *buf = malloc((size_t)n);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *out_buf = buf;
    return n;
}

/* ---------- main --------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (hal_init() != 0) return EXIT_FAILURE;

    g_ink    = lcd_rgb(0x1A, 0x17, 0x14);
    g_cream  = lcd_rgb(0xF4, 0xF1, 0xEC);
    g_accent = lcd_rgb(0xC4, 0x5A, 0x3A);

    /* Try to load the FLAC fixture. The sim is expected to be run
     * from core/ (either via `core build sim && ./build-sim/...` or
     * `make sim && ./build-sim/sim/core-sim`). */
    void *fixture_bytes = NULL;
    long  fixture_len   = load_fixture(FIXTURE_PATH, &fixture_bytes);
    if (fixture_len < 0) {
        log_printf("warning: %s not found; SPACE will be a no-op", FIXTURE_PATH);
    }

    /* The audio engine is ~512 KB (statically-sized ring buffer),
     * so it goes in BSS rather than on the main stack — matches what
     * the hw target's audio task will need too. */
    static audio_engine_t engine;
    audio_engine_init(&engine);

    log_printf("core-sim starting; q/Esc exits, SPACE plays/pauses %s",
               fixture_len > 0 ? FIXTURE_PATH : "(no fixture)");

    uint32_t frame = 0;
    bool running   = true;
    while (running) {
        /* Pump the engine. Cheap if the ring is full or nothing is
         * playing. */
        audio_engine_pump(&engine);

        /* If we hit EOS, stop cleanly so the next SPACE replays. */
        if (audio_engine_eos(&engine)) {
            log_printf("frame %u: EOS, stopping", frame);
            audio_engine_stop(&engine);
        }

        draw_test_pattern(frame,
                          audio_engine_is_playing(&engine),
                          audio_engine_ring_fill(&engine));
        lcd_present();

        button_t b = button_get(16);
        switch (b) {
            case BUTTON_QUIT:
                running = false;
                break;
            case BUTTON_PLAY:
                if (fixture_len <= 0) {
                    log_printf("frame %u: no fixture loaded; SPACE ignored", frame);
                    break;
                }
                if (audio_engine_is_playing(&engine)) {
                    /* Already playing → pause. */
                    audio_engine_pause(&engine);
                    log_printf("frame %u: paused", frame);
                } else if (engine.has_decoder) {
                    /* Paused mid-track → resume. */
                    audio_engine_resume(&engine);
                    log_printf("frame %u: resumed", frame);
                } else {
                    /* Stopped or fresh → play from start. */
                    int rc = audio_engine_play(&engine, flac_decoder_ops(),
                                               fixture_bytes, (size_t)fixture_len);
                    if (rc != 0) {
                        log_printf("frame %u: audio_engine_play failed: %d", frame, rc);
                    } else {
                        log_printf("frame %u: playing %u Hz / %u ch",
                                   frame, engine.sample_rate, engine.channels);
                    }
                }
                break;
            case BUTTON_SELECT:
                log_printf("frame %u: SELECT", frame);
                break;
            case BUTTON_MENU:
                log_printf("frame %u: MENU", frame);
                break;
            case BUTTON_SCROLL_FWD:
                log_printf("frame %u: scroll fwd", frame);
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
    audio_engine_close(&engine);
    free(fixture_bytes);
    hal_shutdown();
    return EXIT_SUCCESS;
}
