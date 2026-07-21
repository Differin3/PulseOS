// GNU nano-style editor for My OS (colors, shortcuts, search, cut/paste).
#include "nano.h"
#include "../drivers/video/terminal.h"
#include "../drivers/input/keyboard.h"
#include "../fs.h"
#include "../kernel.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static inline uint8_t vga_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t)(fg | (bg << 4));
}

#define NANO_BUF_SIZE 8192
#define NANO_CLIP_SIZE 512
#define NANO_MARGIN 7
#define NANO_EDIT_W (80 - NANO_MARGIN)
#define NANO_PROMPT_SZ 96
#define NANO_SEARCH_SZ 64

static char nano_buf[NANO_BUF_SIZE];
static size_t nano_len = 0;
static size_t nano_cursor = 0;
static size_t nano_top_line = 0;
static size_t nano_hscroll = 0;
static char nano_path[128];
static char nano_clip[NANO_CLIP_SIZE];
static size_t nano_clip_len = 0;
static char nano_search[NANO_SEARCH_SZ];
static char nano_status[80];
static char nano_prompt[NANO_PROMPT_SZ];
static size_t nano_prompt_len = 0;

static bool nano_dirty = false;
static bool nano_e0 = false;
static bool nano_show_lnums = true;
static bool nano_help = false;
static uint32_t nano_e0_timeout = 0;
static uint32_t nano_status_ttl = 0;

static size_t nano_scr_h = 25;
static size_t nano_edit_rows = 22;
static size_t nano_row_keys = 23;
static size_t nano_row_stat = 24;

static uint8_t col_title;
static uint8_t col_edit;
static uint8_t col_lnum;
static uint8_t col_lnum_cur;
static uint8_t col_bar;
static uint8_t col_status;
static uint8_t col_prompt;
static uint8_t col_comment;
static uint8_t col_match;
static uint8_t col_modified;

enum nano_ui_mode {
    NANO_UI_EDIT = 0,
    NANO_UI_PROMPT,
    NANO_UI_YESNO
};

enum nano_prompt_kind {
    NANO_P_NONE = 0,
    NANO_P_SAVE,
    NANO_P_SEARCH,
    NANO_P_GOTO,
    NANO_P_EXIT
};

static int nano_ui = NANO_UI_EDIT;
static int nano_prompt_kind = NANO_P_NONE;

enum nano_key {
    NANO_KEY_NONE = 0,
    NANO_KEY_CHAR,
    NANO_KEY_UP, NANO_KEY_DOWN, NANO_KEY_LEFT, NANO_KEY_RIGHT,
    NANO_KEY_PGUP, NANO_KEY_PGDN, NANO_KEY_HOME, NANO_KEY_END,
    NANO_KEY_ESC,
    NANO_KEY_HELP, NANO_KEY_SAVE, NANO_KEY_EXIT,
    NANO_KEY_CUT, NANO_KEY_UNCUT,
    NANO_KEY_SEARCH, NANO_KEY_SEARCH_NEXT,
    NANO_KEY_GOTO, NANO_KEY_BOL, NANO_KEY_EOL,
    NANO_KEY_REFRESH, NANO_KEY_POS, NANO_KEY_DEL,
    NANO_KEY_TOGGLE_LNUM,
    NANO_KEY_YES, NANO_KEY_NO
};

static void nano_init_colors(void) {
    col_title = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE);
    col_edit = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    col_lnum = vga_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    col_lnum_cur = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    col_bar = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_DARK_GREY);
    col_status = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_DARK_GREY);
    col_prompt = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    col_comment = vga_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    col_match = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_DARK_GREY);
    col_modified = vga_color(VGA_COLOR_BROWN, VGA_COLOR_BLUE);
}

static void nano_layout(void) {
    nano_scr_h = terminal_get_height();
    if (nano_scr_h < 8) nano_scr_h = 8;
    nano_edit_rows = nano_scr_h - 3;
    if (nano_edit_rows < 4) nano_edit_rows = 4;
    nano_row_keys = nano_scr_h - 2;
    nano_row_stat = nano_scr_h - 1;
}

static void nano_set_status(const char* msg) {
    size_t i = 0;
    while (msg && msg[i] && i + 1 < sizeof(nano_status)) {
        nano_status[i] = msg[i];
        i++;
    }
    nano_status[i] = 0;
    nano_status_ttl = 8000;
}

static int nano_line_index(size_t pos) {
    int line = 0;
    for (size_t i = 0; i < pos && i < nano_len; i++) {
        if (nano_buf[i] == '\n') line++;
    }
    return line;
}

static int nano_line_count(void) {
    if (nano_len == 0) return 1;
    return nano_line_index(nano_len) + 1;
}

static size_t nano_line_start(int line) {
    int cur = 0;
    for (size_t i = 0; i < nano_len; i++) {
        if (cur == line) return i;
        if (nano_buf[i] == '\n') cur++;
    }
    return nano_len;
}

static size_t nano_line_end(size_t start) {
    size_t i = start;
    while (i < nano_len && nano_buf[i] != '\n') i++;
    return i;
}

static void nano_delete_range(size_t from, size_t to) {
    if (from >= to || from >= nano_len) return;
    if (to > nano_len) to = nano_len;
    size_t delta = to - from;
    for (size_t i = from; i + delta < nano_len; i++) {
        nano_buf[i] = nano_buf[i + delta];
    }
    nano_len -= delta;
    if (nano_cursor > from && nano_cursor >= to) nano_cursor -= delta;
    else if (nano_cursor > from && nano_cursor < to) nano_cursor = from;
    nano_buf[nano_len] = 0;
    nano_dirty = true;
}

static void nano_insert_char(char c) {
    if (nano_len + 1 >= NANO_BUF_SIZE) {
        nano_set_status("[ Buffer full ]");
        return;
    }
    for (size_t i = nano_len; i > nano_cursor; i--) nano_buf[i] = nano_buf[i - 1];
    nano_buf[nano_cursor] = c;
    nano_len++;
    nano_cursor++;
    nano_buf[nano_len] = 0;
    nano_dirty = true;
}

static void nano_delete_char(void) {
    if (nano_cursor == 0) return;
    nano_delete_range(nano_cursor - 1, nano_cursor);
}

static void nano_delete_forward(void) {
    if (nano_cursor >= nano_len) return;
    nano_delete_range(nano_cursor, nano_cursor + 1);
}

static void nano_cut_line(void) {
    int line = nano_line_index(nano_cursor);
    size_t start = nano_line_start(line);
    size_t end = nano_line_end(start);
    if (end < nano_len && nano_buf[end] == '\n') end++;
    size_t len = end - start;
    if (len >= NANO_CLIP_SIZE) len = NANO_CLIP_SIZE - 1;
    for (size_t i = 0; i < len; i++) nano_clip[i] = nano_buf[start + i];
    nano_clip[len] = 0;
    nano_clip_len = len;
    nano_delete_range(start, end);
    nano_cursor = start;
    nano_set_status("[ Cut line ]");
}

static void nano_paste(void) {
    if (nano_clip_len == 0) {
        nano_set_status("[ Clipboard empty ]");
        return;
    }
    if (nano_len + nano_clip_len >= NANO_BUF_SIZE) {
        nano_set_status("[ Paste: buffer full ]");
        return;
    }
    for (size_t i = 0; i < nano_clip_len; i++) nano_insert_char(nano_clip[i]);
    nano_set_status("[ Pasted ]");
}

static bool nano_strstr_from(size_t start, const char* needle, size_t* out_pos) {
    if (!needle || !needle[0]) return false;
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || start >= nano_len) return false;
    for (size_t i = start; i + nlen <= nano_len; i++) {
        bool ok = true;
        for (size_t j = 0; j < nlen; j++) {
            if (nano_buf[i + j] != needle[j]) { ok = false; break; }
        }
        if (ok) {
            if (out_pos) *out_pos = i;
            return true;
        }
    }
    return false;
}

static bool nano_search_fwd(bool from_start) {
    if (!nano_search[0]) return false;
    size_t start = from_start ? 0 : nano_cursor + 1;
    size_t pos = 0;
    if (nano_strstr_from(start, nano_search, &pos)) {
        nano_cursor = pos;
        return true;
    }
    if (!from_start && nano_strstr_from(0, nano_search, &pos)) {
        nano_cursor = pos;
        nano_set_status("[ Search wrapped ]");
        return true;
    }
    return false;
}

static void nano_save_file(void) {
    if (fs_write(nano_path, nano_buf, nano_len) == 0) {
        nano_dirty = false;
        char msg[48];
        msg[0] = '['; msg[1] = ' ';
        msg[2] = 'W'; msg[3] = 'r'; msg[4] = 'o'; msg[5] = 't'; msg[6] = 'e';
        msg[7] = ' ';
        size_t p = 8;
        size_t n = nano_len;
        char tmp[12];
        int t = 0;
        if (n == 0) { tmp[t++] = '0'; }
        while (n > 0 && t < 10) { tmp[t++] = (char)('0' + (n % 10)); n /= 10; }
        while (t > 0 && p + 1 < sizeof(msg)) msg[p++] = tmp[--t];
        const char* tail = " bytes ]";
        for (int i = 0; tail[i] && p + 1 < sizeof(msg); i++) msg[p++] = tail[i];
        msg[p] = 0;
        nano_set_status(msg);
    } else {
        nano_set_status("[ Write failed ]");
    }
}

static void nano_begin_prompt(int kind, const char* label, const char* initial) {
    nano_ui = NANO_UI_PROMPT;
    nano_prompt_kind = kind;
    nano_prompt_len = 0;
    if (label) nano_set_status(label);
    if (initial) {
        while (initial[nano_prompt_len] && nano_prompt_len + 1 < NANO_PROMPT_SZ) {
            nano_prompt[nano_prompt_len] = initial[nano_prompt_len];
            nano_prompt_len++;
        }
    }
    nano_prompt[nano_prompt_len] = 0;
}

static void nano_begin_yesno(const char* question) {
    nano_ui = NANO_UI_YESNO;
    nano_prompt_kind = NANO_P_EXIT;
    nano_set_status(question);
}

static uint8_t nano_syntax_color(size_t pos) {
    size_t ls = nano_line_start(nano_line_index(pos));
    size_t p = ls;
    while (p < nano_len && p < pos && (nano_buf[p] == ' ' || nano_buf[p] == '\t')) p++;
    if (p < nano_len && nano_buf[p] == '#') return col_comment;
    if (p + 1 < nano_len && nano_buf[p] == '/' && nano_buf[p + 1] == '/') return col_comment;
    (void)pos;
    return col_edit;
}

static bool nano_is_match_at(size_t pos) {
    if (!nano_search[0]) return false;
    size_t n = 0;
    while (nano_search[n]) n++;
    if (pos + n > nano_len) return false;
    for (size_t i = 0; i < n; i++) {
        if (nano_buf[pos + i] != nano_search[i]) return false;
    }
    return true;
}

static void nano_draw_shortcuts(void) {
    terminal_editor_clear_row(nano_row_keys, col_bar);
    terminal_editor_print_at(nano_row_keys, 0, "^G Help  ^O Save  ^X Exit  ^W Find  ^K Cut  ^U Paste", col_bar);
}

static void nano_draw_title(void) {
    terminal_editor_clear_row(0, col_title);
    terminal_editor_print_at(0, 0, " nano ", col_title);
    size_t col = 6;
    for (size_t i = 0; nano_path[i] && col < 70; i++) {
        terminal_editor_putat(0, col++, nano_path[i], col_title);
    }
    if (nano_dirty) {
        terminal_editor_print_at(0, col, " *", col_modified);
    }
}

static void nano_print_line_num(size_t row, int line, bool current) {
    int n = line + 1;
    char tmp[6];
    int t = 0;
    if (n == 0) tmp[t++] = '0';
    while (n > 0 && t < 5) {
        tmp[t++] = (char)('0' + (n % 10));
        n /= 10;
    }
    char num[NANO_MARGIN + 1];
    int p = 0;
    int pad = 4 - t;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; i++) num[p++] = ' ';
    while (t > 0) num[p++] = tmp[--t];
    num[p++] = ' ';
    num[p++] = '|';
    num[p] = 0;
    uint8_t c = current ? col_lnum_cur : col_lnum;
    for (size_t i = 0; num[i] && i < NANO_MARGIN; i++) {
        terminal_editor_putat(row, i, num[i], c);
    }
}

static void nano_draw_edit(void) {
    int cur_line = nano_line_index(nano_cursor);
    if ((size_t)cur_line < nano_top_line) nano_top_line = (size_t)cur_line;
    if ((size_t)cur_line >= nano_top_line + nano_edit_rows) {
        nano_top_line = (size_t)cur_line - nano_edit_rows + 1;
    }

    size_t cstart = nano_line_start(cur_line);
    size_t ccol = nano_cursor - cstart;
    size_t edit_col = nano_show_lnums ? NANO_MARGIN : 0;
    size_t edit_w = nano_show_lnums ? NANO_EDIT_W : 80;

    if (ccol >= edit_w) nano_hscroll = ccol - edit_w + 1;
    else if (ccol < nano_hscroll) nano_hscroll = ccol;

    for (size_t row = 0; row < nano_edit_rows; row++) {
        size_t erow = row + 1;
        terminal_editor_clear_row(erow, col_edit);
        int line = (int)(nano_top_line + row);
        size_t start = nano_line_start(line);
        if (start > nano_len) continue;
        size_t end = nano_line_end(start);
        bool cur = (line == cur_line);

        if (nano_show_lnums) nano_print_line_num(erow, line, cur);

        size_t col = 0;
        for (size_t i = start + nano_hscroll; i < end && col < edit_w; i++) {
            char ch = nano_buf[i];
            if (ch == '\r') continue;
            uint8_t fg = nano_syntax_color(i);
            if (nano_is_match_at(i)) fg = col_match;
            terminal_editor_putat(erow, edit_col + col++, ch, fg);
        }
    }

    if ((size_t)cur_line >= nano_top_line) {
        size_t crow = (size_t)(cur_line - (int)nano_top_line) + 1;
        size_t disp_col = edit_col + (ccol >= nano_hscroll ? ccol - nano_hscroll : 0);
        if (disp_col > 79) disp_col = 79;
        terminal_editor_show_cursor(crow, disp_col);
    }
}

static void nano_draw_status(void) {
    if (nano_ui == NANO_UI_PROMPT) {
        terminal_editor_clear_row(nano_row_stat, col_prompt);
        terminal_editor_print_at(nano_row_stat, 0, nano_status, col_prompt);
        terminal_editor_print_at(nano_row_stat, 40, nano_prompt, col_prompt);
        return;
    }
    if (nano_ui == NANO_UI_YESNO) {
        terminal_editor_clear_row(nano_row_stat, col_status);
        terminal_editor_print_at(nano_row_stat, 0, nano_status, col_status);
        return;
    }
    terminal_editor_clear_row(nano_row_stat, col_status);
    if (nano_status_ttl > 0) {
        terminal_editor_print_at(nano_row_stat, 0, nano_status, col_status);
    } else {
        int line = nano_line_index(nano_cursor) + 1;
        int chars = (int)(nano_line_end(nano_line_start(line - 1)) - nano_line_start(line - 1));
        char info[40];
        info[0] = 'L'; info[1] = 'n'; info[2] = 'e';
        info[3] = ' '; info[4] = (char)('0' + (line / 10) % 10); info[5] = (char)('0' + line % 10);
        info[6] = ','; info[7] = ' '; info[8] = 'C'; info[9] = 'o'; info[10] = 'l';
        info[11] = ' '; info[12] = (char)('0' + (chars / 10) % 10); info[13] = (char)('0' + chars % 10);
        info[14] = 0;
        terminal_editor_print_at(nano_row_stat, 0, info, col_status);
    }
}

static void nano_draw_help(void) {
    uint8_t h = vga_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_DARK_GREY);
    for (size_t r = 0; r < nano_scr_h; r++) terminal_editor_clear_row(r, h);
    terminal_editor_print_at(1, 0, " nano help ", h);
    terminal_editor_print_at(3, 0, "^X Exit   ^O Save   ^W Search   ^K Cut line   ^U Paste", h);
    terminal_editor_print_at(4, 0, "^G Help   Esc Cancel prompt   Y/N when saving on exit", h);
    terminal_editor_print_at(nano_scr_h - 1, 0, " any key to close ", h);
}

static void nano_draw(void) {
    if (nano_help) {
        nano_draw_help();
        return;
    }
    nano_draw_title();
    nano_draw_edit();
    nano_draw_shortcuts();
    nano_draw_status();
}

static int nano_poll_key(char* out_ch) {
    uint8_t status;
    asm volatile ("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x64));
    if (!(status & 0x01)) {
        if (nano_e0_timeout > 0) {
            nano_e0_timeout--;
            if (nano_e0_timeout == 0) nano_e0 = false;
        }
        if (nano_status_ttl > 0) nano_status_ttl--;
        return NANO_KEY_NONE;
    }

    uint8_t scancode;
    asm volatile ("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)0x60));

    if (scancode == 0xE0) {
        nano_e0 = true;
        nano_e0_timeout = 1000;
        return NANO_KEY_NONE;
    }

    bool extended = nano_e0;

    if (scancode & 0x80) {
        keyboard_modifiers_update(scancode, extended);
        if (extended) { nano_e0 = false; nano_e0_timeout = 0; }
        return NANO_KEY_NONE;
    }

    if (scancode == 0x2A || scancode == 0x36 || scancode == 0x1D) {
        keyboard_modifiers_update(scancode, extended);
        nano_e0 = false;
        nano_e0_timeout = 0;
        return NANO_KEY_NONE;
    }

    if (extended && nano_e0_timeout > 0) {
        nano_e0 = false;
        nano_e0_timeout = 0;
        keyboard_poll_hit();
        if (scancode == 0x1D) {
            keyboard_modifiers_update(scancode, true);
            return NANO_KEY_NONE;
        }
        if (scancode == 0x53) return NANO_KEY_DEL;
        if (scancode == 0x48) return NANO_KEY_UP;
        if (scancode == 0x50) return NANO_KEY_DOWN;
        if (scancode == 0x4B) return NANO_KEY_LEFT;
        if (scancode == 0x4D) return NANO_KEY_RIGHT;
        if (scancode == 0x49) return NANO_KEY_PGUP;
        if (scancode == 0x51) return NANO_KEY_PGDN;
        if (scancode == 0x47) return NANO_KEY_HOME;
        if (scancode == 0x4F) return NANO_KEY_END;
        return NANO_KEY_NONE;
    }

    if (nano_e0) { nano_e0 = false; nano_e0_timeout = 0; }

    keyboard_poll_hit();
    char c = keyboard_scancode_char(scancode);
    if (c == 0) return NANO_KEY_NONE;

    if (c == 27) return NANO_KEY_ESC;

    if (keyboard_ctrl_down()) {
        char lc = c;
        if (lc >= 1 && lc <= 26) lc = (char)(lc + 'a' - 1);
        if (lc >= 'A' && lc <= 'Z') lc = (char)(lc + 32);
        if (lc == 'g') return NANO_KEY_HELP;
        if (lc == 'o') return NANO_KEY_SAVE;
        if (lc == 'x') return NANO_KEY_EXIT;
        if (lc == 'k') return NANO_KEY_CUT;
        if (lc == 'u') return NANO_KEY_UNCUT;
        if (lc == 'w') return NANO_KEY_SEARCH;
        if (lc == 'q') return NANO_KEY_SEARCH_NEXT;
        if (lc == 'l') return NANO_KEY_REFRESH;
        if (lc == 'c') return NANO_KEY_POS;
        if (lc == 'a') return NANO_KEY_BOL;
        if (lc == 'e') return NANO_KEY_EOL;
        if (lc == 'v') return NANO_KEY_PGDN;
        if (lc == 'y') return NANO_KEY_PGUP;
        if (lc == 'd') return NANO_KEY_DEL;
        if (lc == 'n') return NANO_KEY_TOGGLE_LNUM;
        if (lc == 's') return NANO_KEY_SAVE;
        if (lc == '-' || lc == '_') return NANO_KEY_GOTO;
    }

    *out_ch = c;
    return NANO_KEY_CHAR;
}

static void nano_move_up(void) {
    int line = nano_line_index(nano_cursor);
    if (line > 0) {
        size_t prev = nano_line_start(line - 1);
        size_t off = nano_cursor - nano_line_start(line);
        size_t plen = nano_line_end(prev) - prev;
        nano_cursor = prev + (off < plen ? off : plen);
    }
}

static void nano_move_down(void) {
    int line = nano_line_index(nano_cursor);
    size_t next = nano_line_start(line + 1);
    if (next < nano_len) {
        size_t off = nano_cursor - nano_line_start(line);
        size_t nlen = nano_line_end(next) - next;
        nano_cursor = next + (off < nlen ? off : nlen);
    } else if (nano_cursor < nano_len) {
        nano_cursor = nano_len;
    }
}

static int nano_prompt_submit(void) {
    if (nano_prompt_kind == NANO_P_SAVE) {
        if (nano_prompt_len > 0) {
            size_t i = 0;
            while (i + 1 < sizeof(nano_path) && nano_prompt[i]) {
                nano_path[i] = nano_prompt[i];
                i++;
            }
            nano_path[i] = 0;
        }
        nano_save_file();
    } else if (nano_prompt_kind == NANO_P_SEARCH) {
        size_t i = 0;
        while (i + 1 < NANO_SEARCH_SZ && nano_prompt[i]) {
            nano_search[i] = nano_prompt[i];
            i++;
        }
        nano_search[i] = 0;
        if (!nano_search_fwd(true)) nano_set_status("[ Not found ]");
    } else if (nano_prompt_kind == NANO_P_GOTO) {
        int target = 0;
        for (size_t i = 0; i < nano_prompt_len; i++) {
            if (nano_prompt[i] >= '0' && nano_prompt[i] <= '9') {
                target = target * 10 + (nano_prompt[i] - '0');
            }
        }
        if (target < 1) target = 1;
        if (target > nano_line_count()) target = nano_line_count();
        nano_cursor = nano_line_start(target - 1);
        nano_set_status("[ Go to line ]");
    }
    nano_ui = NANO_UI_EDIT;
    nano_prompt_kind = NANO_P_NONE;
    return 0;
}

static void nano_handle_movement(int key) {
    if (key == NANO_KEY_HOME || key == NANO_KEY_BOL) {
        nano_cursor = nano_line_start(nano_line_index(nano_cursor));
    } else if (key == NANO_KEY_END || key == NANO_KEY_EOL) {
        int line = nano_line_index(nano_cursor);
        nano_cursor = nano_line_end(nano_line_start(line));
    } else if (key == NANO_KEY_PGUP) {
        if (nano_top_line >= nano_edit_rows) nano_top_line -= nano_edit_rows;
        else nano_top_line = 0;
        int line = (int)nano_top_line;
        nano_cursor = nano_line_start(line);
    } else if (key == NANO_KEY_PGDN) {
        nano_top_line += nano_edit_rows;
        int maxl = nano_line_count() - 1;
        if ((int)nano_top_line > maxl) nano_top_line = (size_t)maxl;
        nano_cursor = nano_line_start((int)nano_top_line);
    } else if (key == NANO_KEY_UP) {
        nano_move_up();
    } else if (key == NANO_KEY_DOWN) {
        nano_move_down();
    } else if (key == NANO_KEY_LEFT && nano_cursor > 0) {
        nano_cursor--;
    } else if (key == NANO_KEY_RIGHT && nano_cursor < nano_len) {
        nano_cursor++;
    }
}

int nano_edit(const char* path) {
    if (!path) return -1;

    nano_init_colors();
    nano_layout();

    size_t pi = 0;
    while (path[pi] && pi + 1 < sizeof(nano_path)) {
        nano_path[pi] = path[pi];
        pi++;
    }
    nano_path[pi] = 0;

    nano_len = 0;
    nano_cursor = 0;
    nano_top_line = 0;
    nano_hscroll = 0;
    nano_dirty = false;
    nano_help = false;
    nano_ui = NANO_UI_EDIT;
    nano_prompt_kind = NANO_P_NONE;
    nano_e0 = false;
    nano_e0_timeout = 0;
    nano_status[0] = 0;
    nano_status_ttl = 0;
    nano_search[0] = 0;
    nano_clip_len = 0;
    nano_buf[0] = 0;

    uint32_t fsize = 0;
    if (fs_open(nano_path, &fsize) == 0 && fsize > 0) {
        size_t to_read = fsize < NANO_BUF_SIZE - 1 ? (size_t)fsize : NANO_BUF_SIZE - 1;
        if (fs_read(nano_path, nano_buf, to_read) >= 0) {
            nano_len = to_read;
            nano_buf[nano_len] = 0;
        }
    }

    terminal_editor_begin();
    nano_draw();

    for (;;) {
        char ch = 0;
        int key = nano_poll_key(&ch);

        if (key == NANO_KEY_NONE) {
            for (volatile int w = 0; w < 1500; w++);
            continue;
        }

        if (nano_help) {
            nano_help = false;
            nano_draw();
            continue;
        }

        if (nano_ui == NANO_UI_YESNO) {
            if (key == NANO_KEY_ESC) {
                nano_ui = NANO_UI_EDIT;
                nano_set_status("[ Cancelled ]");
                nano_draw();
                continue;
            }
            if (key == NANO_KEY_EXIT) {
                terminal_editor_end();
                return 0;
            }
            if (key == NANO_KEY_CHAR && (ch == 'y' || ch == 'Y')) {
                nano_save_file();
                terminal_editor_end();
                return 0;
            }
            if (key == NANO_KEY_CHAR && (ch == 'n' || ch == 'N')) {
                terminal_editor_end();
                return 0;
            }
            continue;
        }

        if (nano_ui == NANO_UI_PROMPT) {
            if (key == NANO_KEY_ESC || key == NANO_KEY_EXIT) {
                nano_ui = NANO_UI_EDIT;
                nano_prompt_kind = NANO_P_NONE;
                nano_set_status("[ Cancelled ]");
                nano_draw();
                continue;
            }
            if (key == NANO_KEY_CHAR && (ch == '\n' || ch == '\r')) {
                nano_prompt_submit();
                nano_draw();
                continue;
            }
            if (key == NANO_KEY_CHAR && ch == '\b') {
                if (nano_prompt_len > 0) nano_prompt[--nano_prompt_len] = 0;
                nano_draw();
                continue;
            }
            if (key == NANO_KEY_CHAR && ch >= 32 && ch < 127 && nano_prompt_len + 1 < NANO_PROMPT_SZ) {
                nano_prompt[nano_prompt_len++] = ch;
                nano_prompt[nano_prompt_len] = 0;
                nano_draw();
            }
            continue;
        }

        if (key == NANO_KEY_HELP) {
            nano_help = true;
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_REFRESH) {
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_TOGGLE_LNUM) {
            nano_show_lnums = !nano_show_lnums;
            nano_set_status(nano_show_lnums ? "[ Line numbers on ]" : "[ Line numbers off ]");
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_POS) {
            int line = nano_line_index(nano_cursor) + 1;
            char msg[32];
            msg[0] = 'L'; msg[1] = 'i'; msg[2] = 'n'; msg[3] = 'e'; msg[4] = ' ';
            msg[5] = (char)('0' + (line / 10) % 10);
            msg[6] = (char)('0' + line % 10);
            msg[7] = 0;
            nano_set_status(msg);
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_SAVE) {
            nano_begin_prompt(NANO_P_SAVE, "File Name to Write: ", nano_path);
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_EXIT) {
            if (nano_dirty) {
                nano_begin_yesno("Save modified buffer?  Y Yes  N No  Esc Cancel");
            } else {
                terminal_editor_end();
                return 0;
            }
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_CUT) {
            nano_cut_line();
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_UNCUT) {
            nano_paste();
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_SEARCH) {
            nano_begin_prompt(NANO_P_SEARCH, "Search: ", nano_search);
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_SEARCH_NEXT) {
            if (!nano_search_fwd(false)) nano_set_status("[ Not found ]");
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_GOTO) {
            nano_begin_prompt(NANO_P_GOTO, "Go To Line: ", "");
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_DEL) {
            nano_delete_forward();
            nano_draw();
            continue;
        }
        if (key == NANO_KEY_ESC) {
            if (nano_dirty) {
                nano_begin_yesno("Save modified buffer?  Y Yes  N No  Esc Cancel");
                nano_draw();
            } else {
                terminal_editor_end();
                return 0;
            }
            continue;
        }

        nano_handle_movement(key);

        if (key == NANO_KEY_CHAR) {
            if (ch == '\b') nano_delete_char();
            else if (ch == 127) nano_delete_forward();
            else if (ch == '\n' || ch == '\r') nano_insert_char('\n');
            else if (ch == '\t') {
                for (int t = 0; t < 4; t++) nano_insert_char(' ');
            } else if (ch >= 32 && ch < 127) nano_insert_char(ch);
        }

        nano_draw();
    }
}
