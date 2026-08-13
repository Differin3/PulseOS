#include "utils.h"
#include "sched/task.h"
#include <stdint.h>
#include <stddef.h>

// Объявление функций утилит
extern int cmd_ls(int argc, const char** argv);
extern int cmd_find(int argc, const char** argv);

#define MAX_BUILTIN_COMMANDS 32

static struct builtin_command builtin_commands[MAX_BUILTIN_COMMANDS];
static int builtin_count = 0;

// Регистрация встроенной команды
void register_builtin(const char* name, int (*handler)(int argc, const char** argv), const char* description) {
    if (builtin_count >= MAX_BUILTIN_COMMANDS) return;
    
    builtin_commands[builtin_count].name = name;
    builtin_commands[builtin_count].handler = handler;
    builtin_commands[builtin_count].description = description;
    builtin_count++;
}

// Найти встроенную команду
const struct builtin_command* find_builtin(const char* name) {
    for (int i = 0; i < builtin_count; i++) {
        const char* cmd_name = builtin_commands[i].name;
        int j = 0;
        while (name[j] && cmd_name[j] && name[j] == cmd_name[j]) j++;
        if (!name[j] && !cmd_name[j]) {
            return &builtin_commands[i];
        }
    }
    return 0;
}

// Выполнить встроенную команду
int execute_builtin(const char* name, int argc, const char** argv) {
    const struct builtin_command* cmd = find_builtin(name);
    if (!cmd || !cmd->handler) return -1;
    return cmd->handler(argc, argv);
}

/* Fallback before sched_init */
static char g_cwd_fallback[128] = "/";

const char* utils_get_current_directory() {
    if (sched_ready()) return task_getcwd();
    return g_cwd_fallback;
}

void utils_set_current_directory(const char* path) {
    if (sched_ready()) {
        task_chdir(path);
        return;
    }
    int i = 0;
    while (path && path[i] && i < 127) {
        g_cwd_fallback[i] = path[i];
        i++;
    }
    g_cwd_fallback[i] = 0;
}

int utils_resolve_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size < 2) return -1;

    if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && i + 1 < out_size) {
            out[i] = path[i];
            i++;
        }
        out[i] = 0;
        return 0;
    }

    const char* cwd = utils_get_current_directory();
    size_t p = 0;
    while (cwd[p] && p + 1 < out_size) {
        out[p] = cwd[p];
        p++;
    }
    if (p > 0 && out[p - 1] != '/' && p + 1 < out_size) {
        out[p++] = '/';
    }
    size_t i = 0;
    while (path[i] && p + 1 < out_size) {
        out[p++] = path[i++];
    }
    out[p] = 0;
    return 0;
}

void utils_init() {
    builtin_count = 0;
    g_cwd_fallback[0] = '/';
    g_cwd_fallback[1] = 0;
    
    register_builtin("ls", cmd_ls, "List directory contents");
    register_builtin("find", cmd_find, "Search for files in directory tree");
}
