#include "sched/scheduler.h"
#include "lib/string.h"

static task_t tasks[MAX_TASKS];
static int task_count = 0;
static int current_task_idx = 0;

void scheduler_init() {
    task_count = 1;
    current_task_idx = 0;

    tasks[0].status = TASK_READY;
    tasks[0].esp = 0;  // Captured on first context switch
    strcpy(tasks[0].name, "kernel_main");

    for (int i = 1; i < MAX_TASKS; i++) {
        tasks[i].status = TASK_DEAD;
    }
}

/*
 * Set up a synthetic interrupt frame on the new task's stack.
 * When irq_common_stub pops this frame and irets, execution
 * starts at 'entry'.
 *
 * Stack layout (low → high address):
 *   [esp]     GS, FS, ES, DS          (pop gs/fs/es/ds)
 *            EDI, ESI, EBP, ESP,      (popa)
 *            EBX, EDX, ECX, EAX
 *            error_code                (add esp, 8)
 *            interrupt_number
 *            EIP                       (iret)
 *            CS
 *            EFLAGS
 */
static void setup_task_stack(task_t* task, void (*entry)(void)) {
    uint32_t* sp = (uint32_t*)(task->stack + TASK_STACK_SIZE);

    // Align
    sp = (uint32_t*)((uint32_t)sp & ~3u);

    // Hardware frame – iret pops EIP, CS, EFLAGS
    *--sp = (uint32_t)entry;        // EIP
    *--sp = 0x08;                   // CS – kernel code segment
    *--sp = 0x202;                  // EFLAGS (IF = 1)

    // Pushed by ISR/IRQ macro
    *--sp = 32;                     // interrupt_number (IRQ0 → IDT32)
    *--sp = 0;                      // error_code

    // pusha values – popa order: EDI, ESI, EBP, ESP(ignored), EBX, EDX, ECX, EAX
    *--sp = 0;  // EDI
    *--sp = 0;  // ESI
    *--sp = 0;  // EBP
    *--sp = 0;  // ESP (original – unused)
    *--sp = 0;  // EBX
    *--sp = 0;  // ECX
    *--sp = 0;  // EDX
    *--sp = 0;  // EAX

    // Segment registers – pop order: GS, FS, ES, DS
    *--sp = 0x10;  // GS
    *--sp = 0x10;  // FS
    *--sp = 0x10;  // ES
    *--sp = 0x10;  // DS

    // When irq_handler / irq_common_stub return, ESP will point here.
    task->esp = (uint32_t)sp;
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

/*
 * Called from the timer (IRQ 0) handler inside irq_handler().
 * 'current_esp' points to the interrupt frame on the current
 * task's stack.
 *
 * Returns the ESP of the next task to run.  irq_common_stub
 * will switch to it via "mov esp, eax".
 */
uint32_t scheduler_tick(uint32_t current_esp) {
    // Save current task's stack pointer
    tasks[current_task_idx].esp = current_esp;

    if (task_count <= 1) {
        // Only the kernel-main context – no switching
        return tasks[current_task_idx].esp;
    }

    // Round-robin: find next READY task
    int next = current_task_idx;
    do {
        next = (next + 1) % MAX_TASKS;
    } while (next != current_task_idx &&
             tasks[next].status != TASK_READY &&
             tasks[next].status != TASK_RUNNING);

    current_task_idx = next;
    return tasks[current_task_idx].esp;
}

/*
 * Kill a task by ID.
 * Marks the task as DEAD and frees its resources.
 */
int task_kill(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return -1;
    if (tasks[task_id].status == TASK_DEAD) return -1;

    tasks[task_id].status = TASK_DEAD;
    task_count--;

    return 0;
}

/*
 * Exit the current task.
 * Marks the current task as DEAD and triggers a context switch.
 */
void task_exit(void) {
    if (current_task_idx >= 0 && current_task_idx < MAX_TASKS) {
        tasks[current_task_idx].status = TASK_DEAD;
        task_count--;
    }

    /* Force context switch to next task */
    __asm__ volatile("int $32"); /* Trigger timer interrupt */
    __builtin_unreachable();
}