/*
 * core/tests/hw_clickwheel/clickwheel_test.c — host decode test for the
 * PP5022 click-wheel driver.
 *
 * The driver's button/position/delta decode is pure integer math over a
 * 32-bit status word, so we compile clickwheel.c host-side with
 * -DMMIO_MOCK against the recording fake bus (hw_mmio/mmio_mock.c) and
 * feed SYNTHETIC status words: the mock answers CLICKWHEEL_DATA reads
 * from a script and GPIOA_INPUT_VAL from a constant, so clickwheel_poll()
 * runs its real logic on values we choose.
 *
 * Covers: the valid-gate (header) rejection, per-button and multi-button
 * decode + press/release de-dup, wheel-delta with the sensitivity gate,
 * sub-threshold accumulation, negative motion, wrap in both directions
 * (through 0x5F<->0x00), finger-lift reseeding (no spurious jump delta),
 * and the hold-switch edge / gating.
 *
 * Cleanroom note: every status word here is hand-built from the field
 * layout in core/docs/hw/03-clickwheel.md (via pp5022.h), not lifted from
 * any reference source.
 *
 * main() returns nonzero if any case fails; prints PASS/FAIL per case.
 */

#include "clickwheel.h"
#include "pp5022.h"
#include "mmio_mock.h"

#include <stdint.h>
#include <stdio.h>

/* GPIOA value with the hold bit SET == not held (active-low). */
#define GPIOA_RELEASED  CW_HOLD_BIT
#define GPIOA_HELD      0x00000000u

static int g_fail;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("  FAIL: %s\n", (msg));                      \
            g_fail = 1;                                         \
        }                                                       \
    } while (0)

/* Build a status word from touch flag, absolute position and raw
 * CW_BTN_* button bits. Always includes the valid header (0x8000001A). */
static uint32_t mkword(int touch, int pos, uint32_t btns)
{
    uint32_t w = CW_STAT_HEADER_VALUE;              /* bit 31 + header 0x1A */
    if (touch) {
        w |= CW_STAT_TOUCH;
    }
    w |= ((uint32_t)(pos & CW_POS_MASK)) << CW_POS_SHIFT;
    w |= (btns & CW_BTN_ALL);
    return w;
}

/* Fresh driver + mock state, hold released. */
static void setup_released(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_RELEASED);
    clickwheel_init();
}

/* Present one status word and poll once. */
static bool feed(uint32_t word, wheel_event_t *ev)
{
    mmio_mock_set_read(CLICKWHEEL_DATA_ADDR, word);
    return clickwheel_poll(ev);
}

/* Present one status word and run one tick-sampler pass (latches into the
 * queue the main loop drains); no drain here. */
static void sample(uint32_t word)
{
    mmio_mock_set_read(CLICKWHEEL_DATA_ADDR, word);
    clickwheel_service();
}

/* Present one status word, sample it, then drain — the sampler+drain
 * equivalent of feed(). */
static bool feed_svc(uint32_t word, wheel_event_t *ev)
{
    sample(word);
    return clickwheel_get_event(ev);
}

static void case_valid_gate(void)
{
    printf("case valid-gate:\n");
    setup_released();
    wheel_event_t ev;

    /* All-zero word: bit 31 clear -> rejected. */
    CHECK(!feed(0x00000000u, &ev), "all-zero word must be rejected");
    /* Correct header bit but wrong header byte (0x1B) -> rejected. */
    CHECK(!feed((CW_STAT_VALID | 0x1Bu), &ev), "wrong header byte rejected");
    /* Header byte 0x1A but valid bit clear -> rejected. */
    CHECK(!feed(0x0000001Au, &ev), "missing valid bit rejected");
    /* Proper valid word with a button -> accepted. */
    CHECK(feed(mkword(0, 0, CW_BTN_SELECT), &ev), "valid word accepted");
    CHECK(ev.buttons == WHEEL_BTN_SELECT, "accepted word decodes SELECT");
}

static void case_buttons(void)
{
    printf("case buttons:\n");
    wheel_event_t ev;

    struct { uint32_t raw; uint8_t want; const char *name; } map[] = {
        { CW_BTN_SELECT, WHEEL_BTN_SELECT, "SELECT" },
        { CW_BTN_MENU,   WHEEL_BTN_MENU,   "MENU"   },
        { CW_BTN_PLAY,   WHEEL_BTN_PLAY,   "PLAY"   },
        { CW_BTN_LEFT,   WHEEL_BTN_LEFT,   "LEFT"   },
        { CW_BTN_RIGHT,  WHEEL_BTN_RIGHT,  "RIGHT"  },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        setup_released();
        /* Press: change from the (zero) initial set -> event. */
        CHECK(feed(mkword(0, 0, map[i].raw), &ev), "button press fires");
        CHECK(ev.buttons == map[i].want, map[i].name);
        CHECK(ev.wheel_delta == 0, "no wheel motion on a pure press");
        /* Same button held: no change -> no event (de-dup). */
        CHECK(!feed(mkword(0, 0, map[i].raw), &ev), "held button de-dups");
        /* Release: set changes back to empty -> event with buttons 0. */
        CHECK(feed(mkword(0, 0, 0), &ev), "button release fires");
        CHECK(ev.buttons == 0, "release reports empty set");
    }

    /* Two buttons at once decode to both bits. */
    setup_released();
    CHECK(feed(mkword(0, 0, CW_BTN_MENU | CW_BTN_PLAY), &ev), "combo fires");
    CHECK(ev.buttons == (WHEEL_BTN_MENU | WHEEL_BTN_PLAY), "combo decodes both");
}

static void case_wheel_forward(void)
{
    printf("case wheel-forward:\n");
    setup_released();
    wheel_event_t ev;

    /* First touched sample only seeds the reference position. */
    CHECK(!feed(mkword(1, 10, 0), &ev), "first touch seeds, no event");
    /* +4 == sensitivity -> emit +4. */
    CHECK(feed(mkword(1, 14, 0), &ev), "delta at threshold fires");
    CHECK(ev.wheel_delta == 4, "forward delta == +4");
    CHECK(ev.touched, "touched flag set");
}

static void case_wheel_subthreshold_accumulates(void)
{
    printf("case wheel-subthreshold:\n");
    setup_released();
    wheel_event_t ev;

    CHECK(!feed(mkword(1, 10, 0), &ev), "seed");
    /* +3 < sensitivity(4) -> no event, reference NOT advanced. */
    CHECK(!feed(mkword(1, 13, 0), &ev), "sub-threshold suppressed");
    /* Now at 15: 15-10 == 5 >= 4 -> emit +5 (accumulated from 10). */
    CHECK(feed(mkword(1, 15, 0), &ev), "accumulated motion fires");
    CHECK(ev.wheel_delta == 5, "accumulated delta == +5 (ref stayed at 10)");
}

static void case_wheel_backward(void)
{
    printf("case wheel-backward:\n");
    setup_released();
    wheel_event_t ev;

    CHECK(!feed(mkword(1, 15, 0), &ev), "seed");
    CHECK(feed(mkword(1, 11, 0), &ev), "negative delta fires");
    CHECK(ev.wheel_delta == -4, "backward delta == -4");
}

static void case_wheel_wrap_forward(void)
{
    printf("case wheel-wrap-forward:\n");
    setup_released();
    wheel_event_t ev;

    /* 94 (0x5E) -> 2, forward through 0x5F->0x00: raw diff -92 wraps +96
     * to +4. */
    CHECK(!feed(mkword(1, 94, 0), &ev), "seed near top");
    CHECK(feed(mkword(1, 2, 0), &ev), "wrap-forward fires");
    CHECK(ev.wheel_delta == 4, "wrap-forward delta == +4 (not -92)");
}

static void case_wheel_wrap_backward(void)
{
    printf("case wheel-wrap-backward:\n");
    setup_released();
    wheel_event_t ev;

    /* 2 -> 94, backward through 0x00->0x5F: raw diff +92 wraps -96 to -4. */
    CHECK(!feed(mkword(1, 2, 0), &ev), "seed near bottom");
    CHECK(feed(mkword(1, 94, 0), &ev), "wrap-backward fires");
    CHECK(ev.wheel_delta == -4, "wrap-backward delta == -4 (not +92)");
}

static void case_finger_lift_reseeds(void)
{
    printf("case finger-lift-reseed:\n");
    setup_released();
    wheel_event_t ev;

    CHECK(!feed(mkword(1, 10, 0), &ev), "seed at 10");
    /* Finger lifts (touch clear): no motion, reseed pending. */
    CHECK(!feed(mkword(0, 50, 0), &ev), "untouched sample: no event");
    /* Touch returns far away (20). Because the lift reseeded, this must
     * seed at 20 rather than emit a spurious 20-10==10 delta. */
    CHECK(!feed(mkword(1, 20, 0), &ev), "re-touch reseeds, no jump delta");
    /* +4 from the new reference -> emit. */
    CHECK(feed(mkword(1, 24, 0), &ev), "motion after reseed fires");
    CHECK(ev.wheel_delta == 4, "delta measured from reseeded 20");
}

static void case_hold(void)
{
    printf("case hold:\n");
    setup_released();          /* init sampled hold == released */
    wheel_event_t ev;

    /* Direct query tracks the GPIO. */
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_HELD);
    CHECK(clickwheel_hold(), "clickwheel_hold() true when bit clear");
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_RELEASED);
    CHECK(!clickwheel_hold(), "clickwheel_hold() false when bit set");

    /* Engage hold: the release->held edge fires an event with hold set. */
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_HELD);
    CHECK(clickwheel_poll(&ev), "hold engage fires an edge event");
    CHECK(ev.hold, "edge event reports hold engaged");
    CHECK(ev.buttons == 0 && ev.wheel_delta == 0, "hold edge carries no input");

    /* Still held: no further events (wheel is dead). */
    CHECK(!clickwheel_poll(&ev), "no events while held");

    /* Release: the held->release edge fires with hold clear. */
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_RELEASED);
    CHECK(clickwheel_poll(&ev), "hold release fires an edge event");
    CHECK(!ev.hold, "release edge reports hold disengaged");
}

/* ---- Tick-sampler + drain path (clickwheel_service/clickwheel_get_event) --- */

/* A fresh drain returns nothing. */
static void case_svc_idle(void)
{
    printf("case svc-idle:\n");
    setup_released();
    wheel_event_t ev;
    CHECK(!clickwheel_get_event(&ev), "empty latch drains nothing");
    /* A sample with no button/motion also latches nothing. */
    sample(mkword(0, 0, 0));
    CHECK(!clickwheel_get_event(&ev), "idle sample latches nothing");
}

/* A press sampled by the tick is latched for the main loop to drain, once. */
static void case_svc_press_latch(void)
{
    printf("case svc-press-latch:\n");
    setup_released();
    wheel_event_t ev;

    CHECK(feed_svc(mkword(0, 0, CW_BTN_SELECT), &ev), "sampled press drains");
    CHECK(ev.buttons == WHEEL_BTN_SELECT, "drained press decodes SELECT");
    CHECK(ev.wheel_delta == 0, "no wheel motion on a pure press");
    /* Button still held on the next sample is NOT a new down-edge. */
    CHECK(!feed_svc(mkword(0, 0, CW_BTN_SELECT), &ev), "held button: no re-fire");
}

/*
 * THE BUG FIX: a press that comes AND goes entirely between two main-loop
 * drains is still delivered, because the tick sampled the down-edge. Sample
 * a press then its release with NO drain in between, then drain once.
 */
static void case_svc_press_between_drains(void)
{
    printf("case svc-press-between-drains:\n");
    setup_released();
    wheel_event_t ev;

    sample(mkword(0, 0, CW_BTN_MENU));   /* down  */
    sample(mkword(0, 0, 0));             /* up, before the loop ever drained */
    CHECK(clickwheel_get_event(&ev), "brief tap between drains survives");
    CHECK(ev.buttons == WHEEL_BTN_MENU, "latched down-edge is MENU");
    /* Drained exactly once. */
    CHECK(!clickwheel_get_event(&ev), "tap not re-delivered");
}

/* Multiple distinct buttons pressed between drains OR together in the latch. */
static void case_svc_multi_button_latch(void)
{
    printf("case svc-multi-button-latch:\n");
    setup_released();
    wheel_event_t ev;

    sample(mkword(0, 0, CW_BTN_LEFT));                 /* left down */
    sample(mkword(0, 0, CW_BTN_LEFT | CW_BTN_RIGHT));  /* right also down */
    CHECK(clickwheel_get_event(&ev), "combined down-edges drain");
    CHECK(ev.buttons == (WHEEL_BTN_LEFT | WHEEL_BTN_RIGHT),
          "latch OR-accumulates both down-edges");
}

/* Wheel motion sampled across several ticks accumulates into one drain. */
static void case_svc_wheel_accumulates(void)
{
    printf("case svc-wheel-accum:\n");
    setup_released();
    wheel_event_t ev;

    sample(mkword(1, 10, 0));   /* seed reference */
    sample(mkword(1, 14, 0));   /* +4 */
    sample(mkword(1, 18, 0));   /* +4 */
    CHECK(clickwheel_get_event(&ev), "accumulated motion drains");
    CHECK(ev.wheel_delta == 8, "two +4 ticks accumulate to +8");
    CHECK(ev.touched, "touched flag carried through the latch");
    CHECK(!clickwheel_get_event(&ev), "accumulator cleared after drain");
}

/* The tick sampler gates the wheel on a hold edge and reports it via drain. */
static void case_svc_hold(void)
{
    printf("case svc-hold:\n");
    setup_released();
    wheel_event_t ev;

    /* Engage hold: the sampler latches the release->held edge. */
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_HELD);
    clickwheel_service();
    CHECK(clickwheel_get_event(&ev), "hold engage drains an edge");
    CHECK(ev.hold, "drained edge reports hold engaged");
    CHECK(ev.buttons == 0 && ev.wheel_delta == 0, "hold edge carries no input");

    /* Still held: the wheel is dead, nothing latches. */
    sample(mkword(0, 0, CW_BTN_SELECT));
    CHECK(!clickwheel_get_event(&ev), "no input latched while held");

    /* Release: the held->release edge drains with hold clear. */
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_RELEASED);
    clickwheel_service();
    CHECK(clickwheel_get_event(&ev), "hold release drains an edge");
    CHECK(!ev.hold, "release edge reports hold disengaged");
}

/* A press captured just before a hold edge is dropped (carries no input). */
static void case_svc_hold_clears_pending(void)
{
    printf("case svc-hold-clears-pending:\n");
    setup_released();
    wheel_event_t ev;

    sample(mkword(0, 0, CW_BTN_PLAY));               /* latch a press */
    mmio_mock_set_read(GPIOA_INPUT_VAL_ADDR, GPIOA_HELD);
    clickwheel_service();                            /* hold edge clears it */
    CHECK(clickwheel_get_event(&ev), "hold edge drains");
    CHECK(ev.hold && ev.buttons == 0, "pending press dropped on hold engage");
}

int main(void)
{
    case_valid_gate();
    case_buttons();
    case_wheel_forward();
    case_wheel_subthreshold_accumulates();
    case_wheel_backward();
    case_wheel_wrap_forward();
    case_wheel_wrap_backward();
    case_finger_lift_reseeds();
    case_hold();
    case_svc_idle();
    case_svc_press_latch();
    case_svc_press_between_drains();
    case_svc_multi_button_latch();
    case_svc_wheel_accumulates();
    case_svc_hold();
    case_svc_hold_clears_pending();

    if (g_fail) {
        printf("SOME FAILED\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
