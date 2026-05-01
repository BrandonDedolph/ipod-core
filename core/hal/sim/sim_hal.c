/*
 * core/hal/sim/sim_hal.c — SDL2-backed HAL implementation for host builds.
 *
 * Maps:
 *   LCD              -> SDL_Window (320x240) with an SDL_Texture back buffer
 *   Click wheel + buttons -> keyboard:
 *      Up/Down       = scroll back / forward
 *      Left/Right    = previous / next track
 *      Enter         = select (center)
 *      Esc           = menu (back)
 *      Space         = play/pause
 *      Shift+arrows  = TODO: fast scroll (not yet wired)
 *   Audio            -> SDL2 audio device, callback-driven (16-bit s16le)
 *   ATA              -> TODO (file-backed disk image; not in this scaffold)
 *
 * The window is upscaled by SCALE so 320x240 doesn't look tiny on a
 * modern display.
 */

#define _DEFAULT_SOURCE   /* clock_gettime, struct timespec on glibc */

#include "../hal.h"

#include <SDL2/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SCALE 2  /* render the 320x240 framebuffer scaled 2x */

static SDL_Window   *g_window   = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture  *g_texture  = NULL;
static lcd_pixel_t   g_framebuffer[LCD_WIDTH * LCD_HEIGHT];

static struct timespec g_start_ts;

/* ---------- init / shutdown ---------------------------------------- */

int hal_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    g_window = SDL_CreateWindow(
        "iPod Video (sim)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LCD_WIDTH * SCALE, LCD_HEIGHT * SCALE,
        SDL_WINDOW_SHOWN);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return -2;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return -3;
    }

    g_texture = SDL_CreateTexture(
        g_renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        LCD_WIDTH, LCD_HEIGHT);
    if (!g_texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return -4;
    }

    memset(g_framebuffer, 0, sizeof(g_framebuffer));
    clock_gettime(CLOCK_MONOTONIC, &g_start_ts);

    log_printf("hal/sim: initialized (window %dx%d, scale %d)",
               LCD_WIDTH, LCD_HEIGHT, SCALE);
    return 0;
}

void hal_shutdown(void) {
    hal_audio_close();
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
}

/* ---------- LCD ---------------------------------------------------- */

lcd_pixel_t *lcd_framebuffer(void) {
    return g_framebuffer;
}

void lcd_fill(lcd_pixel_t color) {
    /* Could SIMD this, but the sim isn't perf-critical. */
    for (size_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        g_framebuffer[i] = color;
    }
}

void lcd_present(void) {
    SDL_UpdateTexture(g_texture, NULL, g_framebuffer,
                      LCD_WIDTH * sizeof(lcd_pixel_t));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

/* ---------- Buttons (keyboard mapping) ----------------------------- */

static button_t key_to_button(SDL_Keycode k) {
    switch (k) {
        case SDLK_UP:     return BUTTON_SCROLL_BACK;
        case SDLK_DOWN:   return BUTTON_SCROLL_FWD;
        case SDLK_LEFT:   return BUTTON_LEFT;
        case SDLK_RIGHT:  return BUTTON_RIGHT;
        case SDLK_RETURN: return BUTTON_SELECT;
        case SDLK_ESCAPE: return BUTTON_MENU;
        case SDLK_SPACE:  return BUTTON_PLAY;
        default:          return BUTTON_NONE;
    }
}

button_t button_get(int timeout_ms) {
    SDL_Event ev;
    int rc;

    if (timeout_ms < 0) {
        rc = SDL_WaitEvent(&ev);
    } else if (timeout_ms == 0) {
        rc = SDL_PollEvent(&ev);
    } else {
        rc = SDL_WaitEventTimeout(&ev, timeout_ms);
    }
    if (rc == 0) return BUTTON_NONE;

    switch (ev.type) {
        case SDL_QUIT:
            return BUTTON_QUIT;
        case SDL_KEYDOWN:
            /* Quick exit on Q so you don't have to chase the close button. */
            if (ev.key.keysym.sym == SDLK_q) return BUTTON_QUIT;
            return key_to_button(ev.key.keysym.sym);
        default:
            return BUTTON_NONE;
    }
}

/* ---------- Clock --------------------------------------------------- */

uint32_t clock_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t delta_ms =
        ((uint64_t)(now.tv_sec  - g_start_ts.tv_sec)) * 1000u +
        ((uint64_t)(now.tv_nsec - g_start_ts.tv_nsec)) / 1000000u;
    return (uint32_t)delta_ms;
}

uint32_t clock_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t delta_us =
        ((uint64_t)(now.tv_sec  - g_start_ts.tv_sec)) * 1000000u +
        ((uint64_t)(now.tv_nsec - g_start_ts.tv_nsec)) / 1000u;
    return (uint32_t)delta_us;
}

void sleep_ms(uint32_t ms) {
    SDL_Delay(ms);
}

/* ---------- Audio (SDL2 callback) ---------------------------------- */

static SDL_AudioDeviceID g_audio_dev   = 0;
static SDL_AudioSpec     g_audio_spec_active;
static audio_source_fn   g_audio_src   = NULL;
static void             *g_audio_user  = NULL;

/*
 * SDL2 calls this on its own audio thread. We translate to the HAL's
 * source callback (samples in s16 frames, not bytes), pad the rest
 * with silence on underrun.
 */
static void sdl_audio_cb(void *user, Uint8 *stream, int len_bytes) {
    (void)user;
    int16_t *out = (int16_t *)stream;
    int channels = g_audio_spec_active.channels;
    int frames   = (len_bytes / 2) / channels;

    int got = 0;
    if (g_audio_src) {
        got = g_audio_src(g_audio_user, out, frames);
        if (got < 0) got = 0;
        if (got > frames) got = frames;
    }

    /* Zero the tail on underrun so we don't loop stale samples. */
    if (got < frames) {
        memset(out + got * channels, 0,
               (size_t)(frames - got) * channels * sizeof(int16_t));
    }
}

int hal_audio_init(uint32_t sample_rate, uint16_t channels) {
    if (channels != 1 && channels != 2) return -1;

    /* Idempotent: same params = no-op; different = re-open. */
    if (g_audio_dev != 0
        && (uint32_t)g_audio_spec_active.freq == sample_rate
        && g_audio_spec_active.channels       == channels) {
        return 0;
    }
    if (g_audio_dev != 0) {
        SDL_PauseAudioDevice(g_audio_dev, 1);
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }

    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            log_printf("hal/sim: SDL_InitSubSystem(audio): %s",
                       SDL_GetError());
            return -2;
        }
    }

    SDL_AudioSpec want = {
        .freq     = (int)sample_rate,
        .format   = AUDIO_S16SYS,
        .channels = (Uint8)channels,
        .samples  = 1024,        /* ~23 ms at 44.1 kHz; balances latency vs callback frequency */
        .callback = sdl_audio_cb,
        .userdata = NULL,
    };
    SDL_AudioSpec got;
    /* Pass `0` for allowed-changes — SDL will fail to open if it
     * can't match exactly. We refuse silent resampling because the
     * audio engine will produce wrong-pitch output if the rates
     * differ; better to fail loud and let the engine retry at the
     * device's preferred rate. */
    g_audio_dev = SDL_OpenAudioDevice(
        NULL, /* default output */ 0, &want, &got, 0);
    if (g_audio_dev == 0) {
        log_printf("hal/sim: SDL_OpenAudioDevice: %s", SDL_GetError());
        return -3;
    }

    /* Belt-and-suspenders: even with allowed_changes=0, validate
     * what SDL gave us. */
    if (got.freq != want.freq || got.channels != want.channels
        || got.format != want.format) {
        log_printf("hal/sim: SDL gave %d Hz / %d ch / fmt=0x%04x; wanted %d / %d / 0x%04x",
                   got.freq, got.channels, got.format,
                   want.freq, want.channels, want.format);
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
        return -4;
    }

    g_audio_spec_active = got;
    log_printf("hal/sim: audio open (%d Hz, %d ch, fmt=0x%04x, samples=%d)",
               got.freq, got.channels, got.format, got.samples);
    return 0;
}

void hal_audio_set_source(audio_source_fn fn, void *userdata) {
    /* Lock so we don't tear during a concurrent callback. */
    if (g_audio_dev != 0) SDL_LockAudioDevice(g_audio_dev);
    g_audio_src  = fn;
    g_audio_user = userdata;
    if (g_audio_dev != 0) SDL_UnlockAudioDevice(g_audio_dev);
}

void hal_audio_start(void) {
    if (g_audio_dev != 0) SDL_PauseAudioDevice(g_audio_dev, 0);
}

void hal_audio_stop(void) {
    if (g_audio_dev != 0) SDL_PauseAudioDevice(g_audio_dev, 1);
}

void hal_audio_close(void) {
    /* Lock so we don't race with a final in-flight callback. After
     * SDL_CloseAudioDevice returns, no more callbacks fire — but
     * one could be in flight when we enter this function. */
    if (g_audio_dev != 0) {
        SDL_LockAudioDevice(g_audio_dev);
        g_audio_src  = NULL;
        g_audio_user = NULL;
        SDL_UnlockAudioDevice(g_audio_dev);

        SDL_PauseAudioDevice(g_audio_dev, 1);
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    } else {
        g_audio_src  = NULL;
        g_audio_user = NULL;
    }
}

/* ---------- Log ---------------------------------------------------- */

#ifndef CORE_RELEASE
void log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}
#endif
