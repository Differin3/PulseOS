#include <stddef.h>
#include <stdint.h>

extern "C" {

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dest;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

void* memset(void* dest, int value, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    uint8_t v = (uint8_t)value;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dest;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    if (!dest || !src) return dest;
    while ((*d++ = *src++) != 0) {}
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    if (!dest) return dest;
    if (!src) {
        if (n) dest[0] = 0;
        return dest;
    }
    for (; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = 0;
    return dest;
}

char* strcat(char* dest, const char* src) {
    if (!dest || !src) return dest;
    char* d = dest + strlen(dest);
    while ((*d++ = *src++) != 0) {}
    return dest;
}

char* strchr(const char* s, int c) {
    if (!s) return 0;
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char*)s;
        s++;
    }
    if (ch == 0) return (char*)s;
    return 0;
}

char* strrchr(const char* s, int c) {
    if (!s) return 0;
    char ch = (char)c;
    const char* last = 0;
    while (*s) {
        if (*s == ch) last = s;
        s++;
    }
    if (ch == 0) return (char*)s;
    return (char*)last;
}

static char* strtok_save = 0;

char* strtok(char* str, const char* delim) {
    if (!delim) return 0;
    char* s = str ? str : strtok_save;
    if (!s) return 0;

    // skip leading delimiters
    while (*s) {
        const char* d = delim;
        int is_delim = 0;
        while (*d) {
            if (*s == *d) { is_delim = 1; break; }
            d++;
        }
        if (!is_delim) break;
        s++;
    }
    if (*s == 0) {
        strtok_save = 0;
        return 0;
    }

    char* token = s;
    while (*s) {
        const char* d = delim;
        int is_delim = 0;
        while (*d) {
            if (*s == *d) { is_delim = 1; break; }
            d++;
        }
        if (is_delim) {
            *s = 0;
            strtok_save = s + 1;
            return token;
        }
        s++;
    }
    strtok_save = 0;
    return token;
}

}
