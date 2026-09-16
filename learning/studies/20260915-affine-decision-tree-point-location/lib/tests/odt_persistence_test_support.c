#include "odt_persistence_test_support.h"

#include "odt_test_support.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ODT_TEST_SECTION_SITES = 1,
    ODT_TEST_SECTION_NODES = 2,
    ODT_TEST_SECTION_LEAVES = 3,
    ODT_TEST_FRAGMENT_COUNT_OFFSET = 112,
    ODT_TEST_BUILD_WORK_OFFSET = 120,
    ODT_TEST_PEAK_BUILD_BYTES_OFFSET = 128,
};

static const unsigned char odt_test_format_magic[8] = {'O', 'D', 'T', 'F', 'M', 'T', '0', '1'};

static uint32_t odt_test_crc32c(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    size_t index;

    for (index = 0u; index < size; ++index) {
        unsigned int bit;

        crc ^= bytes[index];
        for (bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));

            crc = (crc >> 1u) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

uint32_t odt_test_load_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8u | (uint32_t)bytes[2] << 16u |
           (uint32_t)bytes[3] << 24u;
}

uint64_t odt_test_load_u64(const unsigned char *bytes) {
    return (uint64_t)odt_test_load_u32(bytes) | (uint64_t)odt_test_load_u32(bytes + 4u) << 32u;
}

void odt_test_store_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8u);
    bytes[2] = (unsigned char)(value >> 16u);
    bytes[3] = (unsigned char)(value >> 24u);
}

void odt_test_store_u64(unsigned char *bytes, uint64_t value) {
    odt_test_store_u32(bytes, (uint32_t)value);
    odt_test_store_u32(bytes + 4u, (uint32_t)(value >> 32u));
}

void odt_test_store_i32(unsigned char *bytes, int32_t value) {
    odt_test_store_u32(bytes, (uint32_t)value);
}

static void odt_test_store_double(unsigned char *bytes, double value) {
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));
    odt_test_store_u64(bytes, bits);
}

void odt_test_recompute_checksum(unsigned char *bytes, size_t size) {
    uint32_t checksum;

    ODT_TEST_CHECK(size >= ODT_TEST_FORMAT_CHECKSUM_OFFSET + sizeof(uint32_t));
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_CHECKSUM_OFFSET, 0u);
    checksum = odt_test_crc32c(bytes, size);
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_CHECKSUM_OFFSET, checksum);
}

static void odt_test_buffer_reserve(odt_test_byte_buffer *buffer, size_t additional) {
    size_t required;
    size_t capacity;
    unsigned char *data;

    ODT_TEST_CHECK(additional <= SIZE_MAX - buffer->size);
    required = buffer->size + additional;
    if (required <= buffer->capacity) {
        return;
    }
    capacity = buffer->capacity == 0u ? 256u : buffer->capacity;
    while (capacity < required) {
        ODT_TEST_CHECK(capacity <= SIZE_MAX / 2u);
        capacity *= 2u;
    }
    data = realloc(buffer->data, capacity);
    ODT_TEST_CHECK(data != NULL);
    buffer->data = data;
    buffer->capacity = capacity;
}

void odt_test_byte_buffer_destroy(odt_test_byte_buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

int odt_test_buffer_sink_write(void *context, const void *data, size_t size) {
    odt_test_byte_buffer *buffer = context;

    buffer->write_calls += 1u;
    if (buffer->fail_at_call != 0u && buffer->write_calls == buffer->fail_at_call) {
        return -1;
    }
    if (buffer->reentrant_generation != NULL) {
        uint64_t count = 0u;

        ODT_TEST_CHECK(odt_generation_get_site_count(buffer->reentrant_generation, &count) ==
                       ODT_OK);
        ODT_TEST_CHECK(count != 0u);
    }
    odt_test_buffer_reserve(buffer, size);
    if (size != 0u) {
        memcpy(buffer->data + buffer->size, data, size);
        buffer->size += size;
    }
    return 0;
}

int odt_test_memory_source_read_at(void *context, uint64_t offset, void *data_out, size_t size) {
    odt_test_memory_source *source = context;

    source->read_calls += 1u;
    if (source->fail_at_call != 0u && source->read_calls == source->fail_at_call) {
        return -1;
    }
    if (source->reentrant_generation != NULL) {
        uint64_t count = 0u;

        ODT_TEST_CHECK(odt_generation_get_leaf_count(source->reentrant_generation, &count) ==
                       ODT_OK);
        ODT_TEST_CHECK(count != 0u);
    }
    if (size > source->maximum_request) {
        source->maximum_request = size;
    }
    if (offset > (uint64_t)source->size || size > source->size - (size_t)offset) {
        return -1;
    }
    if (size != 0u) {
        memcpy(data_out, source->data + (size_t)offset, size);
    }
    return 0;
}

odt_status odt_test_encode_to_buffer(const odt_generation *generation,
                                     odt_test_byte_buffer *buffer) {
    odt_sink sink = {buffer, odt_test_buffer_sink_write};

    buffer->size = 0u;
    buffer->write_calls = 0u;
    return odt_encode(generation, NULL, &sink, NULL);
}

odt_status odt_test_load_from_memory(odt_test_memory_source *source, const odt_load_limits *limits,
                                     const odt_allocator *allocator,
                                     odt_generation **out_generation) {
    odt_source input = {source, (uint64_t)source->size, odt_test_memory_source_read_at};

    source->read_calls = 0u;
    source->maximum_request = 0u;
    return odt_load(&input, limits, allocator, out_generation);
}

static void odt_test_write_descriptor(unsigned char *header, size_t offset, uint32_t kind,
                                      uint64_t section_offset, uint64_t section_length) {
    odt_test_store_u32(header + offset, kind);
    odt_test_store_u32(header + offset + 4u, 0u);
    odt_test_store_u64(header + offset + ODT_TEST_FORMAT_DESCRIPTOR_OFFSET_OFFSET, section_offset);
    odt_test_store_u64(header + offset + ODT_TEST_FORMAT_DESCRIPTOR_LENGTH_OFFSET, section_length);
}

static void odt_test_write_site(unsigned char *record, double x, double y, int32_t region_id) {
    odt_test_store_double(record, x);
    odt_test_store_double(record + 8u, y);
    odt_test_store_i32(record + ODT_TEST_FORMAT_SITE_REGION_OFFSET, region_id);
    odt_test_store_u32(record + ODT_TEST_FORMAT_SITE_RESERVED_OFFSET, 0u);
}

static void odt_test_write_node(unsigned char *record, uint32_t first_ordinal,
                                uint32_t second_ordinal, int32_t first_child,
                                int32_t second_child) {
    odt_test_store_u32(record + ODT_TEST_FORMAT_NODE_FIRST_ORDINAL_OFFSET, first_ordinal);
    odt_test_store_u32(record + ODT_TEST_FORMAT_NODE_SECOND_ORDINAL_OFFSET, second_ordinal);
    odt_test_store_i32(record + ODT_TEST_FORMAT_NODE_FIRST_CHILD_OFFSET, first_child);
    odt_test_store_i32(record + ODT_TEST_FORMAT_NODE_SECOND_CHILD_OFFSET, second_child);
    odt_test_store_u32(record + ODT_TEST_FORMAT_NODE_TIE_DIRECTION_OFFSET, 0u);
    odt_test_store_u32(record + ODT_TEST_FORMAT_NODE_RESERVED_OFFSET, 0u);
}

static void odt_test_write_leaf(unsigned char *record, uint32_t site_ordinal, int32_t region_id) {
    odt_test_store_u32(record + ODT_TEST_FORMAT_LEAF_SITE_ORDINAL_OFFSET, site_ordinal);
    odt_test_store_i32(record + ODT_TEST_FORMAT_LEAF_REGION_OFFSET, region_id);
}

static void
odt_test_write_header(unsigned char *bytes, uint64_t total_size, const odt_domain *domain,
                      uint64_t site_count, uint64_t node_count, uint64_t leaf_count, int32_t root,
                      uint32_t maximum_depth, int32_t filter_scale, uint32_t filter_enabled,
                      uint64_t fragment_count, uint64_t build_work, uint64_t peak_build_bytes,
                      uint64_t site_offset, uint64_t node_offset, uint64_t leaf_offset) {
    memcpy(bytes, odt_test_format_magic, sizeof(odt_test_format_magic));
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_VERSION_OFFSET, ODT_FORMAT_VERSION);
    odt_test_store_u32(bytes + 12u, ODT_TEST_FORMAT_HEADER_SIZE);
    odt_test_store_u64(bytes + ODT_TEST_FORMAT_TOTAL_SIZE_OFFSET, total_size);
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_SECTION_COUNT_OFFSET, 3u);
    odt_test_store_double(bytes + 40u, domain->min_x);
    odt_test_store_double(bytes + 48u, domain->min_y);
    odt_test_store_double(bytes + 56u, domain->max_x);
    odt_test_store_double(bytes + 64u, domain->max_y);
    odt_test_store_u64(bytes + ODT_TEST_FORMAT_SITE_COUNT_OFFSET, site_count);
    odt_test_store_u64(bytes + ODT_TEST_FORMAT_NODE_COUNT_OFFSET, node_count);
    odt_test_store_u64(bytes + ODT_TEST_FORMAT_LEAF_COUNT_OFFSET, leaf_count);
    odt_test_store_i32(bytes + ODT_TEST_FORMAT_ROOT_OFFSET, root);
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_MAX_DEPTH_OFFSET, maximum_depth);
    odt_test_store_i32(bytes + ODT_TEST_FORMAT_FILTER_SCALE_OFFSET, filter_scale);
    odt_test_store_u32(bytes + ODT_TEST_FORMAT_FILTER_ENABLED_OFFSET, filter_enabled);
    odt_test_store_u64(bytes + ODT_TEST_FRAGMENT_COUNT_OFFSET, fragment_count);
    odt_test_store_u64(bytes + ODT_TEST_BUILD_WORK_OFFSET, build_work);
    odt_test_store_u64(bytes + ODT_TEST_PEAK_BUILD_BYTES_OFFSET, peak_build_bytes);
    odt_test_store_u64(bytes + ODT_TEST_BUILD_DURATION_OFFSET, 0u);
    odt_test_write_descriptor(bytes, ODT_TEST_FORMAT_SITE_DESCRIPTOR_OFFSET, ODT_TEST_SECTION_SITES,
                              site_offset, site_count * ODT_TEST_FORMAT_SITE_RECORD_SIZE);
    odt_test_write_descriptor(bytes, ODT_TEST_FORMAT_NODE_DESCRIPTOR_OFFSET, ODT_TEST_SECTION_NODES,
                              node_offset, node_count * ODT_TEST_FORMAT_NODE_RECORD_SIZE);
    odt_test_write_descriptor(bytes, ODT_TEST_FORMAT_LEAF_DESCRIPTOR_OFFSET,
                              ODT_TEST_SECTION_LEAVES, leaf_offset,
                              leaf_count * ODT_TEST_FORMAT_LEAF_RECORD_SIZE);
}

void odt_test_independent_fixture(odt_test_fixture_kind kind, odt_test_byte_buffer *out_buffer) {
    const odt_domain minimal_domain = {0.0, 0.0, 1.0, 1.0};
    const odt_domain representative_domain = {-2.0, -2.0, 2.0, 2.0};
    const uint64_t site_count = kind == ODT_TEST_FIXTURE_MINIMAL ? 1u : 3u;
    const uint64_t node_count = kind == ODT_TEST_FIXTURE_MINIMAL ? 0u : 3u;
    const uint64_t leaf_count = kind == ODT_TEST_FIXTURE_MINIMAL ? 1u : 4u;
    const uint64_t site_offset = ODT_TEST_FORMAT_HEADER_SIZE;
    const uint64_t node_offset = site_offset + site_count * ODT_TEST_FORMAT_SITE_RECORD_SIZE;
    const uint64_t leaf_offset = node_offset + node_count * ODT_TEST_FORMAT_NODE_RECORD_SIZE;
    const uint64_t total_size = leaf_offset + leaf_count * ODT_TEST_FORMAT_LEAF_RECORD_SIZE;
    unsigned char *sites;
    unsigned char *nodes;
    unsigned char *leaves;

    ODT_TEST_CHECK(total_size <= SIZE_MAX);
    odt_test_byte_buffer_destroy(out_buffer);
    out_buffer->data = calloc((size_t)total_size, 1u);
    ODT_TEST_CHECK(out_buffer->data != NULL);
    out_buffer->size = (size_t)total_size;
    out_buffer->capacity = (size_t)total_size;
    sites = out_buffer->data + (size_t)site_offset;
    nodes = out_buffer->data + (size_t)node_offset;
    leaves = out_buffer->data + (size_t)leaf_offset;

    if (kind == ODT_TEST_FIXTURE_MINIMAL) {
        odt_test_write_header(out_buffer->data, total_size, &minimal_domain, site_count, node_count,
                              leaf_count, -1, 0u, -1, 1u, 1u, 33u, 2000u, site_offset, node_offset,
                              leaf_offset);
        odt_test_write_site(sites, 0.5, 0.5, 7);
        odt_test_write_leaf(leaves, 0u, 7);
    } else {
        odt_test_write_header(out_buffer->data, total_size, &representative_domain, site_count,
                              node_count, leaf_count, 1, 2u, -2, 1u, 11u, 403u, 4720u, site_offset,
                              node_offset, leaf_offset);
        odt_test_write_site(sites, -1.0, 0.0, 11);
        odt_test_write_site(sites + ODT_TEST_FORMAT_SITE_RECORD_SIZE, 1.0, 0.0, 22);
        odt_test_write_site(sites + 2u * ODT_TEST_FORMAT_SITE_RECORD_SIZE, 0.0, 1.5, 33);
        odt_test_write_node(nodes, 0u, 1u, 2, 3);
        odt_test_write_node(nodes + ODT_TEST_FORMAT_NODE_RECORD_SIZE, 0u, 2u, -1, -2);
        odt_test_write_node(nodes + 2u * ODT_TEST_FORMAT_NODE_RECORD_SIZE, 1u, 2u, -3, -4);
        odt_test_write_leaf(leaves, 0u, 11);
        odt_test_write_leaf(leaves + ODT_TEST_FORMAT_LEAF_RECORD_SIZE, 2u, 33);
        odt_test_write_leaf(leaves + 2u * ODT_TEST_FORMAT_LEAF_RECORD_SIZE, 1u, 22);
        odt_test_write_leaf(leaves + 3u * ODT_TEST_FORMAT_LEAF_RECORD_SIZE, 2u, 33);
    }
    odt_test_recompute_checksum(out_buffer->data, out_buffer->size);
}

void odt_test_fixture_path(const char *name, char *out_path, size_t capacity) {
    int length = snprintf(out_path, capacity, "%s/%s", ODT_TEST_FIXTURE_DIR, name);

    ODT_TEST_CHECK(length >= 0);
    ODT_TEST_CHECK((size_t)length < capacity);
}

void odt_test_read_fixture(const char *name, odt_test_byte_buffer *out_buffer) {
    char path[1024];
    FILE *file;
    long length;
    size_t read_size;

    odt_test_fixture_path(name, path, sizeof(path));
    file = fopen(path, "rb");
    ODT_TEST_CHECK(file != NULL);
    ODT_TEST_CHECK(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    ODT_TEST_CHECK(length > 0);
    ODT_TEST_CHECK(fseek(file, 0, SEEK_SET) == 0);
    odt_test_byte_buffer_destroy(out_buffer);
    out_buffer->data = malloc((size_t)length);
    ODT_TEST_CHECK(out_buffer->data != NULL);
    read_size = fread(out_buffer->data, 1u, (size_t)length, file);
    ODT_TEST_CHECK(read_size == (size_t)length);
    ODT_TEST_CHECK(fclose(file) == 0);
    out_buffer->size = (size_t)length;
    out_buffer->capacity = (size_t)length;
}

static uint64_t odt_test_hash_u64(uint64_t hash, uint64_t value) {
    unsigned int byte;

    for (byte = 0u; byte < 8u; ++byte) {
        hash ^= value & UINT64_C(0xff);
        hash *= UINT64_C(1099511628211);
        value >>= 8u;
    }
    return hash;
}

uint64_t odt_test_query_digest(const odt_generation *generation) {
    static const odt_point points[] = {
        {-1.0, 0.0},  {1.0, 0.0}, {0.0, 0.0}, {0.0, 1.5},
        {-2.0, -2.0}, {3.0, 0.0}, {NAN, 0.0}, {0.0, INFINITY},
    };
    odt_query_result batch_results[sizeof(points) / sizeof(points[0])];
    odt_batch_stats batch_stats;
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0u; index < sizeof(points) / sizeof(points[0]); ++index) {
        odt_query_result result = {ODT_RESULT_ERROR, ODT_INTERNAL_ERROR, INT32_MIN};
        odt_query_stats stats = {0u, 0u};
        odt_status status = odt_query(generation, &points[index], &result, &stats);

        hash = odt_test_hash_u64(hash, (uint64_t)(unsigned int)status);
        if (status == ODT_OK) {
            hash = odt_test_hash_u64(hash, (uint64_t)(unsigned int)result.kind);
            hash = odt_test_hash_u64(hash, (uint64_t)(unsigned int)result.status);
            hash = odt_test_hash_u64(hash, (uint64_t)(uint32_t)result.region_id);
        }
        hash = odt_test_hash_u64(hash, stats.comparisons);
        hash = odt_test_hash_u64(hash, stats.exact_fallbacks);
    }
    ODT_TEST_STATUS(odt_query_batch(generation, sizeof(points) / sizeof(points[0]), points,
                                    sizeof(points[0]), batch_results, sizeof(batch_results[0]),
                                    &batch_stats),
                    ODT_OK);
    for (index = 0u; index < sizeof(points) / sizeof(points[0]); ++index) {
        hash = odt_test_hash_u64(hash, (uint64_t)(unsigned int)batch_results[index].kind);
        hash = odt_test_hash_u64(hash, (uint64_t)(unsigned int)batch_results[index].status);
        hash = odt_test_hash_u64(hash, (uint64_t)(uint32_t)batch_results[index].region_id);
    }
    hash = odt_test_hash_u64(hash, batch_stats.attempted);
    hash = odt_test_hash_u64(hash, batch_stats.region_count);
    hash = odt_test_hash_u64(hash, batch_stats.outside_count);
    hash = odt_test_hash_u64(hash, batch_stats.failed_count);
    hash = odt_test_hash_u64(hash, batch_stats.comparisons);
    return odt_test_hash_u64(hash, batch_stats.exact_fallbacks);
}

void odt_test_expect_metadata_equal(const odt_generation *first, const odt_generation *second) {
    odt_domain first_domain;
    odt_domain second_domain;
    odt_build_stats first_stats;
    odt_build_stats second_stats;
    uint64_t first_value;
    uint64_t second_value;
    uint32_t first_depth;
    uint32_t second_depth;

    ODT_TEST_STATUS(odt_generation_get_domain(first, &first_domain), ODT_OK);
    ODT_TEST_STATUS(odt_generation_get_domain(second, &second_domain), ODT_OK);
    ODT_TEST_CHECK(memcmp(&first_domain, &second_domain, sizeof(first_domain)) == 0);
    ODT_TEST_STATUS(odt_generation_get_site_count(first, &first_value), ODT_OK);
    ODT_TEST_STATUS(odt_generation_get_site_count(second, &second_value), ODT_OK);
    ODT_TEST_CHECK(first_value == second_value);
    ODT_TEST_STATUS(odt_generation_get_node_count(first, &first_value), ODT_OK);
    ODT_TEST_STATUS(odt_generation_get_node_count(second, &second_value), ODT_OK);
    ODT_TEST_CHECK(first_value == second_value);
    ODT_TEST_STATUS(odt_generation_get_leaf_count(first, &first_value), ODT_OK);
    ODT_TEST_STATUS(odt_generation_get_leaf_count(second, &second_value), ODT_OK);
    ODT_TEST_CHECK(first_value == second_value);
    ODT_TEST_STATUS(odt_generation_get_maximum_depth(first, &first_depth), ODT_OK);
    ODT_TEST_STATUS(odt_generation_get_maximum_depth(second, &second_depth), ODT_OK);
    ODT_TEST_CHECK(first_depth == second_depth);
    ODT_TEST_STATUS(odt_generation_get_build_stats(first, &first_stats), ODT_OK);
    ODT_TEST_STATUS(odt_generation_get_build_stats(second, &second_stats), ODT_OK);
    first_stats.build_duration_ns = 0u;
    second_stats.build_duration_ns = 0u;
    ODT_TEST_CHECK(memcmp(&first_stats, &second_stats, sizeof(first_stats)) == 0);
}
