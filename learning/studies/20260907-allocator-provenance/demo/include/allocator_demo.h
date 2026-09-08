#ifndef ALLOCATOR_DEMO_H
#define ALLOCATOR_DEMO_H

#include <stdbool.h>
#include <stddef.h>

enum allocation_init {
    ALLOC_UNINITIALIZED,
    ALLOC_ZEROED,
};

enum zero_provenance {
    ZERO_PROVENANCE_UNKNOWN,
    ZERO_PROVENANCE_FRESH_ANONYMOUS,
    ZERO_PROVENANCE_EXPLICIT,
    ZERO_PROVENANCE_OS_RECLAIMED,
};

struct arena_allocation {
    unsigned char *ptr;
    size_t size;
    enum zero_provenance zero_source;
};

struct arena_stats {
    size_t backing_mappings;
    size_t logical_allocations;
    size_t requested_bytes;
    size_t explicit_zero_bytes;
    size_t elided_zero_bytes;
};

/* Study-only representation for white-box tests; use an opaque handle in production. */
struct page_arena {
    unsigned char *base;
    size_t capacity;
    size_t cursor;
    size_t reuse_limit;
    enum zero_provenance clean_source;
    struct arena_stats stats;
};

bool page_arena_init(struct page_arena *arena, size_t minimum_capacity);
void page_arena_destroy(struct page_arena *arena);
void page_arena_reset(struct page_arena *arena);
bool page_arena_reclaim(struct page_arena *arena);

struct arena_allocation page_arena_alloc(struct page_arena *arena,
                                         size_t size,
                                         size_t alignment,
                                         enum allocation_init init);

#endif
