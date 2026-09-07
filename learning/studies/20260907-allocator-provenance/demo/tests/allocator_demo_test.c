#include "allocator_demo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool all_zero(const unsigned char *bytes, size_t size)
{
    for (size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

static void test_zero_provenance(void)
{
    struct page_arena arena;
    assert(page_arena_init(&arena, 8192));
    assert(arena.stats.backing_mappings == 1);

    struct arena_allocation fresh =
        page_arena_alloc(&arena, 128, 64, ALLOC_ZEROED);
    assert(fresh.ptr != NULL);
    assert(fresh.zero_source == ZERO_PROVENANCE_FRESH_ANONYMOUS);
    assert(all_zero(fresh.ptr, fresh.size));
    assert(arena.stats.explicit_zero_bytes == 0);
    assert(arena.stats.elided_zero_bytes == 128);

    memset(fresh.ptr, 0xa5, fresh.size);
    page_arena_reset(&arena);

    struct arena_allocation reused =
        page_arena_alloc(&arena, 128, 64, ALLOC_ZEROED);
    assert(reused.ptr == fresh.ptr);
    assert(reused.zero_source == ZERO_PROVENANCE_EXPLICIT);
    assert(all_zero(reused.ptr, reused.size));
    assert(arena.stats.explicit_zero_bytes == 128);

    memset(reused.ptr, 0x5a, reused.size);
    page_arena_reset(&arena);
    assert(page_arena_reclaim(&arena));

    struct arena_allocation reclaimed =
        page_arena_alloc(&arena, 128, 64, ALLOC_ZEROED);
    assert(reclaimed.ptr == fresh.ptr);
    assert(reclaimed.zero_source == ZERO_PROVENANCE_OS_RECLAIMED);
    assert(all_zero(reclaimed.ptr, reclaimed.size));
    assert(arena.stats.explicit_zero_bytes == 128);
    assert(arena.stats.elided_zero_bytes == 256);
    printf("zeroing: explicit=%zu elided=%zu\n",
           arena.stats.explicit_zero_bytes,
           arena.stats.elided_zero_bytes);

    page_arena_destroy(&arena);
}

static void test_alignment_and_failure_are_explicit(void)
{
    struct page_arena arena;
    assert(page_arena_init(&arena, 4096));

    struct arena_allocation aligned =
        page_arena_alloc(&arena, 17, 64, ALLOC_UNINITIALIZED);
    assert(aligned.ptr != NULL);
    assert((uintptr_t)aligned.ptr % 64 == 0);
    memset(aligned.ptr, 0x7d, aligned.size);

    assert(!page_arena_reclaim(&arena));
    page_arena_reset(&arena);
    struct arena_allocation fallback =
        page_arena_alloc(&arena, 17, 64, ALLOC_ZEROED);
    assert(fallback.ptr == aligned.ptr);
    assert(fallback.zero_source == ZERO_PROVENANCE_EXPLICIT);
    assert(all_zero(fallback.ptr, fallback.size));

    size_t cursor = arena.cursor;
    struct arena_allocation bad_alignment =
        page_arena_alloc(&arena, 16, 3, ALLOC_UNINITIALIZED);
    assert(bad_alignment.ptr == NULL);
    assert(arena.cursor == cursor);

    struct arena_allocation too_large =
        page_arena_alloc(&arena, arena.capacity + 1, 8, ALLOC_ZEROED);
    assert(too_large.ptr == NULL);
    assert(arena.cursor == cursor);

    page_arena_destroy(&arena);
}

struct row {
    uint64_t key;
    uint32_t length;
    uint8_t kind;
};

static void test_batch_lifetime_uses_one_backing_mapping(void)
{
    struct page_arena arena;
    assert(page_arena_init(&arena, 64 * 1024));

    uint64_t checksum = 0;
    for (size_t index = 0; index < 1000; ++index) {
        struct arena_allocation allocation =
            page_arena_alloc(&arena, sizeof(struct row), _Alignof(struct row),
                             ALLOC_UNINITIALIZED);
        assert(allocation.ptr != NULL);

        struct row *row = (struct row *)allocation.ptr;
        row->key = index;
        row->length = (uint32_t)(index % 257);
        row->kind = (uint8_t)(index % 7);
        checksum += row->key + row->length + row->kind;
    }

    assert(checksum == 627291);
    assert(arena.stats.backing_mappings == 1);
    assert(arena.stats.logical_allocations == 1000);
    assert(arena.stats.explicit_zero_bytes == 0);

    page_arena_destroy(&arena);
}

int main(void)
{
    test_zero_provenance();
    test_alignment_and_failure_are_explicit();
    test_batch_lifetime_uses_one_backing_mapping();
    puts("FIL-C allocator tests passed: 3 suites");
    return 0;
}
