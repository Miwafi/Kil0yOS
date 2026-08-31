/* Phase 3.1 debug probe: replicate the exact syscall sequence glibc's
 * ld.so uses to load libc.so.6, reporting each step. Static musl build. */
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>

int main(void) {
    long fd = syscall(SYS_openat, -100, "/lib/libc.so.6", O_RDONLY | O_CLOEXEC, 0);
    printf("openat=%ld\n", fd);
    if (fd < 0) return 1;

    struct stat st;
    long r = syscall(SYS_fstat, (long)fd, &st);
    printf("fstat=%ld size=%lu mode=%o\n", r, (unsigned long)st.st_size, st.st_mode);

    char buf[32];
    r = syscall(SYS_read, fd, buf, 16);
    printf("read=%ld\n", r);

    r = syscall(SYS_pread64, fd, buf, 16, 64);
    printf("pread64=%ld\n", r);

    r = syscall(SYS_close, fd);
    printf("close=%ld\n", r);

    /* reopen and do the 4-phase mmap dance ld.so performs */
    fd = syscall(SYS_openat, -100, "/lib/libc.so.6", O_RDONLY | O_CLOEXEC, 0);
    printf("reopen=%ld\n", fd);

    void* m = (void*)syscall(SYS_mmap, 0, 2170256UL, PROT_READ,
                             MAP_PRIVATE | MAP_DENYWRITE, fd, 0);
    printf("mmap_all=%p\n", m);
    if ((long)m < 0 && (long)m > -4096) return 2;

    void* m2 = (void*)syscall(SYS_mmap, (char*)m + 0x28000, 1605632UL,
                              PROT_READ | PROT_EXEC,
                              MAP_PRIVATE | MAP_FIXED | MAP_DENYWRITE, fd, 0x28000);
    printf("mmap_exec=%p\n", m2);

    void* m3 = (void*)syscall(SYS_mmap, (char*)m + 0x1ff000, 24576UL,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_FIXED | MAP_DENYWRITE, fd, 0x1fe000);
    printf("mmap_data=%p\n", m3);

    void* m4 = (void*)syscall(SYS_mmap, (char*)m + 0x205000, 52624UL,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    printf("mmap_bss=%p\n", m4);

    /* verify content: e_ident at base, and a byte deep in the exec segment */
    printf("elf_magic=%02x %c%c%c\n",
           ((uint8_t*)m)[0], ((uint8_t*)m)[1], ((uint8_t*)m)[2], ((uint8_t*)m)[3]);
    printf("exec_byte=%02x (expect nonzero)\n", ((uint8_t*)m2)[0]);
    fflush(stdout);
    return 0;
}
