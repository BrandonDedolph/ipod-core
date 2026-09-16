/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_mmio/headphone_trace_test.c — the headphone-jack detect driver
 * (hal/hw/headphone.c) against the recording mock bus (-DMMIO_MOCK).
 *
 * One source, three binaries, because the driver has three compile-time
 * shapes and each has a promise worth pinning:
 *
 *   hw-headphone-trace     (-DHEADPHONE_DETECT_TRUSTED=1)
 *     The register grammar of one poll — exactly one 32-bit read of the
 *     USEC_TIMER then one of GPIOA_INPUT_VAL, no writes — the bit-7 decode
 *     and polarity, and THE DEBOUNCE: a bouncing input produces exactly one
 *     transition, at the end of the bounce; a glitch shorter than the
 *     window produces none; the first sample primes without waiting; a
 *     USEC_TIMER wrap mid-window does not break the compare.
 *
 * Both non-probe binaries also pin headphone_pin_cfg() — the two-read, no-write
 * accessor behind the About screen's on-screen probe.
 *
 *   hw-headphone-untrusted (-DHEADPHONE_DETECT_TRUSTED=0)
 *     Until the pin has been read on the device, the driver must
 *     answer -1 and must not touch the bus at all. The macro is spelled out
 *     by tests/meson.build rather than inherited from the header, so the
 *     bench flipping that default cannot silently turn this into a second
 *     trusted binary.
 *
 *   hw-headphone-probe     (-DHEADPHONE_PROBE=1 -DHEADPHONE_DETECT_TRUSTED=1)
 *     The probe prints its config block once, then NOTHING while no input
 *     changes, one snapshot + one diff line per change, respects its minimum
 *     sample interval, and goes silent after its event budget — i.e. it
 *     cannot flood the only debug channel the device has.
 *
 * Values are hand-derived from core/docs/hw/10-headphone-jack.md, never from
 * Rockbox source — the cleanroom boundary applies to test vectors too.
 *
 * MUTATION CHECK (2026-09-10): each of these breaks the suite —
 *   - dropping the "back at the believed level cancels the candidate" reset
 *     in headphone_debounce_feed: bounce_one_transition reports at poll 25
 *     instead of 35 and glitch_ignored sees a transition;
 *   - making the window compare strict (>): pure_debouncer, timer_wrap and
 *     bounce_one_transition all fail;
 *   - disabling the probe's event budget: probe_cannot_spam counts 2000
 *     lines from the flood and 200 more afterwards.
 */

#include <string.h>

#include "pp5022.h"
#include "headphone.h"
#include "hal.h"
#include "mmio_mock.h"
#include "trace_expect.h"

/* The window the driver is required to enforce (headphone.h). Duplicated
 * here on purpose: a change to the constant has to be made in both places. */
#define WINDOW_US  200000u

/* ---------- Clock scripting ---------------------------------------------- */

static uint32_t g_now;

static void clock_set(uint32_t us)
{
    g_now = us;
    mmio_mock_set_read(USEC_TIMER_ADDR, g_now);
}

static void clock_advance(uint32_t us)
{
    clock_set(g_now + us);
}

/* The probe binary scripts all twelve ports itself, so these helpers are only
 * for the two single-pin shapes. */
#if !HEADPHONE_PROBE
static void pin_set(int seated)
{
    mmio_mock_set_read(HEADPHONE_DETECT_ADDR,
                       seated ? HEADPHONE_DETECT_BIT : 0u);
}

/* GPIOA configuration registers, from the bank layout in
 * 10-headphone-jack.md: A-D quad at 0x6000D000, port A at +0x00, ENABLE
 * group +0x00 and OUTPUT_EN group +0x10. Hand-derived here rather than
 * shared with the driver on purpose — a typo in the driver's copy is
 * exactly what this is for. */
#define GPIOA_ENABLE_ADDR     0x6000D000u
#define GPIOA_OUTPUT_EN_ADDR  0x6000D010u

/*
 * headphone_pin_cfg() is what the About screen's JACK token shows when the
 * level never moves: it answers "is A7 even a GPIO input?", and the answer
 * decides whether the bench is looking at the wrong pin or at an unconfigured
 * one. It must be TWO READS and NOTHING ELSE — this driver does not
 * reconfigure a pin it has not been proven to own, and a stray write to a
 * GPIO enable register on a board whose pin map is inferred is how you drive
 * an output into something. Compiled into BOTH non-probe shapes: the token is
 * drawn in the shipping (untrusted) image too, which is the whole point of it.
 */
static int test_pin_cfg_grammar(void)
{
    mmio_mock_reset();
    trace_cursor tc = trace_begin("pin_cfg_grammar");

    mmio_mock_set_read(GPIOA_ENABLE_ADDR, 0x80);      /* A7 is a GPIO...  */
    mmio_mock_set_read(GPIOA_OUTPUT_EN_ADDR, 0x00);   /* ...and an input  */
    int got = headphone_pin_cfg();

    expect_r(&tc, 32, GPIOA_ENABLE_ADDR);
    expect_r(&tc, 32, GPIOA_OUTPUT_EN_ADDR);
    trace_expect_end(&tc);
    if (got != HEADPHONE_PIN_ENABLED) {
        fprintf(stderr, "[%s] enabled input: expected %d, got %d\n", tc.name,
                HEADPHONE_PIN_ENABLED, got);
        tc.fails++;
    }

    /* Bit 7 and only bit 7, in each register independently. */
    struct { uint32_t en, oe; int want; } cases[] = {
        { 0x00,       0x00,       0 },
        { 0x80,       0x00,       HEADPHONE_PIN_ENABLED },
        { 0x00,       0x80,       HEADPHONE_PIN_OUTPUT },
        { 0xFF,       0xFF,       HEADPHONE_PIN_ENABLED | HEADPHONE_PIN_OUTPUT },
        { 0x7F,       0x7F,       0 },
        { 0xFFFFFF80, 0xFFFFFF7F, HEADPHONE_PIN_ENABLED },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mmio_mock_reset();
        mmio_mock_set_read(GPIOA_ENABLE_ADDR, cases[i].en);
        mmio_mock_set_read(GPIOA_OUTPUT_EN_ADDR, cases[i].oe);
        int cfg = headphone_pin_cfg();
        if (cfg != cases[i].want) {
            fprintf(stderr, "[%s] en=%08X oe=%08X: expected %d, got %d\n",
                    tc.name, cases[i].en, cases[i].oe, cases[i].want, cfg);
            tc.fails++;
        }
        if (mmio_mock_count(MMIO_OP_WRITE, GPIOA_ENABLE_ADDR) != 0 ||
            mmio_mock_count(MMIO_OP_WRITE, GPIOA_OUTPUT_EN_ADDR) != 0) {
            fprintf(stderr, "[%s] the driver WROTE a GPIO config register\n",
                    tc.name);
            tc.fails++;
        }
    }
    return trace_done(&tc);
}
#endif

/* ======================================================================
 * TRUSTED: grammar + debounce through the real driver
 * ====================================================================== */
#if HEADPHONE_DETECT_TRUSTED && !HEADPHONE_PROBE

/* One poll is one timer read then one pin read; nothing else, ever. */
static int test_poll_grammar(void)
{
    mmio_mock_reset();
    headphone_reset();
    clock_set(1000);
    pin_set(1);

    int got = hal_headphones_present();

    trace_cursor tc = trace_begin("poll_grammar");
    expect_r(&tc, 32, USEC_TIMER_ADDR);
    expect_r(&tc, 32, HEADPHONE_DETECT_ADDR);
    trace_expect_end(&tc);
    if (got != 1) {
        fprintf(stderr, "[%s] seated pin: expected 1, got %d\n", tc.name, got);
        tc.fails++;
    }
    if (mmio_mock_count(MMIO_OP_WRITE, HEADPHONE_DETECT_ADDR) != 0) {
        fprintf(stderr, "[%s] driver WROTE the input register\n", tc.name);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* Bit 7 and only bit 7 decides; every other bit of port A is someone
 * else's (hold is bit 5). Polarity per the default: set = seated. */
static int test_decode(void)
{
    trace_cursor tc = trace_begin("decode");
    struct { uint32_t word; int want; } cases[] = {
        { 0x80, 1 }, { 0x00, 0 }, { 0x7F, 0 }, { 0xFF, 1 }, { 0x20, 0 },
        { 0xFFFFFF7F, 0 }, { 0x00000080, 1 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mmio_mock_reset();
        headphone_reset();
        clock_set(0);
        mmio_mock_set_read(HEADPHONE_DETECT_ADDR, cases[i].word);
        int got = hal_headphones_present();
        if (got != cases[i].want) {
            fprintf(stderr, "[%s] GPIOA=%08X: expected %d, got %d\n",
                    tc.name, cases[i].word, cases[i].want, got);
            tc.fails++;
        }
    }
    return trace_done(&tc);
}

/* Poll `n` times `step_us` apart with the pin scripted by `pattern` (a string
 * of '1'/'0', one char per poll; the last char repeats). Returns the number
 * of transitions in the driver's answers and writes the last answer. */
static int run_pattern(const char *pattern, uint32_t step_us, int n,
                       int *last_out)
{
    int transitions = 0;
    int last = -2;
    const char *p = pattern;
    for (int i = 0; i < n; i++) {
        pin_set(*p == '1');
        if (p[1] != '\0') {
            p++;
        }
        int got = hal_headphones_present();
        if (last != -2 && got != last) {
            transitions++;
        }
        last = got;
        clock_advance(step_us);
    }
    *last_out = last;
    return transitions;
}

/*
 * THE DEBOUNCE. Seated, then a plug pulled with a bouncing switch: the raw
 * level flaps 0/1 every 10 ms for 90 ms and then stays 0. The driver must
 * report exactly ONE transition, and only once the level has held for the
 * full window measured from the LAST bounce — not from the first.
 */
static int test_bounce_one_transition(void)
{
    mmio_mock_reset();
    headphone_reset();
    clock_set(5000000);
    trace_cursor tc = trace_begin("bounce_one_transition");

    int last;
    /* 1 x5 (50 ms seated), bounce 0101010101 (100 ms), then 0 x40 (400 ms). */
    int t = run_pattern("111110101010101000000000000000000000000000000000000000",
                        10000, 55, &last);
    if (t != 1 || last != 0) {
        fprintf(stderr, "[%s] expected exactly 1 transition ending at 0, got "
                        "%d ending at %d\n", tc.name, t, last);
        tc.fails++;
    }

    /* Timing: the last '1' of the bounce is poll 14 (t=140 ms); the first
     * '0' that starts the accepted candidate is poll 15 (t=150 ms). The
     * window is met at t=350 ms, i.e. poll 35, and not one poll earlier. */
    mmio_mock_reset();
    headphone_reset();
    clock_set(0);
    const char *pat = "111110101010101000000000000000000000000000000000000000";
    int first_zero_poll = -1;
    for (int i = 0; i < 55; i++) {
        pin_set(pat[i] == '1');
        int got = hal_headphones_present();
        if (got == 0 && first_zero_poll < 0) {
            first_zero_poll = i;
        }
        clock_advance(10000);
    }
    if (first_zero_poll != 35) {
        fprintf(stderr, "[%s] transition reported at poll %d, expected 35 "
                        "(window from the LAST bounce)\n", tc.name,
                first_zero_poll);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* A glitch shorter than the window — a plug nudged in a pocket for 150 ms —
 * must produce NO transition, and re-arming must start over: two 150 ms
 * glitches 50 ms apart are still nothing. */
static int test_glitch_ignored(void)
{
    mmio_mock_reset();
    headphone_reset();
    clock_set(0);
    trace_cursor tc = trace_begin("glitch_ignored");
    int last;
    int t = run_pattern("11111000000000000000111110000000000000001111111111",
                        10000, 50, &last);
    if (t != 0 || last != 1) {
        fprintf(stderr, "[%s] expected 0 transitions ending at 1, got %d "
                        "ending at %d\n", tc.name, t, last);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* Insert after a genuine unplug: one transition back, after the window. */
static int test_replug(void)
{
    mmio_mock_reset();
    headphone_reset();
    clock_set(0);
    trace_cursor tc = trace_begin("replug");
    int last;
    /* 0 x30 (300 ms out), then 1 x30 (300 ms in). Primes at 0. */
    int t = run_pattern("000000000000000000000000000000111111111111111111111111111111",
                        10000, 60, &last);
    if (t != 1 || last != 1) {
        fprintf(stderr, "[%s] expected 1 transition ending at 1, got %d "
                        "ending at %d\n", tc.name, t, last);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* The first sample is truth: a device booted with the plug out must say 0
 * on the very first poll, not "1 for 200 ms then 0". */
static int test_prime_immediate(void)
{
    mmio_mock_reset();
    headphone_reset();
    clock_set(0);
    trace_cursor tc = trace_begin("prime_immediate");
    pin_set(0);
    int got = hal_headphones_present();
    if (got != 0) {
        fprintf(stderr, "[%s] first poll with plug out: expected 0, got %d\n",
                tc.name, got);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* USEC_TIMER wraps every ~71 min. A candidate that starts 100 ms before the
 * wrap must be accepted 100 ms after it, not 71 minutes later. */
static int test_timer_wrap(void)
{
    mmio_mock_reset();
    headphone_reset();
    trace_cursor tc = trace_begin("timer_wrap");
    clock_set(0xFFFFFFFFu - 100000u);   /* 100 ms before wrap */
    pin_set(1);
    (void)hal_headphones_present();     /* prime at 1 */
    clock_advance(10000);
    pin_set(0);
    (void)hal_headphones_present();     /* candidate starts, 90 ms pre-wrap */
    clock_advance(100000);              /* now 10 ms past the wrap */
    int mid = hal_headphones_present();
    clock_advance(100000);              /* 200 ms since the candidate */
    int end = hal_headphones_present();
    if (mid != 1 || end != 0) {
        fprintf(stderr, "[%s] across the wrap: mid %d (want 1), end %d "
                        "(want 0)\n", tc.name, mid, end);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* The pure debouncer, directly: same contract without the bus, so a future
 * caller (the sim, another switch) can rely on it in isolation. */
static int test_pure_debouncer(void)
{
    trace_cursor tc = trace_begin("pure_debouncer");
    headphone_debounce_t d;
    headphone_debounce_reset(&d);
    if (headphone_debounce_feed(&d, 1, 0) != 1) {
        fprintf(stderr, "[%s] prime failed\n", tc.name);
        tc.fails++;
    }
    /* 0 held for exactly window-1 us: not yet. At window: yes. */
    (void)headphone_debounce_feed(&d, 0, 1000);
    if (headphone_debounce_feed(&d, 0, 1000 + WINDOW_US - 1) != 1) {
        fprintf(stderr, "[%s] accepted one microsecond early\n", tc.name);
        tc.fails++;
    }
    if (headphone_debounce_feed(&d, 0, 1000 + WINDOW_US) != 0) {
        fprintf(stderr, "[%s] not accepted at the window\n", tc.name);
        tc.fails++;
    }
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    fails += test_poll_grammar();
    fails += test_decode();
    fails += test_prime_immediate();
    fails += test_bounce_one_transition();
    fails += test_glitch_ignored();
    fails += test_replug();
    fails += test_timer_wrap();
    fails += test_pure_debouncer();
    fails += test_pin_cfg_grammar();
    return fails == 0 ? 0 : 1;
}

/* ======================================================================
 * UNTRUSTED (default build): -1 and no bus traffic
 * ====================================================================== */
#elif !HEADPHONE_DETECT_TRUSTED

int main(void)
{
    mmio_mock_reset();
    headphone_reset();
    clock_set(0);
    pin_set(1);
    trace_cursor tc = trace_begin("untrusted_is_inert");
    int fails = 0;
    for (int i = 0; i < 10; i++) {
        int got = hal_headphones_present();
        if (got != -1) {
            fprintf(stderr, "[%s] poll %d: expected -1, got %d\n", tc.name,
                    i, got);
            tc.fails++;
        }
        clock_advance(WINDOW_US);
    }
    if (mmio_mock_log_len() != 0) {
        fprintf(stderr, "[%s] touched the bus (%u events) while untrusted\n",
                tc.name, (unsigned)mmio_mock_log_len());
        tc.fails++;
    }
    /* The raw read is still available — it is the About screen's on-screen
     * probe, which is drawn in THIS build, the one that ships — and it still
     * decodes. One read, and with the two configuration reads asserted by
     * test_pin_cfg_grammar below it is the whole bus cost of an untrusted
     * build: nothing here polls, times or debounces anything. */
    if (headphone_raw() != 1) {
        fprintf(stderr, "[%s] headphone_raw() on a seated pin != 1\n",
                tc.name);
        tc.fails++;
    }
    if (mmio_mock_log_len() != 1) {
        fprintf(stderr, "[%s] headphone_raw() cost %u bus events, expected "
                        "1\n", tc.name, (unsigned)mmio_mock_log_len());
        tc.fails++;
    }
    fails += trace_done(&tc);
    fails += test_pin_cfg_grammar();
    return fails == 0 ? 0 : 1;
}

/* ======================================================================
 * PROBE: prints on change only, rate-limited, budgeted
 * ====================================================================== */
#else

/* Every uart_putc ends in one THR write; a line ends in "\r\n", so counting
 * '\n' THR writes counts lines. */
static size_t lines_written(void)
{
    const mmio_event *log = mmio_mock_log();
    size_t n = mmio_mock_log_len(), lines = 0;
    for (size_t i = 0; i < n; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == SER0_THR_ADDR &&
            log[i].value == '\n') {
            lines++;
        }
    }
    return lines;
}

static void set_port_inputs(uint32_t base, uint32_t a, uint32_t b,
                            uint32_t c, uint32_t d)
{
    mmio_mock_set_read(base + 0x30, a);
    mmio_mock_set_read(base + 0x34, b);
    mmio_mock_set_read(base + 0x38, c);
    mmio_mock_set_read(base + 0x3C, d);
}

static void probe_env(void)
{
    mmio_mock_set_read(SER0_LSR_ADDR, SER0_LSR_THRE);   /* TX always ready */
    /* A7 configured as an input by the "ROM": enable set, output-en clear. */
    mmio_mock_set_read(0x6000D000, 0x80);
    set_port_inputs(0x6000D000, 0x80, 0x01, 0x00, 0x00);
    set_port_inputs(0x6000D080, 0x00, 0x00, 0x00, 0x00);
    set_port_inputs(0x6000D100, 0x00, 0x00, 0x00, 0x98);
}

int main(void)
{
    trace_cursor tc = trace_begin("probe_cannot_spam");
    int fails = 0;

    mmio_mock_reset();
    headphone_reset();
    clock_set(0);
    probe_env();

    /* First poll: the 12-line config block + 1 snapshot, no "forced" line
     * because A7 already reads as an input. */
    (void)hal_headphones_present();
    size_t l0 = lines_written();
    if (l0 != 13) {
        fprintf(stderr, "[%s] first poll wrote %u lines, expected 13\n",
                tc.name, (unsigned)l0);
        tc.fails++;
    }

    /* 200 polls, 25 ms apart, nothing changing: not one more byte. */
    for (int i = 0; i < 200; i++) {
        clock_advance(25000);
        (void)hal_headphones_present();
    }
    if (lines_written() != l0) {
        fprintf(stderr, "[%s] printed %u lines with no input change\n",
                tc.name, (unsigned)(lines_written() - l0));
        tc.fails++;
    }

    /* Unplug: one snapshot + exactly one diff line, naming A bit7 1->0. */
    clock_advance(25000);
    mmio_mock_set_read(0x6000D030, 0x00);
    (void)hal_headphones_present();
    if (lines_written() != l0 + 2) {
        fprintf(stderr, "[%s] unplug printed %u lines, expected 2\n",
                tc.name, (unsigned)(lines_written() - l0));
        tc.fails++;
    }
    {
        /* Reassemble the tail of the TX stream and check the diff text. */
        const mmio_event *log = mmio_mock_log();
        size_t n = mmio_mock_log_len();
        char tail[200];
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (log[i].op == MMIO_OP_WRITE && log[i].addr == SER0_THR_ADDR) {
                if (k < sizeof tail - 1) {
                    tail[k++] = (char)log[i].value;
                } else {
                    /* keep only the most recent bytes */
                    for (size_t j = 1; j < sizeof tail - 1; j++) {
                        tail[j - 1] = tail[j];
                    }
                    tail[sizeof tail - 2] = (char)log[i].value;
                }
            }
        }
        tail[k < sizeof tail - 1 ? k : sizeof tail - 1] = '\0';
        if (strstr(tail, "core: hpprobe diff A bit7 1->0\r\n") == NULL) {
            fprintf(stderr, "[%s] diff line missing/wrong; tail:\n%s\n",
                    tc.name, tail);
            tc.fails++;
        }
    }

    /* Rate limit: a change 5 ms after the last sample is not looked at until
     * the 20 ms interval has elapsed, then reported once. */
    size_t l1 = lines_written();
    clock_advance(5000);
    mmio_mock_set_read(0x6000D030, 0x80);
    (void)hal_headphones_present();
    if (lines_written() != l1) {
        fprintf(stderr, "[%s] sampled inside the minimum interval\n", tc.name);
        tc.fails++;
    }
    clock_advance(20000);
    (void)hal_headphones_present();
    if (lines_written() != l1 + 2) {
        fprintf(stderr, "[%s] replug after the interval printed %u lines, "
                        "expected 2\n", tc.name, (unsigned)(lines_written() - l1));
        tc.fails++;
    }

    /* Budget: a pin flapping on every sample for a very long time must be
     * cut off — bounded lines, then silence, however long it goes on.
     *
     * Run in chunks with the mock log reset between them: one change event
     * is ~250 bus events (12 port reads + ~120 characters, each an LSR read
     * and a THR write), so 400 of them would overflow the 64k-event log and
     * silently make the line counts below a fiction. The probe's own state is
     * static in the driver and survives the reset; only the recording does
     * not. Line counts are summed per chunk. */
    size_t flood = 0;
    uint32_t v = 0x00;
    for (int chunk = 0; chunk < 10; chunk++) {
        mmio_mock_reset();
        probe_env();
        for (int i = 0; i < 100; i++) {
            clock_advance(25000);
            mmio_mock_set_read(0x6000D030, v);
            v ^= 0x80;
            (void)hal_headphones_present();
        }
        if (mmio_mock_dropped() != 0) {
            fprintf(stderr, "[%s] mock log overflowed in chunk %d (%u "
                            "dropped); shrink the chunk\n", tc.name, chunk,
                    (unsigned)mmio_mock_dropped());
            tc.fails++;
        }
        flood += lines_written();
    }
    /* 1000 flaps against a 400-event budget: at most 2 lines per event x
     * (400 - 2 events already spent) + the goodbye line. */
    if (flood > 2u * 400u + 1u) {
        fprintf(stderr, "[%s] flapping pin produced %u lines; budget not "
                        "enforced\n", tc.name, (unsigned)flood);
        tc.fails++;
    }
    if (flood < 2u * 300u) {
        /* Sanity on the test itself: the budget must have been REACHED, or
         * the silence check below proves nothing. */
        fprintf(stderr, "[%s] flood too small to exhaust the budget (%u "
                        "lines)\n", tc.name, (unsigned)flood);
        tc.fails++;
    }
    mmio_mock_reset();
    probe_env();
    for (int i = 0; i < 100; i++) {
        clock_advance(25000);
        mmio_mock_set_read(0x6000D030, v);
        v ^= 0x80;
        (void)hal_headphones_present();
    }
    if (lines_written() != 0) {
        fprintf(stderr, "[%s] still printing after the budget (%u lines)\n",
                tc.name, (unsigned)lines_written());
        tc.fails++;
    }

    fails += trace_done(&tc);
    return fails == 0 ? 0 : 1;
}

#endif
