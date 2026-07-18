/* SPDX-License-Identifier: Apache-2.0 */
/*
 * switch.S — ARM context switch + initial-frame trampoline (ARM7TDMI).
 *
 * void switch_context(uint32_t *old_sp_slot, uint32_t new_sp);
 *   r0 = &tcb.sp of the outgoing task
 *   r1 = saved sp of the incoming task
 *
 * Callee-saved registers (r4-r11) plus lr are pushed onto the outgoing
 * stack, its sp stashed, then the incoming sp is loaded and its frame
 * popped. `bx lr` resumes wherever the incoming task last left off.
 */
    .text
    .arm
    .syntax unified

    .global switch_context
    .type switch_context, %function
switch_context:
    stmfd   sp!, {r4-r11, lr}   @ save callee-saved regs + return address
    str     sp, [r0]            @ *old_sp_slot = sp
    mov     sp, r1              @ sp = new_sp
    ldmfd   sp!, {r4-r11, lr}   @ restore incoming context
    bx      lr                  @ resume incoming task
    .size switch_context, . - switch_context

/*
 * void task_trampoline(void);
 *
 * Launch pad for a freshly-created task. The initial frame places the task
 * entry pointer in r4 and this routine's address in lr, so the very first
 * restore "returns" here. We set lr to sched_task_exit (so a task that ever
 * returns lands there) and branch to the entry function in r4.
 */
    .global task_trampoline
    .type task_trampoline, %function
task_trampoline:
    ldr     lr, =sched_task_exit  @ if entry returns, fall into task exit
    bx      r4                    @ jump to task entry
    .size task_trampoline, . - task_trampoline
