/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tools/text_preview.c — render the REAL device text stack to a PNM on the
 * host, so glyph spacing/tracking can be judged and tuned without a flash.
 *
 * This links core/ui/text.c and the generated atlases unmodified: what it
 * draws is what the panel draws, pixel for pixel, including the gamma-correct
 * blend, the 26.6 pen, kerning and tracking. That is the whole point — a
 * preview that re-implemented any of it would agree with the device only by
 * luck, which is how you end up "fixing" spacing against a mock.
 *
 * Build (host, no meson needed):
 *   cc -I core/ui -o /tmp/text_preview tools/text_preview.c core/ui/text.c
 * Usage:
 *   /tmp/text_preview out.ppm [zoom] [strings.txt]
 *
 * With a third argument the built-in samples are replaced by that file's
 * lines (UTF-8, one string per line, '#' comments skipped), so a face swap
 * can be judged on the exact metadata that lives in the user's library
 * rather than on whatever this file happened to hard-code.
 *
 * The right-hand column repeats each line at `zoom` (default 3) with nearest-
 * neighbour scaling, because the defect being tuned here is sub-pixel and is
 * invisible at 1:1 on a desktop monitor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "text.h"

#define W 320
#define H 1600                    /* tall: six faces x a strings file  */
#define BG   0xF7BE               /* Linen surface-ish */
#define INK  0x18E3               /* near-black */

static uint16_t fb[W * H];

/* Strings chosen to exercise the pairs that actually go wrong: capital-heavy
 * title case (kern pairs), round-vs-straight adjacency (AA spill), and real
 * library metadata. */
static const char *const SAMPLES[] = {
    "Taylor Swift",
    "AWOLNATION",
    "The Velvet Underground",
    "To Ta Ye LT Wa AT y. VA",
    "Illenium — Awake",
    "nnnn oooo mmmm iiii",
    "Yesterday Once More",
    "HELLO WORLD hello world",
};
#define NSAMP ((int)(sizeof SAMPLES / sizeof SAMPLES[0]))
#define MAX_LINES 64
#define MAX_LINE  128

/* Lines read from argv[3], if given; otherwise SAMPLES. */
static char  file_lines[MAX_LINES][MAX_LINE];
static const char *samples[MAX_LINES];
static int   nsamp;

static void load_samples(const char *path)
{
    nsamp = 0;
    if (!path) {
        for (int i = 0; i < NSAMP; i++) samples[nsamp++] = SAMPLES[i];
        return;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(1); }
    while (nsamp < MAX_LINES && fgets(file_lines[nsamp], MAX_LINE, fp)) {
        char *l = file_lines[nsamp];
        l[strcspn(l, "\r\n")] = 0;
        if (l[0] == 0 || l[0] == '#') continue;
        samples[nsamp] = l;
        nsamp++;
    }
    fclose(fp);
}

struct face { const char *name; const text_font_t *(*get)(void); };

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "text_preview.ppm";
    int zoom = (argc > 2) ? atoi(argv[2]) : 3;
    if (zoom < 1) zoom = 1;
    load_samples(argc > 3 ? argv[3] : NULL);

    const struct face faces[] = {
        { "regular 9",  text_font_regular_9  },
        { "regular 11", text_font_regular_11 },
        { "regular 12", text_font_regular_12 },
        { "bold 12",    text_font_bold_12    },
        { "bold 13",    text_font_bold_13    },
        { "bold 18",    text_font_bold_18    },
    };
    const int nfaces = (int)(sizeof faces / sizeof faces[0]);

    for (int i = 0; i < W * H; i++) fb[i] = BG;

    int y = 12;
    for (int f = 0; f < nfaces && y < H; f++) {
        const text_font_t *fo = faces[f].get();
        int lh = text_line_height(fo);
        /* Face label in the smallest face so it never dominates. */
        text_draw(fb, W, H, 4, y, faces[f].name, text_font_regular_9(), INK);
        y += text_line_height(text_font_regular_9()) + 2;
        for (int s = 0; s < nsamp; s++) {
            if (y + lh >= H) break;
            text_draw(fb, W, H, 6, y + text_ascent(fo), samples[s], fo, INK);
            y += lh;
        }
        y += 6;
    }
    /* y is a pen, not a bound: it can run past the buffer once the faces stop
     * fitting. Clamp before it is used as a row count. */
    if (y > H) y = H;

    /* RGB565 -> 8-bit PPM, nearest-neighbour zoomed. */
    FILE *fp = fopen(out, "wb");
    if (!fp) { perror("open"); return 1; }
    fprintf(fp, "P6\n%d %d\n255\n", W * zoom, y * zoom);
    for (int row = 0; row < y; row++) {
        for (int rz = 0; rz < zoom; rz++) {
            for (int col = 0; col < W; col++) {
                uint16_t p = fb[row * W + col];
                unsigned r = ((p >> 11) & 0x1F) * 255 / 31;
                unsigned g = ((p >> 5)  & 0x3F) * 255 / 63;
                unsigned b = ( p        & 0x1F) * 255 / 31;
                unsigned char px[3] = {
                    (unsigned char)r, (unsigned char)g, (unsigned char)b };
                for (int cz = 0; cz < zoom; cz++) fwrite(px, 1, 3, fp);
            }
        }
    }
    fclose(fp);
    fprintf(stderr, "wrote %s (%dx%d, zoom %d)\n", out, W * zoom, y * zoom, zoom);
    return 0;
}
