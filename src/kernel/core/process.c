#include "core/process.h"
#include "core/gdt.h"
#include "core/tss.h"
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
        return -1;  /* No free process slots */
    }

    /* Initialize process */
    memset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    proc->state = PROCESS_STATE_READY;
    strncpy(proc->name, name, sizeof(proc->name) - 1);

    /* Set up kernel stack for this process (used during syscalls) */
    int proc_index = proc - processes;
    proc->kernel_stack = (uint64_t)&kernel_stacks[proc_index][KERNEL_STACK_SIZE];

    /* Allocate code area at USER_CODE_BASE (0x400000 = 4 MB)
     * This is above the kernel but below the stack
     * We use identity mapping, so we need to:
     * 1. Allocate physical pages
     * 2. Map them to USER_CODE_BASE
     * 3. Copy code through the identity-mapped physical addresses
     */

    proc->code_base = USER_CODE_BASE;

    /* Calculate number of pages needed */
    size_t pages_needed = (code_size + 4095) / 4096;
    if (pages_needed == 0) pages_needed = 1;

    /* Allocate and map code pages */
    for (size_t i = 0; i < pages_needed; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            /* TODO: free previously allocated pages */
            return -1;  /* Out of memory */
        }

        uint64_t virt = USER_CODE_BASE + i * 4096;

        /* Map user virtual address to physical page */
        /* Flags: Present (1), Writable (2), User (4) = 0x07 */
        vmm_map_page(virt, phys, 0x07);

        vga_puts("[DEBUG] Mapped page: virt=0x");
        vga_puthex(virt);
        vga_puts(" -> phys=0x");
        vga_puthex(phys);
        vga_puts("\n");

        /* Copy code to the page using physical address (identity mapped) */
        /* Since boot.asm identity maps the first 4GB, phys == virt for low addresses */
        /* But USER_CODE_BASE might map to a different physical address, so use phys */
        size_t offset = i * 4096;
        size_t copy_size = (code_size > offset + 4096) ? 4096 : code_size - offset;
        if (copy_size > 0 && offset < code_size) {
            /* Use physical address which is identity-mapped and accessible */
            memcpy((void*)phys, code + offset, copy_size);
        }
    }

    /* Set up stack */
    proc->stack_top = USER_STACK_BASE;

    /* Map user stack pages (stack grows downward) */
    for (uint64_t addr = USER_STACK_BASE - USER_STACK_SIZE + 0x1000;
         addr <= USER_STACK_BASE;
         addr += 4096) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            /* TODO: free previously allocated pages */
            return -1;
        }
        vmm_map_page(addr, phys, 0x07);  /* User, R/W, Present */
    }

    proc->entry_point = entry;
    proc->state = PROCESS_STATE_READY;

    return proc->pid;
}

void process_exit(int status) {
    (void)status;
    if (current_process < 0) return;

    process_t* proc = &processes[current_process];
    proc->state = PROCESS_STATE_ZOMBIE;

    /* For now, just halt */
    while (1) {
        __asm__ volatile("hlt");
    }
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

    /* Mark as running */
    proc->state = PROCESS_STATE_RUNNING;
    current_process = proc - processes;

    /* Set TSS kernel stack for syscall returns */
    tss_set_kernel_stack(proc->kernel_stack);

    /* Jump to user mode */
    jump_to_user(proc->entry_point, proc->stack_top);
}

/* Jump to user mode using iretq
 * This function sets up the stack and uses iretq to jump to Ring 3
 */
void jump_to_user(uint64_t entry, uint64_t stack) {
    /* Set TSS kernel stack pointer - this is where CPU will jump on syscall/interrupt */
    uint64_t kernel_stack;
    __asm__ volatile("mov %%rsp, %0" : "=r"(kernel_stack));
    tss_set_kernel_stack(kernel_stack);

    /* Prepare for iretq on the CURRENT (kernel) stack:
     * We push values onto kernel stack, then iretq will:
     * 1. Pop RIP, CS, RFLAGS, RSP, SS from stack
     * 2. Load them into registers
     * 3. Jump to RIP in Ring 3
     *
     * Stack layout after pushes (grows downward):
     * [top] SS      (user data segment + RPL 3)
     *       RSP     (user stack pointer)
     *       RFLAGS  (with interrupt flag set)
     *       CS      (user code segment + RPL 3)
     *       RIP     (entry point)
     * [bottom - iretq pops from here]
     */

    uint64_t user_cs = USER_CS | 3;  /* 0x18 | 3 = 0x1B */
    uint64_t user_ss = USER_DS | 3;  /* 0x20 | 3 = 0x23 */
    uint64_t rflags;

    /* Get current RFLAGS */
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    rflags |= 0x200;  /* Set interrupt flag */

    /* Debug: Print entry point and stack info */
    vga_puts("\n[DEBUG] jump_to_user:\n");
    vga_puts("  Entry: 0x");
    vga_puthex(entry);
    vga_puts("\n  Stack: 0x");
    vga_puthex(stack);
    vga_puts("\n  CS: 0x");
    vga_puthex(user_cs);
    vga_puts("  SS: 0x");
    vga_puthex(user_ss);
    vga_puts("\n");

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