#include "core/syscall.h"
#include "core/process.h"
#include "drivers/vga.h"
#include "lib/string.h"

/* System call handler table */
static syscall_handler_t syscall_table[SYS_MAX];

void syscall_init(void) {
    memset(syscall_table, 0, sizeof(syscall_table));

    /* Register default handlers */
    syscall_register(SYS_EXIT, sys_exit);
    syscall_register(SYS_WRITE, sys_write);
    syscall_register(SYS_GETPID, sys_getpid);
    syscall_register(SYS_PUTS, sys_puts);

    /* Enable SYSCALL/SYSRET instruction support
     * We need to:
     * 1. Enable SCE (System Call Enable) in EFER MSR
     * 2. Set up STAR/LSTAR/CSTAR MSRs for syscall entry point
     *
     * For simplicity, we'll use int 0x80 instead of syscall instruction
     * which is easier to set up.
     */
}

void syscall_register(syscall_num_t num, syscall_handler_t handler) {
    if (num < SYS_MAX) {
        syscall_table[num] = handler;
    }
}

/* System call dispatcher - called from interrupt handler */
uint64_t syscall_dispatcher(uint64_t num, uint64_t arg0, uint64_t arg1,
                            uint64_t arg2, uint64_t arg3, uint64_t arg4,
                            uint64_t arg5) {
    if (num >= SYS_MAX || syscall_table[num] == NULL) {
        return -1;  /* Invalid syscall */
    }
    return syscall_table[num](arg0, arg1, arg2, arg3, arg4, arg5);
}

/* System call implementations */

uint64_t sys_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                  uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;

    process_exit((int)status);
    return 0;  /* Never reached */
}

uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                   uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3; (void)unused4; (void)unused5;

    if (fd == 1 || fd == 2) {  /* stdout or stderr */
        /* Write to VGA */
        const char* str = (const char*)buf;
        for (uint64_t i = 0; i < count; i++) {
            vga_putchar(str[i]);
        }
        return count;
    }
    return -1;
}

uint64_t sys_getpid(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                    uint64_t unused4, uint64_t unused5, uint64_t unused6) {
    (void)unused1; (void)unused2; (void)unused3;
    (void)unused4; (void)unused5; (void)unused6;

    process_t* proc = process_get_current();
    if (proc) {
        return proc->pid;
    }
    return 0;
}

uint64_t sys_puts(uint64_t str, uint64_t unused1, uint64_t unused2,
                  uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;

    const char* s = (const char*)str;
    vga_puts(s);
    return 0;
}