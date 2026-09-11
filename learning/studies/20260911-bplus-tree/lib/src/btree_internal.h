#ifndef BTREE_INTERNAL_H
#define BTREE_INTERNAL_H

#include "btree.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BTREE_PAGE_MAGIC UINT32_C(0x32525442)

enum btree_page_type {
    BTREE_PAGE_META = 0,
    BTREE_PAGE_LEAF = 1,
    BTREE_PAGE_INTERNAL = 2,
    BTREE_PAGE_FREE = 3
};

enum btree_page_offset {
    BTREE_PAGE_MAGIC_OFFSET = 0,
    BTREE_PAGE_VERSION_OFFSET = 4,
    BTREE_PAGE_TYPE_OFFSET = 6,
    BTREE_PAGE_FLAGS_OFFSET = 7,
    BTREE_PAGE_ID_OFFSET = 8,
    BTREE_PAGE_COUNT_OFFSET = 16,
    BTREE_PAGE_LEVEL_OFFSET = 20,
    BTREE_PAGE_AUX_OFFSET = 24,
    BTREE_PAGE_NEXT_OFFSET = 32,
    BTREE_PAGE_PREVIOUS_OFFSET = 40,
    BTREE_PAGE_CHECKSUM_OFFSET = 48,
    BTREE_PAGE_PAYLOAD_OFFSET = 64
};

enum btree_meta_offset {
    BTREE_META_ROOT_OFFSET = 16,
    BTREE_META_FREE_HEAD_OFFSET = 24,
    BTREE_META_ITEM_COUNT_OFFSET = 32,
    BTREE_META_NEXT_PAGE_OFFSET = 40,
    BTREE_META_HEIGHT_OFFSET = 52,
    BTREE_META_PAGE_SIZE_OFFSET = 56,
    BTREE_META_KEY_SIZE_OFFSET = 64,
    BTREE_META_VALUE_SIZE_OFFSET = 68,
    BTREE_META_COMPARATOR_ID_OFFSET = 72,
    BTREE_META_USED_SIZE = 80
};

static inline bool btree_page_size_valid(uint32_t page_size) {
    return page_size >= BTREE_MIN_PAGE_SIZE && page_size <= BTREE_MAX_PAGE_SIZE &&
           (page_size & (page_size - 1u)) == 0u;
}

static inline uint16_t btree_load_u16(const unsigned char *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static inline uint32_t btree_load_u32(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) | ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

static inline uint64_t btree_load_u64(const unsigned char *data) {
    uint64_t value = 0u;
    unsigned int shift;

    for (shift = 0u; shift < 64u; shift += 8u) {
        value |= (uint64_t)data[shift / 8u] << shift;
    }
    return value;
}

static inline void btree_store_u16(unsigned char *data, uint16_t value) {
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8u);
}

static inline void btree_store_u32(unsigned char *data, uint32_t value) {
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline void btree_store_u64(unsigned char *data, uint64_t value) {
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline bool btree_schema_layout(uint32_t page_size, uint32_t key_size, uint32_t value_size,
                                       size_t *leaf_stride_out, size_t *internal_stride_out,
                                       uint32_t *leaf_capacity_out,
                                       uint32_t *internal_capacity_out) {
    size_t payload_size;
    size_t leaf_stride;
    size_t internal_stride;
    size_t leaf_capacity;
    size_t internal_capacity;

    if (!btree_page_size_valid(page_size) || key_size == 0u || value_size == 0u) {
        return false;
    }
    if ((size_t)key_size > SIZE_MAX - (size_t)value_size ||
        (size_t)key_size > SIZE_MAX - sizeof(uint64_t)) {
        return false;
    }
    leaf_stride = (size_t)key_size + (size_t)value_size;
    internal_stride = (size_t)key_size + sizeof(uint64_t);
    payload_size = (size_t)page_size - (size_t)BTREE_PAGE_PAYLOAD_OFFSET;
    if (leaf_stride > payload_size || internal_stride > payload_size - sizeof(uint64_t)) {
        return false;
    }
    leaf_capacity = payload_size / leaf_stride;
    internal_capacity = (payload_size - sizeof(uint64_t)) / internal_stride;
    if (leaf_capacity < 3u || internal_capacity < 3u || leaf_capacity > UINT32_MAX ||
        internal_capacity > UINT32_MAX) {
        return false;
    }
    *leaf_stride_out = leaf_stride;
    *internal_stride_out = internal_stride;
    *leaf_capacity_out = (uint32_t)leaf_capacity;
    *internal_capacity_out = (uint32_t)internal_capacity;
    return true;
}

uint32_t btree_crc32c(const void *data, size_t size);
uint32_t btree_crc32c_page(const void *page, uint32_t page_size);
bool btree_page_checksum_valid(const void *page, uint32_t page_size);
void btree_page_checksum_store(void *page, uint32_t page_size);

#endif
