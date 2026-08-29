#include "core/process.h"
#include "core/gdt.h"
#include "core/tss.h"
#include "sched/scheduler.h"
#include "shell/shell.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "fs/fs.h"
#include "drivers/vga.h"

/* Process table */
static process_t processes[MAX_PROCESSES];
static int current_process = -1;
static uint32_t next_pid = 1;

/* Kernel stack for user mode transitions (16 KB) */
#define KERNEL_STACK_SIZE 0x4000
static uint8_t kernel_stacks[MAX_PROCESSES][KERNEL_STACK_SIZE] __attribute__((aligned(16)));

static void process_release_memory(process_t* proc);

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

    /* Allocate code area at USER_CODE_BASE (0x10000000 = 256 MB, above
     * the kernel heap arena so we never rewrite heap PTEs):
     * 1. Allocate physical pages
     * 2. Map them into the shared page tables at the user VA
     * 3. Copy code through the user VA
     */

    proc->code_base = USER_CODE_BASE;

    /* Calculate number of pages needed */
    size_t pages_needed = (code_size + 4095) / 4096;
    if (pages_needed == 0) pages_needed = 1;

    /* Allocate and map code pages */
    for (size_t i = 0; i < pages_needed; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            vga_puts("[proc] create failed: PMM out of pages (code)\n");
            proc->code_pages = (uint32_t)i;
            goto fail;
        }

        uint64_t virt = USER_CODE_BASE + i * 4096;

        /* Map user virtual address to physical page */
        /* Flags: Present (1), Writable (2), User (4) = 0x07 */
        vmm_map_page(virt, phys, 0x07);

        /* Copy code through the freshly mapped user VA (ring0 may write
         * user pages). Copying via the physical address relied on the
         * identity map, which we may have just re-pointed above. */
        size_t offset = i * 4096;
        size_t copy_size = (code_size > offset + 4096) ? 4096 : code_size - offset;
        if (copy_size > 0 && offset < code_size) {
            memcpy((void*)virt, code + offset, copy_size);
        }
    }
    proc->code_pages = (uint32_t)pages_needed;

    klog("[create] code mapped\n");

    /* Set up stack */
    proc->stack_top = USER_STACK_BASE;

    /* Map user stack pages (stack grows down) */
    uint32_t stack_pages = 0;
    for (uint64_t addr = USER_STACK_BASE - USER_STACK_SIZE + 0x1000;
         addr <= USER_STACK_BASE;
         addr += 4096) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            vga_puts("[proc] create failed: PMM out of pages (stack)\n");
            proc->stack_pages = stack_pages;
            goto fail;
        }
        vmm_map_page(addr, phys, 0x07);  /* User, R/W, Present */
        stack_pages++;
    }
    proc->stack_pages = stack_pages;

    proc->entry_point = entry;
    proc->state = PROCESS_STATE_READY;

    return proc->pid;

fail:
    /* Undo partial allocation so no half-initialized process remains:
     * unmap mapped VAs, free their physical pages, reset the slot. */
    process_release_memory(proc);
    memset(proc, 0, sizeof(process_t));
    return -1;
}

/* Release all user pages of a process and free its slot.
 * Only safe for processes that are not running. */
static void process_release_memory(process_t* proc) {
    for (uint32_t i = 0; i < proc->code_pages; i++) {
        uint64_t virt = proc->code_base + (uint64_t)i * 4096;
        uint64_t phys = vmm_get_phys(virt);
        vmm_unmap_page(virt);
        if (phys != 0) pmm_free_page(phys);
    }
    for (uint32_t i = 0; i < proc->stack_pages; i++) {
        uint64_t virt = proc->stack_top - USER_STACK_SIZE + (uint64_t)(i + 1) * 4096;
        uint64_t phys = vmm_get_phys(virt);
        vmm_unmap_page(virt);
        if (phys != 0) pmm_free_page(phys);
    }
    proc->code_pages = 0;
    proc->stack_pages = 0;
}

void process_reap_zombies(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROCESS_STATE_ZOMBIE) {
            process_release_memory(&processes[i]);
            processes[i].state = PROCESS_STATE_UNUSED;
        }
    }
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
    process_t* proc = process_get_current();
    if (proc == NULL) return 0;
    if (len == 0) return 1;
    if (uaddr + len < uaddr) return 0;  /* wrap-around */

    uint64_t code_end = proc->code_base + (uint64_t)proc->code_pages * 4096;
    uint64_t stack_lo = proc->stack_top - USER_STACK_SIZE;

    /* Entirely inside the code region or the stack region */
    int in_code  = (uaddr >= proc->code_base && uaddr + len <= code_end);
    int in_stack = (uaddr >= stack_lo && uaddr + len <= proc->stack_top);
    if (!in_code && !in_stack) return 0;

    /* Every touched page must be present in the page tables */
    for (uint64_t a = uaddr & ~0xFFFULL; a < uaddr + len; a += 4096) {
        if (vmm_get_phys(a) == 0) return 0;
    }
    return 1;
}

void process_exit(int status) {
    if (current_process < 0) return;

    klog("[proc] exit syscall\n");

    process_t* proc = &processes[current_process];

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

    klog("[proc] run pid\n");

    /* No tick may fire from here until the iretq into ring 3: the moment
     * state becomes RUNNING, scheduler_tick() would classify this ring0
     * context as the user frame and switch away mid-setup, corrupting the
     * switch sequence. jump_to_user forces IF=1 in the user RFLAGS, so
     * interrupts resume on the first ring-3 instruction. */
    __asm__ volatile("cli");

    /* Mark as running */
    proc->state = PROCESS_STATE_RUNNING;
    current_process = proc - processes;

    /* Set TSS kernel stack for syscall returns */
    tss_set_kernel_stack(proc->kernel_stack);

    /* The shell context is about to be abandoned by iretq below. Park a
     * fresh kernel-main frame (shell_run on the scheduler stack) so that
     * process exit lands back in a working shell instead of a stale
     * snapshot of the boot stack. */
    scheduler_set_main_return(shell_run);

    /* Jump to user mode */
    jump_to_user(proc->entry_point, proc->stack_top);
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

/* Load a user program from the file system */
int load_user_program(const char* path) {
    /* Find the file */
    fs_entry_t* entry = fs_resolve_path(path);
    if (entry == NULL || entry->type != FS_TYPE_FILE) {
        return -1;  /* File not found */
    }

    /* Read file into memory */
    uint8_t* buffer = (uint8_t*)kmalloc(entry->size);
    if (buffer == NULL) {
        return -1;
    }

    int bytes_read = fs_read_file(entry, buffer, entry->size);
    if (bytes_read != (int)entry->size) {
        kfree(buffer);
        return -1;
    }

    /* Parse header */
    if (entry->size < sizeof(user_program_header_t)) {
        kfree(buffer);
        return -1;
    }

    user_program_header_t* header = (user_program_header_t*)buffer;
    if (header->magic != USER_MAGIC) {
        kfree(buffer);
        return -1;  /* Invalid magic */
    }

    /* Create process */
    uint64_t entry_point = USER_CODE_BASE + header->entry_offset;
    int pid = process_create(path,
                              buffer + sizeof(user_program_header_t),
                              header->code_size,
                              entry_point);

    kfree(buffer);

    if (pid < 0) {
        return -1;
    }

    return pid;
}

/* Embedded user program blobs (see Makefile: nasm .incbin) */
extern const uint8_t user_hello_start[];
extern const uint8_t user_hello_end[];
extern const uint8_t user_pong_start[];
extern const uint8_t user_pong_end[];

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

    fs_set_current(prev);
}