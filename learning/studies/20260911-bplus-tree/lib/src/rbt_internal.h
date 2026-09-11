#ifndef RBT_INTERNAL_H
#define RBT_INTERNAL_H

#include "rbt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RBT_PAGE_MAGIC UINT32_C(0x31544252)

enum rbt_page_type {
    RBT_PAGE_META = 0,
    RBT_PAGE_LEAF = 1,
    RBT_PAGE_INTERNAL = 2,
    RBT_PAGE_SCHEMA = 3,
    RBT_PAGE_OVERFLOW = 4,
    RBT_PAGE_FREE = 5,
};

enum rbt_page_offset {
    RBT_PAGE_MAGIC_OFFSET = 0,
    RBT_PAGE_VERSION_OFFSET = 4,
    RBT_PAGE_TYPE_OFFSET = 6,
    RBT_PAGE_FLAGS_OFFSET = 7,
    RBT_PAGE_ID_OFFSET = 8,
    RBT_PAGE_COUNT_OFFSET = 16,
    RBT_PAGE_LEVEL_OFFSET = 20,
    RBT_PAGE_FREE_LOWER_OFFSET = 24,
    RBT_PAGE_FREE_UPPER_OFFSET = 28,
    RBT_PAGE_LINK0_OFFSET = 32,
    RBT_PAGE_LINK1_OFFSET = 40,
    RBT_PAGE_CHECKSUM_OFFSET = 48,
    RBT_PAGE_AUX_OFFSET = 56,
    RBT_PAGE_HEADER_SIZE = 64,
};

enum rbt_meta_offset {
    RBT_META_ROOT_OFFSET = 64,
    RBT_META_FREE_HEAD_OFFSET = 72,
    RBT_META_ITEM_COUNT_OFFSET = 80,
    RBT_META_NEXT_PAGE_OFFSET = 88,
    RBT_META_HEIGHT_OFFSET = 96,
    RBT_META_PAGE_SIZE_OFFSET = 100,
    RBT_META_SCHEMA_HEAD_OFFSET = 104,
    RBT_META_SCHEMA_SIZE_OFFSET = 112,
    RBT_META_SCHEMA_PAGE_COUNT_OFFSET = 120,
    RBT_META_USED_SIZE = 128,
};

enum rbt_schema_encoding {
    RBT_SCHEMA_HEADER_SIZE = 24,
    RBT_SCHEMA_COLUMN_SIZE = 16,
};

enum rbt_slot_encoding {
    RBT_SLOT_SIZE = 8,
};

static inline bool rbt_page_size_valid(uint32_t page_size) {
    return page_size >= RBT_MIN_PAGE_SIZE && page_size <= RBT_MAX_PAGE_SIZE &&
           (page_size & (page_size - 1u)) == 0u;
}

static inline uint16_t rbt_load_u16(const unsigned char *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static inline uint32_t rbt_load_u32(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) | ((uint32_t)data[2] << 16u) |
           ((uint32_t)data[3] << 24u);
}

static inline uint64_t rbt_load_u64(const unsigned char *data) {
    uint64_t value = 0u;
    unsigned int shift;

    for (shift = 0u; shift < 64u; shift += 8u) {
        value |= (uint64_t)data[shift / 8u] << shift;
    }
    return value;
}

static inline void rbt_store_u16(unsigned char *data, uint16_t value) {
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8u);
}

static inline void rbt_store_u32(unsigned char *data, uint32_t value) {
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

static inline void rbt_store_u64(unsigned char *data, uint64_t value) {
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        data[index] = (unsigned char)(value >> (index * 8u));
    }
}

uint32_t rbt_crc32c(const void *data, size_t size);
uint32_t rbt_crc32c_page(const void *page, uint32_t page_size);
bool rbt_page_checksum_valid(const void *page, uint32_t page_size);
void rbt_page_checksum_store(void *page, uint32_t page_size);

typedef int (*rbt_page_update_check_fn)(uint64_t page_id, const void *context);

int rbt_page_updates_validate(const struct rbt_page_update *updates, size_t update_count,
                              uint64_t current_page_count, rbt_page_update_check_fn check_page_id,
                              const void *check_context, uint64_t *out_final_page_count);

#endif
