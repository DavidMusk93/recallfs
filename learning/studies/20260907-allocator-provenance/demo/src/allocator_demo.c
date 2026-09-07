#define _DEFAULT_SOURCE

#include "allocator_demo.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool round_up_size(size_t value, size_t alignment, size_t *result)
{
    size_t mask = alignment - 1;
    if (value > SIZE_MAX - mask) {
        return false;
    }
    *result = (value + mask) & ~mask;
    return true;
}

bool page_arena_init(struct page_arena *arena, size_t minimum_capacity)
{
    if (arena == NULL || minimum_capacity == 0) {
        return false;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || !is_power_of_two((size_t)page_size)) {
        return false;
    }

    size_t capacity;
    if (!round_up_size(minimum_capacity, (size_t)page_size, &capacity)) {
        return false;
    }

    void *mapping = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }

    *arena = (struct page_arena){
        .base = mapping,
        .capacity = capacity,
        .clean_source = ZERO_PROVENANCE_FRESH_ANONYMOUS,
        .stats = {
            .backing_mappings = 1,
        },
    };
    return true;
}

void page_arena_destroy(struct page_arena *arena)
{
    if (arena == NULL) {
        return;
    }
    if (arena->base != NULL) {
        (void)munmap(arena->base, arena->capacity);
    }
    *arena = (struct page_arena){0};
}

void page_arena_reset(struct page_arena *arena)
{
    if (arena != NULL) {
        arena->cursor = 0;
    }
}

bool page_arena_reclaim(struct page_arena *arena)
{
    if (arena == NULL || arena->base == NULL || arena->cursor != 0) {
        return false;
    }
    if (madvise(arena->base, arena->capacity, MADV_DONTNEED) != 0) {
        return false;
    }

    arena->reuse_limit = 0;
    arena->clean_source = ZERO_PROVENANCE_OS_RECLAIMED;
    return true;
}

struct arena_allocation page_arena_alloc(struct page_arena *arena,
                                         size_t size,
                                         size_t alignment,
                                         enum allocation_init init)
{
    struct arena_allocation result = {0};
    if (arena == NULL || arena->base == NULL || size == 0 ||
        !is_power_of_two(alignment)) {
        return result;
    }

    uintptr_t current = (uintptr_t)arena->base + arena->cursor;
    uintptr_t mask = alignment - 1;
    if (current > UINTPTR_MAX - mask) {
        return result;
    }
    uintptr_t aligned = (current + mask) & ~mask;
    size_t start = (size_t)(aligned - (uintptr_t)arena->base);
    if (start > arena->capacity || size > arena->capacity - start) {
        return result;
    }

    size_t previous_reuse_limit = arena->reuse_limit;
    size_t end = start + size;
    bool clean_tail =
        arena->clean_source == ZERO_PROVENANCE_FRESH_ANONYMOUS ||
        arena->clean_source == ZERO_PROVENANCE_OS_RECLAIMED;
    bool proven_zero = clean_tail && start >= previous_reuse_limit;

    result.ptr = arena->base + start;
    result.size = size;
    result.zero_source =
        proven_zero ? arena->clean_source : ZERO_PROVENANCE_UNKNOWN;

    if (init == ALLOC_ZEROED) {
        if (proven_zero) {
            arena->stats.elided_zero_bytes += size;
        } else {
            memset(result.ptr, 0, size);
            result.zero_source = ZERO_PROVENANCE_EXPLICIT;
            arena->stats.explicit_zero_bytes += size;
        }
    }

    arena->cursor = end;
    if (end > arena->reuse_limit) {
        arena->reuse_limit = end;
    }
    arena->stats.logical_allocations++;
    arena->stats.requested_bytes += size;
    return result;
}
