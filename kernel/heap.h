#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t nmemb, size_t size);

#ifdef __cplusplus
}
#endif

#endif
