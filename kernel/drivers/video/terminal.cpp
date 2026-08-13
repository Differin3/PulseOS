#include "terminal.h"
#include "fb.h"
#include "driver_manager.h"
#include "serial_log.h"
#include <stddef.h>
#include <stdint.h>

static volatile uint8_t* const VGA_MEMORY_PTR = (volatile uint8_t*)VGA_MEMORY;

static size_t terminal_row = 0;
static size_t terminal_column = 0;
static uint8_t terminal_color = 0x07;
static size_t term_cols = VGA_WIDTH;
static size_t term_rows = VGA_HEIGHT;
static size_t content_rows = VGA_HEIGHT - 1;
static bool use_fb = false;
static bool status_enabled = true;
static char status_text[128];
static uint8_t status_color = 0x1F; /* white on blue */

#define TERM_MAX_COLS 128
#define TERM_MAX_ROWS 48
#define SCROLLBACK_SIZE 200

static uint16_t cells[TERM_MAX_ROWS][TERM_MAX_COLS];
static uint16_t history_lines[SCROLLBACK_SIZE * TERM_MAX_COLS];
static size_t history_count = 0;
static size_t view_first_line = 0;
static bool scrollback_mode = false;
static bool editor_mode = false;

static struct {
    size_t row, col, view_first;
    bool scrollback;
} editor_saved;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void vga_hw_cursor(size_t row, size_t col) {
    if (use_fb) return;
    if (row >= 25) row = 24;
    if (col >= 80) col = 79;
    uint16_t pos = (uint16_t)(row * 80 + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void backend_draw_cell(size_t row, size_t col, uint16_t entry) {
    char c = (char)(entry & 0xFF);
    uint8_t attr = (uint8_t)(entry >> 8);
    if (use_fb) {
        fb_draw_glyph(col, row, c, attr);
        return;
    }
    if (row >= 25 || col >= 80) return;
    size_t idx = row * 80 + col;
    VGA_MEMORY_PTR[idx * 2] = (uint8_t)c;
    VGA_MEMORY_PTR[idx * 2 + 1] = attr;
}

static void paint_cell(size_t row, size_t col) {
    if (row >= term_rows || col >= term_cols) return;
    backend_draw_cell(row, col, cells[row][col]);
}

static void paint_row(size_t row) {
    for (size_t col = 0; col < term_cols; col++) paint_cell(row, col);
}

static void set_cell(size_t row, size_t col, char c, uint8_t color) {
    if (row >= term_rows || col >= term_cols) return;
    cells[row][col] = (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
    paint_cell(row, col);
}

static void terminal_update_cursor() {
    size_t max_r = editor_mode ? term_rows : content_rows;
    if (max_r == 0) max_r = 1;
    if (terminal_row >= max_r) terminal_row = max_r - 1;
    if (terminal_column >= term_cols) terminal_column = term_cols > 0 ? term_cols - 1 : 0;
    vga_hw_cursor(terminal_row, terminal_column);
}

static void history_push_row_cells(size_t row) {
    if (row >= term_rows) return;
    if (history_count >= SCROLLBACK_SIZE) {
        for (size_t i = 0; i < (SCROLLBACK_SIZE - 1) * TERM_MAX_COLS; i++)
            history_lines[i] = history_lines[i + TERM_MAX_COLS];
        history_count = SCROLLBACK_SIZE - 1;
    }
    size_t dst = history_count * TERM_MAX_COLS;
    for (size_t col = 0; col < TERM_MAX_COLS; col++) {
        history_lines[dst + col] = (col < term_cols) ? cells[row][col]
            : ((uint16_t)' ' | ((uint16_t)terminal_color << 8));
    }
    history_count++;
}

static void terminal_status_paint(void) {
    if (!status_enabled || editor_mode) return;
    size_t sr = term_rows - 1;
    for (size_t col = 0; col < term_cols; col++) {
        char ch = ' ';
        if (col < sizeof(status_text) - 1 && status_text[col]) ch = status_text[col];
        if (scrollback_mode && col + 6 < term_cols && col >= term_cols - 6) {
            const char* ind = "SCROLL";
            size_t i = col - (term_cols - 6);
            if (i < 6) ch = ind[i];
        }
        set_cell(sr, col, ch, status_color);
    }
}

void terminal_status_set(const char* text) {
    size_t i = 0;
    if (!text) text = "";
    while (text[i] && i + 1 < sizeof(status_text)) {
        status_text[i] = text[i];
        i++;
    }
    status_text[i] = 0;
    terminal_status_paint();
}

void terminal_status_redraw(void) {
    terminal_status_paint();
}

static void terminal_scroll_content(size_t lines) {
    if (lines == 0 || content_rows == 0) return;
    for (size_t n = 0; n < lines; n++) {
        history_push_row_cells(0);
        for (size_t row = 0; row + 1 < content_rows; row++) {
            for (size_t col = 0; col < term_cols; col++)
                cells[row][col] = cells[row + 1][col];
            paint_row(row);
        }
        size_t last = content_rows - 1;
        for (size_t col = 0; col < term_cols; col++)
            set_cell(last, col, ' ', terminal_color);
    }
    terminal_status_paint();
}

static void terminal_render_view(size_t first_line) {
    size_t view_h = content_rows;
    size_t total = history_count + content_rows;
    if (first_line + view_h > total) {
        first_line = (total > view_h) ? total - view_h : 0;
    }
    view_first_line = first_line;
    scrollback_mode = (first_line < history_count);

    for (size_t row = 0; row < view_h; row++) {
        size_t line_idx = first_line + row;
        for (size_t col = 0; col < term_cols; col++) {
            uint16_t entry;
            if (line_idx < history_count) {
                entry = history_lines[line_idx * TERM_MAX_COLS + col];
            } else {
                size_t cr = line_idx - history_count;
                entry = (cr < content_rows) ? cells[cr][col]
                    : ((uint16_t)' ' | ((uint16_t)terminal_color << 8));
            }
            backend_draw_cell(row, col, entry);
        }
    }
    terminal_status_paint();
    terminal_row = content_rows > 0 ? content_rows - 1 : 0;
    terminal_column = 0;
    terminal_update_cursor();
}

static void terminal_exit_scrollback() {
    if (!scrollback_mode) return;
    scrollback_mode = false;
    view_first_line = history_count;
    for (size_t row = 0; row < content_rows; row++) paint_row(row);
    terminal_status_paint();
}

void terminal_scroll_up() {
    if (history_count == 0) return;
    size_t first = scrollback_mode ? view_first_line : history_count;
    if (first == 0) return;
    terminal_render_view(first - 1);
}

void terminal_scroll_down() {
    if (!scrollback_mode) return;
    if (view_first_line >= history_count) {
        terminal_exit_scrollback();
        return;
    }
    terminal_render_view(view_first_line + 1);
}

void terminal_scroll_page_up() {
    size_t first = scrollback_mode ? view_first_line : history_count;
    if (first == 0) return;
    size_t step = content_rows > 1 ? content_rows - 1 : 1;
    terminal_render_view(first > step ? first - step : 0);
}

void terminal_scroll_page_down() {
    if (!scrollback_mode) return;
    size_t step = content_rows > 1 ? content_rows - 1 : 1;
    if (view_first_line + step < history_count)
        terminal_render_view(view_first_line + step);
    else
        terminal_exit_scrollback();
}

bool terminal_in_scrollback() { return scrollback_mode; }

void terminal_set_mode(size_t width, size_t height) {
    (void)width;
    (void)height;
    if (!use_fb) {
        term_cols = 80;
        term_rows = 25;
        content_rows = status_enabled ? 24 : 25;
    }
    terminal_clear_viewport();
}

static int terminal_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    char* buf = (char*)buffer;
    size_t read = 0;
    size_t row = offset / term_cols;
    size_t col = offset % term_cols;
    for (size_t i = 0; i < size && row < term_rows; i++) {
        buf[read++] = (char)(cells[row][col] & 0xFF);
        col++;
        if (col >= term_cols) { col = 0; row++; }
    }
    return (int)read;
}

static int terminal_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    (void)offset;
    const char* buf = (const char*)buffer;
    for (size_t i = 0; i < size; i++) terminal_putchar(buf[i]);
    return (int)size;
}

static int terminal_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data;
    if (cmd == 0 && arg) { terminal_setcolor(*(uint8_t*)arg); return 0; }
    if (cmd == 1 && arg) {
        struct { size_t row; size_t col; }* pos = (typeof(pos))arg;
        terminal_set_cursor(pos->row, pos->col);
        return 0;
    }
    if (cmd == 2 && arg) {
        struct { size_t row; size_t col; }* pos = (typeof(pos))arg;
        pos->row = terminal_row;
        pos->col = terminal_column;
        return 0;
    }
    if (cmd == 3 && arg) {
        struct { size_t width; size_t height; }* size = (typeof(size))arg;
        size->width = term_cols;
        size->height = term_rows;
        return 0;
    }
    return -1;
}

void terminal_initialize() {
    use_fb = false;
    term_cols = 80;
    term_rows = 25;
    content_rows = 24;
    status_enabled = true;
    status_text[0] = 0;
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = 0x07;
    history_count = 0;
    view_first_line = 0;
    scrollback_mode = false;
    editor_mode = false;

    for (size_t r = 0; r < TERM_MAX_ROWS; r++)
        for (size_t c = 0; c < TERM_MAX_COLS; c++)
            cells[r][c] = (uint16_t)' ' | 0x0700;

    for (size_t i = 0; i < 80 * 25; i++) {
        VGA_MEMORY_PTR[i * 2] = 0x20;
        VGA_MEMORY_PTR[i * 2 + 1] = terminal_color;
    }
    terminal_status_set("KnitOS");
    terminal_update_cursor();

    struct driver terminal_driver;
    terminal_driver.name[0] = 't';
    terminal_driver.name[1] = 't';
    terminal_driver.name[2] = 'y';
    terminal_driver.name[3] = '0';
    terminal_driver.name[4] = 0;
    terminal_driver.type = DRIVER_VIDEO;
    terminal_driver.device_id = 0;
    terminal_driver.device_data = 0;
    terminal_driver.initialized = true;
    terminal_driver.active = true;
    terminal_driver.ops.init = 0;
    terminal_driver.ops.read = terminal_driver_read;
    terminal_driver.ops.write = terminal_driver_write;
    terminal_driver.ops.ioctl = terminal_driver_ioctl;
    terminal_driver.ops.cleanup = 0;
    driver_register(&terminal_driver);
}

void terminal_init_graphics(uint32_t multiboot_info) {
    if (!fb_init_from_multiboot(multiboot_info)) {
        log_msg(LOG_INFO, "fb", "no framebuffer — VGA text");
        return;
    }
    const struct fb_info* fi = fb_get_info();
    size_t cols = fi->width / FB_FONT_W;
    size_t rows = fi->height / FB_FONT_H;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols < 40) cols = 40;
    if (rows < 10) rows = 10;

    use_fb = true;
    term_cols = cols;
    term_rows = rows;
    content_rows = rows > 1 ? rows - 1 : rows;
    fb_clear(0x000000);
    for (size_t r = 0; r < term_rows; r++)
        for (size_t c = 0; c < term_cols; c++)
            set_cell(r, c, ' ', terminal_color);
    terminal_row = 0;
    terminal_column = 0;
    terminal_status_set("KnitOS fb");
    log_fmt3(LOG_INFO, "fb", "console", "cols", (uint32_t)term_cols,
             "rows", (uint32_t)term_rows, "ok", 1u);
}

void terminal_clear_viewport(void) {
    if (scrollback_mode) terminal_exit_scrollback();
    size_t lim = editor_mode ? term_rows : content_rows;
    for (size_t r = 0; r < lim; r++)
        for (size_t c = 0; c < term_cols; c++)
            set_cell(r, c, ' ', terminal_color);
    terminal_row = 0;
    terminal_column = 0;
    terminal_status_paint();
    terminal_update_cursor();
}

void terminal_put_at(size_t row, size_t col, char c, uint8_t color) {
    set_cell(row, col, c, color);
}

void terminal_write_at(size_t row, size_t col, const char* s, uint8_t color) {
    if (!s) return;
    while (*s && col < term_cols) set_cell(row, col++, *s++, color);
}

bool terminal_using_framebuffer(void) { return use_fb; }
uint32_t terminal_fb_width(void) { return fb_active() ? fb_get_info()->width : 80; }
uint32_t terminal_fb_height(void) { return fb_active() ? fb_get_info()->height : 25; }
uint32_t terminal_fb_bpp(void) { return fb_active() ? fb_get_info()->bpp : 4; }

int terminal_read_cell(size_t row, size_t col) {
    if (row >= term_rows || col >= term_cols) return -1;
    return (int)cells[row][col];
}

void terminal_setcolor(uint8_t color) { terminal_color = color; }
uint8_t terminal_getcolor() { return terminal_color; }

void terminal_putchar(char c) {
    if (scrollback_mode) terminal_exit_scrollback();
    size_t max_r = editor_mode ? term_rows : content_rows;
    if (max_r == 0) max_r = 1;

    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
        if (terminal_row >= max_r) {
            if (!editor_mode) terminal_scroll_content(terminal_row - max_r + 1);
            terminal_row = max_r - 1;
        }
    } else if (c == '\b') {
        if (terminal_column > 0) terminal_column--;
        else if (terminal_row > 0) {
            terminal_row--;
            terminal_column = term_cols - 1;
        }
        set_cell(terminal_row, terminal_column, ' ', terminal_color);
    } else {
        set_cell(terminal_row, terminal_column, c, terminal_color);
        terminal_column++;
        if (terminal_column >= term_cols) {
            terminal_column = 0;
            terminal_row++;
        }
        if (terminal_row >= max_r) {
            if (!editor_mode) terminal_scroll_content(1);
            terminal_row = max_r - 1;
        }
    }
    terminal_update_cursor();
    log_mirror_char(c);
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++) terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    while (*data) terminal_putchar(*data++);
}

void terminal_set_cursor(size_t row, size_t col) {
    terminal_row = row;
    terminal_column = col;
    terminal_update_cursor();
}

size_t terminal_get_row() { return terminal_row; }
size_t terminal_get_column() { return terminal_column; }
size_t terminal_get_height() { return term_rows; }
size_t terminal_get_width() { return term_cols; }
size_t terminal_content_height() { return content_rows; }
void terminal_set_row(size_t row) { terminal_row = row; terminal_update_cursor(); }
void terminal_set_column(size_t col) { terminal_column = col; terminal_update_cursor(); }

void terminal_editor_begin() {
    editor_saved.row = terminal_row;
    editor_saved.col = terminal_column;
    editor_saved.view_first = view_first_line;
    editor_saved.scrollback = scrollback_mode;
    editor_mode = true;
    scrollback_mode = false;
    status_enabled = false;
    for (size_t r = 0; r < term_rows; r++)
        for (size_t c = 0; c < term_cols; c++)
            set_cell(r, c, ' ', terminal_color);
    terminal_row = 0;
    terminal_column = 0;
    terminal_update_cursor();
}

void terminal_editor_end() {
    editor_mode = false;
    status_enabled = true;
    scrollback_mode = editor_saved.scrollback;
    view_first_line = editor_saved.view_first;
    if (editor_saved.scrollback) terminal_render_view(view_first_line);
    else {
        for (size_t r = 0; r < content_rows; r++) paint_row(r);
        terminal_status_paint();
    }
    terminal_row = editor_saved.row;
    terminal_column = editor_saved.col;
    terminal_update_cursor();
}

void terminal_editor_putat(size_t row, size_t col, char c, uint8_t color) {
    set_cell(row, col, c, color);
}

void terminal_editor_print_at(size_t row, size_t col, const char* s, uint8_t color) {
    terminal_write_at(row, col, s, color);
}

void terminal_editor_clear_row(size_t row, uint8_t color) {
    for (size_t col = 0; col < term_cols; col++) set_cell(row, col, ' ', color);
}

void terminal_editor_show_cursor(size_t row, size_t col) {
    terminal_row = row;
    terminal_column = col;
    terminal_update_cursor();
}
