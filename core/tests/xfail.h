/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/xfail.h — assertions that document a KNOWN bug instead of hiding it.
 *
 * Some of the tests in this tree were written against the *documented*
 * contract (hal/hal.h, fs/fat32.h) rather than against what the code happens
 * to do today, because the contract is what the rest of the firmware relies
 * on. Where the current implementation does not yet meet it, and the fix lives
 * in a file the test does not own, deleting or weakening the assertion would
 * lose the finding. Instead:
 *
 *   XPECT(...)  — a normal assertion; failing fails the binary.
 *   XFAIL(...)  — an assertion known to fail today. A failure prints XFAIL
 *                 with the reason and does NOT fail the binary.
 *
 * An XFAIL that PASSES (XPASS) FAILS THE BINARY. That is not pedantry: an
 * XFAIL is a claim that the bug is still there, and once it isn't, the marker
 * is the only thing standing between the fix and a silent regression — the
 * vector no longer asserts anything, and nobody looks at a green suite. The
 * build stopping is what makes someone spend the thirty seconds to promote it
 * to XPECT. Two of these sat XPASSing in a green suite until an audit found
 * them, which is exactly the failure this now prevents.
 *
 * Set CORE_TEST_STRICT_XFAIL=1 in the environment to ALSO turn every still-
 * failing XFAIL into a hard failure — CI sets it, so a known bug has to be
 * either fixed or explicitly re-marked, never quietly inherited.
 */
#ifndef CORE_TESTS_XFAIL_H
#define CORE_TESTS_XFAIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *suite;
    int fails;     /* real failures                                   */
    int xfails;    /* known bugs, still broken (informational)        */
    int xpasses;   /* known bugs that now pass (promote the assertion) */
} xfail_ctx;

static inline int xfail_strict(void)
{
    const char *e = getenv("CORE_TEST_STRICT_XFAIL");
    return e != NULL && strcmp(e, "0") != 0 && e[0] != '\0';
}

static inline void xpect(xfail_ctx *c, const char *label, int cond)
{
    if (cond) {
        printf("[%s %s] PASS\n", c->suite, label);
        return;
    }
    fprintf(stderr, "[%s %s] FAIL\n", c->suite, label);
    c->fails++;
}

/* `why` must name the file that owns the fix, so the report is actionable. */
static inline void xfail(xfail_ctx *c, const char *label, int cond,
                         const char *why)
{
    if (!cond) {
        if (xfail_strict()) {
            fprintf(stderr, "[%s %s] FAIL (strict xfail) — %s\n",
                    c->suite, label, why);
            c->fails++;
            return;
        }
        printf("[%s %s] XFAIL — known bug: %s\n", c->suite, label, why);
        c->xfails++;
        return;
    }
    fprintf(stderr,
            "[%s %s] XPASS — the known bug is FIXED (%s).\n"
            "    ACTION: promote this XFAIL to XPECT so it stays fixed.\n"
            "    Until you do, this vector asserts nothing and a regression\n"
            "    would go unnoticed — which is why this fails the build.\n",
            c->suite, label, why);
    c->xpasses++;
}

/* Returns the process exit status and prints the one-line summary. */
static inline int xfail_done(xfail_ctx *c)
{
    printf("%s: %d failure(s), %d known-bug xfail(s), %d xpass(es)\n",
           c->suite, c->fails, c->xfails, c->xpasses);
    return (c->fails == 0 && c->xpasses == 0) ? 0 : 1;
}

#endif /* CORE_TESTS_XFAIL_H */
