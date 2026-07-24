/*
 * User Program Test
 * This is a simple test program that runs in user mode (Ring 3)
 * It demonstrates basic syscall usage
 */

/* These are user-space helper functions for syscalls */
/* The kernel provides these through system call interface */

/* System call wrapper: int 0x80, syscall number in rax, args in rbx, rcx, rdx, r8, r9, r10 */
static inline long syscall0(long num) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline long syscall1(long num, long arg0) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(arg0) : "memory");
    return ret;
}

static inline long syscall2(long num, long arg0, long arg1) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(arg0), "c"(arg1) : "memory");
    return ret;
}

/* Syscall numbers - must match kernel */
#define SYS_EXIT    0
#define SYS_PUTS    6

/* Entry point - called by kernel after loading */
void _start(void) {
    /* Print a message using syscall */
    const char* msg = "Hello from user mode!\n";
    
    /* Use SYS_PUTS syscall */
    syscall2(SYS_PUTS, (long)msg, 0);
    
    /* Exit with status 0 */
    syscall1(SYS_EXIT, 0);
    
    /* Should never reach here */
    while (1);
}