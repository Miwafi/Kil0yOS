#include "shell/shell.h"
#include "shell/terminal.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "fs/fs.h"
#include "mm/memory.h"
#include "sched/scheduler.h"
#include "core/interrupts.h"
#include "core/smp.h"
#include "drivers/power.h"
#include "drivers/pci.h"
#include "drivers/rtc.h"
#include "fs/edit.h"

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
    {"date", "Show current date", cmd_date},
    {"time", "Show current time", cmd_time},
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
        
        char* content = (char*)kmalloc(MAX_FILE_SIZE);
        if (content == NULL) {
            vga_puts("echo: memory error\n");
            return 1;
        }
        int content_len = 0;

        for (int i = 1; i < redirect_pos; i++) {
            const char* arg = argv[i];
            size_t arg_len = strlen(arg);

            if (content_len + arg_len + 1 > MAX_FILE_SIZE - 1) {
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
    vga_puts("Saving filesystem...\n");
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
    vga_puts("Kil0yOS v2.3.0\n");
    vga_puts("A simple 64-bit x86-64 operating system\n");
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

static int cmd_gfx(int argc, char** argv) {
    vga_puts("Switching to graphical mode...\n");

    vga_set_mode_13h();
    vga_draw_color_bars();

    while (keyboard_getc() != 'q') {
        __asm__ volatile("nop");
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
static int gui_shell_input_x = 0;

static void gui_shell_draw_prompt(void) {
    vga_draw_string(gui_shell_x, gui_shell_y, "> ", 0x0F);
}

static void gui_shell_init(int left_w, int header_h) {
    gui_shell_len = 0;
    gui_shell_buf[0] = '\0';
    gui_shell_x = left_w + 4;
    gui_shell_y = header_h + 14;
    gui_shell_input_x = gui_shell_x + 12;
    gui_shell_draw_prompt();
}

static int execute_command(char* cmd);

static void gui_shell_execute(int left_w, int header_h, int content_h) {
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
    gui_shell_input_x = gui_shell_x + 12;
    gui_shell_draw_prompt();
}

static void gui_draw_content(int left_w, int header_h, int content_h, int active_idx) {
    int cx = left_w + 4;
    int cy = header_h + 4;

    /* clear content area (inside border) */
    vga_fill_rect(left_w + 1, header_h + 1, GFX_WIDTH - left_w - 2, content_h - 2, 0x00);

    switch (active_idx) {
        case 0: /* Shell */
            vga_draw_string(cx, cy, "Shell Terminal", 0x0E);
            term_init_gui(left_w, header_h, content_h);
            gui_shell_init(left_w, header_h);
            break;
        case 1: /* Files */
            vga_draw_string(cx, cy, "File Manager", 0x0E);
            vga_draw_string(cx, cy + 10, "Browse and", 0x0F);
            vga_draw_string(cx, cy + 20, "manage files.", 0x0F);
            break;
        case 2: /* Edit */
            vga_draw_string(cx, cy, "Text Editor", 0x0E);
            vga_draw_string(cx, cy + 10, "Edit text files", 0x0F);
            vga_draw_string(cx, cy + 20, "in memory.", 0x0F);
            break;
        case 3: /* System */
            vga_draw_string(cx, cy, "System Monitor", 0x0E);

            /* CPU stats */
            {
                uint32_t ncpus = smp_get_cpu_count();
                char cpu_title[32];
                char* cp = cpu_title;
                strcpy(cp, "CPUs: ");
                cp += strlen(cp);
                itoa((int)ncpus, cp, 10, 4);
                cp += strlen(cp);
                vga_draw_string(cx, cy + 12, cpu_title, 0x0F);

                int bar_w = GFX_WIDTH - left_w - 12;
                int bar_h = 6;
                int bar_x = cx;
                int max_cores = (int)ncpus;
                if (max_cores > 4) max_cores = 4;

                for (int i = 0; i < max_cores; i++) {
                    int bar_y = cy + 24 + i * 12;
                    uint32_t usage = cpu_usage_percent[i];
                    if (usage > 100) usage = 100;
                    int fill_w = (bar_w * (int)usage) / 100;

                    char label[16];
                    char* lp = label;
                    strcpy(lp, "CPU");
                    lp += strlen(lp);
                    itoa(i, lp, 10, 2);
                    lp += strlen(lp);
                    strcpy(lp, ":");
                    vga_draw_string(bar_x, bar_y - 1, label, 0x0E);

                    vga_fill_rect(bar_x + 30, bar_y, bar_w - 30, bar_h, 0x00);
                    vga_draw_rect(bar_x + 30, bar_y, bar_w - 30, bar_h, 0x0F);

                    uint8_t color = 0x0A;
                    if (usage > 50) color = 0x0E;
                    if (usage > 80) color = 0x0C;
                    vga_fill_rect(bar_x + 31, bar_y + 1, fill_w - 2, bar_h - 2, color);

                    char pct[8];
                    itoa((int)usage, pct, 10, 4);
                    int plen = strlen(pct);
                    vga_draw_string(bar_x + 30 + bar_w - 30 - plen * 6 - 2, bar_y - 1, pct, 0x0F);
                }
            }

            /* Memory stats */
            {
                uint64_t total_p, used_p, free_p;
                pmm_get_stats(&total_p, &used_p, &free_p);
                uint64_t total_mb = (total_p * PAGE_SIZE) / (1024 * 1024);
                uint64_t used_mb  = (used_p  * PAGE_SIZE) / (1024 * 1024);
                uint64_t free_mb  = (free_p  * PAGE_SIZE) / (1024 * 1024);

                char mem_buf[64];
                char* mp = mem_buf;
                strcpy(mp, "Mem: ");
                mp += strlen(mp);
                itoa((int)used_mb, mp, 10, 8);
                mp += strlen(mp);
                strcpy(mp, " / ");
                mp += strlen(mp);
                itoa((int)total_mb, mp, 10, 8);
                mp += strlen(mp);
                strcpy(mp, " MB");
                vga_draw_string(cx, cy + 66, mem_buf, 0x0F);

                /* Progress bar */
                int bar_x = cx;
                int bar_y = cy + 78;
                int bar_w = GFX_WIDTH - left_w - 12;
                int bar_h = 6;
                int fill_w = (bar_w * (int)used_mb) / (int)(total_mb ? total_mb : 1);

                vga_fill_rect(bar_x, bar_y, bar_w, bar_h, 0x00);
                vga_draw_rect(bar_x, bar_y, bar_w, bar_h, 0x0F);
                vga_fill_rect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, 0x0A);

                char free_buf[32];
                char* fp = free_buf;
                strcpy(fp, "Free: ");
                fp += strlen(fp);
                itoa((int)free_mb, fp, 10, 8);
                fp += strlen(fp);
                strcpy(fp, " MB");
                vga_draw_string(cx, cy + 88, free_buf, 0x0B);
            }

            /* Process list */
            {
                int proc_y = cy + 104;
                int ntasks = task_get_count();
                char proc_title[32];
                char* pt = proc_title;
                strcpy(pt, "Processes (");
                pt += strlen(pt);
                itoa(ntasks, pt, 10, 4);
                pt += strlen(pt);
                strcpy(pt, "):");
                vga_draw_string(cx, proc_y, proc_title, 0x0E);

                int row = 0;
                for (int i = 0; i < MAX_TASKS && row < 8; i++) {
                    int st = task_get_status(i);
                    if (st == TASK_DEAD) continue;

                    int py = proc_y + 12 + row * 10;
                    const char* name = task_get_name(i);
                    const char* sstr = task_status_str(st);

                    vga_draw_string(cx, py, name, 0x0F);
                    vga_draw_string(cx + 80, py, sstr,
                        (st == TASK_RUNNING) ? 0x0A : 0x0E);
                    row++;
                }
            }
            break;
        case 4: /* CATs */
            vga_draw_string(cx, cy, "CAT Viewer", 0x0E);
            vga_draw_string(cx, cy + 10, "=^._.^=", 0x0F);
            vga_draw_string(cx, cy + 20, "Meow!", 0x0F);
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
    int x = GFX_WIDTH - len * 6 - 4;
    if (x < 0) x = 0;

    vga_fill_rect(x, GFX_HEIGHT - footer_h + 1, len * 6 + 2, 8, 0x01);
    vga_draw_string(x, GFX_HEIGHT - footer_h + 2, buf, 0x0F);
}

static int cmd_gui(int argc, char** argv) {
    vga_puts("Launching desktop...\n");

    vga_set_mode_13h();

    int header_h = 10;
    int footer_h = 10;
    int left_w = 100;
    int content_h = GFX_HEIGHT - header_h - footer_h;

#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83

    /* menu state */
    const char* menu_items[] = {"Shell", "Files", "Edit", "System", "CATs"};
    int menu_count = 5;
    int selected_idx = 0;
    int active_idx = 0;
    int menu_x = 4;
    int menu_start_y = header_h + 14;
    int menu_spacing = 10;

    /* clear screen */
    vga_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, 0x01);

    /* top header bar */
    vga_fill_rect(0, 0, GFX_WIDTH, header_h, 0x01);
    vga_draw_rect(0, 0, GFX_WIDTH, header_h, 0x0E);
    vga_draw_string(4, 2, "Kil0yOS v2.3.0", 0x0F);

    /* left panel */
    vga_fill_rect(0, header_h, left_w, content_h, 0x00);
    vga_draw_rect(0, header_h, left_w, content_h, 0x0E);
    vga_draw_string(menu_x, header_h + 4, "Menu", 0x0E);

    /* draw menu items */
    for (int i = 0; i < menu_count; i++) {
        uint8_t color = (i == selected_idx) ? 0x0E : 0x0F;
        vga_draw_string(menu_x, menu_start_y + i * menu_spacing, menu_items[i], color);
    }

    /* right content panel */
    vga_fill_rect(left_w, header_h, GFX_WIDTH - left_w, content_h, 0x00);
    vga_draw_rect(left_w, header_h, GFX_WIDTH - left_w, content_h, 0x0E);
    gui_draw_content(left_w, header_h, content_h, active_idx);

    /* bottom footer bar */
    vga_fill_rect(0, GFX_HEIGHT - footer_h, GFX_WIDTH, footer_h, 0x01);
    vga_draw_rect(0, GFX_HEIGHT - footer_h, GFX_WIDTH, footer_h, 0x0E);

    mouse_state_t prev = { .x = -1, .y = -1, .buttons = 0 };
    uint8_t last_second = 0xFF;

    gui_draw_datetime(footer_h);

    while (1) {
        /* update clock every second */
        rtc_time_t t;
        if (rtc_read(&t) == 0 && t.second != last_second) {
            last_second = t.second;
            gui_draw_datetime(footer_h);

            /* auto-refresh System Monitor CPU stats */
            if (active_idx == 3) {
                smp_update_cpu_usage();
                gui_draw_content(left_w, header_h, content_h, active_idx);
            }
        }

        if (keyboard_has_input()) {
            unsigned char c = (unsigned char)keyboard_getc();

            if (c == KEY_UP || c == KEY_DOWN) {
                int old_idx = selected_idx;

                if (c == KEY_UP && selected_idx > 0) {
                    selected_idx--;
                } else if (c == KEY_DOWN && selected_idx < menu_count - 1) {
                    selected_idx++;
                }

                if (old_idx != selected_idx) {
                    /* erase old item */
                    vga_fill_rect(menu_x, menu_start_y + old_idx * menu_spacing,
                                  left_w - 8, 8, 0x00);
                    vga_draw_string(menu_x, menu_start_y + old_idx * menu_spacing,
                                    menu_items[old_idx], 0x0F);

                    /* draw new selected item */
                    vga_fill_rect(menu_x, menu_start_y + selected_idx * menu_spacing,
                                  left_w - 8, 8, 0x00);
                    vga_draw_string(menu_x, menu_start_y + selected_idx * menu_spacing,
                                    menu_items[selected_idx], 0x0E);
                }
            }

            if (c == '\n') {
                if (selected_idx != active_idx) {
                    active_idx = selected_idx;
                    gui_draw_content(left_w, header_h, content_h, active_idx);
                } else if (active_idx == 0) {
                    gui_shell_execute(left_w, header_h, content_h);
                }
            }

            /* shell input handling */
            if (active_idx == 0) {
                if (c >= 32 && c <= 126 && gui_shell_len < GUI_SHELL_BUF_SIZE - 1) {
                    gui_shell_buf[gui_shell_len++] = c;
                    vga_draw_char(gui_shell_input_x, gui_shell_y, c, 0x0F);
                    gui_shell_input_x += 6;
                } else if (c == '\b' && gui_shell_len > 0) {
                    gui_shell_len--;
                    gui_shell_input_x -= 6;
                    vga_fill_rect(gui_shell_input_x, gui_shell_y, 6, 8, 0x00);
                }
            }
        }

        mouse_state_t state;
        mouse_get_state(&state);

        if (state.x != prev.x || state.y != prev.y) {
            mouse_erase_cursor(prev.x, prev.y);
            mouse_draw_cursor(state.x, state.y);
            prev = state;
        }

        for (volatile int i = 0; i < 5000; i++) {
            __asm__ volatile("nop");
        }
    }

    mouse_erase_cursor(prev.x, prev.y);
    vga_set_text_mode();
    term_init_text();
    vga_puts("Returned to text mode.\n");
    return 0;
}

static int execute_command(char* cmd) {
    if (strlen(cmd) == 0) return 0;
    
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
    
    vga_puts(argv[0]);
    vga_puts(" not found\n");
    return 1;
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
            char c = keyboard_getc();
            
            if (c == '\n') {
                command[cmd_len] = '\0';
                vga_putchar('\n');
                execute_command(command);
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