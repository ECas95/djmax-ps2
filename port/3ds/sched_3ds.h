/* sched_3ds.h -- cooperative guest-thread scheduler on the ARM11.
 *
 * The recompiled C has no notion of a thread: guest state is psp_cpu (800 B)
 * and guest stack is psp_cpu.r[SP]. A PSP game creates its worker threads in
 * module_start and expects them to interleave at blocking firmware calls.
 *
 * On the 3DS there are no fibers either. This uses the
 * same idea with two ingredients:
 *   - a per-thread EE stack (malloc'd here) so each thread's C call chain is
 *     addressable on its own,
 *   - setjmp/longjmp, so a thread can be suspended inside a C call and resumed
 *     later at the same address.
 * psp_cpu is saved/restored around every switch, so guest-visible state is
 * exactly the per-thread snapshot.
 */
#ifndef DJMX_3DS_SCHED_H
#define DJMX_3DS_SCHED_H

#include <stdint.h>

#define PS2_THREAD_MAX      8
#define PS2_EE_STACK_SIZE   (64u * 1024u)

void ps2_sched_init(void);
void ps2_sched_dump_stacks(void);
void ps2_sched_dump_states(void);

/* Create returns 0 on success and writes a uid. Does not run anything. */
int  ps2_sched_create(uint32_t entry, const char *name, uint32_t prio,
                      uint32_t stack_size, uint32_t arglen, uint32_t arg,
                      uint32_t *uid);
int  ps2_sched_start(uint32_t uid, uint32_t arglen, uint32_t arg);

/* Yield to the next ready thread; returns when this thread resumes. */
void ps2_sched_yield(void);
int  ps2_sched_set_prio(uint32_t uid, uint32_t prio);
void ps2_sched_preempt(void);
/* Block forever (until the budget stops the run). */
void ps2_sched_block(void);
/* Terminate the current thread (never returns). */
void ps2_sched_exit(uint32_t status);
int  ps2_sched_delete(uint32_t uid);

int  ps2_sched_wait_dead(uint32_t uid);   /* 0 when dead, -1 unknown */
void ps2_sched_block_ef(uint32_t ef_uid);   /* block current thread on an event flag */
int  ps2_sched_wake_ef(uint32_t ef_uid);    /* wake threads blocked on it, returns count */

/* Run ready threads until none are left. Returns the number of switches. */
int  ps2_sched_pump(void);

/* Metrics. */
int  ps2_sched_in_thread(void);
int  ps2_sched_ready(void);
uint32_t ps2_sched_slice_cycles(void);   /* EE cycles the current thread has run since it was switched in */
int      ps2_sched_live(void);
int      ps2_sched_created(void);
uint32_t ps2_sched_current_uid(void);
const char *ps2_sched_current_name(void);

#endif /* DJMX_3DS_SCHED_H */
