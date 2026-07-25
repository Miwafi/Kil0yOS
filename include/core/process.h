#ifndef PROCESS_H
#define PROCESS_H

#include "lib/types.h"

/* Maximum number of processes */
#define MAX_PROCESSES 16

/* Process states */
typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_BLOCKED,
    PROCESS_STATE_ZOMBIE
} process_state_t;

/* User memory layout */
#define USER_CODE_BASE   0x00400000ULL   /* 4 MB - user code starts here */
#define USER_DATA_BASE   0x00800000ULL   /* 8 MB - user data starts here */
#define USER_STACK_BASE  0x7FFFF000ULL   /* ~2 GB - user stack (grows down) */
#define USER_STACK_SIZE  0x10000         /* 64 KB stack */

/* User program header format */
#define USER_MAGIC       0x4B494C30      /* "KIL0" */

typedef struct user_program_header {
    uint32_t magic;           /* Must be USER_MAGIC */
    uint32_t entry_offset;    /* Entry point offset from code base */
    uint32_t code_size;       /* Size of code segment */
    uint32_t data_size;       /* Size of data segment */
    uint32_t bss_size;        /* Size of BSS segment (zero-initialized) */
    uint32_t stack_size;      /* Requested stack size (0 = default) */
} user_program_header_t;

/* Process control block */
typedef struct process {
    uint32_t pid;
    process_state_t state;
    char name[32];

    /* Memory */
    uint64_t code_base;
    uint64_t data_base;
    uint64_t stack_top;

    /* Execution context */
    uint64_t entry_point;
    uint64_t kernel_stack;    /* Kernel stack for syscalls */

    /* Scheduling */
    uint64_t ticks_remaining;
    uint64_t total_ticks;

    /* File system */
    int current_dir_cluster;
} process_t;

/* Process management functions */
void process_init(void);
int process_create(const char* name, uint8_t* code, size_t code_size, uint64_t entry);
void process_run(uint32_t pid);
void process_exit(int status);
process_t* process_get_current(void);
process_t* process_get_by_pid(uint32_t pid);

/* Jump to user mode */
void jump_to_user(uint64_t entry, uint64_t stack);

/* Load program from file system */
int load_user_program(const char* path);

#endif