/*
 * core/sim/main.c — entry point for the host simulator (core-sim).
 *
 * Boots the HAL, initializes the audio engine, hands the main loop
 * to the Cabinet shell. Cabinet draws menus, handles button events,
 * triggers playback through the engine on Now Playing → SELECT.
 *
 * Press Q or Esc (or close the window) to exit.
 */

#include "../apps/audio/engine.h"
#include "../apps/ui/cabinet.h"
#include "../hal/hal.h"

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    /* --shot <path>: render a few frames to settle layout, dump a
     * BMP, exit. Used for headless visual capture. */
    const char *shot_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
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
        /* Run a handful of frames so the first lcd_present has flushed,
         * then capture and exit. No event loop, no audio. */
        for (int i = 0; i < 4; i++) {
            cabinet_draw(&cabinet);
            lcd_present();
        }
        int rc = lcd_screenshot_bmp(shot_path);
        log_printf("screenshot %s -> %s", shot_path, rc == 0 ? "ok" : "FAIL");
        hal_shutdown();
        return rc == 0 ? 0 : 1;
    }

    bool running = true;
    while (running) {
        audio_engine_pump(&engine);
        if (audio_engine_eos(&engine)) {
            audio_engine_stop(&engine);
        }

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
