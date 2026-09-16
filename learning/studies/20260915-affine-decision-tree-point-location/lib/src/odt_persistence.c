#include "odt_internal.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    ODT_FORMAT_MAGIC_SIZE = 8,
    ODT_FORMAT_HEADER_SIZE = 256,
    ODT_FORMAT_CHECKSUM_OFFSET = 24,
    ODT_FORMAT_SECTION_COUNT = 3,
    ODT_FORMAT_SITE_RECORD_SIZE = 24,
    ODT_FORMAT_NODE_RECORD_SIZE = 24,
    ODT_FORMAT_LEAF_RECORD_SIZE = 8,
    ODT_FORMAT_GENERATION_ALIGNMENT = 64,
    ODT_FORMAT_CRC_CHUNK_SIZE = 4096,

    ODT_FORMAT_VERSION_OFFSET = 8,
    ODT_FORMAT_HEADER_SIZE_OFFSET = 12,
    ODT_FORMAT_TOTAL_SIZE_OFFSET = 16,
    ODT_FORMAT_SECTION_COUNT_OFFSET = 28,
    ODT_FORMAT_FLAGS_OFFSET = 32,
    ODT_FORMAT_RESERVED_OFFSET = 36,
    ODT_FORMAT_DOMAIN_OFFSET = 40,
    ODT_FORMAT_SITE_COUNT_OFFSET = 72,
    ODT_FORMAT_NODE_COUNT_OFFSET = 80,
    ODT_FORMAT_LEAF_COUNT_OFFSET = 88,
    ODT_FORMAT_ROOT_OFFSET = 96,
    ODT_FORMAT_MAXIMUM_DEPTH_OFFSET = 100,
    ODT_FORMAT_FILTER_SCALE_OFFSET = 104,
    ODT_FORMAT_FILTER_ENABLED_OFFSET = 108,
    ODT_FORMAT_FRAGMENT_COUNT_OFFSET = 112,
    ODT_FORMAT_BUILD_WORK_OFFSET = 120,
    ODT_FORMAT_PEAK_BUILD_BYTES_OFFSET = 128,
    ODT_FORMAT_BUILD_DURATION_OFFSET = 136,
    ODT_FORMAT_METADATA_RESERVED_OFFSET = 144,
    ODT_FORMAT_SITE_DESCRIPTOR_OFFSET = 160,
    ODT_FORMAT_NODE_DESCRIPTOR_OFFSET = 184,
    ODT_FORMAT_LEAF_DESCRIPTOR_OFFSET = 208,
    ODT_FORMAT_HEADER_RESERVED_OFFSET = 232,

    ODT_FORMAT_SECTION_SITES = 1,
    ODT_FORMAT_SECTION_NODES = 2,
    ODT_FORMAT_SECTION_LEAVES = 3,
};

static const unsigned char ODT_FORMAT_MAGIC[ODT_FORMAT_MAGIC_SIZE] = {'O', 'D', 'T', 'F',
                                                                      'M', 'T', '0', '1'};

typedef struct odt_wire_layout {
    uint64_t site_offset;
    uint64_t site_length;
    uint64_t node_offset;
    uint64_t node_length;
    uint64_t leaf_offset;
    uint64_t leaf_length;
    uint64_t total_size;
} odt_wire_layout;

typedef struct odt_wire_header {
    odt_domain domain;
    uint64_t site_count;
    uint64_t node_count;
    uint64_t leaf_count;
    int32_t root_reference;
    uint32_t maximum_depth;
    int32_t filter_scale_exponent;
    uint32_t filter_enabled;
    uint64_t fragment_count;
    uint64_t build_work;
    uint64_t peak_build_bytes;
    uint64_t build_duration_ns;
    uint32_t checksum;
    odt_wire_layout layout;
} odt_wire_header;

typedef struct odt_validation_stack_entry {
    int32_t reference;
    uint32_t depth;
} odt_validation_stack_entry;

typedef struct odt_site_sort_key {
    double x;
    double y;
    int32_t region_id;
} odt_site_sort_key;

static bool odt_checked_add_size(size_t first, size_t second, size_t *out_value) {
    if (first > SIZE_MAX - second) {
        return false;
    }
    *out_value = first + second;
    return true;
}

static bool odt_checked_multiply_size(size_t first, size_t second, size_t *out_value) {
    if (first != 0u && second > SIZE_MAX / first) {
        return false;
    }
    *out_value = first * second;
    return true;
}

static bool odt_checked_add_u64(uint64_t first, uint64_t second, uint64_t *out_value) {
    if (first > UINT64_MAX - second) {
        return false;
    }
    *out_value = first + second;
    return true;
}

static bool odt_checked_multiply_u64(uint64_t first, uint64_t second, uint64_t *out_value) {
    if (first != 0u && second > UINT64_MAX / first) {
        return false;
    }
    *out_value = first * second;
    return true;
}

static bool odt_is_power_of_two(size_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool odt_align_up_size(size_t value, size_t alignment, size_t *out_value) {
    size_t remainder;

    if (!odt_is_power_of_two(alignment)) {
        return false;
    }
    remainder = value & (alignment - 1u);
    if (remainder == 0u) {
        *out_value = value;
        return true;
    }
    return odt_checked_add_size(value, alignment - remainder, out_value);
}

static uint32_t odt_load_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8u | (uint32_t)bytes[2] << 16u |
           (uint32_t)bytes[3] << 24u;
}

static int32_t odt_load_i32(const unsigned char *bytes) {
    uint32_t value = odt_load_u32(bytes);

    if (value <= (uint32_t)INT32_MAX) {
        return (int32_t)value;
    }
    return -1 - (int32_t)(UINT32_MAX - value);
}

static uint64_t odt_load_u64(const unsigned char *bytes) {
    return (uint64_t)odt_load_u32(bytes) | (uint64_t)odt_load_u32(bytes + 4u) << 32u;
}

static double odt_load_double(const unsigned char *bytes) {
    uint64_t bits = odt_load_u64(bytes);
    double value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void odt_store_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8u);
    bytes[2] = (unsigned char)(value >> 16u);
    bytes[3] = (unsigned char)(value >> 24u);
}

static void odt_store_i32(unsigned char *bytes, int32_t value) {
    odt_store_u32(bytes, (uint32_t)value);
}

static void odt_store_u64(unsigned char *bytes, uint64_t value) {
    odt_store_u32(bytes, (uint32_t)value);
    odt_store_u32(bytes + 4u, (uint32_t)(value >> 32u));
}

static void odt_store_double(unsigned char *bytes, double value) {
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));
    odt_store_u64(bytes, bits);
}

static bool odt_bytes_are_zero(const unsigned char *bytes, size_t size) {
    size_t index;

    for (index = 0u; index < size; ++index) {
        if (bytes[index] != 0u) {
            return false;
        }
    }
    return true;
}

static const odt_site *odt_generation_sites(const odt_generation *generation) {
    return (const odt_site *)((const unsigned char *)generation + generation->sites_offset);
}

static const odt_internal_runtime_node *odt_generation_nodes(const odt_generation *generation) {
    return (const odt_internal_runtime_node *)((const unsigned char *)generation +
                                               generation->nodes_offset);
}

static const odt_internal_runtime_leaf *odt_generation_leaves(const odt_generation *generation) {
    return (const odt_internal_runtime_leaf *)((const unsigned char *)generation +
                                               generation->leaves_offset);
}

static odt_site *odt_generation_sites_mutable(odt_generation *generation) {
    return (odt_site *)((unsigned char *)generation + generation->sites_offset);
}

static odt_internal_runtime_node *odt_generation_nodes_mutable(odt_generation *generation) {
    return (odt_internal_runtime_node *)((unsigned char *)generation + generation->nodes_offset);
}

static odt_internal_runtime_leaf *odt_generation_leaves_mutable(odt_generation *generation) {
    return (odt_internal_runtime_leaf *)((unsigned char *)generation + generation->leaves_offset);
}

static bool odt_wire_layout_init(uint64_t site_count, uint64_t node_count, uint64_t leaf_count,
                                 odt_wire_layout *out_layout) {
    uint64_t offset = ODT_FORMAT_HEADER_SIZE;

    if (!odt_checked_multiply_u64(site_count, ODT_FORMAT_SITE_RECORD_SIZE,
                                  &out_layout->site_length) ||
        !odt_checked_multiply_u64(node_count, ODT_FORMAT_NODE_RECORD_SIZE,
                                  &out_layout->node_length) ||
        !odt_checked_multiply_u64(leaf_count, ODT_FORMAT_LEAF_RECORD_SIZE,
                                  &out_layout->leaf_length)) {
        return false;
    }
    out_layout->site_offset = offset;
    if (!odt_checked_add_u64(offset, out_layout->site_length, &offset)) {
        return false;
    }
    out_layout->node_offset = offset;
    if (!odt_checked_add_u64(offset, out_layout->node_length, &offset)) {
        return false;
    }
    out_layout->leaf_offset = offset;
    if (!odt_checked_add_u64(offset, out_layout->leaf_length, &offset)) {
        return false;
    }
    out_layout->total_size = offset;
    return true;
}

static bool odt_generation_layout(size_t site_count, size_t node_count, size_t leaf_count,
                                  size_t *out_sites_offset, size_t *out_nodes_offset,
                                  size_t *out_leaves_offset, size_t *out_size) {
    size_t offset = sizeof(odt_generation);
    size_t section_size;

    if (!odt_align_up_size(offset, _Alignof(odt_site), &offset)) {
        return false;
    }
    *out_sites_offset = offset;
    if (!odt_checked_multiply_size(site_count, sizeof(odt_site), &section_size) ||
        !odt_checked_add_size(offset, section_size, &offset) ||
        !odt_align_up_size(offset, _Alignof(odt_internal_runtime_node), &offset)) {
        return false;
    }
    *out_nodes_offset = offset;
    if (!odt_checked_multiply_size(node_count, sizeof(odt_internal_runtime_node), &section_size) ||
        !odt_checked_add_size(offset, section_size, &offset) ||
        !odt_align_up_size(offset, _Alignof(odt_internal_runtime_leaf), &offset)) {
        return false;
    }
    *out_leaves_offset = offset;
    if (!odt_checked_multiply_size(leaf_count, sizeof(odt_internal_runtime_leaf), &section_size) ||
        !odt_checked_add_size(offset, section_size, &offset) ||
        !odt_align_up_size(offset, ODT_FORMAT_GENERATION_ALIGNMENT, &offset)) {
        return false;
    }
    *out_size = offset;
    return true;
}

odt_status odt_internal_encoded_size(const odt_generation *generation, uint64_t *out_size) {
    odt_wire_layout layout;

    if (generation == NULL || out_size == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    if ((uintmax_t)generation->site_count > UINT64_MAX ||
        (uintmax_t)generation->node_count > UINT64_MAX ||
        (uintmax_t)generation->leaf_count > UINT64_MAX ||
        !odt_wire_layout_init((uint64_t)generation->site_count, (uint64_t)generation->node_count,
                              (uint64_t)generation->leaf_count, &layout)) {
        return ODT_LIMIT_EXCEEDED;
    }
    *out_size = layout.total_size;
    return ODT_OK;
}

static void odt_header_write_descriptor(unsigned char *header, size_t offset, uint32_t kind,
                                        uint64_t section_offset, uint64_t section_length) {
    odt_store_u32(header + offset, kind);
    odt_store_u64(header + offset + 8u, section_offset);
    odt_store_u64(header + offset + 16u, section_length);
}

static odt_status odt_header_encode(const odt_generation *generation, uint32_t checksum,
                                    unsigned char header[ODT_FORMAT_HEADER_SIZE],
                                    odt_wire_layout *out_layout) {
    if (generation->site_count == 0u || generation->node_count > (size_t)INT32_MAX ||
        generation->leaf_count == 0u || generation->leaf_count > (size_t)INT32_MAX ||
        generation->site_count > UINT32_MAX ||
        generation->build_stats.site_count != (uint64_t)generation->site_count ||
        generation->build_stats.node_count != (uint64_t)generation->node_count ||
        generation->build_stats.leaf_count != (uint64_t)generation->leaf_count ||
        generation->build_stats.reserved_0 != 0u || generation->filter_enabled > 1u ||
        !odt_wire_layout_init((uint64_t)generation->site_count, (uint64_t)generation->node_count,
                              (uint64_t)generation->leaf_count, out_layout)) {
        return ODT_INTERNAL_ERROR;
    }
    memset(header, 0, ODT_FORMAT_HEADER_SIZE);
    memcpy(header, ODT_FORMAT_MAGIC, ODT_FORMAT_MAGIC_SIZE);
    odt_store_u32(header + ODT_FORMAT_VERSION_OFFSET, ODT_FORMAT_VERSION);
    odt_store_u32(header + ODT_FORMAT_HEADER_SIZE_OFFSET, ODT_FORMAT_HEADER_SIZE);
    odt_store_u64(header + ODT_FORMAT_TOTAL_SIZE_OFFSET, out_layout->total_size);
    odt_store_u32(header + ODT_FORMAT_CHECKSUM_OFFSET, checksum);
    odt_store_u32(header + ODT_FORMAT_SECTION_COUNT_OFFSET, ODT_FORMAT_SECTION_COUNT);
    odt_store_double(header + ODT_FORMAT_DOMAIN_OFFSET, generation->domain.min_x);
    odt_store_double(header + ODT_FORMAT_DOMAIN_OFFSET + 8u, generation->domain.min_y);
    odt_store_double(header + ODT_FORMAT_DOMAIN_OFFSET + 16u, generation->domain.max_x);
    odt_store_double(header + ODT_FORMAT_DOMAIN_OFFSET + 24u, generation->domain.max_y);
    odt_store_u64(header + ODT_FORMAT_SITE_COUNT_OFFSET, (uint64_t)generation->site_count);
    odt_store_u64(header + ODT_FORMAT_NODE_COUNT_OFFSET, (uint64_t)generation->node_count);
    odt_store_u64(header + ODT_FORMAT_LEAF_COUNT_OFFSET, (uint64_t)generation->leaf_count);
    odt_store_i32(header + ODT_FORMAT_ROOT_OFFSET, generation->root_reference);
    odt_store_u32(header + ODT_FORMAT_MAXIMUM_DEPTH_OFFSET, generation->build_stats.maximum_depth);
    odt_store_i32(header + ODT_FORMAT_FILTER_SCALE_OFFSET, generation->filter_scale_exponent);
    odt_store_u32(header + ODT_FORMAT_FILTER_ENABLED_OFFSET, generation->filter_enabled);
    odt_store_u64(header + ODT_FORMAT_FRAGMENT_COUNT_OFFSET,
                  generation->build_stats.fragment_count);
    odt_store_u64(header + ODT_FORMAT_BUILD_WORK_OFFSET, generation->build_stats.build_work);
    odt_store_u64(header + ODT_FORMAT_PEAK_BUILD_BYTES_OFFSET,
                  generation->build_stats.peak_build_bytes);
    odt_store_u64(header + ODT_FORMAT_BUILD_DURATION_OFFSET, 0u);
    odt_header_write_descriptor(header, ODT_FORMAT_SITE_DESCRIPTOR_OFFSET, ODT_FORMAT_SECTION_SITES,
                                out_layout->site_offset, out_layout->site_length);
    odt_header_write_descriptor(header, ODT_FORMAT_NODE_DESCRIPTOR_OFFSET, ODT_FORMAT_SECTION_NODES,
                                out_layout->node_offset, out_layout->node_length);
    odt_header_write_descriptor(header, ODT_FORMAT_LEAF_DESCRIPTOR_OFFSET,
                                ODT_FORMAT_SECTION_LEAVES, out_layout->leaf_offset,
                                out_layout->leaf_length);
    return ODT_OK;
}

static odt_status odt_encode_piece(const odt_sink *sink, uint32_t *crc_state, const void *data,
                                   size_t size) {
    if (crc_state != NULL) {
        *crc_state = odt_internal_crc32c_extend(*crc_state, data, size);
        return ODT_OK;
    }
    return sink->write(sink->context, data, size) == 0 ? ODT_OK : ODT_IO_ERROR;
}

static odt_status odt_encode_records(const odt_generation *generation, const odt_sink *sink,
                                     uint32_t *crc_state) {
    const odt_site *sites = odt_generation_sites(generation);
    const odt_internal_runtime_node *nodes = odt_generation_nodes(generation);
    const odt_internal_runtime_leaf *leaves = odt_generation_leaves(generation);
    size_t index;
    odt_status status = ODT_OK;

    for (index = 0u; status == ODT_OK && index < generation->site_count; ++index) {
        unsigned char record[ODT_FORMAT_SITE_RECORD_SIZE] = {0};

        odt_store_double(record, sites[index].x);
        odt_store_double(record + 8u, sites[index].y);
        odt_store_i32(record + 16u, sites[index].region_id);
        status = odt_encode_piece(sink, crc_state, record, sizeof(record));
    }
    for (index = 0u; status == ODT_OK && index < generation->node_count; ++index) {
        unsigned char record[ODT_FORMAT_NODE_RECORD_SIZE] = {0};

        if (nodes[index].reserved_0 != 0u || nodes[index].filter_enabled > 1u) {
            return ODT_INTERNAL_ERROR;
        }
        odt_store_u32(record, nodes[index].first_ordinal);
        odt_store_u32(record + 4u, nodes[index].second_ordinal);
        odt_store_i32(record + 8u, nodes[index].first_child);
        odt_store_i32(record + 12u, nodes[index].second_child);
        odt_store_u32(record + 16u, 0u);
        status = odt_encode_piece(sink, crc_state, record, sizeof(record));
    }
    for (index = 0u; status == ODT_OK && index < generation->leaf_count; ++index) {
        unsigned char record[ODT_FORMAT_LEAF_RECORD_SIZE];

        odt_store_u32(record, leaves[index].site_ordinal);
        odt_store_i32(record + 4u, leaves[index].region_id);
        status = odt_encode_piece(sink, crc_state, record, sizeof(record));
    }
    return status;
}

odt_status odt_encode(const odt_generation *generation, const odt_encode_options *options,
                      const odt_sink *sink, uint64_t *out_encoded_size) {
    odt_encode_options default_options;
    const odt_encode_options *effective_options = options;
    unsigned char header[ODT_FORMAT_HEADER_SIZE];
    odt_wire_layout layout;
    uint32_t crc_state;
    uint32_t checksum;
    odt_status status;

    if (generation == NULL || sink == NULL || sink->write == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    if (effective_options == NULL) {
        status = odt_encode_options_init(&default_options);
        if (status != ODT_OK) {
            return status;
        }
        effective_options = &default_options;
    }
    status = odt_internal_encode_options_validate(effective_options);
    if (status != ODT_OK) {
        return status;
    }
    status = odt_header_encode(generation, 0u, header, &layout);
    if (status != ODT_OK) {
        return status;
    }
    crc_state = odt_internal_crc32c_begin();
    crc_state = odt_internal_crc32c_extend(crc_state, header, sizeof(header));
    status = odt_encode_records(generation, NULL, &crc_state);
    if (status != ODT_OK) {
        return status;
    }
    checksum = odt_internal_crc32c_end(crc_state);
    odt_store_u32(header + ODT_FORMAT_CHECKSUM_OFFSET, checksum);
    status = odt_encode_piece(sink, NULL, header, sizeof(header));
    if (status == ODT_OK) {
        status = odt_encode_records(generation, sink, NULL);
    }
    if (status != ODT_OK) {
        return status;
    }
    if (out_encoded_size != NULL) {
        *out_encoded_size = layout.total_size;
    }
    return ODT_OK;
}

static odt_status odt_source_read(const odt_source *source, uint64_t offset, void *data,
                                  size_t size) {
    uint64_t end;

    if (!odt_checked_add_u64(offset, (uint64_t)size, &end) || end > source->encoded_size) {
        return ODT_CORRUPT_DATA;
    }
    return source->read_at(source->context, offset, data, size) == 0 ? ODT_OK : ODT_IO_ERROR;
}

static bool odt_header_descriptor_matches(const unsigned char *header, size_t offset,
                                          uint32_t expected_kind, uint64_t expected_offset,
                                          uint64_t expected_length) {
    return odt_load_u32(header + offset) == expected_kind &&
           odt_load_u32(header + offset + 4u) == 0u &&
           odt_load_u64(header + offset + 8u) == expected_offset &&
           odt_load_u64(header + offset + 16u) == expected_length;
}

static odt_status odt_header_decode(const odt_source *source, const odt_load_limits *limits,
                                    const unsigned char header[ODT_FORMAT_HEADER_SIZE],
                                    odt_wire_header *out_header) {
    odt_wire_layout expected_layout;

    if (memcmp(header, ODT_FORMAT_MAGIC, ODT_FORMAT_MAGIC_SIZE) != 0) {
        return ODT_CORRUPT_DATA;
    }
    if (odt_load_u32(header + ODT_FORMAT_VERSION_OFFSET) != ODT_FORMAT_VERSION) {
        return ODT_UNSUPPORTED_FORMAT;
    }
    if (odt_load_u32(header + ODT_FORMAT_HEADER_SIZE_OFFSET) != ODT_FORMAT_HEADER_SIZE ||
        odt_load_u32(header + ODT_FORMAT_SECTION_COUNT_OFFSET) != ODT_FORMAT_SECTION_COUNT ||
        odt_load_u32(header + ODT_FORMAT_FLAGS_OFFSET) != 0u ||
        odt_load_u32(header + ODT_FORMAT_RESERVED_OFFSET) != 0u ||
        !odt_bytes_are_zero(header + ODT_FORMAT_METADATA_RESERVED_OFFSET, 16u) ||
        !odt_bytes_are_zero(header + ODT_FORMAT_HEADER_RESERVED_OFFSET,
                            ODT_FORMAT_HEADER_SIZE - ODT_FORMAT_HEADER_RESERVED_OFFSET)) {
        return ODT_CORRUPT_DATA;
    }
    memset(out_header, 0, sizeof(*out_header));
    out_header->layout.total_size = odt_load_u64(header + ODT_FORMAT_TOTAL_SIZE_OFFSET);
    out_header->checksum = odt_load_u32(header + ODT_FORMAT_CHECKSUM_OFFSET);
    out_header->domain.min_x = odt_load_double(header + ODT_FORMAT_DOMAIN_OFFSET);
    out_header->domain.min_y = odt_load_double(header + ODT_FORMAT_DOMAIN_OFFSET + 8u);
    out_header->domain.max_x = odt_load_double(header + ODT_FORMAT_DOMAIN_OFFSET + 16u);
    out_header->domain.max_y = odt_load_double(header + ODT_FORMAT_DOMAIN_OFFSET + 24u);
    out_header->site_count = odt_load_u64(header + ODT_FORMAT_SITE_COUNT_OFFSET);
    out_header->node_count = odt_load_u64(header + ODT_FORMAT_NODE_COUNT_OFFSET);
    out_header->leaf_count = odt_load_u64(header + ODT_FORMAT_LEAF_COUNT_OFFSET);
    out_header->root_reference = odt_load_i32(header + ODT_FORMAT_ROOT_OFFSET);
    out_header->maximum_depth = odt_load_u32(header + ODT_FORMAT_MAXIMUM_DEPTH_OFFSET);
    out_header->filter_scale_exponent = odt_load_i32(header + ODT_FORMAT_FILTER_SCALE_OFFSET);
    out_header->filter_enabled = odt_load_u32(header + ODT_FORMAT_FILTER_ENABLED_OFFSET);
    out_header->fragment_count = odt_load_u64(header + ODT_FORMAT_FRAGMENT_COUNT_OFFSET);
    out_header->build_work = odt_load_u64(header + ODT_FORMAT_BUILD_WORK_OFFSET);
    out_header->peak_build_bytes = odt_load_u64(header + ODT_FORMAT_PEAK_BUILD_BYTES_OFFSET);
    out_header->build_duration_ns = odt_load_u64(header + ODT_FORMAT_BUILD_DURATION_OFFSET);

    if (out_header->layout.total_size != source->encoded_size) {
        return ODT_CORRUPT_DATA;
    }
    if (out_header->site_count > (uint64_t)limits->max_sites ||
        out_header->node_count > (uint64_t)limits->max_internal_nodes ||
        out_header->leaf_count > (uint64_t)limits->max_leaves ||
        out_header->maximum_depth > limits->max_depth || out_header->site_count > UINT32_MAX ||
        out_header->node_count > INT32_MAX || out_header->leaf_count > INT32_MAX) {
        return ODT_LIMIT_EXCEEDED;
    }
    if (out_header->site_count == 0u || out_header->leaf_count == 0u ||
        out_header->node_count == UINT64_MAX ||
        out_header->leaf_count != out_header->node_count + 1u ||
        out_header->site_count > out_header->leaf_count ||
        (out_header->node_count == 0u &&
         (out_header->site_count != 1u || out_header->root_reference != -1 ||
          out_header->maximum_depth != 0u)) ||
        (out_header->node_count != 0u &&
         (out_header->root_reference != 1 || out_header->maximum_depth == 0u ||
          out_header->maximum_depth > out_header->node_count)) ||
        out_header->filter_enabled > 1u || out_header->fragment_count < out_header->leaf_count ||
        out_header->build_work < out_header->fragment_count ||
        out_header->build_duration_ns != 0u || !isfinite(out_header->domain.min_x) ||
        !isfinite(out_header->domain.min_y) || !isfinite(out_header->domain.max_x) ||
        !isfinite(out_header->domain.max_y) ||
        !(out_header->domain.min_x < out_header->domain.max_x) ||
        !(out_header->domain.min_y < out_header->domain.max_y) ||
        !odt_wire_layout_init(out_header->site_count, out_header->node_count,
                              out_header->leaf_count, &expected_layout) ||
        expected_layout.total_size != out_header->layout.total_size) {
        return ODT_CORRUPT_DATA;
    }
    out_header->layout = expected_layout;
    if (!odt_header_descriptor_matches(header, ODT_FORMAT_SITE_DESCRIPTOR_OFFSET,
                                       ODT_FORMAT_SECTION_SITES, expected_layout.site_offset,
                                       expected_layout.site_length) ||
        !odt_header_descriptor_matches(header, ODT_FORMAT_NODE_DESCRIPTOR_OFFSET,
                                       ODT_FORMAT_SECTION_NODES, expected_layout.node_offset,
                                       expected_layout.node_length) ||
        !odt_header_descriptor_matches(header, ODT_FORMAT_LEAF_DESCRIPTOR_OFFSET,
                                       ODT_FORMAT_SECTION_LEAVES, expected_layout.leaf_offset,
                                       expected_layout.leaf_length)) {
        return ODT_CORRUPT_DATA;
    }
    return ODT_OK;
}

static odt_status odt_checksum_validate(const odt_source *source, uint32_t expected_checksum) {
    unsigned char buffer[ODT_FORMAT_CRC_CHUNK_SIZE];
    uint64_t offset = 0u;
    uint32_t crc_state = odt_internal_crc32c_begin();

    while (offset < source->encoded_size) {
        const uint64_t remaining = source->encoded_size - offset;
        const size_t size = remaining < sizeof(buffer) ? (size_t)remaining : (size_t)sizeof(buffer);
        uint64_t checksum_end = ODT_FORMAT_CHECKSUM_OFFSET + sizeof(uint32_t);
        odt_status status = odt_source_read(source, offset, buffer, size);

        if (status != ODT_OK) {
            return status;
        }
        if (offset < checksum_end && offset + (uint64_t)size > ODT_FORMAT_CHECKSUM_OFFSET) {
            const uint64_t first =
                offset > ODT_FORMAT_CHECKSUM_OFFSET ? offset : ODT_FORMAT_CHECKSUM_OFFSET;
            const uint64_t last =
                offset + (uint64_t)size < checksum_end ? offset + (uint64_t)size : checksum_end;

            memset(buffer + (size_t)(first - offset), 0, (size_t)(last - first));
        }
        crc_state = odt_internal_crc32c_extend(crc_state, buffer, size);
        offset += (uint64_t)size;
    }
    return odt_internal_crc32c_end(crc_state) == expected_checksum ? ODT_OK : ODT_CORRUPT_DATA;
}

static int odt_site_coordinate_compare(const void *first_value, const void *second_value) {
    const odt_site_sort_key *first = first_value;
    const odt_site_sort_key *second = second_value;

    if (first->x < second->x) {
        return -1;
    }
    if (first->x > second->x) {
        return 1;
    }
    if (first->y < second->y) {
        return -1;
    }
    if (first->y > second->y) {
        return 1;
    }
    return 0;
}

static int odt_site_region_compare(const void *first_value, const void *second_value) {
    const odt_site_sort_key *first = first_value;
    const odt_site_sort_key *second = second_value;

    if (first->region_id < second->region_id) {
        return -1;
    }
    return first->region_id > second->region_id ? 1 : 0;
}

static odt_status odt_sites_validate(const odt_generation *generation, void *scratch) {
    const odt_site *sites = odt_generation_sites(generation);
    odt_site_sort_key *keys = scratch;
    size_t index;

    for (index = 0u; index < generation->site_count; ++index) {
        if (!isfinite(sites[index].x) || !isfinite(sites[index].y) ||
            sites[index].x < generation->domain.min_x ||
            sites[index].x > generation->domain.max_x ||
            sites[index].y < generation->domain.min_y ||
            sites[index].y > generation->domain.max_y) {
            return ODT_CORRUPT_DATA;
        }
        keys[index].x = sites[index].x;
        keys[index].y = sites[index].y;
        keys[index].region_id = sites[index].region_id;
    }
    qsort(keys, generation->site_count, sizeof(*keys), odt_site_coordinate_compare);
    for (index = 1u; index < generation->site_count; ++index) {
        if (keys[index - 1u].x == keys[index].x && keys[index - 1u].y == keys[index].y) {
            return ODT_CORRUPT_DATA;
        }
    }
    qsort(keys, generation->site_count, sizeof(*keys), odt_site_region_compare);
    for (index = 1u; index < generation->site_count; ++index) {
        if (keys[index - 1u].region_id == keys[index].region_id) {
            return ODT_CORRUPT_DATA;
        }
    }
    return ODT_OK;
}

static bool odt_reference_valid(const odt_generation *generation, int32_t reference) {
    if (reference > 0) {
        return (uint64_t)reference <= (uint64_t)generation->node_count;
    }
    if (reference < 0) {
        return (uint64_t)(-(int64_t)reference) <= (uint64_t)generation->leaf_count;
    }
    return false;
}

static odt_status odt_graph_push(const odt_generation *generation, int32_t reference,
                                 uint32_t depth, unsigned char *node_seen, unsigned char *leaf_seen,
                                 odt_validation_stack_entry *stack, size_t stack_capacity,
                                 size_t *stack_count) {
    size_t index;

    if (!odt_reference_valid(generation, reference) || *stack_count >= stack_capacity) {
        return ODT_CORRUPT_DATA;
    }
    if (reference > 0) {
        index = (size_t)reference - 1u;
        if (node_seen[index] != 0u) {
            return ODT_CORRUPT_DATA;
        }
        node_seen[index] = 1u;
    } else {
        index = (size_t)(-(int64_t)reference) - 1u;
        if (leaf_seen[index] != 0u) {
            return ODT_CORRUPT_DATA;
        }
        leaf_seen[index] = 1u;
    }
    stack[*stack_count] = (odt_validation_stack_entry){reference, depth};
    *stack_count += 1u;
    return ODT_OK;
}

static odt_status odt_graph_validate(const odt_generation *generation, void *scratch,
                                     size_t scratch_size) {
    const odt_internal_runtime_node *nodes = odt_generation_nodes(generation);
    const odt_internal_runtime_leaf *leaves = odt_generation_leaves(generation);
    unsigned char *bytes = scratch;
    unsigned char *node_seen = bytes;
    unsigned char *leaf_seen = node_seen + generation->node_count;
    size_t stack_offset;
    odt_validation_stack_entry *stack;
    size_t stack_capacity;
    size_t stack_count = 0u;
    size_t visited_nodes = 0u;
    size_t visited_leaves = 0u;
    uint32_t observed_depth = 0u;
    size_t index;
    odt_status status;

    if (!odt_align_up_size(generation->node_count + generation->leaf_count,
                           _Alignof(odt_validation_stack_entry), &stack_offset) ||
        stack_offset > scratch_size) {
        return ODT_INTERNAL_ERROR;
    }
    stack = (odt_validation_stack_entry *)(bytes + stack_offset);
    stack_capacity = generation->node_count + generation->leaf_count;
    memset(node_seen, 0, generation->node_count + generation->leaf_count);
    status = odt_graph_push(generation, generation->root_reference, 0u, node_seen, leaf_seen, stack,
                            stack_capacity, &stack_count);
    while (status == ODT_OK && stack_count != 0u) {
        odt_validation_stack_entry entry = stack[--stack_count];

        if (entry.depth > generation->build_stats.maximum_depth) {
            return ODT_CORRUPT_DATA;
        }
        if (entry.depth > observed_depth) {
            observed_depth = entry.depth;
        }
        if (entry.reference > 0) {
            const odt_internal_runtime_node *node = &nodes[(size_t)entry.reference - 1u];

            visited_nodes += 1u;
            status = odt_graph_push(generation, node->second_child, entry.depth + 1u, node_seen,
                                    leaf_seen, stack, stack_capacity, &stack_count);
            if (status == ODT_OK) {
                status = odt_graph_push(generation, node->first_child, entry.depth + 1u, node_seen,
                                        leaf_seen, stack, stack_capacity, &stack_count);
            }
        } else {
            visited_leaves += 1u;
        }
    }
    if (status != ODT_OK || visited_nodes != generation->node_count ||
        visited_leaves != generation->leaf_count ||
        observed_depth != generation->build_stats.maximum_depth) {
        return ODT_CORRUPT_DATA;
    }
    for (index = 0u; index < generation->leaf_count; ++index) {
        if ((size_t)leaves[index].site_ordinal >= generation->site_count ||
            leaves[index].region_id !=
                odt_generation_sites(generation)[leaves[index].site_ordinal].region_id) {
            return ODT_CORRUPT_DATA;
        }
    }
    return ODT_OK;
}

static odt_status odt_site_paths_validate(const odt_generation *generation) {
    const odt_site *sites = odt_generation_sites(generation);
    const odt_internal_runtime_node *nodes = odt_generation_nodes(generation);
    const odt_internal_runtime_leaf *leaves = odt_generation_leaves(generation);
    size_t site_index;

    for (site_index = 0u; site_index < generation->site_count; ++site_index) {
        const odt_point point = {sites[site_index].x, sites[site_index].y};
        int32_t reference = generation->root_reference;
        size_t steps = 0u;

        while (reference > 0) {
            const odt_internal_runtime_node *node;
            bool used_exact;
            int comparison;
            odt_status status;

            if (!odt_reference_valid(generation, reference) || steps >= generation->node_count) {
                return ODT_CORRUPT_DATA;
            }
            node = &nodes[(size_t)reference - 1u];
            status = odt_internal_runtime_bisector_compare(&point, sites, generation->site_count,
                                                           generation->filter_scale_exponent, node,
                                                           &comparison, &used_exact);
            if (status != ODT_OK) {
                return ODT_CORRUPT_DATA;
            }
            (void)used_exact;
            reference = comparison <= 0 ? node->first_child : node->second_child;
            steps += 1u;
        }
        if (!odt_reference_valid(generation, reference) ||
            leaves[(size_t)(-(int64_t)reference) - 1u].site_ordinal != (uint32_t)site_index) {
            return ODT_CORRUPT_DATA;
        }
    }
    return ODT_OK;
}

static bool odt_scratch_size(size_t site_count, size_t node_count, size_t leaf_count,
                             size_t *out_size) {
    size_t graph_prefix;
    size_t graph_size;
    size_t stack_count;
    size_t site_size;

    if (!odt_checked_add_size(node_count, leaf_count, &graph_prefix) ||
        !odt_align_up_size(graph_prefix, _Alignof(odt_validation_stack_entry), &graph_prefix) ||
        !odt_checked_add_size(node_count, leaf_count, &stack_count) ||
        !odt_checked_multiply_size(stack_count, sizeof(odt_validation_stack_entry), &graph_size) ||
        !odt_checked_add_size(graph_prefix, graph_size, &graph_size) ||
        !odt_checked_multiply_size(site_count, sizeof(odt_site_sort_key), &site_size)) {
        return false;
    }
    *out_size = graph_size > site_size ? graph_size : site_size;
    return *out_size != 0u;
}

static odt_status odt_load_records(const odt_source *source, const odt_wire_header *header,
                                   odt_generation *generation) {
    odt_internal_numeric_context numeric;
    odt_site *sites = odt_generation_sites_mutable(generation);
    odt_internal_runtime_node *nodes = odt_generation_nodes_mutable(generation);
    odt_internal_runtime_leaf *leaves = odt_generation_leaves_mutable(generation);
    unsigned char record[ODT_FORMAT_NODE_RECORD_SIZE];
    size_t index;
    odt_status status;

    for (index = 0u; index < generation->site_count; ++index) {
        status = odt_source_read(
            source, header->layout.site_offset + (uint64_t)index * ODT_FORMAT_SITE_RECORD_SIZE,
            record, ODT_FORMAT_SITE_RECORD_SIZE);
        if (status != ODT_OK) {
            return status;
        }
        if (odt_load_u32(record + 20u) != 0u) {
            return ODT_CORRUPT_DATA;
        }
        sites[index].x = odt_load_double(record);
        sites[index].y = odt_load_double(record + 8u);
        sites[index].region_id = odt_load_i32(record + 16u);
    }
    status = odt_internal_numeric_context_init(&generation->domain, sites, generation->site_count,
                                               &numeric);
    if (status != ODT_OK) {
        return ODT_CORRUPT_DATA;
    }
    if (numeric.filter_scale_exponent != header->filter_scale_exponent ||
        (numeric.filter_enabled ? 1u : 0u) != header->filter_enabled) {
        return ODT_CORRUPT_DATA;
    }
    generation->filter_scale_exponent = numeric.filter_scale_exponent;
    generation->filter_enabled = numeric.filter_enabled ? 1u : 0u;

    for (index = 0u; index < generation->node_count; ++index) {
        odt_internal_line_ref line;
        bool filter_enabled;

        status = odt_source_read(
            source, header->layout.node_offset + (uint64_t)index * ODT_FORMAT_NODE_RECORD_SIZE,
            record, ODT_FORMAT_NODE_RECORD_SIZE);
        if (status != ODT_OK) {
            return status;
        }
        nodes[index].first_ordinal = odt_load_u32(record);
        nodes[index].second_ordinal = odt_load_u32(record + 4u);
        nodes[index].first_child = odt_load_i32(record + 8u);
        nodes[index].second_child = odt_load_i32(record + 12u);
        if (nodes[index].first_ordinal >= nodes[index].second_ordinal ||
            (size_t)nodes[index].second_ordinal >= generation->site_count ||
            odt_load_u32(record + 16u) != 0u || odt_load_u32(record + 20u) != 0u) {
            return ODT_CORRUPT_DATA;
        }
        line.kind = ODT_INTERNAL_LINE_BISECTOR;
        line.first_ordinal = nodes[index].first_ordinal;
        line.second_ordinal = nodes[index].second_ordinal;
        status = odt_internal_runtime_filter_init(&numeric, &line, &nodes[index].filter,
                                                  &filter_enabled);
        if (status != ODT_OK) {
            return ODT_CORRUPT_DATA;
        }
        nodes[index].filter_enabled = filter_enabled ? 1u : 0u;
    }
    for (index = 0u; index < generation->leaf_count; ++index) {
        status = odt_source_read(
            source, header->layout.leaf_offset + (uint64_t)index * ODT_FORMAT_LEAF_RECORD_SIZE,
            record, ODT_FORMAT_LEAF_RECORD_SIZE);
        if (status != ODT_OK) {
            return status;
        }
        leaves[index].site_ordinal = odt_load_u32(record);
        leaves[index].region_id = odt_load_i32(record + 4u);
    }
    return ODT_OK;
}

odt_status odt_load(const odt_source *source, const odt_load_limits *limits,
                    const odt_allocator *allocator, odt_generation **out_generation) {
    odt_load_limits default_limits;
    odt_allocator default_allocator;
    const odt_load_limits *effective_limits = limits;
    const odt_allocator *effective_allocator = allocator;
    unsigned char header_bytes[ODT_FORMAT_HEADER_SIZE];
    odt_wire_header header;
    odt_generation *generation = NULL;
    void *scratch = NULL;
    size_t sites_offset;
    size_t nodes_offset;
    size_t leaves_offset;
    size_t generation_size;
    size_t scratch_size;
    size_t total_allocation;
    const size_t scratch_alignment =
        _Alignof(max_align_t) < sizeof(void *) ? sizeof(void *) : _Alignof(max_align_t);
    odt_status status;

    if (out_generation == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    *out_generation = NULL;
    if (source == NULL || source->read_at == NULL) {
        return ODT_INVALID_ARGUMENT;
    }
    if (effective_limits == NULL) {
        status = odt_load_limits_init(&default_limits);
        if (status != ODT_OK) {
            return status;
        }
        effective_limits = &default_limits;
    }
    status = odt_internal_load_limits_validate(effective_limits);
    if (status != ODT_OK) {
        return status;
    }
    if (effective_allocator == NULL) {
        status = odt_allocator_init(&default_allocator);
        if (status != ODT_OK) {
            return status;
        }
        effective_allocator = &default_allocator;
    }
    status = odt_internal_allocator_validate(effective_allocator);
    if (status != ODT_OK) {
        return status;
    }
    status = odt_internal_numeric_environment_check();
    if (status != ODT_OK) {
        return status;
    }
    if (source->encoded_size > effective_limits->max_encoded_bytes) {
        return ODT_LIMIT_EXCEEDED;
    }
    if (source->encoded_size < ODT_FORMAT_HEADER_SIZE) {
        return ODT_CORRUPT_DATA;
    }
    status = odt_source_read(source, 0u, header_bytes, sizeof(header_bytes));
    if (status != ODT_OK) {
        return status;
    }
    status = odt_header_decode(source, effective_limits, header_bytes, &header);
    if (status != ODT_OK) {
        return status;
    }
    status = odt_checksum_validate(source, header.checksum);
    if (status != ODT_OK) {
        return status;
    }
    if (!odt_generation_layout((size_t)header.site_count, (size_t)header.node_count,
                               (size_t)header.leaf_count, &sites_offset, &nodes_offset,
                               &leaves_offset, &generation_size) ||
        !odt_scratch_size((size_t)header.site_count, (size_t)header.node_count,
                          (size_t)header.leaf_count, &scratch_size) ||
        !odt_checked_add_size(generation_size, scratch_size, &total_allocation)) {
        return ODT_LIMIT_EXCEEDED;
    }
    if (total_allocation > effective_limits->max_allocation_bytes) {
        return ODT_LIMIT_EXCEEDED;
    }
    status = odt_internal_generation_allocate(effective_allocator, generation_size,
                                              ODT_FORMAT_GENERATION_ALIGNMENT, &generation);
    if (status != ODT_OK) {
        return status;
    }
    scratch = effective_allocator->allocate(effective_allocator->context, scratch_size,
                                            scratch_alignment);
    if (scratch == NULL) {
        odt_generation_destroy(generation);
        return ODT_OUT_OF_MEMORY;
    }
    generation->domain = header.domain;
    generation->site_count = (size_t)header.site_count;
    generation->node_count = (size_t)header.node_count;
    generation->leaf_count = (size_t)header.leaf_count;
    generation->sites_offset = sites_offset;
    generation->nodes_offset = nodes_offset;
    generation->leaves_offset = leaves_offset;
    generation->root_reference = header.root_reference;
    generation->build_stats.site_count = header.site_count;
    generation->build_stats.node_count = header.node_count;
    generation->build_stats.leaf_count = header.leaf_count;
    generation->build_stats.fragment_count = header.fragment_count;
    generation->build_stats.build_work = header.build_work;
    generation->build_stats.peak_build_bytes = header.peak_build_bytes;
    generation->build_stats.build_duration_ns = header.build_duration_ns;
    generation->build_stats.maximum_depth = header.maximum_depth;

    status = odt_load_records(source, &header, generation);
    if (status == ODT_OK) {
        status = odt_sites_validate(generation, scratch);
    }
    if (status == ODT_OK) {
        status = odt_graph_validate(generation, scratch, scratch_size);
    }
    if (status == ODT_OK) {
        status = odt_site_paths_validate(generation);
    }
    effective_allocator->deallocate(effective_allocator->context, scratch, scratch_size,
                                    scratch_alignment);
    if (status != ODT_OK) {
        odt_generation_destroy(generation);
        return status;
    }
    *out_generation = generation;
    return ODT_OK;
}
