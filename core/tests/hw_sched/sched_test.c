/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sched_test.c — host unit tests for the scheduler pick policy.
 *
 * Builds kernel/sched.c with -DSCHED_TEST alongside sched_test_mocks.c.
 * These tests exercise the pure table + round-robin logic only; no real
 * context switch happens (the mocks are inert).
 */
#include <stdint.h>
#include <stdio.h>

#include "sched.h"

/* Test-only hooks exposed by sched.c under -DSCHED_TEST. */
extern void sched_test_set_current(int id);
extern void sched_test_mark_done(int id);

static int failures = 0;

static void check(const char *case_name, int cond)
{
    if (cond) {
        printf("PASS: %s\n", case_name);
    } else {
        printf("FAIL: %s\n", case_name);
        failures++;
    }
}

/* Per-task stacks (large enough to hold an initial frame). */
static uint8_t stack0[256];
static uint8_t stack1[256];
static uint8_t stack2[256];

static void task_a(void) { for (;;) { } }
static void task_b(void) { for (;;) { } }
static void task_c(void) { for (;;) { } }

/* Case 1: add returns 0,1,2,... then -1 once the table is full. */
static void test_add_fill(void)
{
    int ok = 1;
    sched_init();
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        int id = sched_add_task(task_a, stack0, sizeof(stack0), "t");
        if (id != i) {
            ok = 0;
        }
    }
    /* Table now full: next add must fail. */
    if (sched_add_task(task_a, stack0, sizeof(stack0), "overflow") != -1) {
        ok = 0;
    }
    check("add returns 0..N-1 then -1 when full", ok);
}

/* Case 2: round-robin wrap across 3 tasks. */
static void test_round_robin(void)
{
    int ok = 1;
    sched_init();
    ok &= (sched_add_task(task_a, stack0, sizeof(stack0), "a") == 0);
    ok &= (sched_add_task(task_b, stack1, sizeof(stack1), "b") == 1);
    ok &= (sched_add_task(task_c, stack2, sizeof(stack2), "c") == 2);

    sched_test_set_current(0);
    ok &= (sched_pick_next() == 1);
    sched_test_set_current(1);
    ok &= (sched_pick_next() == 2);
    sched_test_set_current(2);
    ok &= (sched_pick_next() == 0);
    check("round-robin picks next runnable, wrapping", ok);
}

/* Case 3: a done task is skipped. */
static void test_done_skip(void)
{
    int ok = 1;
    sched_init();
    ok &= (sched_add_task(task_a, stack0, sizeof(stack0), "a") == 0);
    ok &= (sched_add_task(task_b, stack1, sizeof(stack1), "b") == 1);
    ok &= (sched_add_task(task_c, stack2, sizeof(stack2), "c") == 2);

    sched_test_mark_done(1);
    sched_test_set_current(0);
    ok &= (sched_pick_next() == 2);
    sched_test_set_current(2);
    ok &= (sched_pick_next() == 0);
    check("done task is skipped by pick_next", ok);
}

/* Case 4: a lone runnable task picks itself. */
static void test_single_runnable(void)
{
    int ok = 1;
    sched_init();
    ok &= (sched_add_task(task_a, stack0, sizeof(stack0), "a") == 0);

    sched_test_set_current(0);
    ok &= (sched_pick_next() == 0);
    check("single runnable task picks itself", ok);
}

int main(void)
{
    test_add_fill();
    test_round_robin();
    test_done_skip();
    test_single_runnable();

    if (failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", failures);
    return 1;
}
