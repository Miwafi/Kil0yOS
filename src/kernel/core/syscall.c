#include "core/syscall.h"
#include "core/process.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "lib/string.h"

/* System call handler table */
static syscall_handler_t syscall_table[SYS_MAX];

void syscall_init(void) {
    memset(syscall_table, 0, sizeof(syscall_table));

    /* Register handlers; SYS_OPEN/SYS_CLOSE stay unregistered (-1 = ENOSYS)
     * until a per-process fd table exists. */
    syscall_register(SYS_EXIT, sys_exit);
    syscall_register(SYS_READ, sys_read);
    syscall_register(SYS_WRITE, sys_write);
    syscall_register(SYS_GETPID, sys_getpid);
    syscall_register(SYS_YIELD, sys_yield);
    syscall_register(SYS_PUTS, sys_puts);
    syscall_register(SYS_GETCHAR, sys_getchar);
    syscall_register(SYS_PUTCHAR, sys_putchar);
    syscall_register(SYS_GFX_MODE, sys_gfx_mode);
    syscall_register(SYS_GFX_CLEAR, sys_gfx_clear);
    syscall_register(SYS_GFX_RECT, sys_gfx_rect);
    syscall_register(SYS_GFX_TEXT, sys_gfx_text);
    syscall_register(SYS_KEY_POLL, sys_key_poll);
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
        klog("[sc] ENOSYS\n");
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
        /* Reject kernel or unmapped user pointers before dereference */
        if (!process_check_user_range(buf, (size_t)count)) {
            return -1;
        }
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

    /* NUL-terminated scan with per-page validation: stops as soon as the
     * pointer leaves the process's mapped user regions, so a kernel
     * pointer is rejected on the first byte. */
    uint64_t p = str;
    uint64_t checked_page = 0;
    for (;;) {
        if ((p & ~0xFFFULL) != checked_page) {
            if (!process_check_user_range(p, 1)) {
                return (uint64_t)-1;
            }
            checked_page = p & ~0xFFFULL;
        }
        char c = *(const char*)p;
        if (c == '\0') break;
        vga_putchar(c);
        p++;
    }
    return 0;
}

uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                  uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3; (void)unused4; (void)unused5;

    if (fd != 0) return -1;  /* only stdin supported */
    if (buf == 0 || count == 0) return 0;
    if (!process_check_user_range(buf, (size_t)count)) {
        return -1;
    }

    char* dst = (char*)buf;
    uint64_t n = 0;
    while (n < count) {
        char c = keyboard_getc();          /* blocks (hlt) until key */
        dst[n++] = c;
        if (c == '\n') break;              /* line-buffered read */
    }
    return n;
}

uint64_t sys_getchar(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                     uint64_t unused4, uint64_t unused5, uint64_t unused6) {
    (void)unused1; (void)unused2; (void)unused3;
    (void)unused4; (void)unused5; (void)unused6;

    return (uint64_t)(unsigned char)keyboard_getc();
}

uint64_t sys_putchar(uint64_t ch, uint64_t unused1, uint64_t unused2,
                     uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;

    vga_putchar((char)ch);
    return (uint64_t)(unsigned char)ch;
}

uint64_t sys_yield(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                   uint64_t unused4, uint64_t unused5, uint64_t unused6) {
    (void)unused1; (void)unused2; (void)unused3;
    (void)unused4; (void)unused5; (void)unused6;

    /* Give up the rest of this time slice: raise IRQ0 so the scheduler
     * parks our frame and resumes the kernel main task. We are running
     * in ring 0 here (inside the int 0x80 handler), so a software int
     * to the IRQ0 gate is legal. */
    __asm__ volatile("int $32");
    return 0;
}

/* --- Ring3 graphics / game support ---------------------------------
 * User programs render through the kernel's mode-13h framebuffer
 * instead of touching 0xA0000 directly (identity-map pages are
 * kernel-only). All vga_* drawing primitives are no-ops when the
 * display is not in graphics mode, so stray calls are harmless. */

uint64_t sys_gfx_mode(uint64_t mode, uint64_t unused1, uint64_t unused2,
                      uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3;
    (void)unused4; (void)unused5;

    if (mode) {
        vga_set_mode_13h();
    } else {
        vga_set_text_mode();
    }
    return 0;
}

uint64_t sys_gfx_clear(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                       uint64_t unused4, uint64_t unused5, uint64_t unused6) {
    (void)unused1; (void)unused2; (void)unused3;
    (void)unused4; (void)unused5; (void)unused6;

    vga_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, COLOR_BLACK);
    return 0;
}

uint64_t sys_gfx_rect(uint64_t x, uint64_t y, uint64_t w,
                      uint64_t h, uint64_t color, uint64_t unused5) {
    (void)unused5;

    vga_fill_rect((int)x, (int)y, (int)w, (int)h, (uint8_t)color);
    return 0;
}

uint64_t sys_gfx_text(uint64_t x, uint64_t y, uint64_t str,
                      uint64_t color, uint64_t unused4, uint64_t unused5) {
    (void)unused4; (void)unused5;

    /* Validate the user string page-by-page while copying into a bounded
     * kernel buffer (same discipline as sys_puts), then draw with the
     * kernel's 8x8 font. */
    char tmp[64];
    uint64_t p = str;
    uint64_t checked_page = 0;
    size_t n = 0;
    for (;;) {
        if ((p & ~0xFFFULL) != checked_page) {
            if (!process_check_user_range(p, 1)) {
                return (uint64_t)-1;
            }
            checked_page = p & ~0xFFFULL;
        }
        char c = *(const char*)p;
        if (c == '\0') break;
        if (n < sizeof(tmp) - 1) tmp[n++] = c;
        p++;
    }
    tmp[n] = '\0';
    vga_draw_string((int)x, (int)y, tmp, (uint8_t)color);
    return 0;
}

uint64_t sys_key_poll(uint64_t unused1, uint64_t unused2, uint64_t unused3,
                      uint64_t unused4, uint64_t unused5, uint64_t unused6) {
    (void)unused1; (void)unused2; (void)unused3;
    (void)unused4; (void)unused5; (void)unused6;

    if (!keyboard_has_input()) return 0;
    return (uint64_t)(unsigned char)keyboard_getc();
}