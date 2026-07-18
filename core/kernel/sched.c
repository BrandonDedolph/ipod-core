/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sched.c — cooperative round-robin scheduler core.
 *
 * Freestanding ARM7TDMI (PP5022). Pure: no libc, no hardware headers.
 * The context switch itself lives in switch.S; this file only manages the
 * task table and picks who runs next.
 */
#include "sched.h"

/* Provided by switch.S. */
extern void switch_context(uint32_t *old_sp_slot, uint32_t new_sp);
extern void task_trampoline(void);

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNABLE,
    TASK_DONE
} task_state_t;

typedef struct {
    uint32_t sp;          /* saved stack pointer (top-of-frame) */
    task_state_t state;
    const char *name;
} tcb_t;

static tcb_t tasks[SCHED_MAX_TASKS];
static int current = -1;

/*
 * Commit the `current` store to memory before a stack switch. `current`
 * has internal linkage and its address is never taken, so escape
 * analysis lets the compiler sink the `current = next` store past the
 * opaque switch_context() call (aggressively under -Os + LTO). If it
 * did, the incoming task would observe a stale `current` on its next
 * reschedule and overwrite the wrong task's saved sp — silent,
 * catastrophic corruption. This barrier orders the store before the
 * switch; it is the standard context-switch idiom.
 */
#define SCHED_BARRIER() __asm__ volatile("" ::: "memory")

/* Number of words in the initial context frame: r4-r11 (8) + lr (1). */
#define SCHED_FRAME_WORDS 9

void sched_init(void)
{
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        tasks[i].sp = 0;
        tasks[i].state = TASK_UNUSED;
        tasks[i].name = 0;
    }
    current = -1;
}

int sched_add_task(task_fn_t entry, void *stack_base, size_t stack_size,
                   const char *name)
{
    uintptr_t top;
    uint32_t *frame;
    int id = -1;

    if (entry == 0 || stack_base == 0) {
        return -1;
    }
    /* Need room for the frame plus up to 7 bytes of top alignment slack. */
    if (stack_size < (SCHED_FRAME_WORDS * sizeof(uint32_t)) + 7u) {
        return -1;
    }

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        return -1;
    }

    /* Carve the initial frame at the (8-byte-aligned) TOP of the stack. */
    top = (uintptr_t)stack_base + (uintptr_t)stack_size;
    top &= ~(uintptr_t)7u;                       /* 8-byte-align the top */
    frame = (uint32_t *)(top - SCHED_FRAME_WORDS * sizeof(uint32_t));

    /*
     * Frame layout, lowest address first, in ldmfd {r4-r11, lr} order:
     *   [0] = entry              -> r4  (task_trampoline branches to it)
     *   [1..7] = 0               -> r5-r11
     *   [8] = &task_trampoline   -> lr  (where the restore returns)
     */
    frame[0] = (uint32_t)(uintptr_t)entry;
    for (int w = 1; w <= 7; w++) {
        frame[w] = 0;
    }
    frame[8] = (uint32_t)(uintptr_t)&task_trampoline;

    tasks[id].sp = (uint32_t)(uintptr_t)frame;
    tasks[id].state = TASK_RUNNABLE;
    tasks[id].name = name;
    return id;
}

int sched_current(void)
{
    return current;
}

int sched_pick_next(void)
{
    if (current < 0) {
        /* No task running yet: return the first runnable, else -1. */
        for (int i = 0; i < SCHED_MAX_TASKS; i++) {
            if (tasks[i].state == TASK_RUNNABLE) {
                return i;
            }
        }
        return -1;
    }

    /* Scan all slots starting just after `current`, wrapping. */
    for (int off = 1; off <= SCHED_MAX_TASKS; off++) {
        int i = (current + off) % SCHED_MAX_TASKS;
        if (tasks[i].state == TASK_RUNNABLE) {
            return i;
        }
    }
    return -1;
}

void sched_yield(void)
{
    int prev = current;
    int next = sched_pick_next();

    /* prev < 0 means yield was called before sched_start(); guard the
     * tasks[prev] index rather than trust the caller's precondition. */
    if (prev < 0 || next < 0 || next == prev) {
        return;
    }
    current = next;
    SCHED_BARRIER();
    switch_context(&tasks[prev].sp, tasks[next].sp);
}

_Noreturn void sched_task_exit(void)
{
    int prev = current;
    int next;

    if (prev >= 0) {
        tasks[prev].state = TASK_DONE;
    }
    next = sched_pick_next();
    if (next >= 0 && next != prev) {
        current = next;
        SCHED_BARRIER();
        switch_context(&tasks[prev].sp, tasks[next].sp);
    }
    /* No other runnable task, or we were switched away and never resumed. */
    for (;;) {
    }
}

_Noreturn void sched_start(void)
{
    static uint32_t throwaway; /* boot context sink; never restored */
    int first = -1;

    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_RUNNABLE) {
            first = i;
            break;
        }
    }
    if (first < 0) {
        for (;;) {
        }
    }
    current = first;
    SCHED_BARRIER();
    switch_context(&throwaway, tasks[first].sp);
    for (;;) { /* unreachable */
    }
}

#ifdef SCHED_TEST
void sched_test_set_current(int id) { current = id; }
void sched_test_mark_done(int id) { tasks[id].state = TASK_DONE; }
#endif
