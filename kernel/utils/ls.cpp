#include "../utils.h"
#include "../fs.h"
#include "../drivers/video/terminal.h"
#include "../kernel.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

struct ls_entry {
    char name[FS_FILENAME_LEN + 2];
    bool is_dir;
};

static int ls_name_cmp(const struct ls_entry* a, const struct ls_entry* b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    const char* sa = a->name;
    const char* sb = b->name;
    for (;;) {
        char ca = *sa++;
        char cb = *sb++;
        if (ca > cb) return 1;
        if (ca < cb) return -1;
        if (ca == 0) return 0;
    }
}

static void ls_sort_entries(struct ls_entry* entries, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (ls_name_cmp(&entries[i], &entries[j]) > 0) {
                struct ls_entry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static size_t ls_display_width(const struct ls_entry* e) {
    size_t w = 0;
    while (e->name[w]) w++;
    if (e->is_dir) w++;
    return w;
}

static void ls_print_name(const struct ls_entry* e, bool show_long) {
    uint8_t old_color = terminal_getcolor();
    if (e->is_dir) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK));
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    }

    for (const char* q = e->name; *q; q++) {
        char str[2] = { *q, 0 };
        terminal_writestring(str);
    }
    if (e->is_dir && !show_long) {
        terminal_putchar('/');
    }

    if (show_long) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        terminal_writestring(e->is_dir ? "  <dir>" : "  <file>");
    }
    terminal_setcolor(old_color);
}

int cmd_ls(int argc, const char** argv) {
    bool show_long = false;
    bool show_all = false;
    const char* target_path = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            const char* opt = argv[i] + 1;
            while (*opt) {
                if (*opt == 'l') show_long = true;
                else if (*opt == 'a') show_all = true;
                opt++;
            }
        } else if (!target_path) {
            target_path = argv[i];
        }
    }

    char resolved[128];
    const char* list_path;
    if (target_path) {
        if (utils_resolve_path(target_path, resolved, sizeof(resolved)) != 0) {
            terminal_writestring("\nls: invalid path: ");
            terminal_writestring(target_path);
            terminal_writestring("\n");
            return -1;
        }
        list_path = resolved;
    } else {
        list_path = utils_get_current_directory();
    }

    char buffer[2048];
    if (fs_list_dir(list_path, buffer, sizeof(buffer)) < 0) {
        if (!fs_ready()) {
            terminal_writestring("\nls: filesystem not initialized\n");
        } else {
            terminal_writestring("\nls: cannot access '");
            terminal_writestring(target_path ? target_path : list_path);
            terminal_writestring("': No such file or directory\n");
        }
        return -1;
    }

    struct ls_entry entries[64];
    int entry_count = 0;
    const char* p = buffer;
    while (*p && entry_count < 64) {
        const char* name_start = p;
        bool is_dir = false;
        while (*p && *p != '\n') {
            if (*p == '/') is_dir = true;
            p++;
        }

        if (!show_all && name_start[0] == '.') {
            if (*p == '\n') p++;
            continue;
        }

        struct ls_entry* e = &entries[entry_count++];
        int ni = 0;
        for (const char* q = name_start; q < p && *q != '/' && ni < FS_FILENAME_LEN; q++) {
            e->name[ni++] = *q;
        }
        e->name[ni] = 0;
        e->is_dir = is_dir;
        if (*p == '\n') p++;
    }

    ls_sort_entries(entries, entry_count);

    terminal_writestring("\n");
    if (target_path) {
        terminal_writestring(list_path);
        terminal_writestring(":\n");
    }

    if (entry_count == 0) {
        terminal_writestring("(empty)\n");
        return 0;
    }

    if (show_long) {
        for (int i = 0; i < entry_count; i++) {
            ls_print_name(&entries[i], true);
            terminal_writestring("\n");
        }
        return 0;
    }

    size_t max_w = 0;
    for (int i = 0; i < entry_count; i++) {
        size_t w = ls_display_width(&entries[i]);
        if (w > max_w) max_w = w;
    }
    if (max_w < 4) max_w = 4;
    size_t col_w = max_w + 2;
    int ncols = (int)(78 / col_w);
    if (ncols < 1) ncols = 1;

    for (int i = 0; i < entry_count; i++) {
        ls_print_name(&entries[i], false);
        bool end_row = ((i + 1) % ncols == 0) || (i + 1 == entry_count);
        if (end_row) {
            terminal_writestring("\n");
        } else {
            size_t w = ls_display_width(&entries[i]);
            for (size_t s = w; s < col_w; s++) terminal_putchar(' ');
        }
    }
    return 0;
}
