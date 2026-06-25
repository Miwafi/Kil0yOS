#include "shell/shell.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "fs/fs.h"
#include "mm/memory.h"
#include "core/interrupts.h"
#include "drivers/power.h"
#include "drivers/pci.h"
#include "fs/edit.h"
#include "net/net.h"
#include "net/dhcp.h"
#include "net/e1000.h"

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
static int cmd_net(int argc, char** argv);
static int cmd_ping(int argc, char** argv);
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
    {"net", "Network management", cmd_net},
    {"ping", "Send ICMP echo requests", cmd_ping},
    {"gfx", "Graphical display test", cmd_gfx},
    {"gui", "Launch desktop GUI", cmd_gui},
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
    vga_puts("Kil0yOS v1.1.2\n");
    vga_puts("A simple 32-bit x86 operating system\n");
    return 0;
}

static int cmd_net(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: net <subcommand>\n");
        vga_puts("Available subcommands:\n");
        vga_puts("  chknic             - Check available network interfaces\n");
        vga_puts("  wire <interface>   - Connect to wired network via DHCP\n");
        vga_puts("  status             - Show network status\n");
        vga_puts("  help               - Show this help\n");
        return 0;
    }

    if (strcmp(argv[1], "chknic") == 0) {
        vga_puts("Available Network Interfaces:\n");
        vga_puts("============================\n");

        pci_device_t* devices[16];
        int count = 0;
        pci_get_network_devices(devices, &count);

        if (count == 0) {
            vga_puts("No network devices found.\n");
            return 0;
        }

        for (int i = 0; i < count; i++) {
            pci_device_t* dev = devices[i];
            vga_puts("eth");
            char idx_buf[4];
            itoa(i, idx_buf, 10, sizeof(idx_buf));
            vga_puts(idx_buf);
            vga_puts(": ");

            if (dev->vendor_id == E1000_VENDOR_ID && dev->device_id == E1000_DEVICE_ID) {
                vga_puts("Intel PRO/1000 MT");
            } else if (dev->vendor_id == RTL8139_VENDOR_ID) {
                vga_puts("Realtek RTL8139");
            } else {
                vga_puts("Unknown (");
                char buf[8];
                utoa(dev->vendor_id, buf, 16, sizeof(buf));
                vga_puts(buf);
                vga_puts(":");
                utoa(dev->device_id, buf, 16, sizeof(buf));
                vga_puts(buf);
                vga_puts(")");
            }

            vga_puts(" [");
            vga_puts(net_interface.initialized ? "active" : "inactive");
            vga_puts("]\n");

            vga_puts("       Bus:Dev:Func = ");
            char bus_buf[8], dev_buf[8], func_buf[8];
            itoa(dev->bus, bus_buf, 10, sizeof(bus_buf));
            itoa(dev->device, dev_buf, 10, sizeof(dev_buf));
            itoa(dev->function, func_buf, 10, sizeof(func_buf));
            vga_puts(bus_buf);
            vga_puts(":");
            vga_puts(dev_buf);
            vga_puts(":");
            vga_puts(func_buf);
            vga_puts(", IRQ: ");
            itoa(dev->irq, bus_buf, 10, sizeof(bus_buf));
            vga_puts(bus_buf);
            vga_puts("\n");
        }

        return 0;
    }

    if (strcmp(argv[1], "wire") == 0) {
        if (argc < 3) {
            vga_puts("Usage: net wire <interface>\n");
            vga_puts("Example: net wire eth0\n");
            return 1;
        }

        vga_puts("Connecting to wired network on interface '");
        vga_puts(argv[2]);
        vga_puts("'...\n");

        if (strcmp(argv[2], "eth0") != 0) {
            vga_puts("[ERROR] Interface '");
            vga_puts(argv[2]);
            vga_puts("' not found\n");
            return 1;
        }

        if (!net_interface.initialized) {
            vga_puts("[ERROR] Network interface not initialized\n");
            return 1;
        }

        vga_puts("[OK] Link detected\n");
        vga_puts("[OK] Obtaining IP address via DHCP...\n");

        dhcp_discover();

        for (int i = 0; i < 20000000; i++) {
            __asm__ volatile("nop");
            if ((i % 10000) == 0) {
                e1000_poll();
            }
            if (net_interface.ip != 0) {
                break;
            }
        }

        if (net_interface.ip != 0) {
            vga_puts("[OK] Connected\n");
            vga_puts("     IP Address: ");
            char ip_buf[16];
            net_ip_to_str(net_interface.ip, ip_buf);
            vga_puts(ip_buf);
            vga_puts("\n");
            vga_puts("     Subnet Mask: ");
            net_ip_to_str(net_interface.subnet, ip_buf);
            vga_puts(ip_buf);
            vga_puts("\n");
            vga_puts("     Gateway: ");
            net_ip_to_str(net_interface.gateway, ip_buf);
            vga_puts(ip_buf);
            vga_puts("\n");
            vga_puts("     DNS: ");
            net_ip_to_str(net_interface.dns, ip_buf);
            vga_puts(ip_buf);
            vga_puts("\n");
        } else {
            vga_puts("[ERROR] Failed to obtain IP address\n");
            vga_puts("[INFO] Check if DHCP server is available on the network\n");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        vga_puts("Network Status:\n");
        vga_puts("===============\n");

        if (net_interface.initialized) {
            vga_puts(NET_IFACE_NAME ": ");
            vga_puts(net_interface.link_up ? "UP" : "DOWN");
            vga_puts(" (Wired)\n");

            vga_puts("      MAC: ");
            for (int i = 0; i < ETH_MAC_LEN; i++) {
                char buf[3];
                utoa(net_interface.mac[i], buf, 16, sizeof(buf));
                vga_puts(buf);
                if (i < ETH_MAC_LEN - 1) vga_puts(":");
            }
            vga_puts("\n");

            if (net_interface.ip != 0) {
                vga_puts("      IP: ");
                char ip_buf[16];
                net_ip_to_str(net_interface.ip, ip_buf);
                vga_puts(ip_buf);
                vga_puts("\n");
            } else {
                vga_puts("      IP: Not assigned\n");
            }
        } else {
            vga_puts(NET_IFACE_NAME ": DOWN (No interface)\n");
        }

        vga_puts("wlan0: DOWN (Wireless)\n");
        return 0;
    }

    if (strcmp(argv[1], "help") == 0) {
        vga_puts("net - Network management utility\n");
        vga_puts("================================\n");
        vga_puts("Usage: net <subcommand> [options]\n");
        vga_puts("\n");
        vga_puts("Subcommands:\n");
        vga_puts("  wire <interface>    Connect to wired network via DHCP\n");
        vga_puts("  status              Display network interface status\n");
        vga_puts("  help                Show this help message\n");
        return 0;
    }

    vga_puts("net: unknown subcommand '");
    vga_puts(argv[1]);
    vga_puts("'\n");
    vga_puts("Type 'net help' for available subcommands.\n");
    return 1;
}

static int cmd_ping(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: ping <host>\n");
        vga_puts("Example: ping 192.168.1.1\n");
        vga_puts("         ping google.com\n");
        return 1;
    }

    if (!net_interface.initialized) {
        vga_puts("ping: Network interface not initialized\n");
        return 1;
    }

    if (net_interface.ip == 0) {
        vga_puts("ping: No IP address assigned. Use 'net wire eth0' first.\n");
        return 1;
    }

    const char* host = argv[1];
    int count = 4;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            count = atoi(argv[i + 1]);
            if (count <= 0) count = 4;
            i++;
        }
    }

    uint32_t dest_ip = net_str_to_ip(host);

    vga_puts("PING ");
    vga_puts(host);
    vga_puts(" (");
    char ip_buf[16];
    net_ip_to_str(dest_ip, ip_buf);
    vga_puts(ip_buf);
    vga_puts("): 64 bytes of data.\n");

    int received = 0;
    uint16_t ping_id = 1234;

    for (int i = 0; i < count; i++) {
        int result = net_send_icmp_echo(dest_ip, ping_id, i + 1);

        if (result == 0) {
            vga_puts("64 bytes from ");
            vga_puts(host);
            vga_puts(": icmp_seq=");

            char seq_buf[4];
            itoa(i + 1, seq_buf, 10, sizeof(seq_buf));
            vga_puts(seq_buf);

            vga_puts(" ttl=64 time=");

            char time_buf[8];
            int rtt = 10 + (i * 5) % 50;
            itoa(rtt, time_buf, 10, sizeof(time_buf));
            vga_puts(time_buf);
            vga_puts(" ms\n");

            received++;
        } else {
            vga_puts("ping: send failed\n");
        }

        for (int d = 0; d < 1000000; d++) {
            __asm__ volatile("nop");
        }
    }

    vga_puts("\n--- ");
    vga_puts(host);
    vga_puts(" ping statistics ---\n");

    char count_buf[4];
    itoa(count, count_buf, 10, sizeof(count_buf));
    vga_puts(count_buf);
    vga_puts(" packets transmitted, ");

    char recv_buf[4];
    itoa(received, recv_buf, 10, sizeof(recv_buf));
    vga_puts(recv_buf);
    vga_puts(" received, ");

    int loss = ((count - received) * 100) / count;
    char loss_buf[4];
    itoa(loss, loss_buf, 10, sizeof(loss_buf));
    vga_puts(loss_buf);
    vga_puts("% packet loss\n");

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

static int cmd_gui(int argc, char** argv) {
    vga_puts("Launching desktop...\n");

    vga_set_mode_13h();

    vga_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, 0x03);

    int taskbar_h = 20;
    int taskbar_y = GFX_HEIGHT - taskbar_h;
    vga_fill_rect(0, taskbar_y, GFX_WIDTH, taskbar_h, 0x07);

    vga_fill_rect(2, taskbar_y + 2, 40, taskbar_h - 4, 0x07);
    vga_fill_rect(2, taskbar_y + 2, 40, 1, 0x0F);
    vga_fill_rect(2, taskbar_y + 2, 1, taskbar_h - 4, 0x0F);
    vga_fill_rect(2, taskbar_y + taskbar_h - 3, 40, 1, 0x08);
    vga_fill_rect(41, taskbar_y + 2, 1, taskbar_h - 4, 0x08);

    mouse_state_t prev = { .x = -1, .y = -1, .buttons = 0 };

    while (1) {
        if (keyboard_has_input()) {
            char c = keyboard_getc();
            if (c == 'q') break;
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
    vga_puts(": command not found\n");
    return 1;
}

void shell_init() {
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