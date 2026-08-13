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
    /* Use inline assembly to avoid absolute address references */
    __asm__ volatile(
        "lea msg(%%rip), %%rbx\n"    /* Load address of msg using RIP-relative addressing */
        "mov $6, %%rax\n"             /* SYS_PUTS */
        "xor %%rcx, %%rcx\n"          /* arg1 = 0 */
        "int $0x80\n"
        "mov $0, %%rax\n"             /* SYS_EXIT */
        "xor %%rbx, %%rbx\n"          /* status = 0 */
        "int $0x80\n"
        "jmp .\n"                     /* Should never reach here */
        "msg: .ascii \"Hello from user mode!\\n\\0\"\n"
        ::: "rax", "rbx", "rcx", "memory"
    );
}