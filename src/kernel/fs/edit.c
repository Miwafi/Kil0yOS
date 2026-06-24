#include "fs/edit.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "lib/string.h"
#include "mm/memory.h"
#include "fs/fs.h"

static char lines[EDIT_MAX_LINES][EDIT_MAX_LINE_LENGTH];
static int line_count = 0;
static int cursor_x = 0;
static int cursor_y = 0;
static int screen_top = 0;
static const char* filename = NULL;

static void edit_draw_status_bar() {
    vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLUE));
    for (int i = 0; i < VGA_WIDTH; i++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + i] = vga_entry(' ', vga_color);
    }
    
    vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLUE));
    vga_puts("-- Kil0yOS Editor -- ");
    vga_puts(filename);
    vga_puts("  Ctrl+S: Save  Ctrl+X: Exit");
}

static void edit_draw() {
    vga_clear();
    
    int visible_lines = VGA_HEIGHT - 1;
    
    for (int i = 0; i < visible_lines; i++) {
        int line_idx = screen_top + i;
        if (line_idx < line_count) {
            vga_set_color(vga_entry_color(COLOR_WHITE, COLOR_BLACK));
            vga_puts(lines[line_idx]);
        }
        vga_putchar('\n');
    }
    
    edit_draw_status_bar();
    
    vga_set_cursor(cursor_x, cursor_y - screen_top);
}

static void edit_insert_char(char c) {
    if (cursor_y >= line_count) return;
    
    char* line = lines[cursor_y];
    int len = strlen(line);
    
    if (len >= EDIT_MAX_LINE_LENGTH - 1) return;
    
    for (int i = len; i >= cursor_x; i--) {
        line[i + 1] = line[i];
    }
    
    line[cursor_x] = c;
    cursor_x++;
}

static void edit_delete_char() {
    if (cursor_y >= line_count) return;
    
    char* line = lines[cursor_y];
    int len = strlen(line);
    
    if (cursor_x == 0) {
        if (cursor_y > 0) {
            int prev_len = strlen(lines[cursor_y - 1]);
            if (prev_len + len < EDIT_MAX_LINE_LENGTH - 1) {
                strcat(lines[cursor_y - 1], line);
            }
            
            for (int i = cursor_y; i < line_count - 1; i++) {
                strcpy(lines[i], lines[i + 1]);
            }
            line_count--;
            cursor_y--;
            cursor_x = prev_len;
        }
        return;
    }
    
    for (int i = cursor_x - 1; i < len; i++) {
        line[i] = line[i + 1];
    }
    
    cursor_x--;
}

static void edit_new_line() {
    if (line_count >= EDIT_MAX_LINES) return;
    
    char* line = lines[cursor_y];
    int len = strlen(line);
    
    if (cursor_x < len) {
        char rest[EDIT_MAX_LINE_LENGTH];
        strcpy(rest, line + cursor_x);
        
        line[cursor_x] = '\0';
        
        for (int i = line_count; i > cursor_y + 1; i--) {
            strcpy(lines[i], lines[i - 1]);
        }
        
        strcpy(lines[cursor_y + 1], rest);
        line_count++;
    } else {
        for (int i = line_count; i > cursor_y + 1; i--) {
            strcpy(lines[i], lines[i - 1]);
        }
        lines[cursor_y + 1][0] = '\0';
        line_count++;
    }
    
    cursor_y++;
    cursor_x = 0;
}

static void edit_move_up() {
    if (cursor_y > 0) {
        cursor_y--;
        int line_len = strlen(lines[cursor_y]);
        if (cursor_x > line_len) {
            cursor_x = line_len;
        }
    }
    
    if (cursor_y < screen_top) {
        screen_top = cursor_y;
    }
}

static void edit_move_down() {
    if (cursor_y < line_count - 1) {
        cursor_y++;
        int line_len = strlen(lines[cursor_y]);
        if (cursor_x > line_len) {
            cursor_x = line_len;
        }
    }
    
    if (cursor_y >= screen_top + VGA_HEIGHT - 1) {
        screen_top++;
    }
}

static void edit_move_left() {
    if (cursor_x > 0) {
        cursor_x--;
    }
}

static void edit_move_right() {
    if (cursor_y < line_count) {
        int line_len = strlen(lines[cursor_y]);
        if (cursor_x < line_len) {
            cursor_x++;
        }
    }
}

static void edit_load_file(const char* fname) {
    fs_entry_t* file = fs_resolve_path(fname);
    if (file == NULL) {
        lines[0][0] = '\0';
        line_count = 1;
        return;
    }
    
    if (file->type != FS_TYPE_FILE) {
        lines[0][0] = '\0';
        line_count = 1;
        return;
    }
    
    if (file->size == 0) {
        lines[0][0] = '\0';
        line_count = 1;
        return;
    }
    
    size_t buffer_size = file->size < MAX_FILE_SIZE ? file->size : MAX_FILE_SIZE;
    uint8_t* buffer = (uint8_t*)kmalloc(buffer_size + 1);
    if (buffer == NULL) {
        lines[0][0] = '\0';
        line_count = 1;
        return;
    }
    
    int size = fs_read_file(file, buffer, buffer_size);
    if (size <= 0) {
        kfree(buffer);
        lines[0][0] = '\0';
        line_count = 1;
        return;
    }
    
    buffer[size] = '\0';
    line_count = 0;
    
    char* ptr = (char*)buffer;
    char* end = ptr + size;
    
    while (ptr < end && line_count < EDIT_MAX_LINES) {
        int i = 0;
        while (ptr < end && *ptr != '\n' && *ptr != '\r' && i < EDIT_MAX_LINE_LENGTH - 1) {
            lines[line_count][i++] = *ptr++;
        }
        lines[line_count][i] = '\0';
        line_count++;
        
        if (ptr < end && (*ptr == '\n' || *ptr == '\r')) {
            ptr++;
            if (ptr < end && *ptr == '\n' && *(ptr - 1) == '\r') {
                ptr++;
            }
        }
    }
    
    kfree(buffer);
    
    if (line_count == 0) {
        lines[0][0] = '\0';
        line_count = 1;
    }
}

static void edit_save_file(const char* fname) {
    int total_size = 0;
    for (int i = 0; i < line_count; i++) {
        total_size += strlen(lines[i]) + 1;
    }
    
    if (total_size > MAX_FILE_SIZE) {
        total_size = MAX_FILE_SIZE;
    }
    
    char* content = (char*)kmalloc(total_size + 1);
    if (content == NULL) return;
    
    int pos = 0;
    for (int i = 0; i < line_count; i++) {
        int len = strlen(lines[i]);
        if (pos + len + 1 > total_size) break;
        
        memcpy(content + pos, lines[i], len);
        pos += len;
        content[pos++] = '\n';
    }
    
    content[pos] = '\0';
    
    fs_entry_t* file = fs_resolve_path(fname);
    if (file == NULL) {
        file = fs_create_file(fname);
    }
    
    if (file != NULL && file->type == FS_TYPE_FILE) {
        fs_write_file(file, (uint8_t*)content, pos);
    }
    
    kfree(content);
}

void edit_file(const char* fname) {
    filename = fname;
    
    cursor_x = 0;
    cursor_y = 0;
    screen_top = 0;
    
    for (int i = 0; i < EDIT_MAX_LINES; i++) {
        lines[i][0] = '\0';
    }
    
    edit_load_file(fname);
    
    edit_draw();
    
    while (1) {
        char c = keyboard_getc();
        
        if (c == 0x13) {
            edit_save_file(fname);
            edit_draw();
            continue;
        }
        
        if (c == 0x18) {
            return;
        }
        
        if (c == '\n') {
            edit_new_line();
            edit_draw();
            continue;
        }
        
        if (c == '\b') {
            edit_delete_char();
            edit_draw();
            continue;
        }
        
        unsigned char uc = (unsigned char)c;
        switch (uc) {
            case 0x80:
                edit_move_up();
                edit_draw();
                continue;
            case 0x81:
                edit_move_down();
                edit_draw();
                continue;
            case 0x82:
                edit_move_left();
                edit_draw();
                continue;
            case 0x83:
                edit_move_right();
                edit_draw();
                continue;
        }
        
        if (c >= 0x20 && c <= 0x7E) {
            edit_insert_char(c);
            edit_draw();
        }
    }
}