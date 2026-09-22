#include "sched/scheduler.h"
#include "core/process.h"
#include "core/isr.h"
#include "core/tss.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "timer/pit.h"

static task_t tasks[MAX_TASKS];
static int task_count = 0;
static int current_task_idx = 0;

volatile uint64_t cpu_busy_ticks = 0;
volatile uint64_t cpu_idle_ticks = 0;

static void setup_task_stack(task_t* task, void (*entry)(void));

/* --- User process time slicing ---
 * While a user process is RUNNING, IRQ0 alternates between the user
 * process frame (parked in user_frame) and the kernel main task frame
 * (tasks[0].rsp). Exit from user mode goes through
 * scheduler_request_main_switch() so the dying process's frame is never
 * resumed. */
static uint64_t user_frame = 0;
static int user_frame_valid = 0;
static volatile int main_switch_requested = 0;

void scheduler_request_main_switch(void) {
    main_switch_requested = 1;
}

/* Drop the parked user-process frame: the process it belonged to has
 * been resumed directly (wait4 exit path / exec), so the next tick
 * must not iretq into the stale snapshot. */
void scheduler_invalidate_user_frame(void) {
    user_frame_valid = 0;
}

void scheduler_set_main_return(void (*entry)(void)) {
    /* Rebuild tasks[0]'s synthetic frame at the top of the scheduler's
     * dedicated stack. From this point the kernel main task runs there,
     * so every tick's save/restore of tasks[0].rsp stays consistent. */
    setup_task_stack(&tasks[0], entry);
    tasks[0].status = TASK_READY;
}

uint64_t scheduler_main_return_rsp(void) {
    return tasks[0].rsp;
}

void scheduler_init() {
    task_count = 1;
    current_task_idx = 0;

    tasks[0].status = TASK_READY;
    tasks[0].rsp = 0;
    strcpy(tasks[0].name, "kernel_main");

    for (int i = 1; i < MAX_TASKS; i++) {
        tasks[i].status = TASK_DEAD;
    }
}

/*
 * Set up a synthetic interrupt frame on the new task's stack.
 * When irq_common_stub pops this frame and iretq, execution
 * starts at 'entry'.
 *
 * Stack layout (low -> high address, matching irq_common_stub):
 *   [rsp]     rax, rbx, rcx, rdx, rsi, rdi, rbp
 *             r8, r9, r10, r11, r12, r13, r14, r15
 *             error_code
 *             interrupt_number
 *             rip                       (iretq)
 *             cs
 *             rflags
 */
static void setup_task_stack(task_t* task, void (*entry)(void)) {
    uint64_t* sp = (uint64_t*)(task->stack + TASK_STACK_SIZE);

    // 16-byte align
    sp = (uint64_t*)((uint64_t)sp & ~0xFull);
    uint64_t frame_top = (uint64_t)sp;

    // Hardware frame – iretq pops RIP, CS, RFLAGS. SS:RSP are appended so
    // that even a 5-word iretq pop loads a VALID ring-0 stack instead of
    // reading the zeroed BSS above the frame (RSP=0/SS=0 -> instant triple
    // fault). A 3-word pop leaves RSP at the RSP slot (top of this stack)
    // - equally valid.
    *--sp = 0x10;                   // SS - kernel data
    *--sp = frame_top;              // RSP - top of this task stack
    *--sp = 0x202;                  // RFLAGS (IF = 1)
    *--sp = 0x08;                   // CS - kernel code segment
    *--sp = (uint64_t)entry;        // RIP

    // Pushed by ISR/IRQ macro
    *--sp = 32;                     // interrupt_number (IRQ0 -> IDT 32)
    *--sp = 0;                      // error_code

    // pusha values – pop order: r15, r14, ..., r8, rbp, rdi, rsi, rdx, rcx, rbx, rax
    *--sp = 0;  // r15
    *--sp = 0;  // r14
    *--sp = 0;  // r13
    *--sp = 0;  // r12
    *--sp = 0;  // r11
    *--sp = 0;  // r10
    *--sp = 0;  // r9
    *--sp = 0;  // r8
    *--sp = 0;  // rbp
    *--sp = 0;  // rdi
    *--sp = 0;  // rsi
    *--sp = 0;  // rdx
    *--sp = 0;  // rcx
    *--sp = 0;  // rbx
    *--sp = 0;  // rax

    task->rsp = (uint64_t)sp;
}

int task_create(void (*entry)(void), const char* name) {
    if (entry == NULL || name == NULL) return -1;

    int idx = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].status == TASK_DEAD) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    tasks[idx].status = TASK_READY;
    strcpy(tasks[idx].name, name);
    tasks[idx].background = 0;
    setup_task_stack(&tasks[idx], entry);

    task_count++;
    return idx;
}

int task_create_bg(void (*entry)(void), const char* name) {
    int idx = task_create(entry, name);
    if (idx >= 0) tasks[idx].background = 1;
    return idx;
}

uint64_t scheduler_tick(uint64_t current_rsp) {
    /* A user process just exited: resume the kernel main task from its
     * last saved frame; never touch the dying process's frame. */
    if (main_switch_requested) {
        main_switch_requested = 0;
        user_frame_valid = 0;
        return tasks[0].rsp;
    }

    /* Time-share between a running user process and the kernel main task */
    process_t* uproc = process_get_current();
    if (uproc != NULL && uproc->state == PROCESS_STATE_RUNNING) {
        if (user_frame_valid) {
            /* current frame belongs to the kernel main task */
            tasks[0].rsp = current_rsp;
            user_frame_valid = 0;

            /* Phase 1.5 round-robin: a forked child (or a preempted
             * process) may hold a parked frame - give it the CPU
             * instead of resuming the same user process. */
            process_t* next = process_pick_ready(uproc->pid);
            if (next != NULL) {
                uproc->parked_rsp = user_frame;   /* park, do not lose */
                uproc->state = PROCESS_STATE_READY;
                process_become_current(next);
                tss_set_kernel_stack(next->kernel_stack);
                syscall_kernel_rsp = next->kernel_stack;
                if (next->cr3) vmm_switch_cr3(next->cr3);
                return next->parked_rsp;
            }
            return user_frame;              /* resume user process */
        } else {
            /* current frame belongs to the user process (or its syscall) */
            user_frame = current_rsp;
            user_frame_valid = 1;
            return tasks[0].rsp;            /* run kernel main */
        }
    }
    user_frame_valid = 0;  /* no user process: stale frame, drop it */

    tasks[current_task_idx].rsp = current_rsp;

    if (task_count <= 1) {
        cpu_idle_ticks++;
        return tasks[current_task_idx].rsp;
    }

    /* Busy/idle accounting ignores background kernel threads: a tick that
     * only alternates kernel_main with idle kthreads (watchdog) is still
     * logically idle, otherwise the System Monitor would pin at 100 %. */
    int fg = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].status != TASK_DEAD && !tasks[i].background) fg++;
    }
    if (fg == 0) cpu_idle_ticks++; else cpu_busy_ticks++;

    int next = current_task_idx;
    do {
        next = (next + 1) % MAX_TASKS;
    } while (next != current_task_idx &&
             tasks[next].status != TASK_READY &&
             tasks[next].status != TASK_RUNNING);

    current_task_idx = next;
    return tasks[current_task_idx].rsp;
}

int task_kill(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;
    if (tasks[task_id].status == TASK_DEAD) return -1;

    tasks[task_id].status = TASK_DEAD;
    task_count--;

    return 0;
}

void task_exit(void) {
    if (current_task_idx >= 0 && current_task_idx < MAX_TASKS) {
        tasks[current_task_idx].status = TASK_DEAD;
        task_count--;
    }

    __asm__ volatile("int $32");
    __builtin_unreachable();
}

int task_get_count(void) {
    return task_count;
}

const char* task_get_name(int idx) {
    if (idx < 0 || idx >= MAX_TASKS) return "???";
    return tasks[idx].name;
}

int task_get_status(int idx) {
    if (idx < 0 || idx >= MAX_TASKS) return TASK_DEAD;
    return tasks[idx].status;
}

const char* task_status_str(int status) {
    switch (status) {
        case TASK_READY:   return "Ready";
        case TASK_RUNNING: return "Running";
        case TASK_DEAD:    return "Dead";
        default:           return "???";
    }
}

/* =====================  Kernel heartbeat watchdog  ====================== */

/* Touched by the kernel main task on every pass through its loops (shell
 * prompt, desktop loop, keyboard wait, busy delays, tar extraction...).
 * A frozen counter means the main task stopped making forward progress.
 * IRQ0 firing deliberately does NOT advance it: a main task wedged in a
 * spin with IF=1 still gets preempted, but its loops never touch. */
static volatile uint64_t kernel_heartbeat = 0;

void kernel_heartbeat_touch(void) {
    kernel_heartbeat++;
}

uint64_t kernel_heartbeat_read(void) {
    return kernel_heartbeat;
}

#define HEARTBEAT_CHECK_INTERVAL_US  (10ULL * 1000000ULL)   /* 10 s */

/* kwatchdog: independent kernel thread, samples the heartbeat every 10 s
 * and panics when the main task has shown no progress in that window.
 *
 * Scope / known limits, by design:
 *  - While a user process runs, kernel main is legitimately parked (wait4,
 *    shell hlt-wait) and stops touching - checks are skipped then
 *    (process_any_active) and monitoring resumes once the process exits.
 *  - A wedge with interrupts disabled stops IRQ0 itself, so this thread
 *    can no longer run; that hang class is covered by the NMI watchdog
 *    (core/nmi_wdt.c - APIC timer NMI delivery is not maskable by cli). */
static void heartbeat_watchdog_main(void) {
    uint64_t last_seen = kernel_heartbeat;

    for (;;) {
        uint64_t deadline = pit_uptime_us() + HEARTBEAT_CHECK_INTERVAL_US;
        while (pit_uptime_us() < deadline) {
            __asm__ volatile("hlt");
        }

        /* A user process owns the CPU time right now: kernel main is
         * parked on purpose - not a hang. Re-arm and keep waiting. */
        if (process_any_active()) continue;

        if (kernel_heartbeat == last_seen) {
            char msg[96];
            char num[16];
            strcpy(msg, "kernel heartbeat stalled: main task unresponsive >10s (count=");
            utoa((uint32_t)last_seen, num, 10, sizeof(num));
            strcat(msg, num);
            strcat(msg, ", uptime=");
            utoa((uint32_t)(pit_uptime_us() / 1000000ULL), num, 10, sizeof(num));
            strcat(msg, num);
            strcat(msg, "s)");
            PANIC(msg);
        }
        last_seen = kernel_heartbeat;
    }
}

void heartbeat_watchdog_init(void) {
    task_create_bg(heartbeat_watchdog_main, "kwatchdog");
}
