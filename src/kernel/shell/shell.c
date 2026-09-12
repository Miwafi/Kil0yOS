#include "shell/shell.h"
#include "shell/terminal.h"
#include "drivers/vga.h"
#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "fs/fs.h"
#include "mm/memory.h"
#include "sched/scheduler.h"
#include "core/interrupts.h"
#include "core/smp.h"
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
    vga_puts("Kil0yOS v2.17.1\n");
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
static int gui_shell_x = 0;
static int gui_shell_y = 0;

static void gui_shell_draw_prompt(void) {
    dt_draw_string(gui_shell_x, gui_shell_y, "> ", 0x00);
}

static void gui_shell_init(int prompt_x, int prompt_y) {
    gui_shell_len = 0;
    gui_shell_buf[0] = '\0';
    gui_shell_x = prompt_x;
    gui_shell_y = prompt_y;
    gui_shell_draw_prompt();
}

static int execute_command(char* cmd);

static void gui_shell_execute(void) {
    gui_shell_buf[gui_shell_len] = '\0';

    char cmd_buf[GUI_SHELL_BUF_SIZE];
    strncpy(cmd_buf, gui_shell_buf, GUI_SHELL_BUF_SIZE - 1);
    cmd_buf[GUI_SHELL_BUF_SIZE - 1] = '\0';

    term_putchar('\n');
    execute_command(cmd_buf);
    term_putchar('\n');
    term_gui_render();

    gui_shell_len = 0;
    gui_shell_buf[0] = '\0';
    gui_shell_y = term_gui_get_cursor_y();
    gui_shell_draw_prompt();
}

/* Left function panel: target of the Win-key menu. Default = text editor. */
static void draw_func_panel(int func) {
    int cx = 4;
    int cy = lay_header_h + 4;
    int content_h = dt_h - lay_header_h - lay_footer_h;

    /* clear interior (inside border) */
    dt_fill_rect(1, lay_header_h + 1, lay_left_w - 2, content_h - 2, 0x0F);

    switch (func) {
        case FUNC_EDITOR:
            dt_draw_string(cx, cy, "Text Editor", 0x09);
            dt_draw_string(cx, cy + 16, "Default view.", 0x00);
            dt_draw_string(cx, cy + 30, "Edit a file", 0x00);
            dt_draw_string(cx, cy + 42, "from shell:", 0x00);
            dt_draw_string(cx, cy + 58, "edit <file>", 0x01);
            break;
        case FUNC_FILES:
            dt_draw_string(cx, cy, "File Manager", 0x09);
            dt_draw_string(cx, cy + 16, "Browse files", 0x00);
            dt_draw_string(cx, cy + 28, "from shell:", 0x00);
            dt_draw_string(cx, cy + 44, "ls / cd / cat", 0x01);
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
    dt_draw_string(4, title_y, "Kil0yOS v2.17.1", 0x00);
    dt_draw_string(dt_w - 84, title_y, "[Win]=Menu", 0x01);

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
    gui_draw_datetime(lay_footer_h);

    draw_func_panel(active_func);
}

/* Full repaint after the menu popup closes (chrome + panes + prompt + log) */
static void desktop_repaint(void) {
    desktop_draw_chrome();
    term_gui_render();
    gui_shell_draw_prompt();
    klog_view_render();
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
        /* update clock every second */
        rtc_time_t t;
        if (rtc_read(&t) == 0 && t.second != last_second) {
            last_second = t.second;

            /* hide pointer first so repaints are not clobbered by stale restore pixels */
            dt_cursor_erase(prev.x, prev.y);

            dt_wait_vsync();
            gui_draw_datetime(lay_footer_h);

            /* auto-refresh System Monitor CPU stats */
            if (active_func == FUNC_SYSTEM) {
                smp_update_cpu_usage();
                draw_func_panel(FUNC_SYSTEM);
            }

            /* pointer back with a fresh background snapshot */
            dt_cursor_draw(prev.x, prev.y);
        }

        /* pump new kernel-log lines into the right-bottom pane */
        if (klog_view_pump()) {
            dt_cursor_erase(prev.x, prev.y);
            klog_view_render();
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
                } else {
                    break;   /* exit desktop */
                }
            } else if (c == KEY_WIN) {
                menu_open = !menu_open;
                dt_cursor_erase(prev.x, prev.y);
                if (menu_open) {
                    menu_sel = active_func;
                    menu_popup_draw(menu_sel);
                } else {
                    desktop_repaint();
                }
                dt_cursor_draw(prev.x, prev.y);
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
                    active_func = menu_sel;
                    dt_cursor_erase(prev.x, prev.y);
                    desktop_repaint();
                    dt_cursor_draw(prev.x, prev.y);
                    klog("[desktop] func -> ");
                    klog(dt_menu_items[active_func]);
                    klog("\n");
                }
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

        if (state.x != prev.x || state.y != prev.y) {
            if (!trace_moved) {
                trace_moved = 1;
                klog("[desktop] first pointer sample\n");
            }
            /* draw_cursor restores the previous position itself when visible */
            dt_cursor_draw(state.x, state.y);
            prev = state;
        }

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
