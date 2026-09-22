#ifndef EDIT_H
#define EDIT_H

#define EDIT_MAX_LINES 256
#define EDIT_MAX_LINE_LENGTH 256

/* Full-screen text-mode editor (BIOS/VGA path). Blocking input loop. */
void edit_file(const char* filename);

/* ===== Core editor engine (drawing-free, shared with the desktop modal) =====
 * The line buffer, cursor and viewport live in edit.c; the desktop editor
 * drives it key-by-key from its own loop and renders it with the dt_*
 * primitives. Control codes (save/exit) stay the caller's business. */
int  edit_core_open(const char* fname);   /* fresh buffer, load file (may be empty) */
void edit_core_save(const char* fname);   /* write buffer back, creating the file */
void edit_core_key(unsigned char c);      /* printable / \n / \b / arrows 0x80-0x83 */
void edit_core_set_viewport(int rows);    /* scroll window; default = VGA text */

int  edit_core_line_count(void);
const char* edit_core_line(int idx);      /* "" outside [0, count) */
int  edit_core_cur_x(void);
int  edit_core_cur_y(void);
int  edit_core_top(void);                 /* first visible line */

#endif
