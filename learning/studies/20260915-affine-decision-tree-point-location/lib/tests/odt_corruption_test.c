#include "odt_persistence_test_support.h"
#include "odt_test_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef void (*mutation_fn)(unsigned char *bytes, size_t size);

static void expect_status(const unsigned char *bytes, size_t size, const odt_load_limits *limits,
                          const odt_allocator *allocator, odt_status expected) {
    odt_test_memory_source source;
    odt_generation *generation = (odt_generation *)(uintptr_t)1u;

    memset(&source, 0, sizeof(source));
    source.data = bytes;
    source.size = size;
    ODT_TEST_STATUS(odt_test_load_from_memory(&source, limits, allocator, &generation), expected);
    ODT_TEST_CHECK(generation == NULL);
}

static unsigned char *mutated_copy(const odt_test_byte_buffer *valid, mutation_fn mutate) {
    unsigned char *copy = malloc(valid->size);

    ODT_TEST_CHECK(copy != NULL);
    memcpy(copy, valid->data, valid->size);
    mutate(copy, valid->size);
    return copy;
}

static size_t section_offset(const unsigned char *bytes, size_t descriptor_offset) {
    uint64_t value =
        odt_test_load_u64(bytes + descriptor_offset + ODT_TEST_FORMAT_DESCRIPTOR_OFFSET_OFFSET);

    ODT_TEST_CHECK(value <= SIZE_MAX);
    return (size_t)value;
}

static void mutate_magic(unsigned char *bytes, size_t size) {
    (void)size;
    bytes[0] ^= UINT8_C(0x80);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_version(unsigned char *bytes, size_t size) {
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_VERSION_OFFSET, ODT_FORMAT_VERSION + 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_header_reserved(unsigned char *bytes, size_t size) {
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_RESERVED_OFFSET, 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_metadata_reserved(unsigned char *bytes, size_t size) {
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_METADATA_RESERVED_OFFSET, 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_section_overlap(unsigned char *bytes, size_t size) {
    uint64_t site_offset = odt_test_load_u64(bytes + ODT_TEST_FORMAT_SITE_DESCRIPTOR_OFFSET +
                                             ODT_TEST_FORMAT_DESCRIPTOR_OFFSET_OFFSET);

    odt_test_store_u64(bytes + ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET +
                           ODT_TEST_FORMAT_DESCRIPTOR_OFFSET_OFFSET,
                       site_offset);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_section_length(unsigned char *bytes, size_t size) {
    uint64_t length = odt_test_load_u64(bytes + ODT_TEST_FORMAT_LEAF_DESCRIPTOR_OFFSET +
                                        ODT_TEST_FORMAT_DESCRIPTOR_LENGTH_OFFSET);

    odt_test_store_u64(bytes + ODT_TEST_FORMAT_LEAF_DESCRIPTOR_OFFSET +
                           ODT_TEST_FORMAT_DESCRIPTOR_LENGTH_OFFSET,
                       length - 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_site_reserved(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_SITE_DESCRIPTOR_OFFSET);

    odt_test_store_u32(bytes + offset + ODT_TEST_FORMAT_SITE_RESERVED_OFFSET, 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_duplicate_site(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_SITE_DESCRIPTOR_OFFSET);

    memcpy(bytes + offset + ODT_TEST_FORMAT_SITE_RECORD_SIZE, bytes + offset, 16u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_duplicate_region(unsigned char *bytes, size_t size) {
    size_t site_offset = section_offset(bytes, ODT_TEST_FORMAT_SITE_DESCRIPTOR_OFFSET);
    size_t leaf_offset = section_offset(bytes, ODT_TEST_FORMAT_LEAF_DESCRIPTOR_OFFSET);
    uint32_t first_region =
        odt_test_load_u32(bytes + site_offset + ODT_TEST_FORMAT_SITE_REGION_OFFSET);

    odt_test_store_u32(bytes + site_offset + ODT_TEST_FORMAT_SITE_RECORD_SIZE +
                           ODT_TEST_FORMAT_SITE_REGION_OFFSET,
                       first_region);
    odt_test_store_u32(bytes + leaf_offset + 2u * ODT_TEST_FORMAT_LEAF_RECORD_SIZE +
                           ODT_TEST_FORMAT_LEAF_REGION_OFFSET,
                       first_region);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_invalid_child(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET);

    odt_test_store_i32(bytes + offset + ODT_TEST_FORMAT_NODE_FIRST_CHILD_OFFSET, INT32_MAX);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_unreachable_node(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET);

    odt_test_store_i32(bytes + offset + ODT_TEST_FORMAT_NODE_FIRST_CHILD_OFFSET, -1);
    odt_test_store_i32(bytes + offset + ODT_TEST_FORMAT_NODE_SECOND_CHILD_OFFSET, -2);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_duplicate_reference(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET);

    odt_test_store_i32(bytes + offset + ODT_TEST_FORMAT_NODE_SECOND_CHILD_OFFSET, 2);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_bad_leaf_region(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_LEAF_DESCRIPTOR_OFFSET);

    odt_test_store_i32(bytes + offset + ODT_TEST_FORMAT_LEAF_REGION_OFFSET, 99);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_semantically_wrong_leaves(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_LEAF_DESCRIPTOR_OFFSET);
    unsigned char first[ODT_TEST_FORMAT_LEAF_RECORD_SIZE];

    memcpy(first, bytes + offset, sizeof(first));
    memcpy(bytes + offset, bytes + offset + ODT_TEST_FORMAT_LEAF_RECORD_SIZE, sizeof(first));
    memcpy(bytes + offset + ODT_TEST_FORMAT_LEAF_RECORD_SIZE, first, sizeof(first));
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_bad_tie_direction(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET);

    odt_test_store_u32(bytes + offset + ODT_TEST_FORMAT_NODE_TIE_DIRECTION_OFFSET, 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_recomputed_filter_mismatch(unsigned char *bytes, size_t size) {
    uint32_t enabled = odt_test_load_u32(bytes + ODT_TEST_FORMAT_FILTER_ENABLED_OFFSET);

    odt_test_store_u32(bytes + ODT_TEST_FORMAT_FILTER_ENABLED_OFFSET, enabled == 0u ? 1u : 0u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_bad_filter_scale(unsigned char *bytes, size_t size) {
    uint32_t scale = odt_test_load_u32(bytes + ODT_TEST_FORMAT_FILTER_SCALE_OFFSET);

    odt_test_store_u32(bytes + ODT_TEST_FORMAT_FILTER_SCALE_OFFSET, scale + 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_node_reserved(unsigned char *bytes, size_t size) {
    size_t offset = section_offset(bytes, ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET);

    odt_test_store_u32(bytes + offset + ODT_TEST_FORMAT_NODE_RESERVED_OFFSET, 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_bad_maximum_depth(unsigned char *bytes, size_t size) {
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_MAX_DEPTH_OFFSET, 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_build_duration(unsigned char *bytes, size_t size) {
    odt_test_store_u64(bytes + ODT_TEST_BUILD_DURATION_OFFSET, 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_total_size(unsigned char *bytes, size_t size) {
    odt_test_store_u64(bytes + ODT_TEST_FORMAT_TOTAL_SIZE_OFFSET, (uint64_t)size - 1u);
    odt_test_recompute_checksum(bytes, size);
}

static void mutate_count_overflow(unsigned char *bytes, size_t size) {
    odt_test_store_u64(bytes + ODT_TEST_FORMAT_SITE_COUNT_OFFSET, UINT64_MAX);
    odt_test_recompute_checksum(bytes, size);
}

static void test_all_truncated_prefixes_and_appended_bytes(void) {
    odt_test_byte_buffer valid = {0};
    unsigned char *appended;
    size_t prefix;

    odt_test_independent_fixture(ODT_TEST_FIXTURE_REPRESENTATIVE, &valid);
    for (prefix = 1u; prefix < valid.size; ++prefix) {
        expect_status(valid.data, prefix, NULL, NULL, ODT_CORRUPT_DATA);
    }
    appended = malloc(valid.size + 1u);
    ODT_TEST_CHECK(appended != NULL);
    memcpy(appended, valid.data, valid.size);
    appended[valid.size] = 0u;
    expect_status(appended, valid.size + 1u, NULL, NULL, ODT_CORRUPT_DATA);
    free(appended);
    odt_test_byte_buffer_destroy(&valid);
}

static void test_checksum_and_checksum_valid_corruption(void) {
    static const struct {
        mutation_fn mutate;
        odt_status expected;
    } cases[] = {
        {mutate_magic, ODT_CORRUPT_DATA},
        {mutate_version, ODT_UNSUPPORTED_FORMAT},
        {mutate_header_reserved, ODT_CORRUPT_DATA},
        {mutate_metadata_reserved, ODT_CORRUPT_DATA},
        {mutate_section_overlap, ODT_CORRUPT_DATA},
        {mutate_section_length, ODT_CORRUPT_DATA},
        {mutate_site_reserved, ODT_CORRUPT_DATA},
        {mutate_duplicate_site, ODT_CORRUPT_DATA},
        {mutate_duplicate_region, ODT_CORRUPT_DATA},
        {mutate_invalid_child, ODT_CORRUPT_DATA},
        {mutate_unreachable_node, ODT_CORRUPT_DATA},
        {mutate_duplicate_reference, ODT_CORRUPT_DATA},
        {mutate_bad_leaf_region, ODT_CORRUPT_DATA},
        {mutate_semantically_wrong_leaves, ODT_CORRUPT_DATA},
        {mutate_bad_tie_direction, ODT_CORRUPT_DATA},
        {mutate_recomputed_filter_mismatch, ODT_CORRUPT_DATA},
        {mutate_bad_filter_scale, ODT_CORRUPT_DATA},
        {mutate_node_reserved, ODT_CORRUPT_DATA},
        {mutate_bad_maximum_depth, ODT_CORRUPT_DATA},
        {mutate_build_duration, ODT_CORRUPT_DATA},
        {mutate_total_size, ODT_CORRUPT_DATA},
        {mutate_count_overflow, ODT_LIMIT_EXCEEDED},
    };
    odt_test_byte_buffer valid = {0};
    size_t index;

    odt_test_independent_fixture(ODT_TEST_FIXTURE_REPRESENTATIVE, &valid);
    valid.data[valid.size - 1u] ^= UINT8_C(0x40);
    expect_status(valid.data, valid.size, NULL, NULL, ODT_CORRUPT_DATA);
    valid.data[valid.size - 1u] ^= UINT8_C(0x40);

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        unsigned char *copy = mutated_copy(&valid, cases[index].mutate);

        expect_status(copy, valid.size, NULL, NULL, cases[index].expected);
        free(copy);
    }
    odt_test_byte_buffer_destroy(&valid);
}

static void expect_limit_before_allocation(const odt_test_byte_buffer *valid,
                                           odt_load_limits *limits) {
    odt_test_allocator_state state = {.live = true};
    odt_allocator allocator;

    odt_test_counting_allocator_init(&allocator, &state);
    expect_status(valid->data, valid->size, limits, &allocator, ODT_LIMIT_EXCEEDED);
    ODT_TEST_CHECK(state.allocate_calls == 0u);
    ODT_TEST_CHECK(state.live_allocations == 0u);
}

static void test_limits_precede_allocation(void) {
    odt_test_byte_buffer valid = {0};
    odt_load_limits limits;

    odt_test_independent_fixture(ODT_TEST_FIXTURE_REPRESENTATIVE, &valid);

    ODT_TEST_STATUS(odt_load_limits_init(&limits), ODT_OK);
    limits.max_encoded_bytes = (uint64_t)valid.size - 1u;
    expect_limit_before_allocation(&valid, &limits);

    ODT_TEST_STATUS(odt_load_limits_init(&limits), ODT_OK);
    limits.max_sites = 2u;
    expect_limit_before_allocation(&valid, &limits);

    ODT_TEST_STATUS(odt_load_limits_init(&limits), ODT_OK);
    limits.max_internal_nodes = 2u;
    expect_limit_before_allocation(&valid, &limits);

    ODT_TEST_STATUS(odt_load_limits_init(&limits), ODT_OK);
    limits.max_leaves = 3u;
    expect_limit_before_allocation(&valid, &limits);

    ODT_TEST_STATUS(odt_load_limits_init(&limits), ODT_OK);
    limits.max_depth = 1u;
    expect_limit_before_allocation(&valid, &limits);

    ODT_TEST_STATUS(odt_load_limits_init(&limits), ODT_OK);
    limits.max_allocation_bytes = 1u;
    expect_limit_before_allocation(&valid, &limits);

    odt_test_byte_buffer_destroy(&valid);
}

int main(void) {
    test_all_truncated_prefixes_and_appended_bytes();
    test_checksum_and_checksum_valid_corruption();
    test_limits_precede_allocation();
    return EXIT_SUCCESS;
}
