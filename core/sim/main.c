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
 *
 * `--music <dir>` scans `dir` (non-recursively) for .flac/.mp3 files
 * and uses them to populate Music → Songs at startup. SELECT on a
 * song row then plays that file through the audio engine. Without
 * the flag, the Songs list shows the synthetic example data.
 *
 * `--tagcache <file.tcdb>` loads a precomputed binary tagcache
 * (built by `core tagcache build <music-dir>`) instead of scanning.
 * Same UI end-state as `--music`, but the strings/art come from the
 * binary file rather than re-parsing tags on every startup. Only one
 * of `--music` / `--tagcache` may be set per invocation.
 *
 * `--capture-audio <path>` switches the SDL2 audio backend to its
 * built-in `disk` driver, which writes the raw S16LE stereo samples
 * the audio callback would have sent to the speakers into <path>.
 * Used by the audio-playback meson test to verify the full
 * decoder → engine → ring → HAL audio path end-to-end. Implies the
 * sim runs to its configured frame budget; only meaningful in
 * `--shot` mode (interactive runs would never end the capture).
 */

#include "../apps/audio/engine.h"
#include "../apps/db/tagcache.h"
#include "../apps/ui/cabinet.h"
#include "../hal/hal.h"

#include <stdio.h>
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
    const char *shot_path     = NULL;
    const char *press_seq     = NULL;
    const char *music_dir     = NULL;
    const char *tagcache_file = NULL;
    const char *capture_audio = NULL;
    int shot_frames = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            press_seq = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            shot_frames = atoi(argv[++i]);
            if (shot_frames < 1) shot_frames = 1;
        } else if (strcmp(argv[i], "--music") == 0 && i + 1 < argc) {
            music_dir = argv[++i];
        } else if (strcmp(argv[i], "--tagcache") == 0 && i + 1 < argc) {
            tagcache_file = argv[++i];
        } else if (strcmp(argv[i], "--capture-audio") == 0 && i + 1 < argc) {
            capture_audio = argv[++i];
        }
    }
    if (music_dir && tagcache_file) {
        fprintf(stderr, "core-sim: --music and --tagcache are mutually exclusive\n");
        return EXIT_FAILURE;
    }

    /* Headless capture mode: tell SDL to use the dummy video/audio
     * drivers so no window pops up and no real audio device is opened.
     * Lets `--shot` runs work in the background without stealing focus
     * or making a sound. The interactive path leaves these unset and
     * gets the normal SDL backends.
     *
     * `--capture-audio` overrides the audio half: the SDL "disk" driver
     * writes raw S16LE samples the engine produces into the named file.
     * Used by the playback meson test. Override mode (1) wins over the
     * default dummy-audio setenv since both branches use setenv flag 0
     * (don't override). */
    if (capture_audio && !shot_path) {
        /* Without --shot the program enters the interactive loop and
         * never closes the audio device, so the disk file would grow
         * unbounded and never flush cleanly. The header docstring
         * already says capture-audio "implies --shot" — enforce it. */
        fprintf(stderr, "core-sim: --capture-audio requires --shot\n");
        return EXIT_FAILURE;
    }
    if (capture_audio) {
        setenv("SDL_AUDIODRIVER", "disk", 1);
        setenv("SDL_DISKAUDIOFILE", capture_audio, 1);
    }
    if (shot_path) {
        setenv("SDL_VIDEODRIVER", "dummy", 0);
        setenv("SDL_AUDIODRIVER", "dummy", 0);
    }

    if (hal_init() != 0) return EXIT_FAILURE;

    if (music_dir) {
        int n = tagcache_library_load(music_dir);
        if (n < 0) {
            log_printf("--music %s: failed to scan", music_dir);
        } else {
            log_printf("--music %s: %d song%s loaded",
                       music_dir, n, n == 1 ? "" : "s");
        }
    } else if (tagcache_file) {
        int n = tagcache_library_load_tcdb(tagcache_file);
        if (n < 0) {
            log_printf("--tagcache %s: failed to load", tagcache_file);
        } else {
            log_printf("--tagcache %s: %d song%s loaded",
                       tagcache_file, n, n == 1 ? "" : "s");
        }
    }

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
