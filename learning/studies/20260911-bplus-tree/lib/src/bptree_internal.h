#ifndef BPTREE_INTERNAL_H
#define BPTREE_INTERNAL_H

#include "bptree.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BPT_PAGE_MAGIC UINT32_C(0x31545042)

enum bpt_page_type {
    BPT_PAGE_META = 0,
    BPT_PAGE_LEAF = 1,
    BPT_PAGE_INTERNAL = 2,
    BPT_PAGE_FREE = 3
};

enum bpt_page_offset {
    BPT_PAGE_MAGIC_OFFSET = 0,
    BPT_PAGE_VERSION_OFFSET = 4,
    BPT_PAGE_TYPE_OFFSET = 6,
    BPT_PAGE_FLAGS_OFFSET = 7,
    BPT_PAGE_ID_OFFSET = 8,
    BPT_PAGE_COUNT_OFFSET = 16,
    BPT_PAGE_LEVEL_OFFSET = 20,
    BPT_PAGE_AUX_OFFSET = 24,
    BPT_PAGE_NEXT_OFFSET = 32,
    BPT_PAGE_PREVIOUS_OFFSET = 40,
    BPT_PAGE_CHECKSUM_OFFSET = 48,
    BPT_PAGE_PAYLOAD_OFFSET = 64
};

enum bpt_meta_offset {
    BPT_META_ROOT_OFFSET = 16,
    BPT_META_FREE_HEAD_OFFSET = 24,
    BPT_META_ITEM_COUNT_OFFSET = 32,
    BPT_META_NEXT_PAGE_OFFSET = 40,
    BPT_META_HEIGHT_OFFSET = 52,
    BPT_META_PAGE_SIZE_OFFSET = 56
};

static inline bool bpt_page_size_valid(uint32_t page_size) {
    return page_size >= BPTREE_MIN_PAGE_SIZE && page_size <= BPTREE_MAX_PAGE_SIZE &&
           (page_size & (page_size - 1u)) == 0u;
}

static inline uint16_t bpt_load_u16(const unsigned char *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static inline uint32_t bpt_load_u32(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) | ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

static inline uint64_t bpt_load_u64(const unsigned char *data) {
    uint64_t value = 0u;
    unsigned int shift;

    for (shift = 0u; shift < 64u; shift += 8u) {
        value |= (uint64_t)data[shift / 8u] << shift;
    }
    return value;
}

static inline void bpt_store_u16(unsigned char *data, uint16_t value) {
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8u);
}

static inline void bpt_store_u32(unsigned char *data, uint32_t value) {
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline void bpt_store_u64(unsigned char *data, uint64_t value) {
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline uint32_t bpt_leaf_capacity(uint32_t page_size) {
    return (page_size - (uint32_t)BPT_PAGE_PAYLOAD_OFFSET) / 16u;
}

static inline uint32_t bpt_internal_capacity(uint32_t page_size) {
    return (page_size - (uint32_t)BPT_PAGE_PAYLOAD_OFFSET - 8u) / 16u;
}

uint32_t bpt_crc32c(const void *data, size_t size);
uint32_t bpt_crc32c_page(const void *page, uint32_t page_size);
bool bpt_page_checksum_valid(const void *page, uint32_t page_size);
void bpt_page_checksum_store(void *page, uint32_t page_size);

#endif
