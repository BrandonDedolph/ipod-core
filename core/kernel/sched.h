/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sched.h — cooperative scheduler core (public API)
 *
 * Freestanding ARM7TDMI (PP5022). No libc; <stdint.h>/<stddef.h> only.
 * Round-robin, cooperative (non-preemptive) task switching.
 */
#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <stdint.h>
#include <stddef.h>

typedef void (*task_fn_t)(void);

#define SCHED_MAX_TASKS 8

/* Zero the task table and reset the running-task marker. */
void sched_init(void);

/*
 * Register a task.
 *   entry      - task entry function.
 *   stack_base - LOW end of a caller-owned stack of stack_size bytes; the
 *                scheduler carves the initial context frame at the TOP.
 *   stack_size - size of that stack in bytes.
 *   name       - optional human-readable name (borrowed, not copied).
 * Returns task id >= 0, or -1 if the table is full or the args are bad.
 */
int sched_add_task(task_fn_t entry, void *stack_base, size_t stack_size,
                   const char *name);

/* Switch into the first runnable task. Never returns. */
_Noreturn void sched_start(void);

/* Cooperative reschedule; returns when the caller is scheduled again. */
void sched_yield(void);

/* A task's entry function returned: mark it done and reschedule. */
_Noreturn void sched_task_exit(void);

/* Running task id, or -1 before sched_start(). */
int sched_current(void);

/*
 * Round-robin policy: the next runnable id after `current` (wrapping).
 * Returns `current` if it is the only runnable task, or -1 if none.
 */
int sched_pick_next(void);

#endif /* KERNEL_SCHED_H */
