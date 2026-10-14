/* Wait queues for blocking Linux-ABI syscalls (P1.1).
 *
 * A syscall that cannot make progress parks its caller: the live
 * syscall-entry frame is converted into a resumable IRQ-layout frame
 * (process_park_entry_frame) and the process goes BLOCKED on a wait
 * queue. Two wake flavors:
 *   - WAIT_RESTART : the parked frame is left pristine (RIP rewound to
 *     the user `syscall` instruction), so resuming re-enters the syscall
 *     and re-evaluates the condition. Use for read/poll/accept/recv.
 *   - WAIT_COMPLETE: the parked RAX slot carries the final return value
 *     (e.g. nanosleep completing after its deadline).
 * Waking is passive: wait_wake() only marks processes READY; the
 * scheduler resumes them at the next tick boundary (or another parked
 * process hands over directly), always with IF=0 kernel discipline.
 *
 * All APIs must be called with interrupts disabled (IRQ handlers and
 * syscall context both qualify). Queues are static and zero-initialized.
 */
#ifndef SCHED_WAIT_H
#define SCHED_WAIT_H

#include "lib/types.h"
#include "core/process.h"

#define WAIT_RESTART   0
#define WAIT_COMPLETE  1

typedef struct wait_queue {
    process_t* head;            /* intrusive list via proc->wait_next */
    process_t* tail;
} wait_queue_t;

/* Global queues (defined in wait.c). */
extern wait_queue_t g_sleep_wq;   /* nanosleep completions   */
extern wait_queue_t g_net_wq;     /* tcp/udp data arrival    */
extern wait_queue_t g_tty_wq;     /* keyboard input          */
extern wait_queue_t g_poll_wq;    /* poll/ppoll/epoll wake   */
extern wait_queue_t g_unix_wq;    /* AF_UNIX readiness       */
extern wait_queue_t g_pipe_wq;    /* pipe/eventfd wake       */

/* Park the current process (must be inside a syscall with the live
 * entry-layout frame at frame_rsp). timeout_us==0 blocks forever;
 * otherwise a one-shot timer wakes the process at the deadline (kind/ret
 * from the timer, not from the block call). If the timer pool is
 * exhausted the call falls back to a bounded busy-wait and RETURNS with
 * the condition unsatisfied - callers must re-evaluate and fail. */
void wait_block(uint64_t frame_rsp, wait_queue_t* wq,
                int kind, uint64_t ret, uint64_t timeout_us);

/* Wake up to max waiters (max < 0 = all): READY with pristine frames. */
void wait_wake(wait_queue_t* wq, int max);

/* Wake the first waiter with ret stored into its parked frame RAX. */
void wait_wake_status(wait_queue_t* wq, uint64_t ret);

/* Mark a specific process READY (frames untouched). */
void wait_wake_proc(process_t* p);

/* One-shot timers: wake wq at now+us with kind/ret. Returns 0, or -1
 * when the pool is exhausted (caller falls back to busy-wait). */
int wait_timer_arm(wait_queue_t* wq, uint64_t us, int kind, uint64_t ret);

/* Expire due timers; called from the IRQ0 tick path with IF=0. */
void wait_timers_poll(void);

#endif /* SCHED_WAIT_H */
