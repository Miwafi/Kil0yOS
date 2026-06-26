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
} task_t;

void scheduler_init();
int  task_create(void (*entry)(void), const char* name);
uint64_t scheduler_tick(uint64_t current_rsp);
int  task_kill(int task_id);
void task_exit(void);

int task_get_count(void);
const char* task_get_name(int idx);
int task_get_status(int idx);
const char* task_status_str(int status);

#endif
