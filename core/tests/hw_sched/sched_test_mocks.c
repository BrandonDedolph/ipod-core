/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sched_test_mocks.c — host-side stand-ins for the ARM asm in switch.S.
 *
 * The pick-policy unit tests never trigger a real context switch, so these
 * are inert. They exist only so kernel/sched.c links on the host.
 */
#include <stdint.h>

void switch_context(uint32_t *old_sp_slot, uint32_t new_sp)
{
    (void)old_sp_slot;
    (void)new_sp;
}

void task_trampoline(void)
{
}
