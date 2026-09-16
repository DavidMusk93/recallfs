#ifndef ODT_PERSISTENCE_TEST_SUPPORT_H
#define ODT_PERSISTENCE_TEST_SUPPORT_H

#include "odt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ODT_TEST_FORMAT_HEADER_SIZE = 256,
    ODT_TEST_FORMAT_CHECKSUM_OFFSET = 24,
    ODT_TEST_FORMAT_VERSION_OFFSET = 8,
    ODT_TEST_FORMAT_TOTAL_SIZE_OFFSET = 16,
    ODT_TEST_FORMAT_SECTION_COUNT_OFFSET = 28,
    ODT_TEST_FORMAT_FLAGS_OFFSET = 32,
    ODT_TEST_FORMAT_RESERVED_OFFSET = 36,
    ODT_TEST_FORMAT_SITE_COUNT_OFFSET = 72,
    ODT_TEST_FORMAT_NODE_COUNT_OFFSET = 80,
    ODT_TEST_FORMAT_LEAF_COUNT_OFFSET = 88,
    ODT_TEST_FORMAT_ROOT_OFFSET = 96,
    ODT_TEST_FORMAT_MAX_DEPTH_OFFSET = 100,
    ODT_TEST_FORMAT_FILTER_SCALE_OFFSET = 104,
    ODT_TEST_FORMAT_FILTER_ENABLED_OFFSET = 108,
    ODT_TEST_BUILD_DURATION_OFFSET = 136,
    ODT_TEST_FORMAT_METADATA_RESERVED_OFFSET = 144,
    ODT_TEST_FORMAT_SITE_DESCRIPTOR_OFFSET = 160,
    ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET = 184,
    ODT_TEST_FORMAT_LEAF_DESCRIPTOR_OFFSET = 208,
    ODT_TEST_FORMAT_HEADER_RESERVED_OFFSET = 232,
    ODT_TEST_FORMAT_DESCRIPTOR_OFFSET_OFFSET = 8,
    ODT_TEST_FORMAT_DESCRIPTOR_LENGTH_OFFSET = 16,
    ODT_TEST_FORMAT_SITE_RECORD_SIZE = 24,
    ODT_TEST_FORMAT_SITE_REGION_OFFSET = 16,
    ODT_TEST_FORMAT_SITE_RESERVED_OFFSET = 20,
    ODT_TEST_FORMAT_NODE_RECORD_SIZE = 24,
    ODT_TEST_FORMAT_NODE_FIRST_ORDINAL_OFFSET = 0,
    ODT_TEST_FORMAT_NODE_SECOND_ORDINAL_OFFSET = 4,
    ODT_TEST_FORMAT_NODE_FIRST_CHILD_OFFSET = 8,
    ODT_TEST_FORMAT_NODE_SECOND_CHILD_OFFSET = 12,
    ODT_TEST_FORMAT_NODE_TIE_DIRECTION_OFFSET = 16,
    ODT_TEST_FORMAT_NODE_RESERVED_OFFSET = 20,
    ODT_TEST_FORMAT_LEAF_RECORD_SIZE = 8,
    ODT_TEST_FORMAT_LEAF_SITE_ORDINAL_OFFSET = 0,
    ODT_TEST_FORMAT_LEAF_REGION_OFFSET = 4,
};

typedef enum odt_test_fixture_kind {
    ODT_TEST_FIXTURE_MINIMAL = 0,
    ODT_TEST_FIXTURE_REPRESENTATIVE = 1
} odt_test_fixture_kind;

typedef struct odt_test_byte_buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t write_calls;
    size_t fail_at_call;
    const odt_generation *reentrant_generation;
} odt_test_byte_buffer;

typedef struct odt_test_memory_source {
    const unsigned char *data;
    size_t size;
    size_t read_calls;
    size_t fail_at_call;
    size_t maximum_request;
    const odt_generation *reentrant_generation;
} odt_test_memory_source;

void odt_test_byte_buffer_destroy(odt_test_byte_buffer *buffer);
int odt_test_buffer_sink_write(void *context, const void *data, size_t size);
int odt_test_memory_source_read_at(void *context, uint64_t offset, void *data_out, size_t size);
odt_status odt_test_encode_to_buffer(const odt_generation *generation,
                                     odt_test_byte_buffer *buffer);
odt_status odt_test_load_from_memory(odt_test_memory_source *source, const odt_load_limits *limits,
                                     const odt_allocator *allocator,
                                     odt_generation **out_generation);

uint32_t odt_test_load_u32(const unsigned char *bytes);
uint64_t odt_test_load_u64(const unsigned char *bytes);
void odt_test_store_u32(unsigned char *bytes, uint32_t value);
void odt_test_store_u64(unsigned char *bytes, uint64_t value);
void odt_test_store_i32(unsigned char *bytes, int32_t value);
void odt_test_recompute_checksum(unsigned char *bytes, size_t size);

void odt_test_independent_fixture(odt_test_fixture_kind kind, odt_test_byte_buffer *out_buffer);
void odt_test_read_fixture(const char *name, odt_test_byte_buffer *out_buffer);
void odt_test_fixture_path(const char *name, char *out_path, size_t capacity);

uint64_t odt_test_query_digest(const odt_generation *generation);
void odt_test_expect_metadata_equal(const odt_generation *first, const odt_generation *second);

#endif
