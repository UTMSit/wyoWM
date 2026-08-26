#include "arena.h"
#include <stdlib.h>
#include <string.h>

void arena_init(Arena *arena, size_t capacity) {
    arena->memory = aligned_alloc(CACHE_LINE_SIZE, capacity);
    arena->capacity = capacity;
    arena->offset = 0;
}

void arena_free(Arena *arena) {
    free(arena->memory);
    arena->memory = NULL;
    arena->capacity = 0;
    arena->offset = 0;
}

void *arena_alloc(Arena *arena, size_t size) {
    return arena_alloc_aligned(arena, size, CACHE_LINE_SIZE);
}

void *arena_alloc_aligned(Arena *arena, size_t size, size_t alignment) {
    size_t current = (size_t)arena->memory + arena->offset;
    size_t aligned_current = (current + alignment - 1) & ~(alignment - 1);
    size_t padding = aligned_current - current;
    if (arena->offset + padding + size > arena->capacity) {
        return NULL;
    }
    arena->offset += padding;
    void *ptr = arena->memory + arena->offset;
    arena->offset += size;
    return ptr;
}

void arena_reset(Arena *arena) {
    arena->offset = 0;
}
