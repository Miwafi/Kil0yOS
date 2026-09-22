#ifndef SCHEDULER_H
#define SCHEDULER_H
#include "lib/types.h"

#define MAX_TASKS        16
#define TASK_STACK_SIZE  32768

#define TASK_DEAD    0
#define TASK_READY   1
#define TASK_RUNNING 2

typedef struct task {
    uint64_t rsp;
    int status;
    char name[32];
    uint8_t stack[TASK_STACK_SIZE];
    uint8_t background;   /* kernel thread: excluded from idle/busy accounting */
} task_t;

void scheduler_init();
int  task_create(void (*entry)(void), const char* name);
/* Background kernel thread (e.g. watchdog): scheduled like any task but
 * its existence does not make idle ticks count as "busy". */
int  task_create_bg(void (*entry)(void), const char* name);
uint64_t scheduler_tick(uint64_t current_rsp);
int  task_kill(int task_id);
void task_exit(void);

/* Kernel heartbeat: the kernel main task touches this counter on every
 * pass through its loops (shell prompt, desktop loop, keyboard wait, ...).
 * The watchdog thread samples it every 10 s; a frozen counter means the
 * main task stopped making progress -> kernel panic. */
void kernel_heartbeat_touch(void);
uint64_t kernel_heartbeat_read(void);

/* Create the heartbeat watchdog kernel thread ("kwatchdog"). */
void heartbeat_watchdog_init(void);

/* Called by process_exit(): next scheduler_tick() must return the saved
 * kernel-main frame instead of the dying user process frame. */
void scheduler_request_main_switch(void);

/* Discard the parked user frame (process resumed outside the tick path
 * or replaced by exec). */
void scheduler_invalidate_user_frame(void);

/* Arm a FRESH kernel-main entry frame on the scheduler's own stack.
 * Must be called right before jump_to_user(): the old tasks[0].rsp frame
 * is a snapshot of the boot stack at some earlier tick depth and has been
 * clobbered by the shell's deeper calls (exec path), so restoring it
 * after process exit would iretq into garbage. */
void scheduler_set_main_return(void (*entry)(void));

/* Current saved kernel-main frame rsp (the resume point used after a
 * user process exits or is killed by a fault). */
uint64_t scheduler_main_return_rsp(void);

int task_get_count(void);
const char* task_get_name(int idx);
int task_get_status(int idx);
const char* task_status_str(int status);

#endif
