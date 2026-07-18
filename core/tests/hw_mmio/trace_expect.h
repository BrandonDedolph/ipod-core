/*
 * tests/hw_mmio/trace_expect.h — tiny cursor matcher over the mock log.
 *
 * A test programs the status registers so every poll is satisfied on
 * the first read, runs the driver, then walks the recorded event log in
 * order asserting each expected access. Any mismatch (wrong op, width,
 * address, value, or a short/long trace) prints a labelled diff and
 * flips the process exit code.
 *
 * Header-only and state-in-one-struct so a single test binary can run
 * several independent cases.
 */

#ifndef CORE_TESTS_HW_MMIO_TRACE_EXPECT_H
#define CORE_TESTS_HW_MMIO_TRACE_EXPECT_H

#include <stdio.h>
#include "mmio_mock.h"

typedef struct {
    const char *name;   /* case label, printed on failure */
    size_t      cursor; /* next log index to match         */
    int         fails;  /* accumulated mismatches          */
} trace_cursor;

static inline trace_cursor trace_begin(const char *name)
{
    trace_cursor tc = { name, 0, 0 };
    return tc;
}

static inline const char *op_name(mmio_op op)
{
    return op == MMIO_OP_READ ? "R" : "W";
}

/* Match the next event against (op,width,addr) and, when check_value is
 * set, its value too. Advances the cursor whether or not it matched, so
 * one desync doesn't cascade into every later line. */
static inline void trace_expect(trace_cursor *tc, mmio_op op, int width,
                                uint32_t addr, uint32_t value,
                                int check_value)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();

    if (tc->cursor >= len) {
        fprintf(stderr,
                "[%s] event %zu: expected %s%d @%08X but trace ended\n",
                tc->name, tc->cursor, op_name(op), width, addr);
        tc->fails++;
        tc->cursor++;
        return;
    }

    const mmio_event *e = &log[tc->cursor];
    int bad = e->op != op || e->width != width || e->addr != addr ||
              (check_value && e->value != value);
    if (bad) {
        fprintf(stderr,
                "[%s] event %zu: expected %s%d @%08X",
                tc->name, tc->cursor, op_name(op), width, addr);
        if (check_value) {
            fprintf(stderr, " =%08X", value);
        }
        fprintf(stderr, " but got %s%d @%08X =%08X\n",
                op_name(e->op), e->width, e->addr, e->value);
        tc->fails++;
    }
    tc->cursor++;
}

static inline void expect_w(trace_cursor *tc, int width, uint32_t addr,
                            uint32_t value)
{
    trace_expect(tc, MMIO_OP_WRITE, width, addr, value, 1);
}

static inline void expect_r(trace_cursor *tc, int width, uint32_t addr)
{
    trace_expect(tc, MMIO_OP_READ, width, addr, 0, 0);
}

/* Assert the trace had exactly as many events as were matched. */
static inline void trace_expect_end(trace_cursor *tc)
{
    size_t len = mmio_mock_log_len();
    if (tc->cursor != len) {
        fprintf(stderr,
                "[%s] trace length %zu, matched %zu (extra events)\n",
                tc->name, len, tc->cursor);
        tc->fails++;
    }
}

/* Returns 0 on success. Print a one-line PASS/FAIL summary. */
static inline int trace_done(trace_cursor *tc)
{
    if (tc->fails == 0) {
        printf("[%s] PASS (%zu events)\n", tc->name, tc->cursor);
        return 0;
    }
    printf("[%s] FAIL (%d mismatch%s)\n", tc->name, tc->fails,
           tc->fails == 1 ? "" : "es");
    return 1;
}

#endif /* CORE_TESTS_HW_MMIO_TRACE_EXPECT_H */
