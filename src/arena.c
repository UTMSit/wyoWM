#include "arena.h"
#include <stdlib.h>
#include <string.h>

void arena_init(Arena *arena, size_t capacity) {
    size_t aligned_capacity = (capacity + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    arena->memory = aligned_alloc(CACHE_LINE_SIZE, aligned_capacity);
    arena->capacity = arena->memory ? aligned_capacity : 0;
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
    if (!arena || !arena->memory || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return NULL;
    }

    uintptr_t current = (uintptr_t)arena->memory + arena->offset;
    uintptr_t aligned_current = (current + alignment - 1) & ~(alignment - 1);
    size_t padding = (size_t)(aligned_current - current);

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
