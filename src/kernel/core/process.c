#include "core/process.h"
#include "core/gdt.h"
#include "core/tss.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "fs/fs.h"
#include "fs/fs.h"  /* for fs_find */

/* Process table */
static process_t processes[MAX_PROCESSES];
static int current_process = -1;
static uint32_t next_pid = 1;

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

    /* Allocate code area */
    proc->code_base = USER_CODE_BASE;

    /* Map user code pages */
    for (size_t offset = 0; offset < code_size; offset += 4096) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            return -1;  /* Out of memory */
        }
        vmm_map_page(proc->code_base + offset, phys, 0x07);  /* User, R/W, Present */
    }

    /* Copy code to user space */
    /* Since we're in kernel mode, we need to access the mapped pages */
    /* For now, use identity mapping assumption */
    memcpy((void*)proc->code_base, code, code_size);

    /* Set up stack */
    proc->stack_top = USER_STACK_BASE;

    /* Map user stack pages */
    for (uint64_t addr = USER_STACK_BASE - USER_STACK_SIZE + 0x1000;
         addr <= USER_STACK_BASE;
         addr += 4096) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            return -1;
        }
        vmm_map_page(addr, phys, 0x07);  /* User, R/W, Present */
    }

    proc->entry_point = entry;
    proc->state = PROCESS_STATE_READY;

    return proc->pid;
}

void process_exit(int status) {
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

/* Jump to user mode using iretq
 * This function sets up the stack and uses iretq to jump to Ring 3
 */
void jump_to_user(uint64_t entry, uint64_t stack) {
    /* Set TSS stack pointer for syscalls */
    uint64_t kernel_stack;
    __asm__ volatile("mov %%rsp, %0" : "=r"(kernel_stack));
    tss_set_kernel_stack(kernel_stack);

    /* Push values for iretq in reverse order:
     * SS, RSP, RFLAGS, CS, RIP
     * We need to set RFLAGS with bit 1 set (reserved) and interrupt flag set
     */

    __asm__ volatile(
        "cli\n"                      /* Disable interrupts */
        "mov %0, %%rax\n"            /* Load stack address */
        "mov %%rax, %%rsp\n"         /* Set user stack */
        "pushq %1\n"                 /* SS = user data segment (0x20 | 3) */
        "pushq %%rax\n"              /* RSP */
        "pushfq\n"                   /* RFLAGS (current) */
        "orq $0x200, (%%rsp)\n"      /* Set interrupt flag in RFLAGS on stack */
        "pushq %2\n"                 /* CS = user code segment (0x18 | 3) */
        "pushq %3\n"                 /* RIP = entry point */
        "iretq\n"                    /* Jump to user mode */
        :
        : "r"(stack), "i"(USER_DS | 3), "i"(USER_CS | 3), "r"(entry)
        : "rax", "memory"
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