#include "shell/shell.h"
#include "shell/terminal.h"
#include "drivers/vga.h"
#include "drivers/fb.h"
#include "drivers/efi_gop.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "fs/fs.h"
#include "mm/memory.h"
#include "sched/scheduler.h"
#include "core/interrupts.h"
#include "core/smp.h"
#include "core/nmi_wdt.h"
#include "core/process.h"
#include "drivers/power.h"
#include "drivers/pci.h"
#include "drivers/rtc.h"
#include "fs/edit.h"
#include "net/netif.h"
#include "net/icmp.h"
#include "net/arp.h"
#include "net/udp.h"
#include "net/tftp.h"
#include "pkg/dpkg.h"
#include "pkg/kilget.h"
#include "usb/usb.h"
#include "drivers/jpeg.h"
#include "drivers/audio.h"
#include "drivers/mp3.h"

/* Redirect VGA output calls inside command handlers to the active terminal */
#define vga_puts      term_puts
#define vga_putchar   term_putchar
#define vga_set_color term_set_color
#define vga_clear     term_clear

static char current_path[MAX_PATH_LENGTH];

static void update_prompt();
static int cmd_ls(int argc, char** argv);
static int cmd_cd(int argc, char** argv);
static int cmd_mkdir(int argc, char** argv);
static int cmd_help(int argc, char** argv);
static int cmd_echo(int argc, char** argv);
static int cmd_shutdown(int argc, char** argv);
static int cmd_pwd(int argc, char** argv);
static int cmd_clear(int argc, char** argv);
static int cmd_rm(int argc, char** argv);
static int cmd_touch(int argc, char** argv);
static int cmd_cat(int argc, char** argv);
static int cmd_whoami(int argc, char** argv);
static int cmd_version(int argc, char** argv);
static int cmd_edit(int argc, char** argv);
static int cmd_date(int argc, char** argv);
static int cmd_time(int argc, char** argv);
static int cmd_gfx(int argc, char** argv);
static int cmd_gui(int argc, char** argv);
static int cmd_desktop(int argc, char** argv);
static int cmd_ping(int argc, char** argv);
static int cmd_ifconfig(int argc, char** argv);
static int cmd_netstat(int argc, char** argv);
static int cmd_net(int argc, char** argv);
static int cmd_usb(int argc, char** argv);
static int cmd_exec(int argc, char** argv);
static int cmd_tftp(int argc, char** argv);
static int cmd_dpkg(int argc, char** argv);
static int cmd_kilget(int argc, char** argv);
static int cmd_memstat(int argc, char** argv);
static int cmd_nmi(int argc, char** argv);
static int cmd_painme(int argc, char** argv);

static shell_command_t commands[] = {
    {"ls", "List directory contents", cmd_ls},
    {"cd", "Change directory", cmd_cd},
    {"pwd", "Print working directory", cmd_pwd},
    {"mkdir", "Create directory", cmd_mkdir},
    {"rm", "Remove file or directory", cmd_rm},
    {"touch", "Create empty file", cmd_touch},
    {"cat", "Display file contents", cmd_cat},
    {"clear", "Clear screen", cmd_clear},
    {"echo", "Print text", cmd_echo},
    {"whoami", "Print current user", cmd_whoami},
    {"version", "Show OS version", cmd_version},
    {"edit", "Edit file", cmd_edit},
    {"gfx", "Graphical display test", cmd_gfx},
    {"gui", "Launch desktop GUI", cmd_gui},
    {"desktop", "Launch GOP desktop (UEFI boot)", cmd_desktop},
    {"ping", "Ping a host", cmd_ping},
    {"ifconfig", "Show network configuration", cmd_ifconfig},
    {"netstat", "Show network status", cmd_netstat},
    {"net", "Network info / subcommand (ping|ifconfig|netstat)", cmd_net},
    {"usb", "USB controller/port/device status", cmd_usb},
    {"tftp", "Download a file via TFTP (installs to /bin)", cmd_tftp},
    {"dpkg", "Package tool: dpkg -i file.deb | -r pkg | -l | -L pkg", cmd_dpkg},
    {"kilget", "Repo client: kilget update|install|show|list|installed", cmd_kilget},
    {"apt-get", "Alias of kilget (update|install)", cmd_kilget},
    {"memstat", "Show memory usage (PMM/kernel/heap/fb/index)", cmd_memstat},
    {"nmi", "NMI watchdog status (armed, delivery ticks)", cmd_nmi},
    {"painme", "Trigger a kernel panic (test)", cmd_painme},
    {"date", "Show current date", cmd_date},
    {"time", "Show current time", cmd_time},
    {"exec", "Execute a user program", cmd_exec},
    {"help", "Show help information", cmd_help},
    {"shutdown", "Shut down the system", cmd_shutdown},
    {NULL, NULL, NULL}
};

static void update_prompt() {
    fs_entry_t* dir = fs_current();
    char temp[MAX_PATH_LENGTH];
    char* pos = temp + MAX_PATH_LENGTH - 1;
    
    *pos = '\0';
    
    while (dir != NULL) {
        const char* name = dir->name;
        size_t len = strlen(name);
        
        if (pos - len < temp) {
            strcpy(current_path, "/");
            return;
        }
        
        pos -= len;
        memcpy(pos, name, len);
        
        if (dir->parent != NULL) {
            if (pos - 1 >= temp) {
                pos--;
                *pos = '/';
            }
        }
        
        dir = dir->parent;
    }
    
    strcpy(current_path, pos);
}

static int cmd_ls(int argc, char** argv) {
    fs_entry_t* dir = fs_current();
    
    if (argc > 1) {
        dir = fs_resolve_path(argv[1]);
        if (dir == NULL) {
            vga_puts("ls: cannot access '");
            vga_puts(argv[1]);
            vga_puts("': No such file or directory\n");
            return 1;
        }
    }
    
    if (dir->type != FS_TYPE_DIRECTORY) {
        vga_puts(dir->name);
        vga_puts("\n");
        return 0;
    }
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir->children[i] != NULL) {
            if (dir->children[i]->type == FS_TYPE_DIRECTORY) {
                vga_set_color(vga_entry_color(COLOR_LIGHT_BLUE, COLOR_BLACK));
            } else {
                vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));
            }
            vga_puts(dir->children[i]->name);
            vga_set_color(vga_entry_color(COLOR_GREY, COLOR_BLACK));
            vga_puts("  ");
        }
    }
    vga_puts("\n");
    return 0;
}

static int cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        fs_set_current(fs_root());
        update_prompt();
        return 0;
    }
    
    fs_entry_t* dir = fs_resolve_path(argv[1]);
    if (dir == NULL) {
        vga_puts("cd: no such file or directory: ");
        vga_puts(argv[1]);
        vga_puts("\n");
        return 1;
    }
    
    if (dir->type != FS_TYPE_DIRECTORY) {
        vga_puts("cd: not a directory: ");
        vga_puts(argv[1]);
        vga_puts("\n");
        return 1;
    }
    
    fs_set_current(dir);
    update_prompt();
    
    return 0;
}

static int cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("mkdir: missing operand\n");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        fs_entry_t* result = fs_create_dir(argv[i]);
        if (result == NULL) {
            int err = fs_get_last_error();
            vga_puts("mkdir: cannot create directory '");
            vga_puts(argv[i]);
            vga_puts("': ");
            if (err == FS_ERR_EXISTS) {
                vga_puts("File exists");
            } else if (err == FS_ERR_FULL) {
                vga_puts("Directory full");
            } else {
                vga_puts("Unknown error");
            }
            vga_puts("\n");
            return 1;
        }
    }
    
    return 0;
}

static int cmd_help(int argc, char** argv) {
    vga_puts("Kil0yOS Shell - Available commands:\n");
    vga_puts("==================================\n");
    
    for (int i = 0; commands[i].name != NULL; i++) {
        vga_puts("  ");
        vga_set_color(vga_entry_color(COLOR_LIGHT_GREEN, COLOR_BLACK));
        vga_puts(commands[i].name);
        vga_set_color(vga_entry_color(COLOR_GREY, COLOR_BLACK));
        vga_puts("  - ");
        vga_puts(commands[i].help);
        vga_puts("\n");
    }
    
    return 0;
}

static int cmd_echo(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("\n");
        return 0;
    }
    
    int redirect_pos = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0) {
            redirect_pos = i;
            break;
        }
    }
    
    if (redirect_pos != -1) {
        if (redirect_pos + 1 >= argc) {
            vga_puts("echo: missing file operand\n");
            return 1;
        }
        
        const char* filename = argv[redirect_pos + 1];
        fs_entry_t* file = fs_resolve_path(filename);
        
        if (file == NULL) {
            file = fs_create_file(filename);
            if (file == NULL) {
                vga_puts("echo: cannot create file '");
                vga_puts(filename);
                vga_puts("'\n");
                return 1;
            }
        }
        
        if (file->type != FS_TYPE_FILE) {
            vga_puts("echo: '");
            vga_puts(filename);
            vga_puts("' is a directory\n");
            return 1;
        }
        
        /* Size the buffer from the actual arguments: kmalloc(MAX_FILE_SIZE)
         * became a 32 MB request after the Phase 4.3 cap raise and failed
         * the kernel heap (error printed to VGA only). */
        size_t total_len = 0;
        for (int i = 1; i < redirect_pos; i++) {
            total_len += strlen(argv[i]) + 1;   /* +1 for the joining space/NUL */
        }
        if (total_len > MAX_FILE_SIZE - 1) {
            total_len = MAX_FILE_SIZE - 1;
        }

        char* content = (char*)kmalloc(total_len + 1);
        if (content == NULL) {
            vga_puts("echo: memory error\n");
            return 1;
        }
        int content_len = 0;

        for (int i = 1; i < redirect_pos; i++) {
            const char* arg = argv[i];
            size_t arg_len = strlen(arg);

            if ((size_t)content_len + arg_len + 1 > total_len) {
                break;
            }

            if (content_len > 0) {
                content[content_len++] = ' ';
            }

            memcpy(content + content_len, arg, arg_len);
            content_len += arg_len;
        }

        content[content_len] = '\0';

        if (fs_write_file(file, (uint8_t*)content, content_len) < 0) {
            vga_puts("echo: write error\n");
            kfree(content);
            return 1;
        }

        kfree(content);
        return 0;
    }
    
    for (int i = 1; i < argc; i++) {
        vga_puts(argv[i]);
        if (i < argc - 1) {
            vga_puts(" ");
        }
    }
    vga_puts("\n");
    return 0;
}

static int cmd_shutdown(int argc, char** argv) {
    fs_save();
    vga_puts("Shutting down Kil0yOS...\n");
    power_shutdown();
    return 0;
}

static int cmd_pwd(int argc, char** argv) {
    vga_puts(current_path);
    vga_puts("\n");
    return 0;
}

static int cmd_clear(int argc, char** argv) {
    vga_clear();
    return 0;
}

static int cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("rm: missing operand\n");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        if (fs_delete_entry(argv[i]) != 0) {
            vga_puts("rm: cannot remove '");
            vga_puts(argv[i]);
            vga_puts("': No such file or directory\n");
            return 1;
        }
    }
    
    return 0;
}

static int cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("touch: missing operand\n");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        fs_entry_t* result = fs_create_file(argv[i]);
        if (result == NULL) {
            int err = fs_get_last_error();
            vga_puts("touch: cannot create file '");
            vga_puts(argv[i]);
            vga_puts("': ");
            if (err == FS_ERR_EXISTS) {
                vga_puts("File exists");
            } else if (err == FS_ERR_FULL) {
                vga_puts("Directory full");
            } else {
                vga_puts("Unknown error");
            }
            vga_puts("\n");
            return 1;
        }
    }
    
    return 0;
}

static int cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("cat: missing operand\n");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        fs_entry_t* file = fs_resolve_path(argv[i]);
        if (file == NULL) {
            vga_puts("cat: cannot access '");
            vga_puts(argv[i]);
            vga_puts("': No such file or directory\n");
            return 1;
        }
        
        if (file->type != FS_TYPE_FILE) {
            vga_puts("cat: '");
            vga_puts(argv[i]);
            vga_puts("' is a directory\n");
            return 1;
        }
        
        uint8_t* buffer = (uint8_t*)kmalloc(file->size + 1);
        if (buffer == NULL) {
            vga_puts("cat: memory error\n");
            return 1;
        }
        
        int bytes_read = fs_read_file(file, buffer, file->size);
        if (bytes_read >= 0) {
            buffer[bytes_read] = '\0';
            vga_puts((char*)buffer);
        }
        vga_puts("\n");
        
        kfree(buffer);
    }
    
    return 0;
}

static int cmd_whoami(int argc, char** argv) {
    vga_puts("root\n");
    return 0;
}

static int cmd_version(int argc, char** argv) {
    vga_puts("Kil0yOS v3.5.0\n");
    vga_puts("A simple 64-bit x86-64 operating system\n");
    vga_puts("User mode (Ring 3) support enabled\n");
    return 0;
}

static int cmd_date(int argc, char** argv) {
    rtc_time_t t;
    if (rtc_read(&t) != 0) {
        vga_puts("Failed to read RTC\n");
        return 1;
    }
    char buf[32];
    vga_puts("Current date: ");
    itoa(t.year, buf, 10, sizeof(buf));
    vga_puts(buf);
    vga_puts("-");
    if (t.month < 10) vga_puts("0");
    itoa(t.month, buf, 10, sizeof(buf));
    vga_puts(buf);
    vga_puts("-");
    if (t.day < 10) vga_puts("0");
    itoa(t.day, buf, 10, sizeof(buf));
    vga_puts(buf);
    vga_puts("\n");
    return 0;
}

static int cmd_time(int argc, char** argv) {
    rtc_time_t t;
    if (rtc_read(&t) != 0) {
        vga_puts("Failed to read RTC\n");
        return 1;
    }
    char buf[32];
    vga_puts("Current time: ");
    if (t.hour < 10) vga_puts("0");
    itoa(t.hour, buf, 10, sizeof(buf));
    vga_puts(buf);
    vga_puts(":");
    if (t.minute < 10) vga_puts("0");
    itoa(t.minute, buf, 10, sizeof(buf));
    vga_puts(buf);
    vga_puts(":");
    if (t.second < 10) vga_puts("0");
    itoa(t.second, buf, 10, sizeof(buf));
    vga_puts(buf);
    vga_puts("\n");
    return 0;
}

static int cmd_edit(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("edit: missing file operand\n");
        return 1;
    }

    edit_file(argv[1]);
    return 0;
}

static int desktop_active = 0;

/* Shared desktop backend: the mode13h desktop (BIOS boot) and the GOP
 * desktop (UEFI boot) drive the same loop; the dt_* wrappers route the
 * drawing primitives to the active surface and dt_w/dt_h carry the
 * mode-specific geometry. */
static int dt_use_fb = 0;
static int dt_w = GFX_WIDTH;
static int dt_h = GFX_HEIGHT;

/* Desktop layout geometry (mode13h and GOP desktops share the layout):
 * left function panel | right-top shell terminal | right-bottom kernel log. */
static int lay_header_h, lay_footer_h;
static int lay_left_w;      /* function panel width */
static int lay_split_y;     /* y of the separator between shell and klog panes */

/* Left-panel functions, selectable through the Win-key popup menu */
enum { FUNC_EDITOR = 0, FUNC_FILES, FUNC_SYSTEM, FUNC_CATS, DT_MENU_COUNT };
static const char* dt_menu_items[DT_MENU_COUNT] = {
    "Editor", "Files", "System", "CATs"
};
static int active_func = FUNC_EDITOR;

/* Fixed function-selector block at the top of the left panel: one row per
 * function with its F-key hint; panel content starts below it. */
#define SEL_ROWS   DT_MENU_COUNT
#define SEL_ROW_H  12
static int panel_content_top(void) {
    return lay_header_h + 1 + SEL_ROWS * SEL_ROW_H + 6;
}

static void dt_fill_rect(int x, int y, int w, int h, uint8_t c) {
    if (dt_use_fb) fb_gfx_fill_rect(x, y, w, h, c);
    else           vga_fill_rect(x, y, w, h, c);
}

static void dt_draw_rect(int x, int y, int w, int h, uint8_t c) {
    if (dt_use_fb) fb_gfx_draw_rect(x, y, w, h, c);
    else           vga_draw_rect(x, y, w, h, c);
}

static void dt_draw_string(int x, int y, const char* s, uint8_t c) {
    if (dt_use_fb) fb_gfx_draw_string(x, y, s, c);
    else           vga_draw_string(x, y, s, c);
}

static void dt_wait_vsync(void) {
    if (!dt_use_fb) vga_wait_vsync();   /* no legacy VGA regs under GOP */
}

static void dt_cursor_draw(int x, int y) {
    if (dt_use_fb) fb_gfx_cursor_draw(x, y);
    else           mouse_draw_cursor(x, y);
}

static void dt_cursor_erase(int x, int y) {
    if (dt_use_fb) fb_gfx_cursor_erase(x, y);
    else           mouse_erase_cursor(x, y);
}

/* ===== true-color pixel (image viewer) =====
 * GOP: native 0xRRGGBB write.  mode12h desktop: the fixed EGA-16 palette
 * gets 4x4 ordered dithering (Bayer matrix) before nearest-color matching
 * so JPEG gradients survive as spatial color mixes. */
static const uint32_t dt_ega_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};
static const uint8_t dt_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

static uint8_t dt_nearest_ega(uint32_t rgb, int x, int y) {
    int d = (dt_bayer[y & 3][x & 3] * 34 - 255) / 6;   /* ~[-42, +42] */
    int r = (int)((rgb >> 16) & 0xFF) + d;
    int g = (int)((rgb >> 8) & 0xFF) + d;
    int b = (int)(rgb & 0xFF) + d;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    uint8_t best = 0;
    int bestd = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)((dt_ega_rgb[i] >> 16) & 0xFF);
        int dg = g - (int)((dt_ega_rgb[i] >> 8) & 0xFF);
        int db = b - (int)(dt_ega_rgb[i] & 0xFF);
        int dist = dr * dr + dg * dg + db * db;
        if (dist < bestd) {
            bestd = dist;
            best = (uint8_t)i;
        }
    }
    return best;
}

static void dt_pixel_rgb(int x, int y, uint32_t rgb) {
    if (dt_use_fb) fb_gfx_pixel_rgb(x, y, rgb);
    else           vga_plot_pixel(x, y, dt_nearest_ega(rgb, x, y));
}

/* ===== Win-key function menu popup ===== */
static void menu_popup_draw(int sel) {
    int spacing = dt_use_fb ? 16 : 12;
    int w = 18 * 8;                          /* widest item + padding */
    int h = DT_MENU_COUNT * spacing + 20;
    int x = (dt_w - w) / 2;
    int y = (dt_h - h) / 2;

    dt_fill_rect(x, y, w, h, 0x0F);
    dt_draw_rect(x, y, w, h, 0x09);
    dt_draw_rect(x + 1, y + 1, w - 2, h - 2, 0x09);
    dt_draw_string(x + 4, y + 3, "Menu", 0x09);

    for (int i = 0; i < DT_MENU_COUNT; i++) {
        int iy = y + 14 + i * spacing;
        if (i == sel) {
            dt_fill_rect(x + 3, iy - 1, w - 6, spacing - 1, 0x01);
            dt_draw_string(x + 8, iy, dt_menu_items[i], 0x0F);
        } else {
            dt_draw_string(x + 8, iy, dt_menu_items[i], 0x00);
        }
    }
}

/* ===== Kernel-log pane =====
 * Renders the klog serial mirror (ring buffer in main.c) as a scrolling
 * read-only view in the right-bottom pane. Line-based: bytes are pumped
 * from the ring, split on '\n', and the last kl_rows lines are drawn. */
#define KL_MAX_ROWS 48
#define KL_MAX_COLS 100
static char kl_lines[KL_MAX_ROWS][KL_MAX_COLS + 1];
static int  kl_head, kl_count;          /* line ring */
static char kl_acc[KL_MAX_COLS + 1];    /* partial line accumulator */
static int  kl_len;
static uint32_t kl_seq;                 /* reader position in the klog ring */
static int  kl_rows, kl_cols;
static int  kl_bx, kl_by;               /* glyph origin */
static int  kl_cx, kl_cy, kl_cw, kl_ch; /* repaint region */

static void klog_view_init(int bx, int by, int cols, int rows,
                           int cx, int cy, int cw, int ch) {
    kl_bx = bx; kl_by = by;
    kl_cols = (cols < 1) ? 1 : (cols > KL_MAX_COLS ? KL_MAX_COLS : cols);
    kl_rows = (rows < 1) ? 1 : (rows > KL_MAX_ROWS ? KL_MAX_ROWS : rows);
    kl_cx = cx; kl_cy = cy; kl_cw = cw; kl_ch = ch;
    kl_head = 0; kl_count = 0; kl_len = 0;
    kl_seq = klog_seq();
    /* show the tail of the logs emitted before the desktop started */
    if (kl_seq > 1024) kl_seq -= 1024;
    for (int r = 0; r < KL_MAX_ROWS; r++) kl_lines[r][0] = '\0';
}

static void kl_push_line(const char* s) {
    /* strip the "[   time] " klog timestamp so narrow panes keep room
     * for the message itself */
    if (s[0] == '[') {
        const char* p = s;
        while (*p && *p != ']') p++;
        if (*p == ']') {
            p++;
            while (*p == ' ') p++;
            s = p;
        }
    }
    if (kl_count == KL_MAX_ROWS) {
        kl_head = (kl_head + 1) % KL_MAX_ROWS;
        kl_count--;
    }
    int slot = (kl_head + kl_count) % KL_MAX_ROWS;
    int i = 0;
    while (s[i] && i < kl_cols) {
        kl_lines[slot][i] = s[i];
        i++;
    }
    kl_lines[slot][i] = '\0';
    kl_count++;
}

/* Pull new bytes from the klog ring; returns 1 when the view needs repaint */
static int klog_view_pump(void) {
    char tmp[256];
    int dirty = 0;
    for (int guard = 0; guard < 8; guard++) {       /* bounded per frame */
        uint32_t n = klog_copy_from(&kl_seq, tmp, (uint32_t)sizeof(tmp));
        if (n == 0) break;
        for (uint32_t i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == '\n') {
                kl_acc[kl_len] = '\0';
                kl_push_line(kl_acc);
                kl_len = 0;
                dirty = 1;
            } else if (c >= 32 && c <= 126) {
                if (kl_len < kl_cols) kl_acc[kl_len++] = c;
                /* beyond the pane width: dropped (log view) */
            }
        }
    }
    return dirty;
}

static void klog_view_render(void) {
    dt_fill_rect(kl_cx, kl_cy, kl_cw, kl_ch, 0x0F);
    int first = (kl_count > kl_rows) ? kl_count - kl_rows : 0;
    for (int r = 0; r < kl_rows; r++) {
        int idx = first + r;
        if (idx >= kl_count) break;
        dt_draw_string(kl_bx, kl_by + r * 8,
                       kl_lines[(kl_head + idx) % KL_MAX_ROWS], 0x00);
    }
}

static int cmd_gfx(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (desktop_active) {
        vga_puts("gfx: not available in graphical mode\n");
        return 1;
    }
    if (fb_is_active()) {
        term_puts("gfx: VGA graphics test needs the BIOS path (BIOS boot)\n");
        return 1;
    }

    vga_puts("Switching to graphical mode...\n");

    vga_set_gfx_mode();
    vga_wait_vsync();
    vga_draw_color_bars();

    while (1) {
        unsigned char k = (unsigned char)keyboard_getc();
        if (k == 'q' || k == KEY_ESC) break;
    }

    vga_set_text_mode();
    vga_puts("Returned to text mode.\n");
    return 0;
}

/* GUI Shell state */
#define GUI_SHELL_BUF_SIZE 64
static char gui_shell_buf[GUI_SHELL_BUF_SIZE];
static int gui_shell_len = 0;

/* The prompt lives in the terminal cell grid (term_gui_prompt): it is
 * painted at the current cell cursor, so typed characters always land
 * after it and full re-renders keep it visible. */
static void gui_shell_draw_prompt(void) {
    term_gui_prompt();
}

static void gui_shell_init(int prompt_x, int prompt_y) {
    (void)prompt_x;
    (void)prompt_y;   /* position comes from the cell cursor (base corner) */
    gui_shell_len = 0;
    gui_shell_buf[0] = '\0';
    gui_shell_draw_prompt();
}

static int execute_command(char* cmd);

static void gui_shell_execute(void) {
    gui_shell_buf[gui_shell_len] = '\0';

    char cmd_buf[GUI_SHELL_BUF_SIZE];
    strncpy(cmd_buf, gui_shell_buf, GUI_SHELL_BUF_SIZE - 1);
    cmd_buf[GUI_SHELL_BUF_SIZE - 1] = '\0';

    term_putchar('\n');
    klog("[dsh] exec: ");         /* TEMP: when did this command run */
    klog(cmd_buf);
    klog("\n");
    execute_command(cmd_buf);
    term_putchar('\n');
    gui_shell_draw_prompt();      /* "> " into cells at the new cursor */
    term_gui_render();            /* repaint incl. the prompt cells */

    gui_shell_len = 0;
    gui_shell_buf[0] = '\0';
}

/* ===== Files panel: graphical file manager =====
 * Left-panel browser over the fs_entry_t tree. Directories open on Enter,
 * files open a text preview; M/N create a directory/file in the browsed
 * directory (never touching the shell's cwd: creation goes through
 * absolute paths), D deletes with confirmation. Mouse: click selects,
 * clicking the selected row opens. */
static void desktop_repaint(void);   /* fwd: modal close repaints everything */
extern void klog(const char* s);

enum { FM_BROWSE = 0, FM_INPUT, FM_CONFIRM, FM_PREVIEW, FM_IMAGE, FM_AUDIO };
static fs_entry_t* fm_dir = NULL;               /* browsed directory */
static int fm_sel = 0;                          /* selected row */
static int fm_scroll = 0;                       /* first visible row */
static fs_entry_t* fm_list[MAX_DIR_ENTRIES];    /* rows; NULL = ".." up */
static int fm_count = 0;                        /* rows incl. ".." */
static int fm_mode = FM_BROWSE;
static int fm_input_kind;                       /* 0 = file, 1 = directory */
static char fm_input_buf[40];
static int fm_input_len;
static char fm_status[64];                      /* last action / error */

/* FM_IMAGE: decoded JPEG planes stay live until the viewer closes */
static jpeg_image_t fm_img;
static char fm_img_title[96];
static int  fm_img_fail;                        /* 1 = show fm_img_err text */
static char fm_img_err[40];

/* FM_AUDIO: MP3 playback over AC97, pumped from the desktop loop */
static mp3_t*   fm_mp3;                         /* decoder handle */
static uint8_t* fm_mp3_data;                    /* whole-file buffer */
static int16_t* fm_mp3_pcm;                     /* one decoded frame */
static int      fm_mp3_pending;                 /* frames not yet in ring */
static int      fm_mp3_pending_off;             /* offset into fm_mp3_pcm */
static int      fm_audio_paused;
static int      fm_audio_done;                  /* EOF reached, draining */
static char     fm_audio_title[96];
static char     fm_audio_info[64];              /* "48 kHz  128 kbps  stereo" */
static uint64_t fm_audio_frames;                /* stereo frames handed over */

/* 24 rows: tall enough for the whole hda.log probe dump on one screen */
#define FM_PV_ROWS 24
#define FM_PV_COLS 76
static char fm_pv[FM_PV_ROWS][FM_PV_COLS + 1];
static int fm_pv_count;
static char fm_pv_title[96];

/* list geometry captured by fm_render for mouse hit testing */
static int fm_list_y0, fm_row_h, fm_vis;

static void fm_clipn(char* out, int outsz, const char* s, int maxc) {
    int i = 0;
    if (maxc > outsz - 1) maxc = outsz - 1;
    while (s[i] && i < maxc) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

/* clipped draw inside the left panel (panel interior ends at lay_left_w-2) */
static void fm_sclip(int x, int y, const char* s, uint8_t col) {
    int avail = lay_left_w - 2 - x;
    if (avail < 8) return;
    char buf[40];
    fm_clipn(buf, (int)sizeof(buf), s, avail / 8);
    dt_draw_string(x, y, buf, col);
}

static void fm_abs_path(fs_entry_t* e, char* out, int outsz) {
    if (e == NULL || e->parent == NULL) {
        strcpy(out, "/");
        return;
    }
    const char* comps[32];
    int n = 0;
    for (fs_entry_t* p = e; p != NULL && p->parent != NULL && n < 32; p = p->parent) {
        comps[n++] = p->name;
    }
    int pos = 0;
    out[pos++] = '/';
    for (int i = n - 1; i >= 0; i--) {
        int len = strlen(comps[i]);
        if (pos + len + 2 >= outsz) break;
        memcpy(out + pos, comps[i], len);
        pos += len;
        if (i > 0) out[pos++] = '/';
    }
    out[pos] = '\0';
}

static int fm_ci_cmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return (ca < cb) ? -1 : 1;
        a++;
        b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* Pointer validation: the shell can delete (and free) the directory we are
 * browsing, so never dereference fm_dir without proving it is still in the
 * tree. Only compares pointers, safe even for a dangling fm_dir. */
static int fm_in_tree(fs_entry_t* node, fs_entry_t* target) {
    if (node == target) return 1;
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (node->children[i] != NULL &&
            node->children[i]->type == FS_TYPE_DIRECTORY &&
            fm_in_tree(node->children[i], target)) {
            return 1;
        }
    }
    return 0;
}

/* Rebuild the sorted row list: ".." first, then directories before files,
 * each group case-insensitive alphabetical. */
static void fm_refresh(void) {
    if (fm_dir == NULL) fm_dir = fs_current();
    if (fm_dir == NULL || !fm_in_tree(fs_root(), fm_dir)) {
        fm_dir = fs_root();
        fm_sel = 0;
        fm_scroll = 0;
    }

    static fs_entry_t* tmp[MAX_DIR_ENTRIES];
    int t = 0;
    for (int i = 0; i < MAX_DIR_ENTRIES && t < MAX_DIR_ENTRIES; i++) {
        if (fm_dir->children[i] != NULL) tmp[t++] = fm_dir->children[i];
    }
    for (int i = 1; i < t; i++) {
        fs_entry_t* key = tmp[i];
        int j = i - 1;
        while (j >= 0) {
            int rj = (tmp[j]->type != FS_TYPE_DIRECTORY) ? 1 : 0;
            int rk = (key->type != FS_TYPE_DIRECTORY) ? 1 : 0;
            if (rj > rk || (rj == rk && fm_ci_cmp(tmp[j]->name, key->name) > 0)) {
                tmp[j + 1] = tmp[j];
                j--;
            } else {
                break;
            }
        }
        tmp[j + 1] = key;
    }

    int n = 0;
    if (fm_dir->parent != NULL) fm_list[n++] = NULL;   /* ".." row */
    for (int i = 0; i < t && n < MAX_DIR_ENTRIES; i++) {
        fm_list[n++] = tmp[i];
    }
    fm_count = n;
    if (fm_sel >= fm_count) fm_sel = (fm_count > 0) ? fm_count - 1 : 0;
    if (fm_sel < 0) fm_sel = 0;
}

static void fm_size_str(uint32_t sz, char* out) {
    if (sz < 1024) {
        itoa((int)sz, out, 10, 8);
    } else if (sz < 1024u * 1024) {
        itoa((int)(sz / 1024), out, 10, 8);
        strcat(out, "K");
    } else {
        itoa((int)(sz / (1024u * 1024)), out, 10, 8);
        strcat(out, "M");
    }
}

static void fm_render(void) {
    int cx = 4;
    int ctop = panel_content_top();
    int y = ctop + 2;
    int bottom = dt_h - lay_footer_h - 1;

    dt_fill_rect(1, ctop, lay_left_w - 2, bottom - ctop, 0x0F);

    fm_sclip(cx, y, "Files", 0x09);
    if (fm_dir != NULL) {
        char cnt[12];
        itoa(fm_count, cnt, 10, sizeof(cnt));
        int cxx = lay_left_w - 2 - (int)strlen(cnt) * 8 - 4;
        if (cxx > cx + 48) fm_sclip(cxx, y, cnt, 0x08);
    }
    y += 11;

    /* current path, tail-truncated (deepest components matter most) */
    char p[MAX_PATH_LENGTH];
    fm_abs_path(fm_dir, p, (int)sizeof(p));
    int pmax = (lay_left_w - 2 - cx) / 8;
    if ((int)strlen(p) > pmax) {
        char pt[MAX_PATH_LENGTH];
        pt[0] = '~';
        strcpy(pt + 1, p + strlen(p) - (pmax - 1));
        fm_sclip(cx, y, pt, 0x08);
    } else {
        fm_sclip(cx, y, p, 0x08);
    }
    y += 10;
    dt_fill_rect(cx, y, lay_left_w - 8, 1, 0x07);
    y += 4;

    fm_row_h = dt_use_fb ? 12 : 10;
    int list_h = bottom - 34 - y;
    if (list_h < fm_row_h) list_h = fm_row_h;
    fm_list_y0 = y;
    fm_vis = list_h / fm_row_h;
    if (fm_vis < 1) fm_vis = 1;

    if (fm_scroll > fm_sel) fm_scroll = fm_sel;
    if (fm_sel >= fm_scroll + fm_vis) fm_scroll = fm_sel - fm_vis + 1;
    if (fm_scroll < 0) fm_scroll = 0;
    if (fm_count > 0 && fm_scroll > fm_count - 1) fm_scroll = fm_count - 1;

    int text_x = cx + 2;
    if (fm_count == 0) {
        fm_sclip(text_x, fm_list_y0 + 2, "(empty)", 0x08);
    } else {
        for (int r = 0; r < fm_vis; r++) {
            int idx = fm_scroll + r;
            if (idx >= fm_count) break;
            int ry = fm_list_y0 + r * fm_row_h;
            fs_entry_t* e = fm_list[idx];
            int is_dir = (e == NULL) || (e->type == FS_TYPE_DIRECTORY);
            if (idx == fm_sel) {
                dt_fill_rect(cx + 1, ry - 1, lay_left_w - 7 - cx, fm_row_h - 1, 0x01);
            }
            uint8_t col = (idx == fm_sel) ? 0x0F
                        : (is_dir)          ? 0x01
                                            : 0x00;
            if (e == NULL) {
                fm_sclip(text_x, ry, "..", col);
            } else {
                char nb[64];
                fm_clipn(nb, (int)sizeof(nb) - 2, e->name, sizeof(nb) - 3);
                if (is_dir) strcat(nb, "/");
                fm_sclip(text_x, ry, nb, col);
                if (!is_dir) {
                    char sb[12];
                    fm_size_str(e->size, sb);
                    int sx = lay_left_w - 6 - (int)strlen(sb) * 8;
                    if (sx > text_x + (int)strlen(nb) * 8 + 8) {
                        fm_sclip(sx, ry, sb, 0x08);
                    }
                }
            }
        }
        if (fm_count > fm_vis) {
            int sb_x = lay_left_w - 5;
            dt_fill_rect(sb_x, fm_list_y0, 2, list_h, 0x07);
            int thumb = (fm_vis * list_h) / fm_count;
            if (thumb < 4) thumb = 4;
            int max_off = fm_count - fm_vis;
            int ty = fm_list_y0 + ((list_h - thumb) * fm_scroll) / (max_off > 0 ? max_off : 1);
            dt_fill_rect(sb_x, ty, 2, thumb, 0x01);
        }
    }

    /* status + key hints */
    if (fm_status[0]) fm_sclip(cx, bottom - 26, fm_status, 0x04);
    fm_sclip(cx, bottom - 16, "Enter:open BS:up", 0x08);
    fm_sclip(cx, bottom - 8, "M:dir N:file D:del", 0x08);
}

static void fm_navigate(fs_entry_t* d) {
    if (d == NULL || d->type != FS_TYPE_DIRECTORY) return;
    fm_dir = d;
    fm_sel = 0;
    fm_scroll = 0;
    fm_status[0] = '\0';
    fm_refresh();
}

static void fm_preview_show(fs_entry_t* e) {
    fm_pv_count = 0;
    char sb[12];
    fm_size_str(e->size, sb);
    fm_clipn(fm_pv_title, (int)sizeof(fm_pv_title) - 8, e->name, 64);
    strcat(fm_pv_title, " (");
    strcat(fm_pv_title, sb);
    strcat(fm_pv_title, ")");

    if (e->size == 0) {
        strcpy(fm_pv[0], "<empty file>");
        fm_pv_count = 1;
    } else {
        size_t rd = (e->size < 4096) ? e->size : 4096;
        uint8_t* buf = (uint8_t*)kmalloc(rd + 1);
        if (buf == NULL) {
            strcpy(fm_pv[0], "<out of memory>");
            fm_pv_count = 1;
        } else {
            int got = fs_read_file(e, buf, rd);
            if (got <= 0) {
                strcpy(fm_pv[0], "<read error>");
                fm_pv_count = 1;
            } else {
                int binary = 0;
                for (int i = 0; i < got; i++) {
                    if (buf[i] == 0) { binary = 1; break; }
                }
                if (binary) {
                    strcpy(fm_pv[0], "<binary data>");
                    fm_pv_count = 1;
                } else {
                    const uint8_t* ptr = buf;
                    const uint8_t* end = buf + got;
                    while (ptr < end && fm_pv_count < FM_PV_ROWS) {
                        int i = 0;
                        while (ptr < end && *ptr != '\n' && *ptr != '\r' &&
                               i < FM_PV_COLS) {
                            char c = (char)*ptr;
                            if (c == '\t') c = ' ';
                            else if (c < 32 || c > 126) c = '.';
                            fm_pv[fm_pv_count][i++] = c;
                            ptr++;
                        }
                        fm_pv[fm_pv_count][i] = '\0';
                        fm_pv_count++;
                        while (ptr < end && (*ptr == '\n' || *ptr == '\r')) ptr++;
                    }
                    if (ptr < end && fm_pv_count > 0) {
                        strcpy(fm_pv[fm_pv_count - 1], "...");
                    }
                }
            }
            kfree(buf);
        }
    }
    fm_mode = FM_PREVIEW;
}

/* ===== FM_IMAGE viewer: baseline JPEG decode + render ===== */
static int fm_is_jpeg_name(const char* name) {
    size_t n = strlen(name);
    if (n >= 5 && fm_ci_cmp(name + n - 5, ".jpeg") == 0) return 1;
    if (n >= 4 && fm_ci_cmp(name + n - 4, ".jpg") == 0) return 1;
    return 0;
}

static void fm_image_show(fs_entry_t* e) {
    fm_img_fail = 1;
    fm_img_err[0] = '\0';
    fm_clipn(fm_img_title, (int)sizeof(fm_img_title) - 8, e->name, 64);

    if (e->size < 4) {
        strcpy(fm_img_err, "empty file");
    } else if (e->size > 8u * 1024 * 1024) {
        strcpy(fm_img_err, "file too large");
    } else {
        uint8_t* buf = (uint8_t*)kmalloc((size_t)e->size);
        if (buf == NULL) {
            strcpy(fm_img_err, "out of memory");
        } else {
            int got = fs_read_file(e, buf, (size_t)e->size);
            if (got <= 0) {
                strcpy(fm_img_err, "read error");
            } else if (jpeg_decode(buf, (size_t)got, &fm_img) != 0) {
                strcpy(fm_img_err, "unsupported jpeg");
            } else {
                fm_img_fail = 0;
                klog("[files] decoded ");
                klog(e->name);
                klog("\n");
            }
            kfree(buf);
        }
    }
    fm_mode = FM_IMAGE;
}

/* ===== FM_AUDIO player: MP3 decode -> AC97 DMA ring ===== */
static void fm_close_modal(void);   /* defined below the pump */

static int fm_is_mp3_name(const char* name) {
    size_t n = strlen(name);
    return n >= 4 && fm_ci_cmp(name + n - 4, ".mp3") == 0;
}

static void fm_audio_show(fs_entry_t* e) {
    fm_mp3 = NULL;
    fm_mp3_data = NULL;
    fm_mp3_pcm = NULL;
    fm_mp3_pending = 0;
    fm_mp3_pending_off = 0;
    fm_audio_paused = 0;
    fm_audio_done = 0;
    fm_audio_frames = 0;
    fm_clipn(fm_audio_title, (int)sizeof(fm_audio_title) - 8, e->name, 64);

    if (audio_init() != 0) {
        fm_img_fail = 1;
        /* the front-end keeps a short reason (which controller stage failed) */
        fm_clipn(fm_img_err, (int)sizeof(fm_img_err) - 1, audio_last_error(),
                 (int)sizeof(fm_img_err) - 1);
        fm_mode = FM_AUDIO;
        return;
    }
    if (e->size < 512 || e->size > 64u * 1024 * 1024) {
        fm_img_fail = 1;
        strcpy(fm_img_err, "bad file size");
        fm_mode = FM_AUDIO;
        return;
    }
    fm_mp3_data = (uint8_t*)kmalloc((size_t)e->size);
    if (fm_mp3_data == NULL) {
        fm_img_fail = 1;
        strcpy(fm_img_err, "out of memory");
        fm_mode = FM_AUDIO;
        return;
    }
    int got = fs_read_file(e, fm_mp3_data, (size_t)e->size);
    if (got <= 0) {
        kfree(fm_mp3_data);
        fm_mp3_data = NULL;
        fm_img_fail = 1;
        strcpy(fm_img_err, "read error");
        fm_mode = FM_AUDIO;
        return;
    }
    fm_mp3 = mp3_open(fm_mp3_data, (size_t)got);
    if (fm_mp3 == NULL) {
        kfree(fm_mp3_data);
        fm_mp3_data = NULL;
        fm_img_fail = 1;
        strcpy(fm_img_err, "no mp3 frames");
        fm_mode = FM_AUDIO;
        return;
    }
    if (audio_open((uint32_t)mp3_rate(fm_mp3)) != 0) {
        mp3_close(fm_mp3);
        fm_mp3 = NULL;
        kfree(fm_mp3_data);
        fm_mp3_data = NULL;
        fm_img_fail = 1;
        strcpy(fm_img_err, "audio open failed");
        fm_mode = FM_AUDIO;
        return;
    }
    fm_mp3_pcm = (int16_t*)kmalloc(sizeof(int16_t) * 1152 * 2);
    if (fm_mp3_pcm == NULL) {
        audio_close();
        mp3_close(fm_mp3);
        fm_mp3 = NULL;
        kfree(fm_mp3_data);
        fm_mp3_data = NULL;
        fm_img_fail = 1;
        strcpy(fm_img_err, "out of memory");
        fm_mode = FM_AUDIO;
        return;
    }
    fm_img_fail = 0;
    {
        const char* m = mp3_channels(fm_mp3) == 2 ? "stereo" : "mono";
        fm_audio_info[0] = '\0';
        char num[16];
        itoa(mp3_rate(fm_mp3), num, 10, sizeof(num));
        strcat(fm_audio_info, num);
        strcat(fm_audio_info, " Hz  ");
        itoa(mp3_bitrate(fm_mp3), num, 10, sizeof(num));
        strcat(fm_audio_info, num);
        strcat(fm_audio_info, " kbps  ");
        strcat(fm_audio_info, m);
    }
    klog("[audio] playing ");
    klog(e->name);
    klog("\n");
    fm_mode = FM_AUDIO;
}

/* decode + feed the AC97 ring from the desktop loop; called every frame */
static void fm_audio_pump(void) {
    if (fm_mode != FM_AUDIO || fm_mp3 == NULL || fm_img_fail) return;
    if (fm_audio_paused) return;

    if (fm_audio_done) {
        if (audio_queued() <= 0) {
            klog("[audio] playback finished\n");
            fm_close_modal();
        }
        return;
    }

    /* feed until the ring refuses (playhead caught up) */
    for (;;) {
        if (fm_mp3_pending == 0) {
            int n = mp3_decode(fm_mp3, fm_mp3_pcm);
            if (n <= 0) {
                fm_audio_done = 1;
                break;
            }
            if (mp3_channels(fm_mp3) < 2) {
                /* the DAC only plays 16-bit stereo: duplicate the mono
                 * samples in place, back to front (1152 -> 2304 fits) */
                for (int i = n - 1; i >= 0; i--) {
                    fm_mp3_pcm[i * 2]     = fm_mp3_pcm[i];
                    fm_mp3_pcm[i * 2 + 1] = fm_mp3_pcm[i];
                }
                n *= 2;
            }
            fm_mp3_pending = n / 2;        /* stereo frames */
            fm_mp3_pending_off = 0;
        }
        int acc = audio_write(fm_mp3_pcm + fm_mp3_pending_off * 2,
                              fm_mp3_pending);
        fm_mp3_pending -= acc;
        fm_mp3_pending_off += acc;
        fm_audio_frames += (uint64_t)acc;
        if (fm_audio_frames >= 2048 && !audio_playing()) {
            audio_play();                  /* enough buffered: start the DAC */
        }
        if (acc == 0) break;               /* ring full, come back later */
    }
}

/* modal teardown shared by every close path; frees viewer planes */
static void fm_close_modal(void) {
    if (fm_mode == FM_IMAGE) {
        if (!fm_img_fail) jpeg_image_free(&fm_img);
        fm_img_fail = 0;
    }
    if (fm_mode == FM_AUDIO) {
        audio_close();
        if (fm_mp3 != NULL) { mp3_close(fm_mp3); fm_mp3 = NULL; }
        if (fm_mp3_pcm != NULL) { kfree(fm_mp3_pcm); fm_mp3_pcm = NULL; }
        if (fm_mp3_data != NULL) { kfree(fm_mp3_data); fm_mp3_data = NULL; }
        fm_img_fail = 0;
    }
    fm_mode = FM_BROWSE;
}

static void fm_open_selected(void) {
    if (fm_count == 0 || fm_dir == NULL) return;
    fs_entry_t* e = fm_list[fm_sel];
    if (e == NULL) {
        fm_navigate(fm_dir->parent);       /* ".." */
        return;
    }
    if (e->type == FS_TYPE_DIRECTORY) {
        fm_navigate(e);
        return;
    }
    if (fm_is_jpeg_name(e->name)) {
        fm_image_show(e);
        return;
    }
    if (fm_is_mp3_name(e->name)) {
        fm_audio_show(e);
        return;
    }
    fm_preview_show(e);
}

static void fm_status_err(const char* op) {
    int err = fs_get_last_error();
    strcpy(fm_status, op);
    strcat(fm_status, ": ");
    strcat(fm_status, (err == FS_ERR_EXISTS) ? "exists"
                    : (err == FS_ERR_FULL)   ? "full"
                    : (err == FS_ERR_IO)     ? "io error"
                                             : "error");
}

/* Create in the BROWSED directory via absolute path - the shell's cwd
 * (fs_current) must stay untouched. */
static void fm_input_confirm(void) {
    const char* name = fm_input_buf;
    if (name[0] == '\0' || !strcmp(name, ".") || !strcmp(name, "..")) {
        strcpy(fm_status, "invalid name");
        return;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        strcpy(fm_status, "invalid name");
        return;
    }

    char base[MAX_PATH_LENGTH];
    fm_abs_path(fm_dir, base, (int)sizeof(base));
    if (strlen(base) > 1) strcat(base, "/");
    if (strlen(base) + strlen(name) >= sizeof(base)) {
        strcpy(fm_status, "name too long");
        return;
    }
    strcat(base, name);

    fs_entry_t* created = (fm_input_kind == 1) ? fs_create_dir(base)
                                               : fs_create_file(base);
    if (created == NULL) {
        fm_status_err(fm_input_kind == 1 ? "mkdir" : "touch");
        return;
    }

    strcpy(fm_status, "created ");
    strcat(fm_status, name);
    klog("[files] created ");
    klog(base);
    klog("\n");
    fm_refresh();
    for (int i = 0; i < fm_count; i++) {
        if (fm_list[i] == created) { fm_sel = i; break; }
    }
}

static void fm_do_delete(void) {
    if (fm_sel < 0 || fm_sel >= fm_count || fm_dir == NULL) return;
    fs_entry_t* e = fm_list[fm_sel];
    if (e == NULL) return;

    char base[MAX_PATH_LENGTH];
    fm_abs_path(fm_dir, base, (int)sizeof(base));
    if (strlen(base) > 1) strcat(base, "/");
    if (strlen(base) + strlen(e->name) >= sizeof(base)) {
        strcpy(fm_status, "name too long");
        return;
    }
    strcat(base, e->name);

    if (fs_delete_entry(base) != 0) {
        strcpy(fm_status, "delete failed");
        return;
    }
    strcpy(fm_status, "deleted ");
    strcat(fm_status, e->name);
    klog("[files] deleted ");
    klog(base);
    klog("\n");
    fm_refresh();
    if (fm_sel >= fm_count && fm_sel > 0) fm_sel--;
}

/* ===== modal popups (centered, overlap the right panes) ===== */
static void fm_input_box_draw(void) {
    int w = dt_w - 40; if (w > 300) w = 300; if (w < 180) w = 180;
    int h = 64;
    int x = (dt_w - w) / 2;
    int y = (dt_h - h) / 2;

    dt_fill_rect(x, y, w, h, 0x0F);
    dt_draw_rect(x, y, w, h, 0x09);
    dt_draw_rect(x + 1, y + 1, w - 2, h - 2, 0x09);
    dt_draw_string(x + 8, y + 6, fm_input_kind ? "New folder" : "New file", 0x09);

    char line[sizeof(fm_input_buf) + 2];
    fm_clipn(line, (int)sizeof(line), fm_input_buf, (w - 20) / 8 - 1);
    strcat(line, "_");
    dt_draw_string(x + 8, y + 24, line, 0x00);
    dt_fill_rect(x + 8, y + 34, w - 16, 1, 0x07);
    dt_draw_string(x + 8, y + 42, "Enter:ok  Esc:cancel", 0x08);
}

static void fm_confirm_draw(void) {
    int w = dt_w - 60; if (w > 280) w = 280; if (w < 200) w = 200;
    int h = 56;
    int x = (dt_w - w) / 2;
    int y = (dt_h - h) / 2;

    dt_fill_rect(x, y, w, h, 0x0F);
    dt_draw_rect(x, y, w, h, 0x09);
    dt_draw_rect(x + 1, y + 1, w - 2, h - 2, 0x09);
    dt_draw_string(x + 8, y + 6, "Delete entry?", 0x04);
    char nb[64];
    if (fm_count > 0 && fm_list[fm_sel] != NULL) {
        fm_clipn(nb, (int)sizeof(nb), fm_list[fm_sel]->name, (w - 20) / 8);
    } else {
        nb[0] = '\0';
    }
    dt_draw_string(x + 8, y + 22, nb, 0x01);
    dt_draw_string(x + 8, y + 38, "Y:delete  N/Esc:no", 0x08);
}

static void fm_preview_draw(void) {
    int w = dt_w - 40; if (w > 380) w = 380; if (w < 200) w = 200;
    int h = 28 + FM_PV_ROWS * 10 + 12;
    int x = (dt_w - w) / 2;
    int y = (dt_h - h) / 2;

    dt_fill_rect(x, y, w, h, 0x0F);
    dt_draw_rect(x, y, w, h, 0x09);
    dt_draw_rect(x + 1, y + 1, w - 2, h - 2, 0x09);
    dt_fill_rect(x + 2, y + 2, w - 4, 13, 0x01);
    char title[96];
    fm_clipn(title, (int)sizeof(title), fm_pv_title, (w - 12) / 8);
    dt_draw_string(x + 6, y + 4, title, 0x0F);
    for (int i = 0; i < FM_PV_ROWS && i < fm_pv_count; i++) {
        char row[FM_PV_COLS + 1];
        fm_clipn(row, (int)sizeof(row), fm_pv[i], (w - 16) / 8);
        dt_draw_string(x + 8, y + 20 + i * 10, row, 0x00);
    }
    dt_draw_string(x + 8, y + h - 12, "any key closes", 0x08);
}

/* FM_IMAGE: centered viewer window; the image is power-of-two halved until
 * it fits, every destination pixel is nearest-neighbor sampled from the
 * YCbCr planes and pushed through dt_pixel_rgb (native RGB on GOP, dithered
 * EGA-16 on the mode12h desktop). */
static void fm_image_draw(void) {
    if (fm_img_fail) {
        int w = 240, h = 64;
        int x = (dt_w - w) / 2;
        int y = (dt_h - h) / 2;
        dt_fill_rect(x, y, w, h, 0x0F);
        dt_draw_rect(x, y, w, h, 0x04);
        dt_draw_string(x + 8, y + 6, fm_img_title, 0x04);
        dt_draw_string(x + 8, y + 24, fm_img_err, 0x04);
        dt_draw_string(x + 8, y + 44, "any key closes", 0x08);
        return;
    }

    int maxw = dt_w - 48;
    int maxh = dt_h - 110;
    if (maxw < 64) maxw = 64;
    if (maxh < 64) maxh = 64;
    int dw = fm_img.W, dh = fm_img.H;
    while ((dw > maxw || dh > maxh) && (dw > 1 || dh > 1)) {
        dw = (dw + 1) >> 1;
        dh = (dh + 1) >> 1;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    int w = dw + 16;
    if (w < 200) w = 200;
    int h = 13 + dh + 8 + 12;
    int x = (dt_w - w) / 2;
    int y = (dt_h - h) / 2;

    dt_fill_rect(x, y, w, h, 0x0F);
    dt_draw_rect(x, y, w, h, 0x09);
    dt_fill_rect(x + 2, y + 2, w - 4, 13, 0x01);
    char title[112];
    fm_clipn(title, (int)sizeof(title) - 16, fm_img_title, (w - 12) / 8 - 10);
    strcat(title, " ");
    char num[16];
    itoa(fm_img.W, num, 10, sizeof(num));
    strcat(title, num);
    strcat(title, "x");
    itoa(fm_img.H, num, 10, sizeof(num));
    strcat(title, num);
    dt_draw_string(x + 6, y + 4, title, 0x0F);

    int ix = x + 8;
    int iy = y + 17;
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * fm_img.H / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * fm_img.W / dw;
            int Y = jpeg_sample(&fm_img, 0, sx, sy);
            int r, g, b;
            if (fm_img.ncomp == 3) {
                int cb = jpeg_sample(&fm_img, 1, sx, sy) - 128;
                int cr = jpeg_sample(&fm_img, 2, sx, sy) - 128;
                r = Y + ((91881 * cr) >> 16);
                g = Y - ((22554 * cb + 46802 * cr) >> 16);
                b = Y + ((116130 * cb) >> 16);
            } else {
                r = g = b = Y;
            }
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            dt_pixel_rgb(ix + dx, iy + dy,
                         ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
        }
    }
    dt_draw_string(x + 8, y + h - 12, "any key closes", 0x08);
}

/* FM_AUDIO: centered player window with progress bar */
static void fm_audio_draw(void) {
    if (fm_img_fail) {
        int w = 328, h = 64;      /* wide enough for the probe-failure reason */
        int x = (dt_w - w) / 2;
        int y = (dt_h - h) / 2;
        dt_fill_rect(x, y, w, h, 0x0F);
        dt_draw_rect(x, y, w, h, 0x04);
        dt_draw_string(x + 8, y + 6, fm_audio_title, 0x04);
        dt_draw_string(x + 8, y + 24, fm_img_err, 0x04);
        dt_draw_string(x + 8, y + 44, "any key closes", 0x08);
        return;
    }

    int w = 340;
    int h = 92;
    int x = (dt_w - w) / 2;
    int y = (dt_h - h) / 2;
    dt_fill_rect(x, y, w, h, 0x0F);
    dt_draw_rect(x, y, w, h, 0x09);
    dt_fill_rect(x + 2, y + 2, w - 4, 13, 0x01);
    char title[112];
    fm_clipn(title, (int)sizeof(title) - 12, fm_audio_title, (w - 16) / 8 - 6);
    strcat(title, "  MP3");
    dt_draw_string(x + 6, y + 4, title, 0x0F);

    dt_draw_string(x + 10, y + 22, fm_audio_info, 0x08);

    /* progress: elapsed = frames handed to ring minus still-queued */
    char line[48];
    int rate = mp3_rate(fm_mp3);
    uint64_t total_sec = 0;
    if (mp3_bitrate(fm_mp3) > 0) {
        total_sec = (uint64_t)mp3_total_bytes(fm_mp3) * 8 /
                    ((uint64_t)mp3_bitrate(fm_mp3) * 1000);
    }
    uint64_t queued = (uint64_t)audio_queued();
    uint64_t played = (fm_audio_frames > queued) ? fm_audio_frames - queued : 0;
    if (rate > 0) played /= (uint64_t)rate;
    uint64_t mm = played / 60, ss = played % 60;
    itoa((int)mm, line, 10, sizeof(line));
    char t2[48];
    strcpy(t2, line);
    strcat(t2, ":");
    char num[16];
    itoa((int)ss % 60, num, 10, sizeof(num));
    if (ss < 10) strcat(t2, "0");
    strcat(t2, num);
    strcat(t2, " / ");
    itoa((int)(total_sec / 60), num, 10, sizeof(num));
    strcat(t2, num);
    strcat(t2, ":");
    itoa((int)(total_sec % 60), num, 10, sizeof(num));
    if (total_sec % 60 < 10) strcat(t2, "0");
    strcat(t2, num);
    dt_draw_string(x + 10, y + 38, t2, 0x08);

    /* progress bar */
    int bw = w - 20;
    dt_draw_rect(x + 10, y + 52, bw, 10, 0x08);
    uint64_t denom = total_sec > 0 ? total_sec : 1;
    uint64_t fillw = (uint64_t)bw * 10 * played / denom / 10;
    if (fillw > (uint64_t)bw - 2) fillw = bw - 2;
    if (fillw > 0) dt_fill_rect(x + 11, y + 53, (int)fillw, 8, 0x02);

    const char* state = fm_audio_done ? "stopping"
                        : fm_audio_paused ? "paused" : "playing";
    dt_draw_string(x + 10, y + h - 12, state, 0x08);
    dt_draw_string(x + w - 10 - 8 * 24, y + h - 12, "Space=pause  any=stop", 0x08);
}

/* dispatch whatever modal the panel is currently showing */
static void fm_modal_draw(void) {
    switch (fm_mode) {
        case FM_INPUT:   fm_input_box_draw();   break;
        case FM_CONFIRM: fm_confirm_draw();     break;
        case FM_PREVIEW: fm_preview_draw();     break;
        case FM_IMAGE:   fm_image_draw();       break;
        case FM_AUDIO:   fm_audio_draw();       break;
        default: break;
    }
}

/* repaint helpers: the mouse cursor must hop over every repaint */
static void fm_repaint_panel(int cur_x, int cur_y) {
    dt_cursor_erase(cur_x, cur_y);
    fm_render();
    dt_cursor_draw(cur_x, cur_y);
}

static void fm_repaint_modal(int cur_x, int cur_y) {
    dt_cursor_erase(cur_x, cur_y);
    fm_modal_draw();
    dt_cursor_draw(cur_x, cur_y);
}

static void fm_repaint_all(int cur_x, int cur_y) {
    dt_cursor_erase(cur_x, cur_y);
    desktop_repaint();
    dt_cursor_draw(cur_x, cur_y);
}

/* Returns 1: the Files panel owns the keyboard while active */
static int fm_handle_key(unsigned char c, int cur_x, int cur_y) {
    if (fm_mode == FM_AUDIO) {
        if (c == ' ') {
            fm_audio_paused = !fm_audio_paused;
            if (fm_audio_paused) audio_pause();
            else if (!fm_audio_done) audio_play();
            klog(fm_audio_paused ? "[audio] paused\n" : "[audio] resumed\n");
        } else {
            fm_close_modal();                 /* any other key stops */
        }
        fm_repaint_all(cur_x, cur_y);
        return 1;
    }
    if (fm_mode == FM_PREVIEW || fm_mode == FM_IMAGE) {
        fm_close_modal();
        fm_repaint_all(cur_x, cur_y);
        return 1;
    }
    if (fm_mode == FM_CONFIRM) {
        if (c == 'y' || c == 'Y') fm_do_delete();
        fm_close_modal();
        fm_repaint_all(cur_x, cur_y);
        return 1;
    }
    if (fm_mode == FM_INPUT) {
        if (c == '\n') {
            fm_input_confirm();
            fm_close_modal();
            fm_repaint_all(cur_x, cur_y);
        } else if (c == '\b') {
            if (fm_input_len > 0) {
                fm_input_buf[--fm_input_len] = '\0';
                fm_repaint_modal(cur_x, cur_y);
            }
        } else if (c >= 32 && c <= 126 &&
                   fm_input_len < (int)sizeof(fm_input_buf) - 1) {
            fm_input_buf[fm_input_len++] = (char)c;
            fm_input_buf[fm_input_len] = '\0';
            fm_repaint_modal(cur_x, cur_y);
        }
        return 1;
    }

    /* browse */
    switch (c) {
        case KEY_UP:
            if (fm_sel > 0) {
                fm_sel--;
                fm_repaint_panel(cur_x, cur_y);
            }
            return 1;
        case KEY_DOWN:
            if (fm_sel < fm_count - 1) {
                fm_sel++;
                fm_repaint_panel(cur_x, cur_y);
            }
            return 1;
        case KEY_LEFT:
        case '\b':
            if (fm_dir != NULL && fm_dir->parent != NULL) {
                fm_navigate(fm_dir->parent);
                fm_repaint_panel(cur_x, cur_y);
            }
            return 1;
        case '\n':
            fm_open_selected();
            if (fm_mode != FM_BROWSE) fm_repaint_all(cur_x, cur_y);
            else fm_repaint_panel(cur_x, cur_y);
            return 1;
        case 'm': case 'M':
            fm_input_kind = 1;
            fm_input_len = 0;
            fm_input_buf[0] = '\0';
            fm_mode = FM_INPUT;
            fm_repaint_all(cur_x, cur_y);
            return 1;
        case 'n': case 'N':
            fm_input_kind = 0;
            fm_input_len = 0;
            fm_input_buf[0] = '\0';
            fm_mode = FM_INPUT;
            fm_repaint_all(cur_x, cur_y);
            return 1;
        case 'd': case 'D':
            if (fm_count > 0 && fm_list[fm_sel] != NULL) {
                fm_mode = FM_CONFIRM;
                fm_repaint_all(cur_x, cur_y);
            }
            return 1;
        default:
            return 1;   /* swallowed: Files panel is modal */
    }
}

/* Left click: select a row; clicking the selected row opens it */
static int fm_handle_click(int mx, int my, int cur_x, int cur_y) {
    if (fm_mode == FM_PREVIEW || fm_mode == FM_IMAGE) {
        fm_close_modal();
        fm_repaint_all(cur_x, cur_y);
        return 1;
    }
    if (fm_mode != FM_BROWSE) return 1;   /* input/confirm are keyboard-modal */
    if (fm_count == 0 || fm_vis <= 0) return 1;
    if (mx < 1 || mx >= lay_left_w - 1) return 1;
    if (my < fm_list_y0 || my >= fm_list_y0 + fm_vis * fm_row_h) return 1;

    int idx = fm_scroll + (my - fm_list_y0) / fm_row_h;
    if (idx >= fm_count) return 1;
    if (idx == fm_sel) {
        fm_open_selected();
        if (fm_mode != FM_BROWSE) fm_repaint_all(cur_x, cur_y);
        else fm_repaint_panel(cur_x, cur_y);
    } else {
        fm_sel = idx;
        fm_repaint_panel(cur_x, cur_y);
    }
    return 1;
}

/* ===== Editor panel: desktop file editor over the edit.c core =====
 * ED_PICK lists the files of the shell's cwd; Enter opens the file in
 * ED_EDIT (edit_core_* line buffer, rendered into the panel, viewport
 * scrolls with the cursor). N opens ED_INPUT to type a new file name —
 * the file itself is only created on the first Ctrl+S save. The desktop
 * loop stays alive: no blocking keyboard waits here. */
enum { ED_IDLE = 0, ED_PICK, ED_EDIT, ED_INPUT };
static int ed_mode = ED_IDLE;
static fs_entry_t* ed_list[MAX_DIR_ENTRIES];
static int ed_count = 0;
static int ed_sel = 0;
static int ed_scroll = 0;
static char ed_path[MAX_PATH_LENGTH];   /* file being edited (absolute) */
static char ed_status[48];              /* save results / errors */
static char ed_input_buf[40];
static int  ed_input_len;
/* picker list geometry captured by ed_pick_draw for mouse hit testing */
static int ed_list_y0, ed_row_h, ed_vis;

static void draw_func_panel(int func);   /* fwd: editor repaints the panel */
static void ed_refresh(void);            /* fwd: called by ed_enter_picker */
static int  ed_view_rows(void);          /* fwd: viewport sizing on entry */

static void ed_enter_picker(void) {
    ed_mode = ED_PICK;
    ed_status[0] = '\0';
    ed_refresh();
}

static void ed_enter_edit(void) {
    edit_core_open(ed_path);
    edit_core_set_viewport(ed_view_rows());
    ed_status[0] = '\0';
    ed_mode = ED_EDIT;
}

/* visible text rows in the editor panel (title/path/status reserved) */
static int ed_view_rows(void) {
    int top_y = panel_content_top() + 28;
    int bottom = dt_h - lay_footer_h - 24;
    int rows = (bottom - top_y) / 10;
    return (rows < 1) ? 1 : rows;
}

static void ed_repaint_panel(int cur_x, int cur_y) {
    dt_cursor_erase(cur_x, cur_y);
    draw_func_panel(active_func);
    dt_cursor_draw(cur_x, cur_y);
}

static void ed_repaint_all(int cur_x, int cur_y) {
    dt_cursor_erase(cur_x, cur_y);
    desktop_repaint();
    dt_cursor_draw(cur_x, cur_y);
}

/* Rebuild the picker list from the shell's cwd: files only,
 * case-insensitive alphabetical. */
static void ed_refresh(void) {
    fs_entry_t* dir = fs_current();
    if (dir == NULL || dir->type != FS_TYPE_DIRECTORY) dir = fs_root();

    int t = 0;
    for (int i = 0; i < MAX_DIR_ENTRIES && t < MAX_DIR_ENTRIES; i++) {
        fs_entry_t* e = dir->children[i];
        if (e != NULL && e->type == FS_TYPE_FILE) ed_list[t++] = e;
    }
    for (int i = 1; i < t; i++) {
        fs_entry_t* key = ed_list[i];
        int j = i - 1;
        while (j >= 0 && fm_ci_cmp(ed_list[j]->name, key->name) > 0) {
            ed_list[j + 1] = ed_list[j];
            j--;
        }
        ed_list[j + 1] = key;
    }
    ed_count = t;
    if (ed_sel >= ed_count) ed_sel = (ed_count > 0) ? ed_count - 1 : 0;
    if (ed_sel < 0) ed_sel = 0;
    if (ed_scroll > ed_count - 1) ed_scroll = ed_count - 1;
    if (ed_scroll < 0) ed_scroll = 0;
}

static void ed_pick_draw(void) {
    int cx = 4;
    int ctop = panel_content_top();
    int bottom = dt_h - lay_footer_h - 1;

    dt_fill_rect(1, ctop, lay_left_w - 2, bottom - ctop, 0x0F);

    fm_sclip(cx, ctop + 2, "Editor", 0x09);

    char p[MAX_PATH_LENGTH];
    fm_abs_path(fs_current(), p, (int)sizeof(p));
    fm_sclip(cx, ctop + 12, p, 0x08);
    dt_fill_rect(cx, ctop + 23, lay_left_w - 8, 1, 0x07);

    ed_row_h = 12;
    ed_list_y0 = ctop + 28;
    int list_h = bottom - 26 - ed_list_y0;
    ed_vis = (list_h > 0) ? list_h / ed_row_h : 1;
    if (ed_vis < 1) ed_vis = 1;

    if (ed_scroll > ed_sel) ed_scroll = ed_sel;
    if (ed_sel >= ed_scroll + ed_vis) ed_scroll = ed_sel - ed_vis + 1;

    if (ed_count == 0) {
        fm_sclip(cx + 2, ed_list_y0 + 2, "(no files)", 0x08);
    }
    for (int r = 0; r < ed_vis; r++) {
        int idx = ed_scroll + r;
        if (idx >= ed_count) break;
        int ry = ed_list_y0 + r * ed_row_h;
        if (idx == ed_sel) {
            dt_fill_rect(cx + 1, ry - 1, lay_left_w - 7 - cx, ed_row_h - 1, 0x01);
        }
        fm_sclip(cx + 2, ry, ed_list[idx]->name,
                 (idx == ed_sel) ? 0x0F : 0x00);
    }

    if (ed_status[0]) fm_sclip(cx, bottom - 22, ed_status, 0x04);
    fm_sclip(cx, bottom - 12, "Enter:open N:new", 0x08);
}

static void ed_edit_draw(void) {
    int cx = 4;
    int ctop = panel_content_top();
    int bottom = dt_h - lay_footer_h - 1;

    dt_fill_rect(1, ctop, lay_left_w - 2, bottom - ctop, 0x0F);

    fm_sclip(cx, ctop + 2, "Editor", 0x09);
    fm_sclip(cx, ctop + 12, ed_path, 0x08);
    dt_fill_rect(cx, ctop + 23, lay_left_w - 8, 1, 0x07);

    int y0 = ctop + 28;
    int rows = ed_view_rows();
    int top = edit_core_top();
    int count = edit_core_line_count();
    for (int r = 0; r < rows; r++) {
        int idx = top + r;
        if (idx >= count) break;
        fm_sclip(cx, y0 + r * 10, edit_core_line(idx), 0x00);
    }

    /* caret bar at the cursor cell (only when the row is on screen) */
    int cury = edit_core_cur_y();
    if (cury >= top && cury < top + rows) {
        int px = cx + edit_core_cur_x() * 8;
        int py = y0 + (cury - top) * 10;
        if (px + 2 <= lay_left_w - 4) {
            dt_fill_rect(px, py, 2, 9, 0x04);
        }
    }

    if (ed_status[0]) fm_sclip(cx, bottom - 22, ed_status, 0x04);
    fm_sclip(cx, bottom - 12, "Ctrl+S:save X:close", 0x08);
}

/* centered "new file" name input (overlay, drawn after the panel) */
static void ed_input_draw(void) {
    int w = dt_w - 40; if (w > 300) w = 300; if (w < 180) w = 180;
    int h = 64;
    int x = (dt_w - w) / 2;
    int y = (dt_h - h) / 2;

    dt_fill_rect(x, y, w, h, 0x0F);
    dt_draw_rect(x, y, w, h, 0x09);
    dt_draw_rect(x + 1, y + 1, w - 2, h - 2, 0x09);
    dt_draw_string(x + 8, y + 6, "New file", 0x09);

    char line[sizeof(ed_input_buf) + 2];
    fm_clipn(line, (int)sizeof(line), ed_input_buf, (w - 20) / 8 - 1);
    strcat(line, "_");
    dt_draw_string(x + 8, y + 24, line, 0x00);
    dt_fill_rect(x + 8, y + 34, w - 16, 1, 0x07);
    dt_draw_string(x + 8, y + 42, "Enter:ok  Esc:cancel", 0x08);
}

static void ed_open_selected(void) {
    if (ed_count == 0 || ed_sel < 0 || ed_sel >= ed_count) return;
    fs_entry_t* e = ed_list[ed_sel];
    if (e == NULL || e->type != FS_TYPE_FILE) return;
    fm_abs_path(e, ed_path, (int)sizeof(ed_path));
    ed_enter_edit();
}

static void ed_input_confirm(void) {
    const char* name = ed_input_buf;
    if (name[0] == '\0' || !strcmp(name, ".") || !strcmp(name, "..") ||
        strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        strcpy(ed_status, "invalid name");
        return;                              /* stay in ED_INPUT */
    }

    char base[MAX_PATH_LENGTH];
    fm_abs_path(fs_current(), base, (int)sizeof(base));
    if (strlen(base) > 1) strcat(base, "/");
    if (strlen(base) + strlen(name) >= sizeof(base)) {
        strcpy(ed_status, "name too long");
        return;
    }
    strcat(base, name);
    strcpy(ed_path, base);
    ed_enter_edit();                         /* created on first save */
}

/* Returns 1: the editor panel owns the keyboard while not idle */
static int ed_handle_key(unsigned char c, int cur_x, int cur_y) {
    if (ed_mode == ED_INPUT) {
        if (c == '\n') {
            ed_input_confirm();
            ed_repaint_all(cur_x, cur_y);
        } else if (c == KEY_ESC) {
            ed_mode = ED_PICK;
            ed_repaint_all(cur_x, cur_y);
        } else if (c == '\b') {
            if (ed_input_len > 0) {
                ed_input_buf[--ed_input_len] = '\0';
                ed_repaint_all(cur_x, cur_y);
            }
        } else if (c >= 32 && c <= 126 &&
                   ed_input_len < (int)sizeof(ed_input_buf) - 1) {
            ed_input_buf[ed_input_len++] = (char)c;
            ed_input_buf[ed_input_len] = '\0';
            ed_repaint_all(cur_x, cur_y);
        }
        return 1;
    }

    if (ed_mode == ED_EDIT) {
        if (c == 0x13) {                     /* Ctrl+S */
            edit_core_save(ed_path);
            strcpy(ed_status, "saved");
            klog("[edit] saved ");
            klog(ed_path);
            klog("\n");
            ed_repaint_panel(cur_x, cur_y);
        } else if (c == 0x18) {              /* Ctrl+X */
            ed_mode = ED_PICK;
            ed_refresh();
            ed_repaint_all(cur_x, cur_y);
        } else {
            edit_core_key(c);
            ed_repaint_panel(cur_x, cur_y);
        }
        return 1;
    }

    /* ED_PICK */
    switch (c) {
        case KEY_UP:
            if (ed_sel > 0) {
                ed_sel--;
                ed_repaint_panel(cur_x, cur_y);
            }
            return 1;
        case KEY_DOWN:
            if (ed_sel < ed_count - 1) {
                ed_sel++;
                ed_repaint_panel(cur_x, cur_y);
            }
            return 1;
        case '\n':
            ed_open_selected();
            ed_repaint_all(cur_x, cur_y);
            return 1;
        case 'n': case 'N':
            ed_input_len = 0;
            ed_input_buf[0] = '\0';
            ed_mode = ED_INPUT;
            ed_repaint_all(cur_x, cur_y);
            return 1;
        case KEY_ESC:
            ed_mode = ED_IDLE;               /* keyboard back to the shell */
            ed_repaint_all(cur_x, cur_y);
            return 1;
        default:
            return 1;                        /* picker is modal */
    }
}

/* Left click in the picker: select a row; clicking the selected row opens */
static int ed_handle_click(int mx, int my, int cur_x, int cur_y) {
    if (ed_mode != ED_PICK) return 1;
    if (ed_count == 0 || ed_vis <= 0) return 1;
    if (mx < 1 || mx >= lay_left_w - 1) return 1;
    if (my < ed_list_y0 || my >= ed_list_y0 + ed_vis * ed_row_h) return 1;

    int idx = ed_scroll + (my - ed_list_y0) / ed_row_h;
    if (idx >= ed_count) return 1;
    if (idx == ed_sel) {
        ed_open_selected();
        ed_repaint_all(cur_x, cur_y);
    } else {
        ed_sel = idx;
        ed_repaint_panel(cur_x, cur_y);
    }
    return 1;
}

/* Fixed selector block at the top of the left panel: one row per function
 * with its F-key hint, active row highlighted. Clicking a row switches. */
static void draw_func_selector(void) {
    for (int i = 0; i < SEL_ROWS; i++) {
        int ry = lay_header_h + 3 + i * SEL_ROW_H;
        int sel = (i == active_func);
        if (sel) {
            dt_fill_rect(1, ry - 1, lay_left_w - 2, SEL_ROW_H - 1, 0x01);
        }
        dt_draw_string(4, ry, dt_menu_items[i], sel ? 0x0F : 0x00);
        char fk[3] = { 'F', (char)('1' + i), '\0' };
        dt_draw_string(lay_left_w - 6 - 16, ry, fk, sel ? 0x0F : 0x08);
    }
}

/* Left function panel: target of the Win-key menu. Default = text editor. */
static void draw_func_panel(int func) {
    int cx = 4;
    int cy = panel_content_top() + 3;
    int content_h = dt_h - lay_header_h - lay_footer_h;

    /* clear interior (inside border) */
    dt_fill_rect(1, lay_header_h + 1, lay_left_w - 2, content_h - 2, 0x0F);

    draw_func_selector();

    switch (func) {
        case FUNC_EDITOR:
            if (ed_mode == ED_PICK) {
                ed_pick_draw();
            } else if (ed_mode == ED_EDIT) {
                ed_edit_draw();
            } else {
                dt_draw_string(cx, cy, "Text Editor", 0x09);
                dt_draw_string(cx, cy + 16, "F1: file list", 0x00);
                dt_draw_string(cx, cy + 30, "Keyboard: shell", 0x00);
            }
            break;
        case FUNC_FILES:
            fm_refresh();
            fm_render();
            break;
        case FUNC_SYSTEM: {
            char buf[40];
            int y = cy + 14;
            int bar_w = lay_left_w - 10;

            uint32_t ncpus = smp_get_cpu_count();
            int max_cores = (int)ncpus;
            if (max_cores > 4) max_cores = 4;
            if (lay_left_w < 160 && max_cores > 2) max_cores = 2;

            strcpy(buf, "CPUs: ");
            itoa((int)ncpus, buf + 6, 10, 8);
            dt_draw_string(cx, y, buf, 0x00);

            for (int i = 0; i < max_cores; i++) {
                uint32_t usage = cpu_usage_percent[i];
                if (usage > 100) usage = 100;

                y += 12;
                strcpy(buf, "CPU");
                char* lp = buf + 3;
                itoa(i, lp, 10, 2);
                lp += strlen(lp);
                strcpy(lp, ":");
                dt_draw_string(cx, y, buf, 0x00);

                char pct[8];
                itoa((int)usage, pct, 10, 4);
                int px = cx + lay_left_w - 6 - (int)strlen(pct) * 8;
                if (px < cx + 40) px = cx + 40;
                dt_draw_string(px, y, pct, 0x00);

                y += 9;
                dt_fill_rect(cx, y, bar_w, 6, 0x07);
                dt_draw_rect(cx, y, bar_w, 6, 0x00);
                int fill_w = ((bar_w - 2) * (int)usage) / 100;
                if (fill_w < 0) fill_w = 0;
                uint8_t color = 0x0A;
                if (usage > 50) color = 0x0E;
                if (usage > 80) color = 0x0C;
                dt_fill_rect(cx + 1, y + 1, fill_w, 4, color);
                y += 9;
            }

            /* memory */
            uint64_t total_p, used_p, free_p;
            pmm_get_stats(&total_p, &used_p, &free_p);
            uint64_t total_mb = (total_p * PAGE_SIZE) / (1024 * 1024);
            uint64_t used_mb  = (used_p  * PAGE_SIZE) / (1024 * 1024);
            uint64_t free_mb  = (free_p  * PAGE_SIZE) / (1024 * 1024);

            y += 2;
            strcpy(buf, "Mem: ");
            char* mp = buf + 5;
            itoa((int)used_mb, mp, 10, 8);
            mp += strlen(mp);
            strcpy(mp, "/");
            mp += strlen(mp);
            itoa((int)total_mb, mp, 10, 8);
            mp += strlen(mp);
            strcpy(mp, "MB");
            if ((int)strlen(buf) * 8 <= lay_left_w - 8)
                dt_draw_string(cx, y, buf, 0x00);

            y += 10;
            dt_fill_rect(cx, y, bar_w, 6, 0x07);
            dt_draw_rect(cx, y, bar_w, 6, 0x00);
            int fill_w = (bar_w * (int)used_mb) / (int)(total_mb ? total_mb : 1);
            if (fill_w > bar_w - 2) fill_w = bar_w - 2;
            if (fill_w < 0) fill_w = 0;
            dt_fill_rect(cx + 1, y + 1, fill_w, 4, 0x0A);

            y += 12;
            strcpy(buf, "Free: ");
            char* fp = buf + 6;
            itoa((int)free_mb, fp, 10, 8);
            fp += strlen(fp);
            strcpy(fp, "MB");
            if ((int)strlen(buf) * 8 <= lay_left_w - 8)
                dt_draw_string(cx, y, buf, 0x0B);

            /* processes */
            y += 12;
            int ntasks = task_get_count();
            strcpy(buf, "Procs: ");
            itoa(ntasks, buf + 7, 10, 8);
            dt_draw_string(cx, y, buf, 0x00);

            int bottom = dt_h - lay_footer_h - 2;
            for (int i = 0; i < MAX_TASKS; i++) {
                int st = task_get_status(i);
                if (st == TASK_DEAD) continue;
                if (y + 9 + 8 > bottom) break;
                y += 9;
                dt_draw_string(cx, y, task_get_name(i), 0x00);
                const char* sstr = task_status_str(st);
                int sx = cx + lay_left_w - 6 - (int)strlen(sstr) * 8;
                if (sx < cx + 40) sx = cx + 40;
                dt_draw_string(sx, y, sstr,
                               (st == TASK_RUNNING) ? 0x0A : 0x01);
            }
            break;
        }
        case FUNC_CATS:
            dt_draw_string(cx, cy, "CAT Viewer", 0x09);
            dt_draw_string(cx, cy + 16, "=^._.^=", 0x00);
            dt_draw_string(cx, cy + 28, "Meow!", 0x00);
            break;
    }
}

static void gui_draw_datetime(int footer_h) {
    rtc_time_t t;
    if (rtc_read(&t) != 0) return;

    char buf[32];
    char* p = buf;

    /* year */
    itoa((int)t.year, p, 10, 16);
    while (*p) p++;
    *p++ = '-';
    /* month */
    if (t.month < 10) *p++ = '0';
    itoa((int)t.month, p, 10, 4);
    while (*p) p++;
    *p++ = '-';
    /* day */
    if (t.day < 10) *p++ = '0';
    itoa((int)t.day, p, 10, 4);
    while (*p) p++;
    *p++ = ' ';
    /* hour */
    if (t.hour < 10) *p++ = '0';
    itoa((int)t.hour, p, 10, 4);
    while (*p) p++;
    *p++ = ':';
    /* minute */
    if (t.minute < 10) *p++ = '0';
    itoa((int)t.minute, p, 10, 4);
    while (*p) p++;
    *p++ = ':';
    /* second */
    if (t.second < 10) *p++ = '0';
    itoa((int)t.second, p, 10, 4);
    while (*p) p++;
    *p = '\0';

    int len = strlen(buf);
    int x = dt_w - len * 8 - 4;
    if (x < 0) x = 0;

    dt_fill_rect(x, dt_h - footer_h + 1, len * 8 + 2, 8, 0x01);
    dt_draw_string(x, dt_h - footer_h + 2, buf, 0x0F);
}

/* Desktop chrome: backdrop, header bar, left function panel, right-top
 * shell pane, right-bottom kernel-log pane, footer + clock. Shared by the
 * mode13h and GOP desktops; geometry lives in the lay_* statics. */
static void desktop_draw_chrome(void) {
    int content_h = dt_h - lay_header_h - lay_footer_h;
    int right_w = dt_w - lay_left_w;
    int title_y = (lay_header_h - 8) / 2;
    if (title_y < 2) title_y = 2;

    /* clear screen (aligned to vertical retrace to avoid tearing) */
    dt_wait_vsync();
    dt_fill_rect(0, 0, dt_w, dt_h, 0x0F);

    /* top header bar */
    dt_fill_rect(0, 0, dt_w, lay_header_h, 0x0F);
    dt_draw_rect(0, 0, dt_w, lay_header_h, 0x03);
    dt_draw_string(4, title_y, "Kil0yOS v3.5.0", 0x00);
    dt_draw_string(dt_w - 148, title_y, "[Win]=Menu  F1-F4", 0x01);

    /* left function panel */
    dt_fill_rect(0, lay_header_h, lay_left_w, content_h, 0x0F);
    dt_draw_rect(0, lay_header_h, lay_left_w, content_h, 0x03);

    /* right-top shell pane */
    dt_fill_rect(lay_left_w, lay_header_h, right_w,
                 lay_split_y - lay_header_h, 0x0F);
    dt_draw_rect(lay_left_w, lay_header_h, right_w,
                 lay_split_y - lay_header_h, 0x03);
    dt_draw_string(lay_left_w + 4, lay_header_h + 2, "Shell", 0x09);

    /* right-bottom kernel-log pane */
    int klog_h = dt_h - lay_footer_h - lay_split_y;
    dt_fill_rect(lay_left_w, lay_split_y, right_w, klog_h, 0x0F);
    dt_draw_rect(lay_left_w, lay_split_y, right_w, klog_h, 0x03);
    dt_draw_string(lay_left_w + 4, lay_split_y + 2, "Kernel Log", 0x09);

    /* bottom footer bar */
    dt_fill_rect(0, dt_h - lay_footer_h, dt_w, lay_footer_h, 0x0F);
    dt_draw_rect(0, dt_h - lay_footer_h, dt_w, lay_footer_h, 0x03);
    {
        char pl[40];
        strcpy(pl, "Panel: ");
        strcat(pl, dt_menu_items[active_func]);
        dt_draw_string(4, dt_h - lay_footer_h + 6, pl, 0x08);
    }
    gui_draw_datetime(lay_footer_h);

    draw_func_panel(active_func);
}

/* Full repaint after the menu popup closes (chrome + panes + prompt + log).
 * An open Files modal popup (input/confirm/preview) is drawn last so it
 * survives the repaint that closes the Win-key menu over it; same for the
 * editor's centered new-file input. */
static void desktop_repaint(void) {
    desktop_draw_chrome();
    term_gui_render();   /* paints the prompt too: it lives in the cells */
    klog_view_render();
    if (active_func == FUNC_FILES) {
        fm_modal_draw();
    } else if (active_func == FUNC_EDITOR && ed_mode == ED_INPUT) {
        ed_input_draw();
    }
}

/* Switch the active left-panel function (F-keys, arrows, selector clicks,
 * Win menu) with a full repaint; the Editor panel re-opens its file picker
 * on every entry. */
static void desktop_switch_func(int f, mouse_state_t* prev) {
    if (f == active_func) return;
    active_func = f;
    if (f == FUNC_EDITOR) ed_enter_picker();
    dt_cursor_erase(prev->x, prev->y);
    desktop_repaint();
    dt_cursor_draw(prev->x, prev->y);
    klog("[desktop] func -> ");
    klog(dt_menu_items[active_func]);
    klog("\n");
}

/* Shared desktop main loop: Win-key menu popup, shell input, kernel-log
 * pump, clock, pointer. Runs until ESC; both desktops differ only in
 * geometry/backend. Keyboard focus stays on the right-top shell pane. */
static void desktop_run_loop(void) {
    extern void klog(const char* s);
    int menu_open = 0;
    int menu_sel = 0;
    static int trace_moved = 0;

    mouse_state_t prev = { .x = -1, .y = -1, .buttons = 0 };
    uint8_t last_second = 0xFF;

    /* show pointer from the first frame on */
    mouse_get_state(&prev);
    dt_cursor_draw(prev.x, prev.y);
    klog("[desktop] loop enter\n");

    while (1) {
        kernel_heartbeat_touch();   /* kwatchdog: desktop loop is alive */
        /* update clock every second */
        rtc_time_t t;
        if (rtc_read(&t) == 0 && t.second != last_second) {
            last_second = t.second;
            klog(".");   /* TEMP heartbeat: proves the loop is alive */

            /* hide pointer first so repaints are not clobbered by stale restore pixels */
            dt_cursor_erase(prev.x, prev.y);

            dt_wait_vsync();
            gui_draw_datetime(lay_footer_h);

            /* auto-refresh System Monitor CPU stats */
            if (active_func == FUNC_SYSTEM) {
                smp_update_cpu_usage();
                draw_func_panel(FUNC_SYSTEM);
            }

            /* MP3 player: tick the elapsed-time readout and progress bar */
            if (active_func == FUNC_FILES && fm_mode == FM_AUDIO) {
                fm_modal_draw();
            }

            /* pointer back with a fresh background snapshot */
            dt_cursor_draw(prev.x, prev.y);
        }

        /* pump new kernel-log lines into the right-bottom pane */
        if (klog_view_pump()) {
            dt_cursor_erase(prev.x, prev.y);
            klog_view_render();
            /* the log pane overlaps the centered Files modal (e.g. the
             * image viewer) - repaint it above the fresh log lines; same
             * for the editor's centered new-file input */
            if (active_func == FUNC_FILES && fm_mode != FM_BROWSE) {
                fm_modal_draw();
            } else if (active_func == FUNC_EDITOR && ed_mode == ED_INPUT) {
                ed_input_draw();
            }
            dt_cursor_draw(prev.x, prev.y);
        }

        if (keyboard_has_input()) {
            unsigned char c = (unsigned char)keyboard_getc();

            if (c == KEY_ESC) {
                if (menu_open) {
                    menu_open = 0;
                    dt_cursor_erase(prev.x, prev.y);
                    desktop_repaint();
                    dt_cursor_draw(prev.x, prev.y);
                } else if (active_func == FUNC_FILES && fm_mode != FM_BROWSE) {
                    fm_close_modal();  /* modal dialog: close, keep desktop */
                    dt_cursor_erase(prev.x, prev.y);
                    desktop_repaint();
                    dt_cursor_draw(prev.x, prev.y);
                } else if (active_func == FUNC_EDITOR && ed_mode != ED_IDLE) {
                    ed_handle_key(KEY_ESC, prev.x, prev.y);
                } else {
                    break;   /* exit desktop */
                }
            } else if (c == KEY_WIN) {
                if ((active_func == FUNC_FILES && fm_mode != FM_BROWSE) ||
                    (active_func == FUNC_EDITOR && ed_mode != ED_IDLE)) {
                    /* modal dialog owns the keyboard: swallow */
                } else {
                    menu_open = !menu_open;
                    dt_cursor_erase(prev.x, prev.y);
                    if (menu_open) {
                        menu_sel = active_func;
                        menu_popup_draw(menu_sel);
                    } else {
                        desktop_repaint();
                    }
                    dt_cursor_draw(prev.x, prev.y);
                }
            } else if (menu_open) {
                if (c == KEY_UP || c == KEY_DOWN) {
                    int old_sel = menu_sel;
                    if (c == KEY_UP && menu_sel > 0) {
                        menu_sel--;
                    } else if (c == KEY_DOWN && menu_sel < DT_MENU_COUNT - 1) {
                        menu_sel++;
                    }
                    if (old_sel != menu_sel) {
                        dt_cursor_erase(prev.x, prev.y);
                        menu_popup_draw(menu_sel);
                        dt_cursor_draw(prev.x, prev.y);
                    }
                } else if (c == '\n') {
                    menu_open = 0;
                    desktop_switch_func(menu_sel, &prev);
                }
            } else if (c >= KEY_F1 && c <= KEY_F4) {
                /* panel shortcuts, unless a modal editor/viewer owns the
                 * keyboard; F1 on the idle editor reopens the file list */
                int f = c - KEY_F1;
                int files_modal = (active_func == FUNC_FILES &&
                                   fm_mode != FM_BROWSE);
                int ed_modal = (active_func == FUNC_EDITOR &&
                                (ed_mode == ED_EDIT || ed_mode == ED_INPUT));
                if (f == active_func && f == FUNC_EDITOR && ed_mode == ED_IDLE) {
                    ed_enter_picker();
                    dt_cursor_erase(prev.x, prev.y);
                    draw_func_panel(FUNC_EDITOR);
                    dt_cursor_draw(prev.x, prev.y);
                } else if (!files_modal && !ed_modal) {
                    desktop_switch_func(f, &prev);
                }
            } else if ((c == KEY_UP || c == KEY_DOWN) &&
                       active_func != FUNC_FILES &&
                       !(active_func == FUNC_EDITOR && ed_mode != ED_IDLE)) {
                /* arrows cycle the panel selector when the panel itself
                 * does not consume them (Files uses them for its list) */
                int d = (c == KEY_DOWN) ? 1 : -1;
                int f = active_func + d;
                if (f < 0) f = DT_MENU_COUNT - 1;
                if (f >= DT_MENU_COUNT) f = 0;
                desktop_switch_func(f, &prev);
            } else if (active_func == FUNC_FILES) {
                fm_handle_key(c, prev.x, prev.y);
            } else if (active_func == FUNC_EDITOR && ed_mode != ED_IDLE) {
                ed_handle_key(c, prev.x, prev.y);
            } else {
                /* keyboard focus: right-top shell pane */
                if (c == '\n') {
                    gui_shell_execute();
                } else if (c >= 32 && c <= 126 && gui_shell_len < GUI_SHELL_BUF_SIZE - 1) {
                    gui_shell_buf[gui_shell_len++] = c;
                    term_gui_type_char(c);          /* store in cells + echo once */
                } else if (c == '\b' && gui_shell_len > 0) {
                    gui_shell_len--;
                    term_gui_backspace();
                }
            }
        }

        mouse_state_t state;
        mouse_get_state(&state);

        /* click edge detection must run even when the pointer stands still */
        int left_now = (state.buttons & 1) != 0;
        int left_was = (prev.buttons & 1) != 0;

        if (state.x != prev.x || state.y != prev.y) {
            if (!trace_moved) {
                trace_moved = 1;
                klog("[desktop] first pointer sample\n");
            }
            /* draw_cursor restores the previous position itself when visible */
            dt_cursor_draw(state.x, state.y);
            prev = state;
        }

        if (left_now && !left_was) {
            if (active_func == FUNC_FILES) {
                fm_handle_click(state.x, state.y, state.x, state.y);
            } else if (active_func == FUNC_EDITOR) {
                ed_handle_click(state.x, state.y, state.x, state.y);
            }
            /* selector band at the top of the left panel: click to switch */
            int files_modal = (active_func == FUNC_FILES &&
                               fm_mode != FM_BROWSE);
            int ed_modal = (active_func == FUNC_EDITOR &&
                            (ed_mode == ED_EDIT || ed_mode == ED_INPUT));
            if (!files_modal && !ed_modal &&
                state.x < lay_left_w &&
                state.y >= lay_header_h + 2 &&
                state.y < panel_content_top() - 4) {
                int f = (state.y - (lay_header_h + 3)) / SEL_ROW_H;
                if (f >= 0 && f < DT_MENU_COUNT) {
                    desktop_switch_func(f, &state);
                }
            }
        }
        if (state.buttons != prev.buttons) prev.buttons = state.buttons;

        /* keep the MP3 decoder fed; repaints itself once a second */
        if (active_func == FUNC_FILES && fm_mode == FM_AUDIO) fm_audio_pump();

        __asm__ volatile("hlt");   /* sleep until next interrupt instead of spinning */
    }

    dt_cursor_erase(prev.x, prev.y);
    klog("[desktop] loop exit\n");
}

static int cmd_gui(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (desktop_active) {
        vga_puts("gui: desktop already running\n");
        return 1;
    }
    if (fb_is_active()) {
        term_puts("gui: the VGA desktop needs the VGA path (use 'desktop' on UEFI boot)\n");
        return 1;
    }

    vga_puts("Launching desktop...\n");
    desktop_active = 1;

    dt_use_fb = 0;
    dt_w = GFX_WIDTH;
    dt_h = GFX_HEIGHT;
    mouse_set_bounds(dt_w, dt_h);

    vga_set_gfx_mode();

    /* layout (640x480): proportional band split like the GOP desktop */
    lay_header_h = 16;
    lay_footer_h = 16;
    lay_left_w = 200;
    lay_split_y = lay_header_h +
                  (dt_h - lay_header_h - lay_footer_h) * 55 / 100;

    /* right-top shell terminal grid */
    int sh_bx = lay_left_w + 4;
    int sh_by = lay_header_h + 14;
    int sh_cols = (dt_w - lay_left_w - 8) / 8;
    int sh_rows = (lay_split_y - 12 - sh_by) / 8;

    desktop_draw_chrome();
    term_init_gui_at(sh_bx, sh_by, sh_cols, sh_rows,
                     lay_left_w + 1, lay_header_h + 12, dt_w - lay_left_w - 2,
                     lay_split_y - (lay_header_h + 13));
    gui_shell_init(sh_bx, sh_by);

    /* right-bottom kernel-log pane */
    klog_view_init(lay_left_w + 4, lay_split_y + 14, sh_cols,
                   (dt_h - lay_footer_h - lay_split_y - 16) / 8,
                   lay_left_w + 1, lay_split_y + 12, dt_w - lay_left_w - 2,
                   dt_h - lay_footer_h - (lay_split_y + 13));

    desktop_run_loop();

    desktop_active = 0;
    mouse_set_bounds(GFX_WIDTH, GFX_HEIGHT);
    vga_set_text_mode();
    term_init_text();
    vga_puts("Returned to text mode.\n");
    return 0;
}

static int cmd_desktop(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (desktop_active) {
        vga_puts("desktop: already running\n");
        return 1;
    }
    if (!fb_is_active()) {
        vga_puts("desktop: needs the UEFI GOP framebuffer (use 'gui' on BIOS boot)\n");
        return 1;
    }

    term_puts("Launching GOP desktop...\n");
    desktop_active = 1;

    dt_use_fb = 1;
    dt_w = fb_width();
    dt_h = fb_height();
    mouse_set_bounds(dt_w, dt_h);

    lay_header_h = 20;
    lay_footer_h = 20;
    lay_left_w = dt_w / 5;   /* 1/5 of the screen, clamped to a usable band */
    if (lay_left_w < 120) lay_left_w = 120;
    if (lay_left_w > 240) lay_left_w = 240;
    lay_split_y = lay_header_h +
                  (dt_h - lay_header_h - lay_footer_h) * 55 / 100;

    /* right-top shell terminal grid */
    int sh_bx = lay_left_w + 8;
    int sh_by = lay_header_h + 26;
    int sh_cols = (dt_w - lay_left_w - 16) / 8;
    int sh_rows = (lay_split_y - sh_by - 4) / 8;
    int sh_clr_y = lay_header_h + 14;

    desktop_draw_chrome();
    term_init_gop_gui(sh_bx, sh_by, sh_cols, sh_rows,
                      lay_left_w + 2, sh_clr_y, dt_w - lay_left_w - 4,
                      lay_split_y - sh_clr_y - 1);
    gui_shell_init(sh_bx, sh_by);

    /* right-bottom kernel-log pane */
    int kl_by = lay_split_y + 16;
    int kl_clr_y = lay_split_y + 12;
    klog_view_init(lay_left_w + 8, kl_by, sh_cols,
                   (dt_h - lay_footer_h - kl_by - 2) / 8,
                   lay_left_w + 2, kl_clr_y, dt_w - lay_left_w - 4,
                   dt_h - lay_footer_h - kl_clr_y - 1);

    /* The desktop owns the whole framebuffer now: mute the plain fb text
     * console so klog diagnostics go to serial only and never paint over
     * the UI (unmute + full repaint on exit below). */
    fb_console_mute(1);

    desktop_run_loop();
    fb_console_mute(0);

    desktop_active = 0;
    mouse_set_bounds(GFX_WIDTH, GFX_HEIGHT);
    fb_clear();
    term_init_text();
    term_puts("Returned to shell.\n");
    return 0;
}

static uint32_t parse_ip(const char* s) {
    uint32_t ip = 0;
    int part = 0;
    int dots = 0;
    while (*s) {
        if (*s == '.') {
            ip = (ip << 8) | (part & 0xFF);
            part = 0;
            dots++;
        } else if (*s >= '0' && *s <= '9') {
            part = part * 10 + (*s - '0');
        }
        s++;
    }
    ip = (ip << 8) | (part & 0xFF);
    return ip;
}

static void print_ip(uint32_t ip) {
    uint8_t* b = (uint8_t*)&ip;
    char buf[16];
    itoa(b[3], buf, 10, 4);
    vga_puts(buf);
    vga_putchar('.');
    itoa(b[2], buf, 10, 4);
    vga_puts(buf);
    vga_putchar('.');
    itoa(b[1], buf, 10, 4);
    vga_puts(buf);
    vga_putchar('.');
    itoa(b[0], buf, 10, 4);
    vga_puts(buf);
}

static int cmd_ifconfig(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!g_netif.flags) {
        vga_puts("No network interface available.\n");
        return 1;
    }

    vga_puts("eth0: flags=UP\n");
    vga_puts("  MAC: ");
    for (int i = 0; i < 6; i++) {
        char buf[4];
        itoa(g_netif.mac[i], buf, 16, 3);
        if (g_netif.mac[i] < 16) vga_putchar('0');
        vga_puts(buf);
        if (i < 5) vga_putchar(':');
    }
    vga_puts("\n  IP: ");
    print_ip(g_netif.ip);
    vga_puts("\n  Netmask: ");
    print_ip(g_netif.netmask);
    vga_puts("\n  Gateway: ");
    print_ip(g_netif.gateway);
    vga_puts("\n");
    return 0;
}

static int cmd_ping(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: ping <ip>\n");
        return 1;
    }

    if (!g_netif.flags) {
        vga_puts("Network not available.\n");
        return 1;
    }

    uint32_t target = parse_ip(argv[1]);
    vga_puts("Pinging ");
    print_ip(target);
    vga_puts(" ...\n");

    int ok = 0;
    for (int i = 0; i < 4; i++) {
        if (icmp_ping(&g_netif, target, (uint16_t)i, 1000) == 0) {
            vga_puts("Reply from ");
            print_ip(target);
            vga_puts(": seq=");
            char buf[8];
            itoa(i, buf, 10, 3);
            vga_puts(buf);
            vga_puts(" ok\n");
            ok++;
        } else {
            vga_puts("Request timed out.\n");
        }
    }

    vga_puts("--- ping statistics ---\n");
    char buf[16];
    itoa(ok, buf, 10, 3);
    vga_puts(buf);
    vga_puts("/4 packets received\n");
    return (ok == 0) ? 1 : 0;
}

static int cmd_netstat(int argc, char** argv) {
    (void)argc;
    (void)argv;

    vga_puts("ARP cache:\n");
    int arp_count = 0;
    const arp_entry_t* arp = arp_get_cache(&arp_count);
    if (arp_count == 0) {
        vga_puts("  (empty)\n");
    } else {
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (arp[i].valid) {
                vga_puts("  ");
                print_ip(arp[i].ip);
                vga_puts(" -> ");
                for (int j = 0; j < 6; j++) {
                    char buf[4];
                    itoa(arp[i].mac[j], buf, 16, 3);
                    if (arp[i].mac[j] < 16) vga_putchar('0');
                    vga_puts(buf);
                    if (j < 5) vga_putchar(':');
                }
                vga_puts("\n");
            }
        }
    }

    vga_puts("UDP sockets:\n");
    int udp_count = 0;
    const udp_socket_t* socks = udp_get_sockets(&udp_count);
    int any = 0;
    for (int i = 0; i < udp_count; i++) {
        if (socks[i].used) {
            any = 1;
            vga_puts("  port=");
            char buf[8];
            itoa(socks[i].local_port, buf, 10, 6);
            vga_puts(buf);
            if (socks[i].rx_count) {
                vga_puts(" rx_pending=");
                itoa(socks[i].rx_count, buf, 10, 6);
                vga_puts(buf);
            }
            vga_puts("\n");
        }
    }
    if (!any) vga_puts("  (none)\n");
    return 0;
}

static int cmd_net(int argc, char** argv) {
    if (argc < 2) {
        /* No subcommand: aggregate overview (interface + ARP + sockets) */
        cmd_ifconfig(0, NULL);
        cmd_netstat(0, NULL);
        return 0;
    }

    if (strcmp(argv[1], "ping") == 0)  return cmd_ping(argc - 1, argv + 1);
    if (strcmp(argv[1], "ifconfig") == 0) return cmd_ifconfig(argc - 1, argv + 1);
    if (strcmp(argv[1], "netstat") == 0)  return cmd_netstat(argc - 1, argv + 1);

    vga_puts("Usage: net [ping <ip> | ifconfig | netstat]\n");
    return 1;
}

static int cmd_usb(int argc, char** argv) {
    (void)argc; (void)argv;
    /* serialise against the IRQ0 usb_tick context */
    int irqon = irq_save();
    usb_dump();
    irq_restore(irqon);
    return 0;
}

/* Shared launch path: load `file` with the given argv and run it until
 * it exits. argv[0] is passed through (busybox dispatches applets by
 * argv[0] basename). Returns the process exit code or -1. */
static int shell_launch_program(const char* file, char** argv, int argc) {
    if (process_any_active()) {
        vga_puts("exec: another process is still active\n");
        return -1;
    }
    process_reap_zombies();

    int pid = exec_load_program(file, argv, argc);
    if (pid < 0) return -1;

    process_run(pid);
    return 0;
}

static int cmd_exec(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: exec <program> [args...]\n");
        vga_puts("Example: exec /bin/hello.bin\n");
        return 1;
    }

    if (shell_launch_program(argv[1], argv + 1, argc - 1) < 0) {
        vga_puts("exec: failed to load: ");
        vga_puts(argv[1]);
        vga_puts("\n");
        return 1;
    }
    return 0;
}

/* Phase 1.4: pull a file from a TFTP server and install it. Without a
 * server the gateway is used (matches QEMU's built-in TFTP and PXE
 * conventions); without a local name the file lands in /bin under its
 * basename, so downloaded ELFs are directly runnable. */
static int cmd_tftp(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: tftp [server-ip] <file> [local-name]\n");
        vga_puts("Default server: gateway. Example: tftp busybox -> /bin/busybox\n");
        return 1;
    }

    if (!g_netif.flags) {
        vga_puts("Network not available.\n");
        return 1;
    }

    const char* remote;
    uint32_t server;
    if (argc >= 3) {
        server = parse_ip(argv[1]);
        remote = argv[2];
    } else {
        server = g_netif.gateway;
        remote = argv[1];
    }

    klog("[shell] tftp: fetching ");
    klog(remote);
    klog("\n");

    vga_puts("Downloading ");
    vga_puts(remote);
    vga_puts(" from ");
    print_ip(server);
    vga_puts(" ...\n");

    uint8_t* buf = NULL;
    size_t size = 0;
    if (tftp_download(&g_netif, server, remote, &buf, &size) != 0) {
        vga_puts("tftp: download failed\n");
        klog("[shell] tftp: download failed\n");
        return 1;
    }

    /* Default destination: /bin/<basename of remote> */
    const char* base = remote;
    for (const char* p = remote; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    char local[128];
    if (argc >= 4) {
        strncpy(local, argv[3], sizeof(local) - 1);
        local[sizeof(local) - 1] = '\0';
    } else {
        strcpy(local, "/bin/");
        size_t room = sizeof(local) - strlen(local) - 1;
        size_t nlen = strlen(base);
        if (nlen > room) nlen = room;
        memcpy(local + strlen(local), base, nlen);
        local[strlen(local) + nlen] = '\0';
    }

    /* fs_create_file works in the current directory: switch to /bin for
     * the default case, then restore. */
    fs_entry_t* saved_cwd = fs_current();
    int installed = 0;
    if (fs_resolve_path("/bin") != NULL && local[0] != '/') {
        fs_set_current(fs_resolve_path("/bin"));
    }
    fs_entry_t* f = fs_resolve_path(local);
    if (f == NULL) {
        if (local[0] == '/') {
            /* absolute destination: fs_create_file resolves the parent dir */
            f = fs_create_file(local);
        } else {
            /* create under the chosen directory using the final component */
            const char* lname = local;
            for (const char* p = local; *p; p++) {
                if (*p == '/') lname = p + 1;
            }
            f = fs_create_file(lname);
        }
    }
    if (f != NULL) {
        installed = (fs_write_file(f, buf, size) >= 0);
    }
    fs_set_current(saved_cwd);

    if (!installed) {
        vga_puts("tftp: install failed (");
        vga_puts(local);
        vga_puts(")\n");
        klog("[shell] tftp: install failed (");
        klog(local);
        klog(")\n");
        kfree(buf);
        return 1;
    }

    char num[16];
    itoa((int)size, num, 10, sizeof(num));
    klog("[shell] tftp: installed ");
    klog(local);
    klog(" (");
    klog(num);
    klog(" bytes)\n");
    vga_puts("Installed ");
    vga_puts(num);
    vga_puts(" bytes to ");
    vga_puts(local);
    vga_puts("\n");
    kfree(buf);
    return 0;
}

/* Phase 4.2: dpkg frontend shell command */
static int cmd_dpkg(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: dpkg -i <file.deb> | -r <pkg> | -l | -L <pkg>\n");
        return 1;
    }
    if (strcmp(argv[1], "-i") == 0 && argc >= 3) {
        return dpkg_install_file(argv[2]) == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "-r") == 0 && argc >= 3) {
        return dpkg_remove(argv[2]) == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "-l") == 0) {
        dpkg_list();
        return 0;
    }
    if (strcmp(argv[1], "-L") == 0 && argc >= 3) {
        return dpkg_show_files(argv[2]) == 0 ? 0 : 1;
    }
    vga_puts("dpkg: unknown option ");
    vga_puts(argv[1]);
    vga_puts("\n");
    return 1;
}

/* Watchdog visibility: does the LAPIC timer actually deliver NMIs? Run
 * twice a few seconds apart - a growing tick count means deliveries are
 * arriving; armed=0 means init bailed out (see the boot log reason). */
static int cmd_nmi(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char b[24];
    int armed = nmi_wdt_armed();
    uint32_t ticks = nmi_wdt_tick_count();

    vga_puts("nmi: armed=");
    vga_puts(armed ? "1" : "0");
    vga_puts(" ticks=");
    utoa(ticks, b, 10, sizeof(b));
    vga_puts(b);
    vga_puts("\n");
    klog("nmi: armed=");
    klog(armed ? "1" : "0");
    klog(" ticks=");
    klog(b);
    klog("\n");
    return 0;
}

/* Test entry: invoke panic() directly to exercise the panic path (serial
 * + VGA + framebuffer dump, halt loop, NMI re-entry guard). */
static int cmd_painme(int argc, char** argv) {
    (void)argc;
    (void)argv;
    klog("painme: test panic requested from shell\n");
    PANIC_CODE("painme: test panic requested from shell", 0x54455354); /* 'TEST' */
    return 0;   /* unreachable - panic() halts */
}

/* Memory usage report: PMM pages, kernel image, static reserves, heap
 * arena, framebuffer and the transient kilget index table. All KB
 * figures are 1024-byte units. */
static int cmd_memstat(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char b[24];

    vga_puts("=== memstat ===\n");

    uint64_t pmm_total = 0, pmm_used = 0, pmm_free = 0;
    pmm_get_stats(&pmm_total, &pmm_used, &pmm_free);
    vga_puts("PMM     : ");
    utoa((uint32_t)pmm_used, b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" used / ");
    utoa((uint32_t)pmm_free, b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" free / ");
    utoa((uint32_t)pmm_total, b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" pages (4 KB)\n");

    extern char kernel_start[];
    extern char kernel_end[];
    uint32_t kimg_kb = (uint32_t)((uint64_t)kernel_end - (uint64_t)kernel_start) / 1024;
    vga_puts("kernel  : ");
    utoa(kimg_kb, b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" KB image + ");
    utoa((uint32_t)(PMM_MAX_PAGES / 8) / 1024, b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" KB pmm bitmap\n");

    heap_stats_t hs;
    heap_get_stats(&hs);
    vga_puts("heap    : ");
    utoa((uint32_t)(hs.used_bytes / 1024), b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" KB used / ");
    utoa((uint32_t)(hs.free_bytes / 1024), b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" KB free (arena ");
    utoa((uint32_t)(hs.arena / 1024), b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" KB)\n");
    vga_puts("          ");
    utoa((uint32_t)hs.free_blocks, b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" free blocks, largest ");
    utoa((uint32_t)(hs.largest_free / 1024), b, 10, sizeof(b)); vga_puts(b);
    vga_puts(" KB\n");

    if (fb_base() != 0 && fb_size() != 0) {
        vga_puts("fb      : ");
        utoa((uint32_t)(fb_size() / 1024), b, 10, sizeof(b)); vga_puts(b);
        vga_puts(" KB framebuffer @ ");
        utoa((uint32_t)(fb_base() >> 4), b, 16, sizeof(b)); vga_puts(b);
        vga_puts("x\n");
    }

    size_t idx_bytes = kilget_index_bytes();
    vga_puts("kilget  : ");
    if (idx_bytes == 0) {
        vga_puts("index not loaded\n");
    } else {
        utoa((uint32_t)(idx_bytes / 1024), b, 10, sizeof(b)); vga_puts(b);
        vga_puts(" KB index table (transient)\n");
    }
    return 0;
}

/* Phase 4.4/4.5: kilget repo client (apt-get equivalent) */
static int cmd_kilget(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: kilget update | install <pkg> | show <pkg> |"
                 " list | installed\n");
        vga_puts("Sources: /etc/kilget/sources.list\n");
        return 1;
    }
    if (strcmp(argv[1], "update") == 0) {
        return kilget_update() == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "install") == 0 && argc >= 3) {
        return kilget_install(argv[2]) == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "show") == 0 && argc >= 3) {
        kilget_show(argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        kilget_list();
        return 0;
    }
    if (strcmp(argv[1], "installed") == 0) {
        dpkg_list();
        return 0;
    }
    vga_puts("kilget: unknown command ");
    vga_puts(argv[1]);
    vga_puts("\n");
    return 1;
}

static int execute_command(char* cmd) {
    if (strlen(cmd) == 0) return 0;

    heap_verify("pre-exec");

    char* argv[MAX_ARGUMENTS];
    int argc = 0;
    
    char* token = strtok(cmd, " \t");
    while (token != NULL && argc < MAX_ARGUMENTS - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    
    if (argc == 0) return 0;

    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            return commands[i].func(argc, argv);
        }
    }

    /* Not a builtin: run a Linux-ABI program. Try /bin/<cmd> first,
     * then the busybox multi-call binary (it dispatches the applet by
     * argv[0] basename, so argv[0] must stay the typed command). */
    {
        char* bargv[MAX_ARGUMENTS];
        char path[128];
        int i;

        strcpy(path, "/bin/");
        {
            size_t room = sizeof(path) - strlen(path) - 1;
            size_t nlen = strlen(argv[0]);
            if (nlen > room) nlen = room;
            memcpy(path + strlen(path), argv[0], nlen);
            path[strlen(path) + nlen] = '\0';
        }

        if (fs_resolve_path(path) != NULL) {
            bargv[0] = path;
        } else {
            strcpy(path, "/bin/busybox");
            if (fs_resolve_path(path) == NULL) {
                vga_puts(argv[0]);
                vga_puts(" not found\n");
                return 1;
            }
            bargv[0] = argv[0];
        }
        for (i = 1; i < argc; i++) bargv[i] = argv[i];
        bargv[argc] = NULL;

        if (shell_launch_program(path, bargv, argc) < 0) {
            vga_puts(argv[0]);
            vga_puts(": failed to load\n");
            return 1;
        }
        return 0;
    }
}

void shell_init() {
    term_init_text();
    update_prompt();
}

static int find_last_word_start(char* str, int len) {
    int i = len - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\t')) {
        i--;
    }
    while (i >= 0 && str[i] != ' ' && str[i] != '\t') {
        i--;
    }
    return i + 1;
}

static int tab_complete_command(char* prefix, char* result) {
    int match_count = 0;
    char longest_match[MAX_COMMAND_LENGTH] = "";
    int longest_len = 0;
    
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strncmp(commands[i].name, prefix, strlen(prefix)) == 0) {
            match_count++;
            
            if (match_count == 1) {
                strcpy(longest_match, commands[i].name);
                longest_len = strlen(longest_match);
            } else {
                int j = 0;
                while (j < longest_len && j < (int)strlen(commands[i].name) && 
                       longest_match[j] == commands[i].name[j]) {
                    j++;
                }
                longest_len = j;
                longest_match[j] = '\0';
            }
        }
    }
    
    if (match_count == 0) return 0;
    
    if (match_count == 1) {
        strcpy(result, longest_match);
        return 1;
    }
    
    strcpy(result, longest_match);
    return match_count;
}

static int tab_complete_path(char* prefix, char* result) {
    fs_entry_t* search_dir = fs_current();
    char dir_path[MAX_PATH_LENGTH] = "";
    char file_prefix[MAX_PATH_LENGTH] = "";
    
    char* last_slash = strrchr(prefix, '/');
    if (last_slash != NULL) {
        strncpy(dir_path, prefix, last_slash - prefix + 1);
        strcpy(file_prefix, last_slash + 1);
        
        fs_entry_t* dir = fs_resolve_path(dir_path);
        if (dir != NULL && dir->type == FS_TYPE_DIRECTORY) {
            search_dir = dir;
        }
    } else {
        strcpy(file_prefix, prefix);
    }
    
    int match_count = 0;
    char longest_match[MAX_PATH_LENGTH] = "";
    int longest_len = 0;
    
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (search_dir->children[i] != NULL) {
            const char* name = search_dir->children[i]->name;
            if (strncmp(name, file_prefix, strlen(file_prefix)) == 0) {
                match_count++;
                
                if (match_count == 1) {
                    strcpy(longest_match, name);
                    longest_len = strlen(longest_match);
                } else {
                    int j = 0;
                    while (j < longest_len && j < (int)strlen(name) && 
                           longest_match[j] == name[j]) {
                        j++;
                    }
                    longest_len = j;
                    longest_match[j] = '\0';
                }
            }
        }
    }
    
    if (match_count == 0) return 0;
    
    if (match_count == 1) {
        strcpy(result, longest_match);
        return 1;
    }
    
    strcpy(result, longest_match);
    return match_count;
}

static void tab_complete(char* command, int* cmd_len) {
    int word_start = find_last_word_start(command, *cmd_len);
    int word_len = *cmd_len - word_start;
    
    if (word_len == 0) {
        return;
    }
    
    char prefix[MAX_COMMAND_LENGTH];
    strncpy(prefix, command + word_start, word_len);
    prefix[word_len] = '\0';
    
    char result[MAX_COMMAND_LENGTH];
    int match_count;
    
    if (word_start == 0) {
        match_count = tab_complete_command(prefix, result);
    } else {
        match_count = tab_complete_path(prefix, result);
    }
    
    if (match_count == 0) return;
    
    int add_len = strlen(result) - word_len;
    if (add_len > 0 && *cmd_len + add_len < MAX_COMMAND_LENGTH - 1) {
        memmove(command + word_start + strlen(result), 
                command + *cmd_len, 
                MAX_COMMAND_LENGTH - *cmd_len - 1);
        strcpy(command + word_start, result);
        *cmd_len += add_len;
        
        for (int i = 0; i < add_len; i++) {
            vga_putchar(result[word_len + i]);
        }
    }
}

void shell_run() {
    char command[MAX_COMMAND_LENGTH];
    int cmd_len = 0;
    
    while (1) {
        vga_set_color(vga_entry_color(COLOR_LIGHT_GREEN, COLOR_BLACK));
        vga_puts(current_path);
        vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));
        vga_puts("$ ");
        
        cmd_len = 0;
        while (cmd_len < MAX_COMMAND_LENGTH - 1) {
            kernel_heartbeat_touch();   /* kwatchdog: main task is cycling */
            /* While a user process is running (time-sliced with us),
             * leave the keyboard buffer alone - its sys_read owns it. */
            if (process_any_active()) {
                __asm__ volatile("hlt");
                continue;
            }
            char c = keyboard_getc();
            
            if (c == '\n') {
                command[cmd_len] = '\0';
                vga_putchar('\n');
                execute_command(command);
                heap_verify("post-exec");
                break;
            } else if (c == '\b') {
                if (cmd_len > 0) {
                    cmd_len--;
                    vga_putchar('\b');
                }
            } else if (c == '\t') {
                tab_complete(command, &cmd_len);
            } else {
                command[cmd_len++] = c;
                vga_putchar(c);
            }
        }
    }
}
