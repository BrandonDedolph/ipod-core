/*
 * core/hal/sim/headphone_sim.c — headphone-jack presence for the host build.
 *
 * The sim has no jack. The state is read once from the environment so a
 * headless run can start "unplugged" without a keyboard hook, and can be
 * driven programmatically by tests through sim_headphones_set(). No debounce:
 * there is no switch to bounce, and the debouncer itself is exercised against
 * the real hw driver in tests/hw_mmio/headphone_trace_test.c.
 *
 *   CORE_SIM_HEADPHONES=0   start with nothing plugged in (default: 1, seated)
 */

#include "../hal.h"

#include <stdlib.h>

static int g_present = -1;   /* -1: environment not consulted yet */

int hal_headphones_present(void) {
    if (g_present < 0) {
        const char *e = getenv("CORE_SIM_HEADPHONES");
        g_present = (e == NULL) ? 1 : (atoi(e) != 0);
    }
    return g_present;
}

/* Sim-only: set the plug state (1 seated, 0 absent). */
void sim_headphones_set(int present) {
    g_present = present ? 1 : 0;
}
