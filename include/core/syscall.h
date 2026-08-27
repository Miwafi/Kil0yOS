#ifndef SYSCALL_H
#define SYSCALL_H

#include "lib/types.h"

/* System call numbers */
typedef enum {
    SYS_EXIT = 0,
    SYS_READ,
    SYS_WRITE,
    SYS_OPEN,       /* not implemented: dispatcher returns -1 */
    SYS_CLOSE,      /* not implemented: dispatcher returns -1 */
    SYS_GETPID,
    SYS_YIELD,
    SYS_PUTS,       /* Debug: print string */
    SYS_GETCHAR,
    SYS_PUTCHAR,
    SYS_MAX
} syscall_num_t;

/* System call handler type */
typedef uint64_t (*syscall_handler_t)(uint64_t arg0, uint64_t arg1,
                                       uint64_t arg2, uint64_t arg3,
                                       uint64_t arg4, uint64_t arg5);

/* User-space calling convention (must match isr.asm syscall_entry):
 *   num in rax, args in rbx, rcx, rdx, r8, r9, r10, return in rax */

/* Initialize system calls */
void syscall_init(void);

/* Register a system call handler */
void syscall_register(syscall_num_t num, syscall_handler_t handler);

/* System call dispatcher (called from assembly) */
uint64_t syscall_dispatcher(uint64_t num, uint64_t arg0, uint64_t arg1,
                            uint64_t arg2, uint64_t arg3, uint64_t arg4,
                            uint64_t arg5);

/* Individual syscall implementations */
uint64_t sys_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                  uint64_t unused3, uint64_t unused4, uint64_t unused5);
uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                  uint64_t unused3, uint64_t unused4, uint64_t unused5);
uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                   uint64_t unused3, uint64_t unused4, uint64_t unused5);
uint64_t sys_getpid(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                    uint64_t unused4, uint64_t unused5, uint64_t unused6);
uint64_t sys_yield(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                   uint64_t unused4, uint64_t unused5, uint64_t unused6);
uint64_t sys_puts(uint64_t str, uint64_t unused1, uint64_t unused2,
                  uint64_t unused3, uint64_t unused4, uint64_t unused5);
uint64_t sys_getchar(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                     uint64_t unused4, uint64_t unused5, uint64_t unused6);
uint64_t sys_putchar(uint64_t ch, uint64_t unused1, uint64_t unused2,
                     uint64_t unused3, uint64_t unused4, uint64_t unused5);

#endif
