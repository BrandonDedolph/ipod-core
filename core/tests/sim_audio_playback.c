/*
 * core/tests/sim_audio_playback.c — end-to-end audio playback test.
 *
 * Spawns core-sim with --capture-audio (which switches the SDL2 audio
 * backend to its built-in disk driver), drives the UI through MENU →
 * Songs → SELECT to play the FLAC fixture, runs enough frames for the
 * audio engine to push at least one second of PCM through the ring +
 * HAL audio callback, then bit-compares the captured raw PCM against
 * the codec_kat reference.
 *
 * This is the *full* path: ID3/Vorbis tag parse → tagcache library
 * load → cabinet wiring → audio_engine_play → engine_fill_cb in the
 * SDL audio thread → disk driver writes bytes. If any link breaks
 * (decoder regresses, ring overruns, callback wired wrong, etc) this
 * test catches it where the codec KAT alone wouldn't.
 *
 * Layout assumption: the disk driver writes a leading silence block
 * (the audio thread starts before the ring has any data) followed by
 * the engine's PCM. We scan past the leading zeros, then compare the
 * next reference-length window byte-for-byte.
 *
 * Argv:
 *   sim_audio_playback <core-sim binary> <music-dir> <vectors-dir>
 *     music-dir:    a directory the sim's `--music` scan will walk
 *                    (the codec-vectors dir doubles as this — its
 *                    sole .flac sorts to row 0, which the press
 *                    sequence selects).
 *     vectors-dir:  the codec-vectors directory holding the reference
 *                    PCM (sine_440hz_1s_44k_s16_stereo.pcm).
 *
 * Capture file + screenshot land in a freshly mkdtemp'd dir so
 * parallel test runs can't clobber each other; the dir is removed on
 * success. (We deliberately leave it on failure for postmortem.)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define REFERENCE_PCM_NAME "sine_440hz_1s_44k_s16_stereo.pcm"

/* Frames the sim runs after the press sequence. Each frame sleeps 16ms,
 * so 200 frames ≈ 3.2 s of wall time — enough for the SDL audio thread
 * to drain the full 1 s reference plus margin. */
#define SIM_FRAMES "200"

/* Press sequence to navigate from the main menu (Music idx=0) to Songs
 * (Music sub-menu idx=2) and play the first row.
 *   E   — main menu: enter Music
 *   D D — scroll Music sub-menu Artists -> Albums -> Songs (idx 2)
 *   E   — enter Songs
 *   E   — play the first (and only) song
 */
#define PLAY_PRESSES "EDDEE"

static long load_file(const char *path, void **out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return -1; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return -1; }
    void *buf = malloc((size_t)n);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *out = buf;
    return n;
}

/* Find the byte offset within `cap` where the reference signal begins.
 *
 * The reference here is a 440 Hz sine starting at phase 0, so its first
 * stereo frame is silence (sin(0) = 0). That makes "first non-zero
 * sample" alone a bad anchor — the SDL audio thread also writes leading
 * silence while the engine spins up, and the boundary between the two
 * silences is invisible.
 *
 * Approach: pick the reference's first non-zero stereo frame as a
 * unique anchor (8 bytes: L L R R), find its first occurrence in cap,
 * then back up by the reference's leading silence count to align with
 * ref[0]. Returns -1 if the anchor isn't present (capture too short or
 * audio never reached the disk driver).
 */
static long find_audio_start(const uint8_t *cap, long cap_len,
                             const uint8_t *ref, long ref_len) {
    /* Locate the reference's first non-zero stereo frame (4 bytes). */
    long ref_anchor = -1;
    for (long i = 0; i + 4 <= ref_len; i += 4) {
        if (ref[i] | ref[i + 1] | ref[i + 2] | ref[i + 3]) {
            ref_anchor = i;
            break;
        }
    }
    if (ref_anchor < 0) {
        /* Pathological: reference is all silence. Anchor at offset 0. */
        return 0;
    }
    /* Search cap for the 8-byte signature ref[anchor..anchor+8). The
     * pair-of-stereo-frames width makes accidental matches against
     * ramp-up silence vanishingly unlikely, but stays cheap. */
    long sig_len = (ref_anchor + 8 <= ref_len) ? 8 : 4;
    for (long i = 0; i + sig_len <= cap_len; i += 4) {
        if (memcmp(cap + i, ref + ref_anchor, (size_t)sig_len) == 0) {
            return i - ref_anchor;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <core-sim> <music-dir> <vectors-dir>\n", argv[0]);
        return 2;
    }
    const char *sim_bin     = argv[1];
    const char *music_dir   = argv[2];
    const char *vectors_dir = argv[3];

    /* Allocate a per-run temp dir for the capture + screenshot.
     * /tmp on Linux/CI; if the platform uses something else, set
     * TMPDIR. */
    char tmpl[] = "/tmp/sim_audio_playback.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    const char *out_dir = tmpl;

    char ref_path[1024];
    snprintf(ref_path, sizeof(ref_path), "%s/%s", vectors_dir, REFERENCE_PCM_NAME);
    char capture_path[1024];
    snprintf(capture_path, sizeof(capture_path), "%s/captured.raw", out_dir);
    char shot_path[1024];
    snprintf(shot_path, sizeof(shot_path), "%s/shot.bmp", out_dir);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        /* Child: run the sim. argv list ends with NULL (POSIX). */
        execl(sim_bin, sim_bin,
              "--music",         music_dir,
              "--shot",          shot_path,
              "--press",         PLAY_PRESSES,
              "--frames",        SIM_FRAMES,
              "--capture-audio", capture_path,
              (char *)NULL);
        perror("execl");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "sim exited with status %d\n", status);
        return 1;
    }

    void *ref_buf = NULL, *cap_buf = NULL;
    long ref_len = load_file(ref_path,     &ref_buf);
    long cap_len = load_file(capture_path, &cap_buf);
    if (ref_len <= 0 || cap_len <= 0) {
        fprintf(stderr, "load failed (ref=%ld cap=%ld)\n", ref_len, cap_len);
        return 1;
    }

    long start = find_audio_start((const uint8_t *)cap_buf, cap_len,
                                  (const uint8_t *)ref_buf, ref_len);
    if (start < 0) {
        fprintf(stderr, "FAIL: reference signature not found in capture "
                "(%ld bytes captured) — playback never reached the audio thread\n",
                cap_len);
        return 1;
    }
    if (start + ref_len > cap_len) {
        fprintf(stderr, "FAIL: capture too short — start=%ld + ref_len=%ld > cap_len=%ld\n",
                start, ref_len, cap_len);
        return 1;
    }

    if (memcmp((const uint8_t *)cap_buf + start, ref_buf, (size_t)ref_len) != 0) {
        /* Find the first mismatching sample to give a useful diagnostic. */
        const uint8_t *a = (const uint8_t *)cap_buf + start;
        const uint8_t *b = (const uint8_t *)ref_buf;
        long mismatch = -1;
        for (long i = 0; i < ref_len; i++) {
            if (a[i] != b[i]) { mismatch = i; break; }
        }
        fprintf(stderr,
                "FAIL: capture vs reference differ at byte %ld of %ld (start=%ld)\n",
                mismatch, ref_len, start);
        return 1;
    }

    free(ref_buf);
    free(cap_buf);

    /* Clean up the temp dir on success — leave it intact on failure
     * so the user can inspect captured.raw + shot.bmp. */
    (void)unlink(capture_path);
    (void)unlink(shot_path);
    (void)rmdir(out_dir);

    fprintf(stdout, "ok: %ld bytes of audio matched (capture started at byte %ld)\n",
            ref_len, start);
    return 0;
}
