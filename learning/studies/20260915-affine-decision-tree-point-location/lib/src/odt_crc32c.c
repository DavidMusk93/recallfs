#include "odt_internal.h"

uint32_t odt_internal_crc32c_begin(void) {
    return UINT32_MAX;
}

uint32_t odt_internal_crc32c_extend(uint32_t state, const void *data, size_t size) {
    static const uint32_t table[16] = {
        UINT32_C(0x00000000), UINT32_C(0x105ec76f), UINT32_C(0x20bd8ede), UINT32_C(0x30e349b1),
        UINT32_C(0x417b1dbc), UINT32_C(0x5125dad3), UINT32_C(0x61c69362), UINT32_C(0x7198540d),
        UINT32_C(0x82f63b78), UINT32_C(0x92a8fc17), UINT32_C(0xa24bb5a6), UINT32_C(0xb21572c9),
        UINT32_C(0xc38d26c4), UINT32_C(0xd3d3e1ab), UINT32_C(0xe330a81a), UINT32_C(0xf36e6f75),
    };
    const unsigned char *bytes = data;
    size_t index;

    for (index = 0u; index < size; ++index) {
        state ^= bytes[index];
        state = (state >> 4u) ^ table[state & 15u];
        state = (state >> 4u) ^ table[state & 15u];
    }
    return state;
}

uint32_t odt_internal_crc32c_end(uint32_t state) {
    return ~state;
}

uint32_t odt_internal_crc32c(const void *data, size_t size) {
    return odt_internal_crc32c_end(
        odt_internal_crc32c_extend(odt_internal_crc32c_begin(), data, size));
}
