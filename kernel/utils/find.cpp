#include "../utils.h"
#include "../fs.h"
#include "../vfs.h"
#include "../drivers/video/terminal.h"
#include "../kernel.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum find_type_filter {
    FIND_TYPE_ANY = 0,
    FIND_TYPE_FILE,
    FIND_TYPE_DIR
};

struct find_options {
    const char* name_pattern;
    bool has_name;
    enum find_type_filter type_filter;
};

static bool fnmatch(const char* pattern, const char* name) {
    if (!pattern || !name) return false;
    const char* p = pattern;
    const char* n = name;

    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return true;
            while (*n) {
                if (fnmatch(p, n)) return true;
                n++;
            }
            return fnmatch(p, n);
        }
        if (*p == '?') {
            if (!*n) return false;
            p++;
            n++;
            continue;
        }
        if (*p != *n) return false;
        p++;
        n++;
    }
    return *n == 0;
}

static const char* basename_of(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return base;
}

static void find_join_path(char* out, size_t out_sz, const char* dir, const char* name, size_t name_len) {
    size_t o = 0;
    if (dir[0] == '/' && dir[1] == 0) {
        if (o < out_sz - 1) out[o++] = '/';
    } else {
        for (size_t i = 0; dir[i] && o + 1 < out_sz; i++) out[o++] = dir[i];
        if (o > 0 && out[o - 1] != '/' && o + 1 < out_sz) out[o++] = '/';
    }
    for (size_t i = 0; i < name_len && o + 1 < out_sz; i++) out[o++] = name[i];
    out[o] = 0;
}

static bool find_type_matches(bool is_dir, enum find_type_filter filter) {
    if (filter == FIND_TYPE_ANY) return true;
    if (filter == FIND_TYPE_DIR) return is_dir;
    return !is_dir;
}

static void find_print_if_match(const char* path, bool is_dir, const struct find_options* opts) {
    if (!find_type_matches(is_dir, opts->type_filter)) return;
    if (opts->has_name && !fnmatch(opts->name_pattern, basename_of(path))) return;
    terminal_writestring(path);
    terminal_writestring("\n");
}

static void find_walk(const char* dir_path, const struct find_options* opts) {
    char list_buf[2048];
    if (vfs_list(dir_path, list_buf, sizeof(list_buf)) < 0) return;

    find_print_if_match(dir_path, true, opts);

    size_t lp = 0;
    while (list_buf[lp]) {
        size_t nl = 0;
        while (list_buf[lp + nl] && list_buf[lp + nl] != '\n') nl++;
        if (nl > 0) {
            bool is_dir = (list_buf[lp + nl - 1] == '/');
            size_t name_len = is_dir ? nl - 1 : nl;

            char child[128];
            find_join_path(child, sizeof(child), dir_path, list_buf + lp, name_len);

            if (!is_dir) {
                find_print_if_match(child, false, opts);
            } else {
                find_walk(child, opts);
            }
        }
        lp += nl;
        if (list_buf[lp] == '\n') lp++;
    }
}

static void find_usage(void) {
    terminal_writestring("\nUsage: find [path] [-name <pattern>] [-type f|d]");
    terminal_writestring("\n  find /etc -name resolv.conf");
    terminal_writestring("\n  find / -name \"*.conf\"");
    terminal_writestring("\n  find . -type f");
    terminal_writestring("\n  find . -type d");
    terminal_writestring("\nPatterns: * = any, ? = one character\n");
}

int cmd_find(int argc, const char** argv) {
    const char* start_path = 0;
    struct find_options opts;
    opts.name_pattern = 0;
    opts.has_name = false;
    opts.type_filter = FIND_TYPE_ANY;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == 'n' && argv[i][2] == 'a' && argv[i][3] == 'm' &&
                argv[i][4] == 'e' && argv[i][5] == 0) {
                if (i + 1 >= argc) {
                    find_usage();
                    return -1;
                }
                opts.name_pattern = argv[++i];
                opts.has_name = true;
            } else if (argv[i][1] == 't' && argv[i][2] == 'y' && argv[i][3] == 'p' &&
                       argv[i][4] == 'e' && argv[i][5] == 0) {
                if (i + 1 >= argc) {
                    find_usage();
                    return -1;
                }
                const char* t = argv[++i];
                if (t[0] == 'f' && t[1] == 0) {
                    opts.type_filter = FIND_TYPE_FILE;
                } else if (t[0] == 'd' && t[1] == 0) {
                    opts.type_filter = FIND_TYPE_DIR;
                } else {
                    terminal_writestring("\nfind: unknown -type argument\n");
                    return -1;
                }
            } else {
                terminal_writestring("\nfind: unknown predicate: ");
                terminal_writestring(argv[i]);
                terminal_writestring("\n");
                return -1;
            }
        } else {
            if (start_path) {
                find_usage();
                return -1;
            }
            start_path = argv[i];
        }
    }

    if (!start_path) start_path = ".";

    char resolved[128];
    if (utils_resolve_path(start_path, resolved, sizeof(resolved)) != 0) {
        terminal_writestring("\nfind: invalid path: ");
        terminal_writestring(start_path);
        terminal_writestring("\n");
        return -1;
    }

    char dir_test[4];
    if (vfs_list(resolved, dir_test, sizeof(dir_test)) < 0) {
        terminal_writestring("\nfind: '");
        terminal_writestring(start_path);
        terminal_writestring("': No such file or directory\n");
        return -1;
    }

    terminal_writestring("\n");
    find_walk(resolved, &opts);
    return 0;
}
