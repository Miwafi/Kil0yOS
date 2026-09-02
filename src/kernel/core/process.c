#include "core/process.h"
#include "core/gdt.h"
#include "core/tss.h"
#include "core/uvm.h"
#include "core/elf.h"
#include "sched/scheduler.h"
#include "shell/shell.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "fs/fs.h"
#include "timer/pit.h"
#include "drivers/vga.h"

/* Process table */
static process_t processes[MAX_PROCESSES];
static int current_process = -1;
static uint32_t next_pid = 1;

/* Top of the current process's kernel stack. The Linux-ABI `syscall`
 * entry cannot use the TSS (the instruction does not switch stacks), so
 * it loads its kernel RSP from here. Set in process_run(). */
uint64_t syscall_kernel_rsp = 0;

/* Kernel stack for user mode transitions (16 KB) */
#define KERNEL_STACK_SIZE 0x4000
static uint8_t kernel_stacks[MAX_PROCESSES][KERNEL_STACK_SIZE] __attribute__((aligned(16)));

static void process_release_memory(process_t* proc);
process_t* process_find_waiter(int child_pid);
process_t* process_find_child(process_t* parent, int pid);
extern void restore_frame(uint64_t rsp);   /* isr.asm */

/* --- Saved-frame layouts -------------------------------------------------
 * syscall_lnx_entry frame (indexes of uint64_t from RSP):
 *   0..14  r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax
 *   15     user RIP    16 CS(0x1B)   17 RFLAGS   18 user RSP   19 SS(0x23)
 * irq_common_stub frame:
 *   0..14  same register order
 *   15     int num     16 error code 17 RIP      18 CS         19 RFLAGS
 *   20     RSP         21 SS
 * ----------------------------------------------------------------------- */
#define IRQ_INTNUM 15
#define IRQ_ERR    16
#define IRQ_RIP    17
#define IRQ_CS     18
#define IRQ_RFLAGS 19
#define IRQ_RSP    20
#define IRQ_SS     21
#define IRQ_RAX    14
#define FRAME_QWORDS 22
#define FRAME_BYTES  (FRAME_QWORDS * 8)

#define ENTY_RIP    15
#define ENTY_CS     16
#define ENTY_RFLAGS 17
#define ENTY_RSP    18
#define ENTY_SS     19

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

#define MSR_FS_BASE 0xC0000100

void process_init(void) {
    memset(processes, 0, sizeof(processes));
    current_process = -1;
    next_pid = 1;
}

static process_t* find_free_process(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROCESS_STATE_UNUSED) {
            return &processes[i];
        }
    }
    return NULL;
}

int process_create(const char* name, uint8_t* code, size_t code_size, uint64_t entry) {
    process_t* proc = find_free_process();
    if (proc == NULL) {
        vga_puts("[proc] create failed: no free process slot\n");
        return -1;  /* No free process slots */
    }

    /* Initialize process. NOTE: state stays UNUSED (0) until every
     * resource is allocated - a half-initialized process must never be
     * visible as READY, or the shell's process_any_active() check will
     * hang the input loop forever. */
    memset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    strncpy(proc->name, name, sizeof(proc->name) - 1);

    /* Set up kernel stack for this process (used during syscalls) */
    int proc_index = proc - processes;
    proc->kernel_stack = (uint64_t)&kernel_stacks[proc_index][KERNEL_STACK_SIZE];

    proc->wait_pid = 0;
    proc->cr3 = vmm_create_address_space();
    if (proc->cr3 == 0) {
        memset(proc, 0, sizeof(process_t));
        vga_puts("[proc] create failed: no address space\n");
        return -1;
    }

    /* Legacy raw-binary layout: one RWX code image at USER_CODE_BASE
     * (above the kernel heap arena) plus the user stack, tracked as
     * ordinary uvm regions so teardown/exit works for every format. */
    proc->code_base = USER_CODE_BASE;
    proc->stack_top = USER_STACK_BASE;

    if (uvm_map_range(proc, USER_CODE_BASE, USER_CODE_BASE + code_size,
                      UVM_PROT_READ | UVM_PROT_WRITE | UVM_PROT_EXEC) != 0) {
        vga_puts("[proc] create failed: PMM out of pages (code)\n");
        goto fail;
    }
    proc->code_pages = (uint32_t)((code_size + 4095) / 4096);
    uvm_write_user_va(proc, USER_CODE_BASE, code, code_size);

    if (uvm_map_range(proc, USER_STACK_BASE - USER_STACK_SIZE, USER_STACK_BASE,
                      UVM_PROT_READ | UVM_PROT_WRITE) != 0) {
        vga_puts("[proc] create failed: PMM out of pages (stack)\n");
        goto fail;
    }
    proc->stack_pages = USER_STACK_SIZE / 4096;

    proc->brk_start = proc->brk_cur = (USER_CODE_BASE + code_size + 4095) & ~4095ULL;
    proc->mmap_top = UVM_MMAP_BASE;

    proc->entry_point = entry;
    proc->state = PROCESS_STATE_READY;

    klog("[create] code mapped\n");
    return proc->pid;

fail:
    /* Undo partial allocation so no half-initialized process remains */
    uvm_release_all(proc);
    vmm_destroy_address_space(proc->cr3);
    memset(proc, 0, sizeof(process_t));
    return -1;
}

/* Release all user pages of a process and free its slot.
 * Only safe for processes that are not running. */
static void process_release_memory(process_t* proc) {
    uvm_release_all(proc);
    proc->code_pages = 0;
    proc->stack_pages = 0;
}

void process_reap_zombies(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROCESS_STATE_ZOMBIE) {
            process_reap(&processes[i]);
        }
    }
}

/* Free every resource of an exited process and release its slot
 * (wait4 reaping). The process must not be running. */
void process_reap(process_t* proc) {
    process_release_memory(proc);
    if (proc->cr3) {
        vmm_destroy_address_space(proc->cr3);
        proc->cr3 = 0;
    }
    proc->parked_rsp = 0;
    proc->state = PROCESS_STATE_UNUSED;
}

int process_any_active(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_state_t s = processes[i].state;
        if (s == PROCESS_STATE_READY || s == PROCESS_STATE_RUNNING ||
            s == PROCESS_STATE_BLOCKED) {
            return 1;
        }
    }
    return 0;
}

uint64_t process_kill_current(int status) {
    process_t* proc = process_get_current();
    if (proc == NULL) return 0;

    /* Flush and free the process's open files */
    lnxvfs_close_all(proc);

    /* Same IRQ0-exclusion rule as process_exit(): no tick may observe the
     * half-updated state. isr_handler already runs with IF=0 (interrupt
     * gate), and the stub will iretq to the frame we return. */
    proc->state = PROCESS_STATE_ZOMBIE;
    proc->exit_status = status;
    current_process = -1;

    scheduler_request_main_switch();
    return scheduler_main_return_rsp();
}

int process_check_user_range(uint64_t uaddr, size_t len) {
    /* Generic region-based check: covers ELF segments, brk heap, mmap
     * arena and the stack alike. */
    return uvm_check_range(process_get_current(), uaddr, len);
}

void process_exit(int status) {
    if (current_process < 0) return;

    process_t* proc = &processes[current_process];

    /* Flush and free the process's open files before anything else */
    lnxvfs_close_all(proc);

    /* A parent blocked in wait4 resumes directly from here with the
     * child's status delivered into its parked frame. */
    process_t* par = process_find_waiter(proc->pid);
    if (par != NULL) {
        __asm__ volatile("cli");

        proc->state = PROCESS_STATE_ZOMBIE;
        proc->exit_status = status;

        /* wait4 status format: exit code in bits 15..8 */
        if (par->wait_status_ptr) {
            process_write_user_int(par, par->wait_status_ptr,
                                   (status & 0xFF) << 8);
        }
        uint64_t* f = (uint64_t*)par->parked_rsp;
        f[IRQ_RAX] = proc->pid;              /* wait4() returns child pid */

        current_process = par - processes;
        par->state = PROCESS_STATE_RUNNING;
        par->wait_pid = 0;
        tss_set_kernel_stack(par->kernel_stack);
        syscall_kernel_rsp = par->kernel_stack;
        if (par->cr3) vmm_switch_cr3(par->cr3);
        scheduler_invalidate_user_frame();

        restore_frame(par->parked_rsp);      /* never returns */
    }

    /* Atomic w.r.t. IRQ0: no timer tick may observe the half-updated
     * state below and save this dying frame into tasks[0].rsp. */
    __asm__ volatile("cli");

    proc->state = PROCESS_STATE_ZOMBIE;
    proc->exit_status = status;
    current_process = -1;

    /* From now on every scheduler tick returns the saved kernel-main
     * frame, so the context that follows never runs again. */
    scheduler_request_main_switch();

    /* sti takes effect after the next instruction (nop), so the
     * software IRQ0 fires with IF=1 recorded in its frame - the shell
     * resumes with interrupts enabled. If a real timer tick lands in
     * the nop window it takes the pending-switch path anyway. */
    __asm__ volatile("sti; nop; int $32");
    __builtin_unreachable();
}

process_t* process_get_current(void) {
    if (current_process < 0) return NULL;
    return &processes[current_process];
}

process_t* process_get_by_pid(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == pid && processes[i].state != PROCESS_STATE_UNUSED) {
            return &processes[i];
        }
    }
    return NULL;
}

/* Run a process - jump to user mode */
void process_run(uint32_t pid) {
    process_t* proc = process_get_by_pid(pid);
    if (proc == NULL || proc->state != PROCESS_STATE_READY) {
        return;
    }

    /* No tick may fire from here until the iretq into ring 3: the moment
     * state becomes RUNNING, scheduler_tick() would classify this ring0
     * context as the user frame and switch away mid-setup, corrupting the
     * switch sequence. jump_to_user forces IF=1 in the user RFLAGS, so
     * interrupts resume on the first ring-3 instruction. */
    __asm__ volatile("cli");

    /* Mark as running */
    proc->state = PROCESS_STATE_RUNNING;
    current_process = proc - processes;

    /* Switch to the process's private address space. From here on the
     * kernel runs (and the vmm walks) inside this process's page tables
     * until the scheduler returns to the kernel-main task. */
    if (proc->cr3) vmm_switch_cr3(proc->cr3);

    /* Set TSS kernel stack for interrupt/syscall returns, and the global
     * copy used by the Linux-ABI `syscall` entry (which does not switch
     * stacks by itself). */
    tss_set_kernel_stack(proc->kernel_stack);
    syscall_kernel_rsp = proc->kernel_stack;

    /* Fresh TLS state: the previous process's FS base must not leak */
    wrmsr(MSR_FS_BASE, 0);
    proc->fs_base = 0;

    /* The shell context is about to be abandoned by iretq below. Park a
     * fresh kernel-main frame (shell_run on the scheduler stack) so that
     * process exit lands back in a working shell instead of a stale
     * snapshot of the boot stack. */
    scheduler_set_main_return(shell_run);

    /* Jump to user mode (ELF path uses the auxv block RSP) */
    jump_to_user(proc->entry_point,
                 proc->user_rsp ? proc->user_rsp : proc->stack_top);
}

/* Jump to user mode using iretq
 * This function sets up the stack and uses iretq to jump to Ring 3
 * NOTE: never returns - the caller's stack frame is abandoned. The
 * scheduler brings the kernel back to life from its saved IRQ0 frame.
 */
void jump_to_user(uint64_t entry, uint64_t stack) {
    /* TSS RSP0 was already set to the process kernel stack by
     * process_run(); interrupts/syscalls from ring 3 land there. */

    uint64_t user_cs = USER_CS | 3;  /* 0x18 | 3 = 0x1B */
    uint64_t user_ss = USER_DS | 3;  /* 0x20 | 3 = 0x23 */
    uint64_t rflags;

    /* Get current RFLAGS */
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    rflags |= 0x200;  /* Set interrupt flag */

    /* Disable interrupts and jump to user mode */
    __asm__ volatile(
        "pushq %[ss]\n"            /* SS */
        "pushq %[stack]\n"         /* RSP (user stack) */
        "pushq %[rflags]\n"        /* RFLAGS */
        "pushq %[cs]\n"            /* CS */
        "pushq %[entry]\n"         /* RIP */
        "iretq\n"                  /* Jump to user mode */
        :
        : [ss] "r"(user_ss), [stack] "r"(stack),
          [rflags] "r"(rflags), [cs] "r"(user_cs), [entry] "r"(entry)
        : "memory"
    );
}

/* --- Linux-ABI process stack layout (Phase 0.6, auxv from roadmap 1.1)
 *
 * At the ELF entry point the user stack holds, from RSP upward:
 *   argc, argv[] (NULL-terminated), envp[] (NULL-terminated), auxv pairs.
 * Above those live the argv/env strings and the 16 AT_RANDOM bytes. ---- */

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

#define AUXV_PAIRS  16

static uint64_t rand_seed = 0;

static void rand_fill16(uint8_t* dst) {
    if (rand_seed == 0) rand_seed = pit_uptime_us() | 1;
    for (int i = 0; i < 2; i++) {
        rand_seed ^= rand_seed << 13;
        rand_seed ^= rand_seed >> 7;
        rand_seed ^= rand_seed << 17;
        for (int b = 0; b < 8; b++) dst[i * 8 + b] = (uint8_t)(rand_seed >> (b * 8));
    }
}

/* Build argc/argv/envp/auxv at the top of the user stack.
 * interp_base: load base of the ELF interpreter (0 = none/static).
 * exec_path: path string published via AT_EXECFN.
 * Returns the RSP to hand to jump_to_user. All writes go through the
 * identity map so the process's CR3 need not be active yet. */
static uint64_t process_build_stack(process_t* proc, const elf_load_result_t* res,
                                    char* const* argv, int argc,
                                    uint64_t interp_base, const char* exec_path) {
    uint64_t sp = proc->stack_top;

    int n = (argc < 8) ? argc : 8;   /* argv[0] = program path anyway */
    uint64_t ustr[8];

    /* envp: just PATH for now */
    static const char env_str[] = "PATH=/bin";
    size_t env_len = sizeof(env_str);   /* includes NUL */
    sp -= env_len;
    uint64_t uenv = sp;
    uvm_write_user_va(proc, sp, env_str, env_len);

    size_t exec_len = strlen(exec_path) + 1;
    sp -= exec_len;
    uint64_t uexecfn = sp;
    uvm_write_user_va(proc, sp, exec_path, exec_len);

    for (int i = 0; i < n; i++) {
        const char* s = argv[i] ? argv[i] : "";
        size_t l = strlen(s) + 1;
        sp -= l;
        ustr[i] = sp;
        uvm_write_user_va(proc, sp, s, l);
    }

    uint8_t rnd[16];
    rand_fill16(rnd);
    sp -= 16;
    uint64_t urandom = sp;
    uvm_write_user_va(proc, sp, rnd, 16);

    /* Final block: argc + argv[] + NULL + envp[] + NULL + auxv.
     * RSP must be 16-byte aligned at the entry point per the ABI. */
    size_t block = 8 + 8 * ((size_t)n + 1) + 8 * 3 + 8 * (2 * AUXV_PAIRS);
    sp = (sp - block) & ~0xFULL;

    uint64_t blk[1 + 8 + 1 + 3 + 2 * AUXV_PAIRS];
    uint64_t* u = blk;
    *u++ = (uint64_t)n;
    for (int i = 0; i < n; i++) *u++ = ustr[i];
    *u++ = 0;                 /* argv end */
    *u++ = uenv;              /* envp[0] */
    *u++ = 0;                 /* envp end */

    struct { uint64_t k, v; } aux[AUXV_PAIRS] = {
        {AT_PHDR,   res->phdr},
        {AT_PHENT,  56},
        {AT_PHNUM,  res->phnum},
        {AT_PAGESZ, 4096},
        {AT_BASE,   interp_base},
        {AT_ENTRY,  res->entry},
        {AT_UID,    0},
        {AT_EUID,   0},
        {AT_GID,    0},
        {AT_EGID,   0},
        {AT_HWCAP,  0},
        {AT_CLKTCK, 100},
        {AT_SECURE, 0},
        {AT_RANDOM, urandom},
        {AT_EXECFN, uexecfn},
        {AT_NULL,   0},
    };
    for (int i = 0; i < AUXV_PAIRS; i++) {
        *u++ = aux[i].k;
        *u++ = aux[i].v;
    }
    uvm_write_user_va(proc, sp, blk, (size_t)((uint8_t*)u - (uint8_t*)blk));
    return sp;
}

/* Allocate and initialize a bare process slot (no user memory yet).
 * Each process gets its own address space (Phase 1.5). */
static process_t* process_alloc_slot(const char* name) {
    process_t* proc = find_free_process();
    if (proc == NULL) return NULL;
    memset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    int proc_index = proc - processes;
    proc->kernel_stack = (uint64_t)&kernel_stacks[proc_index][KERNEL_STACK_SIZE];
    proc->wait_pid = 0;
    proc->cr3 = vmm_create_address_space();
    if (proc->cr3 == 0) {
        memset(proc, 0, sizeof(process_t));
        return NULL;
    }
    proc->parent_pid = (current_process >= 0) ? processes[current_process].pid : 0;
    return proc;
}

/* Unified exec path: ELF64 / KIL0 header / raw binary. */
int exec_load_program(const char* path, char* const* argv, int argc) {
    fs_entry_t* entry = fs_resolve_path(path);
    if (entry == NULL || entry->type != FS_TYPE_FILE) {
        return -1;  /* File not found */
    }

    uint8_t* buffer = (uint8_t*)kmalloc(entry->size);
    if (buffer == NULL) return -1;
    if (fs_read_file(entry, buffer, entry->size) != (int)entry->size) {
        kfree(buffer);
        return -1;
    }

    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    if (argc > 0 && argv && argv[0]) {
        const char* a0 = argv[0];
        const char* b = a0;
        for (const char* p = a0; *p; p++) {
            if (*p == '/') b = p + 1;
        }
        base = b;
    }

    if (elf_is_elf64(buffer, entry->size)) {
        process_t* proc = process_alloc_slot(base);
        if (proc == NULL) {
            kfree(buffer);
            return -1;
        }

        elf_load_result_t res;
        int rc = elf_load(proc, buffer, entry->size, &res);
        if (rc != 0) {
            const char* why = "invalid ELF";
            if (rc == ELF_ERR_LAYOUT) why = "linked below 0x10000000 (kernel heap); relink with -Wl,-Ttext-segment=0x10000000";
            else if (rc == ELF_ERR_MAP) why = "out of memory mapping segments";
            else if (rc == ELF_ERR_PHDRS) why = "program headers outside PT_LOAD";
            klog("[exec] ELF rejected: ");
            klog(why);
            klog(" size=");
            klog_hex("", (uint32_t)entry->size);
            if (rc == ELF_ERR_FORMAT && entry->size >= 64) {
                klog_hex(" class=", buffer[4]);
                klog_hex(" ei_data=", buffer[5]);
                klog_hex(" type=", buffer[16] | ((uint32_t)buffer[17] << 8));
                klog_hex(" machine=", buffer[18] | ((uint32_t)buffer[19] << 8));
                klog_hex(" phentsize=", buffer[54] | ((uint32_t)buffer[55] << 8));
                klog_hex(" phnum=", buffer[56] | ((uint32_t)buffer[57] << 8));
                /* first phdr (offset 64): type/offset/filesz */
                klog_hex(" p0type=", buffer[64] | ((uint32_t)buffer[65] << 8));
                klog_hex(" p0off=", (uint32_t)buffer[72] | ((uint32_t)buffer[73] << 8));
                klog_hex(" p0fsz=", (uint32_t)buffer[96] | ((uint32_t)buffer[97] << 8));
                /* raw bytes 72..79 + checksum of the first 128 bytes */
                klog_hex(" b72=", buffer[72]);
                klog_hex(" b73=", buffer[73]);
                klog_hex(" b74=", buffer[74]);
                klog_hex(" b75=", buffer[75]);
                {
                    uint32_t sum = 0;
                    for (int q = 0; q < 128; q++) sum += buffer[q];
                    klog_hex(" sum128=", sum);
                }
            }
            klog("\n");
            uvm_release_all(proc);
            memset(proc, 0, sizeof(process_t));
            kfree(buffer);
            return -1;
        }

        /* Phase 3.0: dynamically linked image - map the interpreter and
         * enter through it. The interpreter itself is a musl ET_DYN that
         * finds the main image via auxv AT_PHDR and itself via AT_BASE. */
        uint64_t interp_base = 0;
        uint64_t entry = res.entry;
        if (res.interp[0]) {
            fs_entry_t* ie = fs_resolve_path(res.interp);
            if (ie == NULL || ie->type != FS_TYPE_FILE) {
                klog("[exec] interpreter not found: ");
                klog(res.interp);
                klog("\n");
                uvm_release_all(proc);
                memset(proc, 0, sizeof(process_t));
                kfree(buffer);
                return -1;
            }
            uint8_t* ibuf = (uint8_t*)kmalloc(ie->size);
            if (ibuf == NULL ||
                fs_read_file(ie, ibuf, ie->size) != (int)ie->size) {
                klog("[exec] failed to read interpreter\n");
                if (ibuf) kfree(ibuf);
                uvm_release_all(proc);
                memset(proc, 0, sizeof(process_t));
                kfree(buffer);
                return -1;
            }
            elf_load_result_t ires;
            rc = elf_load_interp(proc, ibuf, ie->size, UVM_INTERP_BASE, &ires);
            kfree(ibuf);
            if (rc != 0) {
                klog("[exec] interpreter load failed\n");
                uvm_release_all(proc);
                memset(proc, 0, sizeof(process_t));
                kfree(buffer);
                return -1;
            }
            interp_base = UVM_INTERP_BASE;
            entry = ires.entry;
            /* The mmap arena of a dynamic process starts above the
             * interpreter so loader/library mappings cannot collide. */
            if (ires.brk_start > UVM_MMAP_BASE) {
                proc->mmap_top = ires.brk_start;
            }
        }

        /* Map the user stack and lay out argc/argv/envp/auxv */
        proc->stack_top = USER_STACK_BASE;
        if (uvm_map_range(proc, USER_STACK_BASE - USER_STACK_SIZE, USER_STACK_BASE,
                          UVM_PROT_READ | UVM_PROT_WRITE) != 0) {
            klog("[exec] out of memory for user stack\n");
            uvm_release_all(proc);
            memset(proc, 0, sizeof(process_t));
            kfree(buffer);
            return -1;
        }
        proc->stack_pages = USER_STACK_SIZE / 4096;

        proc->entry_point = entry;
        proc->brk_start = proc->brk_cur = res.brk_start;
        if (proc->mmap_top < UVM_MMAP_BASE) proc->mmap_top = UVM_MMAP_BASE;

        proc->user_rsp = process_build_stack(proc, &res, argv, argc,
                                             interp_base, path);
        proc->state = PROCESS_STATE_READY;
        if (interp_base != 0) {
            klog("[exec] dynamic exec via ");
            klog(res.interp);
            klog("\n");
        }
        kfree(buffer);
        return proc->pid;
    }

    /* Legacy KIL0-header or raw binary path */
    uint64_t entry_point = USER_CODE_BASE;
    const uint8_t* code = buffer;
    size_t code_size = entry->size;

    if (entry->size >= sizeof(user_program_header_t)) {
        user_program_header_t* h = (user_program_header_t*)buffer;
        if (h->magic == USER_MAGIC) {
            entry_point = USER_CODE_BASE + h->entry_offset;
            code = buffer + sizeof(user_program_header_t);
            code_size = h->code_size;
        }
    }

    int pid = process_create(base, (uint8_t*)code, code_size, entry_point);
    kfree(buffer);
    return pid;
}

/* Load a user program from the file system */
int load_user_program(const char* path) {
    char* argv0[1];
    argv0[0] = (char*)path;
    return exec_load_program(path, argv0, 1);
}

/* Embedded user program blobs (see Makefile: nasm .incbin) */
extern const uint8_t user_hello_start[];
extern const uint8_t user_hello_end[];
extern const uint8_t user_pong_start[];
extern const uint8_t user_pong_end[];

/* Optional musl static ELF (embedded only when musl-gcc is available) */
extern const uint8_t user_hello_lnx_start[] __attribute__((weak));
extern const uint8_t user_hello_lnx_end[] __attribute__((weak));

/* musl DYNAMIC PIE (Phase 3.0 acceptance, embedded when musl-gcc exists) */
extern const uint8_t user_hello_dyn_start[] __attribute__((weak));
extern const uint8_t user_hello_dyn_end[] __attribute__((weak));

/* musl dynamic interpreter: libc.so deployed as /lib/ld-musl-x86_64.so.1 */
extern const uint8_t user_ldmusl_start[] __attribute__((weak));
extern const uint8_t user_ldmusl_end[] __attribute__((weak));

/* glibc dynamic chain (Phase 3.1): hello + Debian rtld + libc.so.6 */
extern const uint8_t user_hello_glibc_start[] __attribute__((weak));
extern const uint8_t user_hello_glibc_end[] __attribute__((weak));
extern const uint8_t user_ldlinux_start[] __attribute__((weak));
extern const uint8_t user_ldlinux_end[] __attribute__((weak));
extern const uint8_t user_glibc_start[] __attribute__((weak));
extern const uint8_t user_glibc_end[] __attribute__((weak));
extern const uint8_t user_probe_ld_start[] __attribute__((weak));
extern const uint8_t user_probe_ld_end[] __attribute__((weak));

/* glibc dynamic + pthread primitives (Phase 3.2) */
extern const uint8_t user_pthread_start[] __attribute__((weak));
extern const uint8_t user_pthread_end[] __attribute__((weak));

/* Freestanding Linux-ABI syscall probe (always embedded) */
extern const uint8_t user_mini_start[];
extern const uint8_t user_mini_end[];

/* Freestanding brk/mmap/mprotect acceptance probe (always embedded) */
extern const uint8_t user_mmt_start[];
extern const uint8_t user_mmt_end[];

/* Freestanding TCP connect/GET probe (Phase 3.3, always embedded) */
extern const uint8_t user_nettest_start[];
extern const uint8_t user_nettest_end[];

/* musl static busybox (embedded only when built via tools/build_busybox.sh) */
extern const uint8_t user_busybox_start[] __attribute__((weak));
extern const uint8_t user_busybox_end[] __attribute__((weak));

/* Phase 3: deploy an embedded file under a root directory (created on
 * demand), e.g. ("lib64", "ld-linux-x86-64.so.2", ...). Skipped when the
 * blob is absent (weak symbols) or the file already exists. */
static void user_install_blob(const char* dir, const char* name,
                              const uint8_t* start, const uint8_t* end,
                              const char* note) {
    if (start == NULL || end == NULL || end <= start) return;
    if (fs_resolve_path(dir) == NULL) {
        fs_entry_t* prev_root = fs_current();
        fs_set_current(fs_root());
        fs_entry_t* d = fs_create_dir(dir + 1);   /* strip leading '/' */
        fs_set_current(prev_root);
        if (d == NULL) {
            klog("[user] failed to create ");
            klog(dir);
            klog("\n");
            return;
        }
    }
    char full[MAX_PATH_LENGTH];
    int flen = 0;
    while (dir[flen] && flen < (int)sizeof(full) - 1) {
        full[flen] = dir[flen];
        flen++;
    }
    if (flen < (int)sizeof(full) - 1) full[flen++] = '/';
    for (int i = 0; name[i] && flen < (int)sizeof(full) - 1; i++) {
        full[flen++] = name[i];
    }
    full[flen] = 0;
    if (fs_resolve_path(full) != NULL) return;

    fs_entry_t* d = fs_resolve_path(dir);
    fs_entry_t* prev = fs_current();
    fs_set_current(d);
    fs_entry_t* f = fs_create_file(name);
    if (f != NULL) {
        fs_write_file(f, start, (size_t)(end - start));
        klog("[user] ");
        klog(full);
        klog(" installed");
        if (note && note[0]) {
            klog(" (");
            klog(note);
            klog(")");
        }
        klog("\n");
    }
    fs_set_current(prev);
}

/* Boot-time sentinel: read /bin/hello-lnx back after ALL installs and
 * compare with the embedded blob. Catches FAT corruption (e.g. a too-small
 * FAT spilling entries into the data area) right where it happens. */
static void verify_hello_lnx(void) {
    static uint8_t chk[36864];
    const uint8_t* blob = user_hello_lnx_start;
    size_t size = (size_t)(user_hello_lnx_end - user_hello_lnx_start);
    if (blob == NULL || size == 0 || size > sizeof(chk)) return;

    fs_entry_t* vf = fs_resolve_path("/bin/hello-lnx");
    if (vf == NULL || (size_t)vf->size != size ||
        fs_read_file(vf, chk, size) != (int)size) {
        klog("[chk] hello-lnx readback size failed\n");
        return;
    }
    int bad = -1;
    for (size_t i = 0; i < size; i++) {
        if (chk[i] != blob[i]) { bad = (int)i; break; }
    }
    if (bad < 0) {
        klog("[chk] hello-lnx ok\n");
    } else {
        klog("[chk] hello-lnx MISMATCH at ");
        klog_hex("", (uint64_t)bad);
        klog_hex(" got=", chk[bad]);
        klog_hex(" want=", blob[bad]);
        klog("\n");
    }
}

void user_programs_install(void) {
    fs_entry_t* bin = fs_resolve_path("/bin");
    if (bin == NULL || bin->type != FS_TYPE_DIRECTORY) {
        return;  /* /bin missing - fs not formatted? */
    }

    fs_entry_t* prev = fs_current();
    fs_set_current(bin);

    if (fs_resolve_path("/bin/hello.bin") == NULL) {
        fs_entry_t* f = fs_create_file("hello.bin");
        if (f != NULL) {
            size_t size = (size_t)(user_hello_end - user_hello_start);
            fs_write_file(f, user_hello_start, size);
        }
    }
    if (fs_resolve_path("/bin/pong.bin") == NULL) {
        fs_entry_t* f = fs_create_file("pong.bin");
        if (f != NULL) {
            size_t size = (size_t)(user_pong_end - user_pong_start);
            fs_write_file(f, user_pong_start, size);
        }
    }
    if (&user_hello_lnx_start != NULL &&
        &user_hello_lnx_end > &user_hello_lnx_start &&
        fs_resolve_path("/bin/hello-lnx") == NULL) {
        fs_entry_t* f = fs_create_file("hello-lnx");
        if (f != NULL) {
            size_t size = (size_t)(user_hello_lnx_end - user_hello_lnx_start);
            fs_write_file(f, user_hello_lnx_start, size);
            klog("[user] /bin/hello-lnx installed (musl static ELF)\n");
        }
    }

    if (fs_resolve_path("/bin/mini") == NULL) {
        fs_entry_t* f = fs_create_file("mini");
        if (f != NULL) {
            size_t size = (size_t)(user_mini_end - user_mini_start);
            fs_write_file(f, user_mini_start, size);
            klog("[user] /bin/mini installed (syscall probe)\n");
        }
    }

    if (fs_resolve_path("/bin/mmt") == NULL) {
        fs_entry_t* f = fs_create_file("mmt");
        if (f != NULL) {
            size_t size = (size_t)(user_mmt_end - user_mmt_start);
            fs_write_file(f, user_mmt_start, size);
            klog("[user] /bin/mmt installed (brk/mmap/mprotect probe)\n");
        }
    }

    if (fs_resolve_path("/bin/nettest") == NULL) {
        fs_entry_t* f = fs_create_file("nettest");
        if (f != NULL) {
            size_t size = (size_t)(user_nettest_end - user_nettest_start);
            fs_write_file(f, user_nettest_start, size);
            klog("[user] /bin/nettest installed (TCP probe)\n");
        }
    }

    user_install_blob("/bin", "busybox", user_busybox_start, user_busybox_end,
                      "musl static");
    user_install_blob("/bin", "hello-dyn", user_hello_dyn_start,
                      user_hello_dyn_end, "musl dynamic PIE");
    /* musl interpreter: libc.so IS ld-musl; SONAME "libc.so" matches
     * DT_NEEDED so the loader resolves libc to itself (Phase 3.0) */
    user_install_blob("/lib", "ld-musl-x86_64.so.1", user_ldmusl_start,
                      user_ldmusl_end, "musl ldso");
    /* glibc dynamic chain (Phase 3.1): .interp=/lib64/ld-linux-x86-64.so.2,
     * DT_NEEDED libc.so.6 found via ld.so's built-in /lib search path */
    user_install_blob("/bin", "hello-glibc", user_hello_glibc_start,
                      user_hello_glibc_end, "glibc dynamic PIE");
    user_install_blob("/bin", "probe-ld", user_probe_ld_start,
                      user_probe_ld_end, "ldso probe");
    user_install_blob("/lib64", "ld-linux-x86-64.so.2", user_ldlinux_start,
                      user_ldlinux_end, "glibc rtld");
    user_install_blob("/lib", "libc.so.6", user_glibc_start, user_glibc_end,
                      "glibc");
    user_install_blob("/bin", "hello-pthread", user_pthread_start,
                      user_pthread_end, "glibc pthread probe");

    fs_set_current(prev);

    /* /etc/resolv.conf: busybox nslookup/wget parse this file directly.
     * 10.0.2.3 is QEMU user-net's built-in DNS forwarder. */
    if (fs_resolve_path("/etc") == NULL) {
        fs_entry_t* prev_root = fs_current();
        fs_set_current(fs_root());
        fs_create_dir("etc");
        fs_set_current(prev_root);
    }
    if (fs_resolve_path("/etc/resolv.conf") == NULL) {
        static const char resolv_conf[] = "nameserver 10.0.2.3\n";
        fs_entry_t* d = fs_resolve_path("/etc");
        if (d != NULL && d->type == FS_TYPE_DIRECTORY) {
            fs_entry_t* prev2 = fs_current();
            fs_set_current(d);
            fs_entry_t* f = fs_create_file("resolv.conf");
            if (f != NULL)
                fs_write_file(f, (const uint8_t*)resolv_conf,
                              sizeof(resolv_conf) - 1);
            fs_set_current(prev2);
        }
    }

    /* /etc/hosts: musl getaddrinfo (busybox wget) checks it before DNS.
     * "kil0yos" maps to the QEMU user-net host (10.0.2.2). */
    if (fs_resolve_path("/etc/hosts") == NULL) {
        static const char hosts[] = "127.0.0.1 localhost\n10.0.2.2 kil0yos\n";
        fs_entry_t* d = fs_resolve_path("/etc");
        if (d != NULL && d->type == FS_TYPE_DIRECTORY) {
            fs_entry_t* prev2 = fs_current();
            fs_set_current(d);
            fs_entry_t* f = fs_create_file("hosts");
            if (f != NULL)
                fs_write_file(f, (const uint8_t*)hosts, sizeof(hosts) - 1);
            fs_set_current(prev2);
        }
    }

    /* /etc/kilget/sources.list: default repo baked in so 'kilget update'
     * works out of the box - Aliyun's Ubuntu jammy mirror over slirp. */
    if (fs_resolve_path("/etc/kilget") == NULL) {
        fs_entry_t* d = fs_resolve_path("/etc");
        if (d != NULL && d->type == FS_TYPE_DIRECTORY) {
            fs_entry_t* prev2 = fs_current();
            fs_set_current(d);
            fs_create_dir("kilget");
            fs_set_current(prev2);
        }
    }
    if (fs_resolve_path("/etc/kilget/sources.list") == NULL) {
        static const char sources_list[] =
            "deb http://mirrors.aliyun.com/ubuntu/ jammy main\n";
        fs_entry_t* d = fs_resolve_path("/etc/kilget");
        if (d != NULL && d->type == FS_TYPE_DIRECTORY) {
            fs_entry_t* prev2 = fs_current();
            fs_set_current(d);
            fs_entry_t* f = fs_create_file("sources.list");
            if (f != NULL)
                fs_write_file(f, (const uint8_t*)sources_list,
                              sizeof(sources_list) - 1);
            fs_set_current(prev2);
        }
    }

    verify_hello_lnx();
}

/* ======================================================================== */
/*  fork / wait4 (Phase 1.5)                                                */
/* ======================================================================== */

process_t* process_find_waiter(int child_pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = &processes[i];
        if (p->state != PROCESS_STATE_BLOCKED) continue;
        if (p->wait_pid == child_pid ||
            (p->wait_pid == -1 && p->parent_pid != 0)) {
            /* wait_pid == -1: waiter of this child only if it forked it */
            if (p->wait_pid == -1) {
                int found = 0;
                for (int j = 0; j < MAX_PROCESSES; j++) {
                    if (processes[j].pid == (uint32_t)child_pid &&
                        processes[j].parent_pid == (int)p->pid) { found = 1; break; }
                }
                if (!found) continue;
            }
            return p;
        }
    }
    return NULL;
}

process_t* process_find_child(process_t* parent, int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* c = &processes[i];
        if (c->state == PROCESS_STATE_UNUSED) continue;
        if (c->parent_pid != (int)parent->pid) continue;
        if (pid > 0 && (int)c->pid != pid) continue;
        if (c->state == PROCESS_STATE_READY ||
            c->state == PROCESS_STATE_RUNNING ||
            c->state == PROCESS_STATE_ZOMBIE) {
            return c;
        }
    }
    return NULL;
}

/* Write an int into a user VA of the given process (wait4 status
 * delivery; the process may not be the currently active CR3). */
void process_write_user_int(process_t* p, uint64_t uaddr, int val) {
    uint64_t old = vmm_current_root();
    vmm_set_root_ptr(p->cr3);
    uint64_t page = uaddr & ~0xFFFULL;
    uint64_t phys = vmm_get_phys(page);
    if (phys != 0) {
        *(int*)(phys + (uaddr - page)) = val;
    }
    vmm_set_root_ptr(old);
}

/* fork from the Linux-ABI syscall context. frame_rsp points at the
 * caller's syscall-entry frame holding the parent's user context; the
 * child gets a clone of it (IRQ layout, RAX=0) parked on its own kernel
 * stack so the scheduler can resume it like any user frame. */
int process_fork(uint64_t frame_rsp) {
    process_t* p = process_get_current();
    if (p == NULL) return -1;

    process_t* c = process_alloc_slot(p->name);
    if (c == NULL) return -1;
    c->parent_pid = (int)p->pid;

    if (uvm_fork(p, c) != 0) {
        vmm_destroy_address_space(c->cr3);
        memset(c, 0, sizeof(process_t));
        return -1;
    }
    lnxvfs_inherit_fds(p, c);

    uint64_t* e = (uint64_t*)frame_rsp;               /* entry layout */
    uint64_t frsp = ((c->kernel_stack - FRAME_BYTES) & ~0xFULL);
    uint64_t* f = (uint64_t*)frsp;
    for (int i = 0; i < 15; i++) f[i] = e[i];
    f[IRQ_RAX] = 0;                                   /* child return value */
    f[IRQ_INTNUM] = 32;
    f[IRQ_ERR] = 0;
    f[IRQ_RIP] = e[ENTY_RIP];
    f[IRQ_CS] = 0x1B;
    f[IRQ_RFLAGS] = e[ENTY_RFLAGS];
    f[IRQ_RSP] = e[ENTY_RSP];
    f[IRQ_SS] = 0x23;

    c->entry_point = e[ENTY_RIP];
    c->user_rsp = e[ENTY_RSP];
    c->parked_rsp = frsp;
    c->state = PROCESS_STATE_READY;

    klog("[fork] child ready\n");
    return (int)c->pid;
}

/* wait4 slow path: park the caller (BLOCKED) and run the child until it
 * exits; the exit path then resumes the caller's frame with the status
 * patched into RAX. Never returns. */
void process_wait_run(process_t* child, uint64_t status_ptr, uint64_t frame_rsp) {
    process_t* p = process_get_current();
    if (p == NULL) return;

    __asm__ volatile("cli");

    /* Convert the caller's live entry-layout frame into a resumable
     * IRQ-layout frame parked below it on the caller's kernel stack. */
    uint64_t* e = (uint64_t*)frame_rsp;
    uint64_t frsp = ((frame_rsp - FRAME_BYTES) & ~0xFULL);
    uint64_t* f = (uint64_t*)frsp;
    for (int i = 0; i < 15; i++) f[i] = e[i];
    f[IRQ_RAX] = 0;                    /* patched with child pid at exit */
    f[IRQ_INTNUM] = 32;
    f[IRQ_ERR] = 0;
    f[IRQ_RIP] = e[ENTY_RIP];
    f[IRQ_CS] = e[ENTY_CS];
    f[IRQ_RFLAGS] = e[ENTY_RFLAGS];
    f[IRQ_RSP] = e[ENTY_RSP];
    f[IRQ_SS] = e[ENTY_SS];

    p->state = PROCESS_STATE_BLOCKED;
    p->wait_pid = (int)child->pid;
    p->wait_status_ptr = status_ptr;
    p->parked_rsp = frsp;

    /* Hand the CPU to the child */
    current_process = child - processes;
    child->state = PROCESS_STATE_RUNNING;
    tss_set_kernel_stack(child->kernel_stack);
    syscall_kernel_rsp = child->kernel_stack;
    if (child->cr3) vmm_switch_cr3(child->cr3);
    scheduler_invalidate_user_frame();

    restore_frame(child->parked_rsp);  /* never returns */
}

/* Scheduler helper: another READY process holding a parked user frame
 * (fork child / preempted parent), or NULL. */
process_t* process_pick_ready(uint32_t exclude_pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = &processes[i];
        if (p->state == PROCESS_STATE_READY && p->parked_rsp != 0 &&
            p->pid != exclude_pid) {
            return p;
        }
    }
    return NULL;
}

/* Scheduler hook: make the given process the current one (tick path
 * resuming a parked user frame). */
void process_become_current(process_t* proc) {
    current_process = proc - processes;
    proc->state = PROCESS_STATE_RUNNING;
}

/* execve (Phase 1.5): replace the current process image with a new ELF.
 * The caller passes kernel-side copies of path/argv (user memory is
 * about to be torn down). On success the live syscall-entry frame is
 * rewritten so its iretq lands in the new program; returns 0. On early
 * failure (before the old image is dropped) returns -1. */
int exec_replace(uint64_t frame_rsp, const char* path,
                 char* const* argv, int argc) {
    fs_entry_t* entry = fs_resolve_path(path);
    if (entry == NULL || entry->type != FS_TYPE_FILE) return -1;

    uint8_t* buffer = (uint8_t*)kmalloc(entry->size);
    if (buffer == NULL) return -1;
    if (fs_read_file(entry, buffer, entry->size) != (int)entry->size) {
        kfree(buffer);
        return -1;
    }
    if (!elf_is_elf64(buffer, entry->size)) {
        kfree(buffer);
        return -1;
    }

    process_t* proc = process_get_current();
    if (proc == NULL) {
        kfree(buffer);
        return -1;
    }

    /* Point of no return: drop the old image. Any failure from here on
     * terminates the process - there is nothing left to return to. */
    uvm_release_all(proc);

    proc->stack_top = USER_STACK_BASE;
    if (uvm_map_range(proc, USER_STACK_BASE - USER_STACK_SIZE, USER_STACK_BASE,
                      UVM_PROT_READ | UVM_PROT_WRITE) != 0) {
        goto dead;
    }
    proc->stack_pages = USER_STACK_SIZE / 4096;

    elf_load_result_t res;
    if (elf_load(proc, buffer, entry->size, &res) != 0) goto dead;
    kfree(buffer);
    buffer = NULL;   /* dead: frees it; interp failures jump past here */

    /* Phase 3.0: map the PT_INTERP interpreter, if any, and enter
     * through it (same layout as the shell exec path). */
    uint64_t interp_base = 0;
    uint64_t entry_va = res.entry;
    if (res.interp[0]) {
        fs_entry_t* ie = fs_resolve_path(res.interp);
        if (ie == NULL || ie->type != FS_TYPE_FILE) goto dead;
        uint8_t* ibuf = (uint8_t*)kmalloc(ie->size);
        if (ibuf == NULL) goto dead;
        if (fs_read_file(ie, ibuf, ie->size) != (int)ie->size) {
            kfree(ibuf);
            goto dead;
        }
        elf_load_result_t ires;
        if (elf_load_interp(proc, ibuf, ie->size, UVM_INTERP_BASE, &ires) != 0) {
            kfree(ibuf);
            goto dead;
        }
        kfree(ibuf);
        interp_base = UVM_INTERP_BASE;
        entry_va = ires.entry;
        if (ires.brk_start > UVM_MMAP_BASE) {
            proc->mmap_top = ires.brk_start;
        }
    }

    proc->entry_point = entry_va;
    proc->brk_start = proc->brk_cur = res.brk_start;
    if (proc->mmap_top < UVM_MMAP_BASE) proc->mmap_top = UVM_MMAP_BASE;
    proc->user_rsp = process_build_stack(proc, &res, argv, argc,
                                         interp_base, path);

    /* Rewrite the live syscall frame: the epilogue's iretq now enters
     * the new image with RAX = 0 (execve "never returns" to the caller). */
    uint64_t* f = (uint64_t*)frame_rsp;
    f[IRQ_RAX] = 0;
    f[ENTY_RIP] = entry_va;
    f[ENTY_RSP] = proc->user_rsp;

    return 0;

dead:
    if (buffer) kfree(buffer);
    process_exit(127);   /* never returns */
    __builtin_unreachable();
}