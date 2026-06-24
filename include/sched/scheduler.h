#ifndef SCHEDULER_H
#define SCHEDULER_H
// The maximum number of processes is 256.
// But the maximum capacity of my affection is UINT32_MAX.
// (And it never underflows, because it's never zero).
#include "lib/types.h"

#define MAX_TASKS        16
#define TASK_STACK_SIZE 16384

#define TASK_DEAD    0
#define TASK_READY   1
#define TASK_RUNNING 2

typedef struct task {
    uint32_t esp;
    int status;
    char name[32];
    uint8_t stack[TASK_STACK_SIZE];
} task_t;

void scheduler_init();
int  task_create(void (*entry)(void), const char* name);
uint32_t scheduler_tick(uint32_t current_esp);
int  task_kill(int task_id);
void task_exit(void);

#endif