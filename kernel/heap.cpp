#include "heap.h"
#include <stdint.h>

// Simple freestanding heap (first-fit freelist) for FS and small kernel allocs.
#define HEAP_SIZE (512u * 1024u)
#define HEAP_ALIGN 8u

struct heap_block {
    size_t size; // payload bytes
    uint8_t free;
    heap_block* next;
};

static uint8_t heap_arena[HEAP_SIZE] __attribute__((aligned(8)));
static heap_block* heap_head = 0;
static bool heap_ready = false;

static size_t align_up(size_t n) {
    return (n + (HEAP_ALIGN - 1u)) & ~(size_t)(HEAP_ALIGN - 1u);
}

static void heap_init(void) {
    if (heap_ready) return;
    heap_head = (heap_block*)heap_arena;
    heap_head->size = HEAP_SIZE - sizeof(heap_block);
    heap_head->free = 1;
    heap_head->next = 0;
    heap_ready = true;
}

static void heap_coalesce(void) {
    heap_block* cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(heap_block) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

extern "C" void* malloc(size_t size) {
    if (size == 0) return 0;
    heap_init();
    size = align_up(size);

    heap_block* cur = heap_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            size_t remain = cur->size - size;
            if (remain > sizeof(heap_block) + HEAP_ALIGN) {
                uint8_t* split_addr = (uint8_t*)cur + sizeof(heap_block) + size;
                heap_block* split = (heap_block*)split_addr;
                split->size = remain - sizeof(heap_block);
                split->free = 1;
                split->next = cur->next;
                cur->next = split;
                cur->size = size;
            }
            cur->free = 0;
            return (uint8_t*)cur + sizeof(heap_block);
        }
        cur = cur->next;
    }
    return 0;
}

extern "C" void free(void* ptr) {
    if (!ptr) return;
    heap_init();
    heap_block* blk = (heap_block*)((uint8_t*)ptr - sizeof(heap_block));
    if ((uint8_t*)blk < heap_arena || (uint8_t*)blk >= heap_arena + HEAP_SIZE) return;
    blk->free = 1;
    heap_coalesce();
}

extern "C" void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (!p) return 0;
    uint8_t* b = (uint8_t*)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}

extern "C" void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return 0;
    }
    heap_block* blk = (heap_block*)((uint8_t*)ptr - sizeof(heap_block));
    if (blk->size >= size) return ptr;
    void* n = malloc(size);
    if (!n) return 0;
    uint8_t* dst = (uint8_t*)n;
    uint8_t* src = (uint8_t*)ptr;
    for (size_t i = 0; i < blk->size && i < size; i++) dst[i] = src[i];
    free(ptr);
    return n;
}
