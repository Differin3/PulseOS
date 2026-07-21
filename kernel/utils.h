#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

// Структура встроенной команды
struct builtin_command {
    const char* name;
    int (*handler)(int argc, const char** argv);
    const char* description;
};

// Регистрация встроенной команды
void register_builtin(const char* name, int (*handler)(int argc, const char** argv), const char* description);

// Найти встроенную команду
const struct builtin_command* find_builtin(const char* name);

// Выполнить встроенную команду
int execute_builtin(const char* name, int argc, const char** argv);

// Инициализация системы утилит
void utils_init();

// Получить текущую директорию
const char* utils_get_current_directory();

// Установить текущую директорию
void utils_set_current_directory(const char* path);

// Собрать абсолютный путь (cwd + относительный или /path)
int utils_resolve_path(const char* path, char* out, size_t out_size);

#endif

