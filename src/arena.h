#ifndef ARENA_H
#define ARENA_H
#include <stddef.h>
#include <stdint.h>

#define CACHE_LINE_SIZE 64

typedef struct {
    uint8_t *memory;
    size_t capacity;
    size_t offset;
} Arena;

void arena_init(Arena *arena, size_t capacity);
void arena_free(Arena *arena);
void *arena_alloc(Arena *arena, size_t size);
void *arena_alloc_aligned(Arena *arena, size_t size, size_t alignment);
void arena_reset(Arena *arena);
#endif
