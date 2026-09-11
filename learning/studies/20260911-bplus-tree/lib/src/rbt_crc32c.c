#include "rbt_internal.h"

static uint32_t rbt_crc32c_update(uint32_t crc, const unsigned char *data, size_t size) {
    static const uint32_t table[16] = {
        UINT32_C(0x00000000), UINT32_C(0x105ec76f), UINT32_C(0x20bd8ede), UINT32_C(0x30e349b1),
        UINT32_C(0x417b1dbc), UINT32_C(0x5125dad3), UINT32_C(0x61c69362), UINT32_C(0x7198540d),
        UINT32_C(0x82f63b78), UINT32_C(0x92a8fc17), UINT32_C(0xa24bb5a6), UINT32_C(0xb21572c9),
        UINT32_C(0xc38d26c4), UINT32_C(0xd3d3e1ab), UINT32_C(0xe330a81a), UINT32_C(0xf36e6f75),
    };
    size_t index;

    for (index = 0u; index < size; ++index) {
        crc ^= data[index];
        crc = (crc >> 4u) ^ table[crc & 15u];
        crc = (crc >> 4u) ^ table[crc & 15u];
    }
    return crc;
}

uint32_t rbt_crc32c(const void *data, size_t size) {
    return ~rbt_crc32c_update(UINT32_MAX, data, size);
}

uint32_t rbt_crc32c_page(const void *page, uint32_t page_size) {
    const unsigned char *bytes = page;
    static const unsigned char zeros[4] = {0u, 0u, 0u, 0u};
    const size_t suffix = (size_t)RBT_PAGE_CHECKSUM_OFFSET + sizeof(uint32_t);
    uint32_t crc = UINT32_MAX;

    crc = rbt_crc32c_update(crc, bytes, (size_t)RBT_PAGE_CHECKSUM_OFFSET);
    crc = rbt_crc32c_update(crc, zeros, sizeof(zeros));
    crc = rbt_crc32c_update(crc, bytes + suffix, (size_t)page_size - suffix);
    return ~crc;
}

bool rbt_page_checksum_valid(const void *page, uint32_t page_size) {
    const unsigned char *bytes = page;

    return rbt_load_u32(bytes + RBT_PAGE_CHECKSUM_OFFSET) == rbt_crc32c_page(page, page_size);
}

void rbt_page_checksum_store(void *page, uint32_t page_size) {
    unsigned char *bytes = page;

    rbt_store_u32(bytes + RBT_PAGE_CHECKSUM_OFFSET, 0u);
    rbt_store_u32(bytes + RBT_PAGE_CHECKSUM_OFFSET, rbt_crc32c_page(page, page_size));
}
