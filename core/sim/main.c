/*
 * core/sim/main.c — entry point for the host simulator (core-sim).
 *
 * Interactive mode: HAL + audio engine + Cabinet UI run a normal
 * event loop until the user closes the window or hits q/Esc.
 *
 * Headless capture mode (`--shot <path>`): run a fixed sequence,
 * capture the framebuffer to BMP, exit. Optional `--press <keys>`
 * fires synthetic button events before the loop starts (each char
 * is one event: D=down, U=up, L=left, R=right, E=enter/select,
 * M=menu, P=play). Optional `--frames N` runs N frames after the
 * presses to let layout settle / scrubber advance.
 */

#include "../apps/audio/engine.h"
#include "../apps/ui/cabinet.h"
#include "../hal/hal.h"

#include <stdlib.h>
#include <string.h>

static button_t key_to_button(char k) {
    switch (k) {
        case 'D': return BUTTON_SCROLL_FWD;
        case 'U': return BUTTON_SCROLL_BACK;
        case 'L': return BUTTON_LEFT;
        case 'R': return BUTTON_RIGHT;
        case 'E': return BUTTON_SELECT;
        case 'M': return BUTTON_MENU;
        case 'P': return BUTTON_PLAY;
        default:  return BUTTON_NONE;
    }
}

int main(int argc, char **argv) {
    const char *shot_path = NULL;
    const char *press_seq = NULL;
    int shot_frames = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            press_seq = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            shot_frames = atoi(argv[++i]);
            if (shot_frames < 1) shot_frames = 1;
        }
    }

    if (hal_init() != 0) return EXIT_FAILURE;

    static audio_engine_t engine;
    audio_engine_init(&engine);

    static cabinet_t cabinet;
    cabinet_init(&cabinet, &engine);

    log_printf("core-sim starting; q/Esc exits%s",
               shot_path ? " (--shot mode)" : "");

    if (shot_path) {
        /* Pre-fire any synthetic key presses. */
        if (press_seq) {
            for (const char *k = press_seq; *k; k++) {
                button_t b = key_to_button(*k);
                if (b != BUTTON_NONE) cabinet_handle_button(&cabinet, b);
            }
        }
        /* Run the configured number of frames, pumping audio so the
         * scrubber advances if a track is playing. */
        for (int i = 0; i < shot_frames; i++) {
            audio_engine_pump(&engine);
            cabinet_draw(&cabinet);
            lcd_present();
            sleep_ms(16);   /* let the audio thread make progress */
        }
        int rc = lcd_screenshot_bmp(shot_path);
        log_printf("screenshot %s -> %s", shot_path, rc == 0 ? "ok" : "FAIL");
        audio_engine_close(&engine);
        hal_shutdown();
        return rc == 0 ? 0 : 1;
    }

    bool running = true;
    while (running) {
        audio_engine_pump(&engine);
        /* No auto-stop on EOS — the Now Playing screen keeps showing
         * the track at 100% until the user manually MENUs back. */

        cabinet_draw(&cabinet);
        lcd_present();

        button_t b = button_get(16);
        if (b == BUTTON_QUIT) {
            running = false;
        } else if (b != BUTTON_NONE) {
            cabinet_handle_button(&cabinet, b);
        }
    }

    audio_engine_close(&engine);
    hal_shutdown();
    return EXIT_SUCCESS;
}
