/* Wait queues for blocking Linux-ABI syscalls (P1.1). See wait.h. */
#include "sched/wait.h"
#include "sched/scheduler.h"
#include "core/tss.h"
#include "core/uvm.h"
#include "timer/pit.h"
#include "drivers/video/vga.h"

/* Global queues */
wait_queue_t g_sleep_wq;
wait_queue_t g_net_wq;
wait_queue_t g_tty_wq;
wait_queue_t g_poll_wq;
wait_queue_t g_unix_wq;
wait_queue_t g_pipe_wq;

/* One-shot wake timers, driven from the IRQ0 tick */
#define WAIT_TIMERS 64
typedef struct {
    int used;
    uint64_t deadline_us;
    wait_queue_t* wq;
    int kind;
    uint64_t ret;
} wait_timer_t;

static wait_timer_t timers[WAIT_TIMERS];

extern void restore_frame(uint64_t rsp) __attribute__((noreturn));  /* isr.asm */

static void wq_push(wait_queue_t* wq, process_t* p) {
    p->wait_next = NULL;
    if (wq->tail == NULL) {
        wq->head = wq->tail = p;
    } else {
        wq->tail->wait_next = p;
        wq->tail = p;
    }
}

static process_t* wq_pop(wait_queue_t* wq) {
    process_t* p = wq->head;
    if (p == NULL) return NULL;
    wq->head = p->wait_next;
    if (wq->head == NULL) wq->tail = NULL;
    p->wait_next = NULL;
    return p;
}

static void wq_unlink(wait_queue_t* wq, process_t* p) {
    process_t** pp = &wq->head;
    while (*pp) {
        if (*pp == p) {
            *pp = p->wait_next;
            if (wq->tail == p) {
                wq->tail = wq->head;
                while (wq->tail && wq->tail->wait_next) wq->tail = wq->tail->wait_next;
            }
            p->wait_next = NULL;
            return;
        }
        pp = &(*pp)->wait_next;
    }
}

static void wake_locked(process_t* p, int kind, uint64_t ret) {
    if (kind == WAIT_COMPLETE) {
        process_frame_set_ret(p, ret);
    }
    p->state = PROCESS_STATE_READY;
}

void wait_wake(wait_queue_t* wq, int max) {
    int n = 0;
    while (wq->head != NULL && (max < 0 || n < max)) {
        process_t* p = wq_pop(wq);
        if (p->state == PROCESS_STATE_BLOCKED) {
            wake_locked(p, WAIT_RESTART, 0);
            n++;
        }
    }
}

void wait_wake_status(wait_queue_t* wq, uint64_t ret) {
    while (wq->head != NULL) {
        process_t* p = wq_pop(wq);
        if (p->state == PROCESS_STATE_BLOCKED) {
            wake_locked(p, WAIT_COMPLETE, ret);
            return;
        }
    }
}

void wait_wake_proc(process_t* p) {
    if (p != NULL && p->state == PROCESS_STATE_BLOCKED) {
        p->state = PROCESS_STATE_READY;
    }
}

int wait_timer_arm(wait_queue_t* wq, uint64_t us, int kind, uint64_t ret) {
    for (int i = 0; i < WAIT_TIMERS; i++) {
        if (!timers[i].used) {
            timers[i].used = 1;
            timers[i].deadline_us = pit_uptime_us() + us;
            timers[i].wq = wq;
            timers[i].kind = kind;
            timers[i].ret = ret;
            return 0;
        }
    }
    return -1;
}

void wait_timers_poll(void) {
    uint64_t now = pit_uptime_us();
    for (int i = 0; i < WAIT_TIMERS; i++) {
        if (timers[i].used && now >= timers[i].deadline_us) {
            wait_queue_t* wq = timers[i].wq;
            int kind = timers[i].kind;
            uint64_t ret = timers[i].ret;
            timers[i].used = 0;
            /* Wake the first still-blocked waiter with the timer's flavor */
            process_t* p = wq->head;
            while (p != NULL) {
                if (p->state == PROCESS_STATE_BLOCKED) {
                    wq_unlink(wq, p);
                    wake_locked(p, kind, ret);
                    break;
                }
                p = p->wait_next;
            }
        }
    }
}

/* Hand the CPU away from a parked process. Never returns. */
static __attribute__((noreturn)) void park_switch(process_t* p) {
    /* Prefer another READY parked process (fork child, woken syscall) */
    process_t* next = process_pick_ready(p);
    if (next != NULL) {
        fpu_restore(next);
        process_apply_fs_base(next);
        process_become_current(next);
        tss_set_kernel_stack(next->kernel_stack);
        syscall_kernel_rsp = next->kernel_stack;
        if (next->mm && next->mm->cr3) vmm_switch_cr3(next->mm->cr3);
        scheduler_invalidate_user_frame();
        restore_frame(next->parked_rsp);
    }

    /* Otherwise fall back to the kernel-main task (shell/desktop) */
    process_set_current_none();
    scheduler_request_main_switch();
    __asm__ volatile("sti; nop; int $32");
    __builtin_unreachable();
}

/* Complete the park of the current process: it must already have its
 * resumable frame in parked_rsp, state BLOCKED, FPU saved, and the
 * caller must hold cli. Hands the CPU to another parked process or the
 * kernel main task; never returns. */
void wait_park_now(process_t* p) {
    park_switch(p);
}

void wait_block(uint64_t frame_rsp, wait_queue_t* wq,
                int kind, uint64_t ret, uint64_t timeout_us) {
    process_t* p = process_get_current();
    if (p == NULL) {
        /* No process context (should not happen): bounded spin */
        uint64_t deadline = pit_uptime_us() + (timeout_us ? timeout_us : 1000);
        while (pit_uptime_us() < deadline) __asm__ volatile("pause");
        /* Cannot return into the caller sanely; the caller retries via
         * restart semantics, so returning is acceptable here. */
        return;
    }

    __asm__ volatile("cli");

    int tmo_ok = (timeout_us != 0) && (wait_timer_arm(wq, timeout_us, kind, ret) == 0);

    if (timeout_us != 0 && !tmo_ok) {
        /* Timer pool exhausted: bounded spin without parking */
        uint64_t deadline = pit_uptime_us() + timeout_us;
        while (pit_uptime_us() < deadline) __asm__ volatile("pause");
        return;
    }

    process_park_entry_frame(p, frame_rsp,
                             kind == WAIT_RESTART, kind == WAIT_COMPLETE ? ret : 0);
    wq_push(wq, p);
    p->state = PROCESS_STATE_BLOCKED;
    fpu_save(p);                       /* park site: image valid for resume */

    park_switch(p);
}
