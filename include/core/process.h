#ifndef PROCESS_H
#define PROCESS_H

#include "lib/types.h"
#include "core/lnxvfs.h"

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

/* Per-process user VM regions (ELF segments, brk heap, mmap arena, stack).
 * The whole user address space lives in the shared kernel page tables, so
 * regions exist only to track ownership for syscalls and teardown. */
#define MAX_VM_REGIONS 24

typedef struct {
    int used;
    uint64_t start;   /* first mapped byte */
    uint64_t end;     /* one past last mapped byte */
    uint32_t prot;    /* UVM_PROT_* */
} vm_region_t;

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
    uint32_t code_pages;      /* mapped code page count (raw binary path) */
    uint32_t stack_pages;     /* mapped stack page count */

    /* User VM (ELF / mmap / brk bookkeeping) */
    vm_region_t regions[MAX_VM_REGIONS];
    uint64_t brk_start;       /* heap base = page-aligned end of ELF image */
    uint64_t brk_cur;         /* current program break */
    uint64_t mmap_top;        /* bump allocator top of the mmap arena */
    uint64_t fs_base;         /* ARCH_SET_FS value (TLS pointer) */

    /* Execution context */
    uint64_t entry_point;
    uint64_t user_rsp;        /* initial user RSP (ELF path: argc/auxv block) */
    uint64_t kernel_stack;    /* Kernel stack for syscalls */

    /* Scheduling */
    uint64_t ticks_remaining;
    uint64_t total_ticks;

    /* Termination */
    int exit_status;

    /* File system */
    int current_dir_cluster;

    /* Linux-ABI open file table (fd 0/1/2 = console, never in the array) */
    struct lnx_file* fds[LNX_MAX_FDS];

    /* Address space + fork/wait state (Phase 1.5). cr3 is the phys of
     * this process's PML4 (0 = legacy kernel-shared). parked_rsp holds a
     * resumable IRQ-frame while the process is blocked (wait4). */
    uint64_t cr3;
    uint64_t parked_rsp;
    int parent_pid;
    int wait_pid;             /* -1 = any child */
    uint64_t wait_status_ptr; /* user pointer for the wait4 status int */
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

/* Free every resource of an exited process and release its slot */
void process_reap(process_t* proc);

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

/* Unified exec path (Phase 0.6): detects ELF64 / KIL0 / raw binary,
 * builds the process, and lays out argc/argv/envp/auxv on the user stack.
 * argv points to kernel strings. Returns the new pid or -1. */
int exec_load_program(const char* path, char* const* argv, int argc);

/* execve (Phase 1.5): replace the CURRENT process image with a new ELF.
 * frame_rsp points at the live syscall-entry frame; on success its
 * RIP/RSP/RAX fields are rewritten so the syscall epilogue's iretq lands
 * directly in the new program. Returns 0, or -1 (old image intact). */
int exec_replace(uint64_t frame_rsp, const char* path,
                 char* const* argv, int argc);

/* fork/wait4 (Phase 1.5). frame_rsp points at the caller's syscall-entry
 * frame holding the user context to clone / park. */
int  process_fork(uint64_t frame_rsp);
void process_wait_run(process_t* child, uint64_t status_ptr, uint64_t frame_rsp);
process_t* process_find_waiter(int child_pid);
process_t* process_find_child(process_t* parent, int pid);

/* Write an int into a user VA of the given process (wait4 status) */
void process_write_user_int(process_t* p, uint64_t uaddr, int val);

/* Scheduler helper: another READY process holding a parked user frame
 * (fork child / preempted parent), or NULL. */
process_t* process_pick_ready(uint32_t exclude_pid);

/* Scheduler hook: mark the given process RUNNING and current */
void process_become_current(process_t* proc);

/* Top of the current process's kernel stack; the Linux-ABI `syscall`
 * entry switches to it because the syscall instruction does not load
 * RSP0 from the TSS the way an interrupt does. */
extern uint64_t syscall_kernel_rsp;

#endif