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

/* User memory layout
 * USER_CODE_BASE must sit ABOVE the kernel heap arena: the heap is
 * identity-mapped in the shared kernel page tables (0x200000..0x10000000),
 * and vmm_map_page() rewrites those same tables. Mapping user code below
 * heap_end would redirect heap VAs to random physical pages and corrupt
 * the heap free list (kfree #GP). 0x10000000 = heap_end exactly. */
#define USER_CODE_BASE   0x10000000ULL   /* 256 MB - above kernel heap arena */
#define USER_DATA_BASE   0x11000000ULL   /* user data follows code region */
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
    uint32_t code_pages;      /* mapped code page count */
    uint32_t stack_pages;     /* mapped stack page count */

    /* Execution context */
    uint64_t entry_point;
    uint64_t kernel_stack;    /* Kernel stack for syscalls */

    /* Scheduling */
    uint64_t ticks_remaining;
    uint64_t total_ticks;

    /* Termination */
    int exit_status;

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

/* Free resources of exited processes; call before creating new ones */
void process_reap_zombies(void);

/* True if any user process is loaded (READY/RUNNING/BLOCKED) */
int process_any_active(void);

/* Copy built-in user programs (hello.bin) into /bin */
void user_programs_install(void);

/* Jump to user mode */
void jump_to_user(uint64_t entry, uint64_t stack);

/* Kill the current user process from a fault context (e.g. ring3
 * exception in isr_handler). Marks it ZOMBIE and returns the new kernel
 * rsp to resume on (the saved kernel-main frame), or 0 if no process. */
uint64_t process_kill_current(int status);

/* True if [uaddr, uaddr+len) lies entirely inside the current process's
 * mapped user regions (code or stack) and every touched page is present.
 * Used by syscalls to reject user-supplied pointers before dereference. */
int process_check_user_range(uint64_t uaddr, size_t len);

/* Load program from file system */
int load_user_program(const char* path);

#endif