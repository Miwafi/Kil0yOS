#include "sched/scheduler.h"
#include "lib/string.h"

static task_t tasks[MAX_TASKS];
static int task_count = 0;
static int current_task_idx = 0;

volatile uint64_t cpu_busy_ticks = 0;
volatile uint64_t cpu_idle_ticks = 0;

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

    // Hardware frame – iretq pops RIP, CS, RFLAGS
    *--sp = 0x202;                  // RFLAGS (IF = 1)
    *--sp = 0x08;                   // CS – kernel code segment
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
    setup_task_stack(&tasks[idx], entry);

    task_count++;
    return idx;
}

uint64_t scheduler_tick(uint64_t current_rsp) {
    tasks[current_task_idx].rsp = current_rsp;

    if (task_count <= 1) {
        cpu_idle_ticks++;
        return tasks[current_task_idx].rsp;
    }

    cpu_busy_ticks++;

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
