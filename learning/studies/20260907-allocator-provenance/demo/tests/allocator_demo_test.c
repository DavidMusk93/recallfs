#include "allocator_demo.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void check(bool condition, const char *expression, const char *file,
                  int line)
{
    if (!condition) {
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

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
    CHECK(page_arena_init(&arena, 8192));
    CHECK(arena.stats.backing_mappings == 1);

    struct arena_allocation fresh =
        page_arena_alloc(&arena, 128, 64, ALLOC_ZEROED);
    CHECK(fresh.ptr != NULL);
    CHECK(fresh.zero_source == ZERO_PROVENANCE_FRESH_ANONYMOUS);
    CHECK(all_zero(fresh.ptr, fresh.size));
    CHECK(arena.stats.explicit_zero_bytes == 0);
    CHECK(arena.stats.elided_zero_bytes == 128);

    memset(fresh.ptr, 0xa5, fresh.size);
    page_arena_reset(&arena);

    struct arena_allocation reused =
        page_arena_alloc(&arena, 128, 64, ALLOC_ZEROED);
    CHECK(reused.ptr == fresh.ptr);
    CHECK(reused.zero_source == ZERO_PROVENANCE_EXPLICIT);
    CHECK(all_zero(reused.ptr, reused.size));
    CHECK(arena.stats.explicit_zero_bytes == 128);

    memset(reused.ptr, 0x5a, reused.size);
    page_arena_reset(&arena);
    CHECK(page_arena_reclaim(&arena));

    struct arena_allocation reclaimed =
        page_arena_alloc(&arena, 128, 64, ALLOC_ZEROED);
    CHECK(reclaimed.ptr == fresh.ptr);
    CHECK(reclaimed.zero_source == ZERO_PROVENANCE_OS_RECLAIMED);
    CHECK(all_zero(reclaimed.ptr, reclaimed.size));
    CHECK(arena.stats.explicit_zero_bytes == 128);
    CHECK(arena.stats.elided_zero_bytes == 256);
    printf("zeroing: explicit=%zu elided=%zu\n",
           arena.stats.explicit_zero_bytes,
           arena.stats.elided_zero_bytes);

    memset(reclaimed.ptr, 0x3c, reclaimed.size);
    page_arena_reset(&arena);
    struct arena_allocation reclaimed_reused =
        page_arena_alloc(&arena, 128, 64, ALLOC_ZEROED);
    CHECK(reclaimed_reused.ptr == reclaimed.ptr);
    CHECK(reclaimed_reused.zero_source == ZERO_PROVENANCE_EXPLICIT);
    CHECK(all_zero(reclaimed_reused.ptr, reclaimed_reused.size));
    CHECK(arena.stats.explicit_zero_bytes == 256);

    page_arena_reset(&arena);
    struct arena_allocation full_dirty =
        page_arena_alloc(&arena, arena.capacity, 1, ALLOC_UNINITIALIZED);
    CHECK(full_dirty.ptr == arena.base);
    CHECK(full_dirty.size == arena.capacity);
    memset(full_dirty.ptr, 0x6d, full_dirty.size);
    full_dirty.ptr[full_dirty.size - 1] = 0xe7;

    page_arena_reset(&arena);
    CHECK(page_arena_reclaim(&arena));
    struct arena_allocation full_reclaimed =
        page_arena_alloc(&arena, arena.capacity, 1, ALLOC_ZEROED);
    CHECK(full_reclaimed.ptr == arena.base);
    CHECK(full_reclaimed.size == arena.capacity);
    CHECK(full_reclaimed.zero_source == ZERO_PROVENANCE_OS_RECLAIMED);
    CHECK(all_zero(full_reclaimed.ptr, full_reclaimed.size));
    CHECK(full_reclaimed.ptr[full_reclaimed.size - 1] == 0);

    page_arena_destroy(&arena);

    struct page_arena tail_arena;
    CHECK(page_arena_init(&tail_arena, 4096));
    struct arena_allocation dirty_prefix =
        page_arena_alloc(&tail_arena, 128, 1, ALLOC_UNINITIALIZED);
    CHECK(dirty_prefix.ptr != NULL);
    memset(dirty_prefix.ptr, 0x91, dirty_prefix.size);

    page_arena_reset(&tail_arena);
    struct arena_allocation reused_prefix =
        page_arena_alloc(&tail_arena, 128, 1, ALLOC_UNINITIALIZED);
    CHECK(reused_prefix.ptr == dirty_prefix.ptr);
    CHECK(reused_prefix.zero_source == ZERO_PROVENANCE_UNKNOWN);
    memset(reused_prefix.ptr, 0x42, reused_prefix.size);

    struct arena_allocation fresh_tail =
        page_arena_alloc(&tail_arena, 128, 1, ALLOC_ZEROED);
    CHECK(fresh_tail.ptr == tail_arena.base + reused_prefix.size);
    CHECK(fresh_tail.zero_source == ZERO_PROVENANCE_FRESH_ANONYMOUS);
    CHECK(all_zero(fresh_tail.ptr, fresh_tail.size));

    page_arena_destroy(&tail_arena);
}

static void test_alignment_and_failure_are_explicit(void)
{
    struct page_arena overflow_arena = {0};
    CHECK(!page_arena_init(&overflow_arena, SIZE_MAX));
    CHECK(overflow_arena.base == NULL);

    struct page_arena arena;
    CHECK(page_arena_init(&arena, 4096));

    struct arena_allocation aligned =
        page_arena_alloc(&arena, 17, 64, ALLOC_UNINITIALIZED);
    CHECK(aligned.ptr != NULL);
    CHECK((uintptr_t)aligned.ptr % 64 == 0);
    memset(aligned.ptr, 0x7d, aligned.size);

    CHECK(!page_arena_reclaim(&arena));
    page_arena_reset(&arena);
    struct arena_allocation fallback =
        page_arena_alloc(&arena, 17, 64, ALLOC_ZEROED);
    CHECK(fallback.ptr == aligned.ptr);
    CHECK(fallback.zero_source == ZERO_PROVENANCE_EXPLICIT);
    CHECK(all_zero(fallback.ptr, fallback.size));

    size_t cursor = arena.cursor;
    struct arena_allocation bad_alignment =
        page_arena_alloc(&arena, 16, 3, ALLOC_UNINITIALIZED);
    CHECK(bad_alignment.ptr == NULL);
    CHECK(arena.cursor == cursor);

    struct arena_allocation too_large =
        page_arena_alloc(&arena, arena.capacity + 1, 8, ALLOC_ZEROED);
    CHECK(too_large.ptr == NULL);
    CHECK(arena.cursor == cursor);

    page_arena_destroy(&arena);
}

static void test_reclaim_failure_keeps_dirty_provenance(void)
{
    struct page_arena arena;
    CHECK(page_arena_init(&arena, 8192));

    struct arena_allocation dirty =
        page_arena_alloc(&arena, arena.capacity, 1, ALLOC_UNINITIALIZED);
    CHECK(dirty.ptr == arena.base);
    CHECK(dirty.size == arena.capacity);
    memset(dirty.ptr, 0x6d, dirty.size);

    page_arena_reset(&arena);
    CHECK(mlock(arena.base, arena.capacity) == 0);
    CHECK(!page_arena_reclaim(&arena));
    CHECK(munlock(arena.base, arena.capacity) == 0);

    struct arena_allocation fallback =
        page_arena_alloc(&arena, arena.capacity, 1, ALLOC_ZEROED);
    CHECK(fallback.ptr == arena.base);
    CHECK(fallback.size == arena.capacity);
    CHECK(fallback.zero_source == ZERO_PROVENANCE_EXPLICIT);
    CHECK(all_zero(fallback.ptr, fallback.size));

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
    CHECK(page_arena_init(&arena, 64 * 1024));

    uint64_t checksum = 0;
    for (size_t index = 0; index < 1000; ++index) {
        struct arena_allocation allocation =
            page_arena_alloc(&arena, sizeof(struct row), _Alignof(struct row),
                             ALLOC_UNINITIALIZED);
        CHECK(allocation.ptr != NULL);

        struct row *row = (struct row *)allocation.ptr;
        row->key = index;
        row->length = (uint32_t)(index % 257);
        row->kind = (uint8_t)(index % 7);
        checksum += row->key + row->length + row->kind;
    }

    CHECK(checksum == 627291);
    CHECK(arena.stats.backing_mappings == 1);
    CHECK(arena.stats.logical_allocations == 1000);
    CHECK(arena.stats.requested_bytes == 1000 * sizeof(struct row));
    CHECK(arena.stats.explicit_zero_bytes == 0);

    page_arena_destroy(&arena);
}

int main(void)
{
    test_zero_provenance();
    test_alignment_and_failure_are_explicit();
    test_reclaim_failure_keeps_dirty_provenance();
    test_batch_lifetime_uses_one_backing_mapping();
    puts("FIL-C allocator tests passed: 4 suites");
    return 0;
}
