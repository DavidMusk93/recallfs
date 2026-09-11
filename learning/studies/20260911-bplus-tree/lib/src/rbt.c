#include "rbt_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RBT_VALUE_DESCRIPTOR_SIZE = 16,
    RBT_LEAF_CELL_HEADER_SIZE = 8,
    RBT_VALUE_NULL = 0,
    RBT_VALUE_INLINE = 1,
    RBT_VALUE_OVERFLOW = 2,
};

enum rbt_validation_owner {
    RBT_VALIDATION_UNOWNED = 0,
    RBT_VALIDATION_METADATA = 1,
    RBT_VALIDATION_TREE = 2,
    RBT_VALIDATION_OVERFLOW = 3,
    RBT_VALIDATION_FREELIST = 4,
    RBT_VALIDATION_SCHEMA = 5,
};

struct rbt_metadata {
    uint64_t root_page_id;
    uint64_t free_head;
    uint64_t item_count;
    uint64_t next_page_id;
    uint32_t height;
    uint64_t schema_head;
    uint64_t schema_size;
    uint64_t schema_page_count;
};

struct rbt {
    struct rbt_storage storage;
    struct rbt_schema schema;
    struct rbt_column *columns;
    struct rbt_metadata metadata;
    uint64_t *schema_pages;
    uint32_t maximum_key_size;
    uint32_t leaf_capacity;
    uint32_t internal_capacity;
    uint32_t inline_threshold;
    bool poisoned;
    bool scan_active;
    bool storage_acquired;
};

struct rbt_row {
    struct rbt_value *key;
    struct rbt_value *value;
    size_t key_count;
    size_t value_count;
    bool callback_lifetime;
};

struct rbt_page_header {
    uint8_t type;
    uint32_t count;
    uint32_t level;
    uint32_t free_lower;
    uint32_t free_upper;
    uint64_t link0;
    uint64_t link1;
    uint64_t aux;
};

struct rbt_txn_page {
    uint64_t page_id;
    unsigned char *data;
    bool dirty;
};

struct rbt_txn {
    struct rbt *tree;
    struct rbt_metadata metadata;
    struct rbt_txn_page *pages;
    size_t page_count;
    size_t page_capacity;
    size_t *page_index;
    size_t page_index_capacity;
};

struct rbt_path_entry {
    uint64_t page_id;
    uint32_t child_index;
};

struct rbt_path {
    struct rbt_path_entry entries[64];
    size_t depth;
    uint64_t leaf_page_id;
};

struct rbt_cell_image {
    unsigned char *data;
    uint32_t size;
};

struct rbt_internal_image {
    uint64_t *children;
    struct rbt_cell_image *keys;
    uint32_t count;
    uint32_t capacity;
};

struct rbt_validation {
    struct rbt *tree;
    unsigned char *state;
    uint64_t *leaf_ids;
    size_t leaf_count;
    size_t leaf_capacity;
    uint64_t items;
    uint64_t tree_pages;
    uint64_t overflow_pages;
    char *error;
    size_t error_capacity;
};

static int rbt_txn_load_typed_page(struct rbt_txn *transaction, uint64_t page_id,
                                   uint8_t expected_type, unsigned char **out_page,
                                   struct rbt_page_header *out_header);
static int rbt_txn_free_overflow(struct rbt_txn *transaction, uint64_t first_page, uint64_t column,
                                 uint32_t logical_size);

static bool rbt_storage_valid(const struct rbt_storage *storage) {
    return storage != NULL && rbt_page_size_valid(storage->page_size) && storage->acquire != NULL &&
           storage->release != NULL && storage->read_page != NULL && storage->page_count != NULL &&
           storage->commit_pages != NULL;
}

static int rbt_backend_result(struct rbt *tree, int result) {
    if (result == -EOWNERDEAD) {
        tree->poisoned = true;
    }
    return result;
}

static int rbt_read_page(struct rbt *tree, uint64_t page_id, unsigned char *page) {
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    return rbt_backend_result(tree, tree->storage.read_page(tree->storage.context, page_id, page));
}

static int rbt_page_count(struct rbt *tree, uint64_t *out_count) {
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    return rbt_backend_result(tree, tree->storage.page_count(tree->storage.context, out_count));
}

static bool rbt_allocation_count_fits(uint64_t count, size_t element_size) {
    return count <= (uint64_t)SIZE_MAX && (size_t)count <= SIZE_MAX / element_size;
}

static int rbt_page_count_validate(uint64_t page_count) {
    if (!rbt_allocation_count_fits(page_count, sizeof(uint64_t))) {
        return -EBADMSG;
    }
    return 0;
}

static int rbt_commit(struct rbt *tree, const struct rbt_page_update *updates,
                      size_t update_count) {
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    return rbt_backend_result(
        tree, tree->storage.commit_pages(tree->storage.context, updates, update_count));
}

static void rbt_page_initialize(unsigned char *page, uint32_t page_size, uint64_t page_id,
                                uint8_t type, uint32_t count, uint32_t level) {
    memset(page, 0, (size_t)page_size);
    rbt_store_u32(page + RBT_PAGE_MAGIC_OFFSET, RBT_PAGE_MAGIC);
    rbt_store_u16(page + RBT_PAGE_VERSION_OFFSET, (uint16_t)RBT_FORMAT_VERSION);
    page[RBT_PAGE_TYPE_OFFSET] = type;
    rbt_store_u64(page + RBT_PAGE_ID_OFFSET, page_id);
    rbt_store_u32(page + RBT_PAGE_COUNT_OFFSET, count);
    rbt_store_u32(page + RBT_PAGE_LEVEL_OFFSET, level);
    rbt_store_u32(page + RBT_PAGE_FREE_LOWER_OFFSET, RBT_PAGE_HEADER_SIZE);
    rbt_store_u32(page + RBT_PAGE_FREE_UPPER_OFFSET, page_size);
}

static int rbt_decode_page_header_fields(const struct rbt *tree, const unsigned char *page,
                                         uint64_t expected_page_id,
                                         struct rbt_page_header *out_header) {
    uint16_t version;

    if (rbt_load_u32(page + RBT_PAGE_MAGIC_OFFSET) != RBT_PAGE_MAGIC ||
        rbt_load_u64(page + RBT_PAGE_ID_OFFSET) != expected_page_id) {
        return -EBADMSG;
    }
    version = rbt_load_u16(page + RBT_PAGE_VERSION_OFFSET);
    if (version != (uint16_t)RBT_FORMAT_VERSION || page[RBT_PAGE_FLAGS_OFFSET] != 0u) {
        return -ENOTSUP;
    }
    if (page[RBT_PAGE_TYPE_OFFSET] > (uint8_t)RBT_PAGE_FREE) {
        return -EBADMSG;
    }
    out_header->type = page[RBT_PAGE_TYPE_OFFSET];
    out_header->count = rbt_load_u32(page + RBT_PAGE_COUNT_OFFSET);
    out_header->level = rbt_load_u32(page + RBT_PAGE_LEVEL_OFFSET);
    out_header->free_lower = rbt_load_u32(page + RBT_PAGE_FREE_LOWER_OFFSET);
    out_header->free_upper = rbt_load_u32(page + RBT_PAGE_FREE_UPPER_OFFSET);
    out_header->link0 = rbt_load_u64(page + RBT_PAGE_LINK0_OFFSET);
    out_header->link1 = rbt_load_u64(page + RBT_PAGE_LINK1_OFFSET);
    out_header->aux = rbt_load_u64(page + RBT_PAGE_AUX_OFFSET);
    if (out_header->free_lower < RBT_PAGE_HEADER_SIZE ||
        out_header->free_lower > tree->storage.page_size ||
        out_header->free_upper < RBT_PAGE_HEADER_SIZE ||
        out_header->free_upper > tree->storage.page_size ||
        out_header->free_lower > out_header->free_upper) {
        return -EBADMSG;
    }
    return 0;
}

static int rbt_decode_page_header(const struct rbt *tree, const unsigned char *page,
                                  uint64_t expected_page_id, struct rbt_page_header *out_header) {
    if (!rbt_page_checksum_valid(page, tree->storage.page_size)) {
        return -EBADMSG;
    }
    return rbt_decode_page_header_fields(tree, page, expected_page_id, out_header);
}

static int rbt_read_typed_page(struct rbt *tree, uint64_t page_id, uint8_t expected_type,
                               unsigned char *page, struct rbt_page_header *out_header) {
    int result;

    if (page_id >= tree->metadata.next_page_id) {
        return -EBADMSG;
    }
    result = rbt_read_page(tree, page_id, page);
    if (result != 0) {
        return result;
    }
    result = rbt_decode_page_header(tree, page, page_id, out_header);
    if (result != 0) {
        return result;
    }
    return out_header->type == expected_type ? 0 : -EBADMSG;
}

static bool rbt_utf8_valid(const unsigned char *data, size_t size) {
    size_t index = 0u;

    while (index < size) {
        unsigned char first = data[index++];
        uint32_t codepoint;
        size_t continuation;
        size_t part;

        if (first <= 0x7fu) {
            continue;
        }
        if (first >= 0xc2u && first <= 0xdfu) {
            codepoint = (uint32_t)(first & 0x1fu);
            continuation = 1u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            codepoint = (uint32_t)(first & 0x0fu);
            continuation = 2u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codepoint = (uint32_t)(first & 0x07u);
            continuation = 3u;
        } else {
            return false;
        }
        if (continuation > size - index) {
            return false;
        }
        for (part = 0u; part < continuation; ++part) {
            unsigned char byte = data[index++];

            if ((byte & 0xc0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6u) | (uint32_t)(byte & 0x3fu);
        }
        if ((continuation == 2u && codepoint < 0x800u) ||
            (continuation == 3u && codepoint < 0x10000u) ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) || codepoint > 0x10ffffu) {
            return false;
        }
    }
    return true;
}

static size_t rbt_scalar_size(enum rbt_type type) {
    if (type == RBT_TYPE_BOOL) {
        return 1u;
    }
    if (type == RBT_TYPE_I64 || type == RBT_TYPE_U64) {
        return 8u;
    }
    return 0u;
}

static bool rbt_column_valid(const struct rbt_column *column, bool key_column) {
    const uint32_t known_flags = RBT_COLUMN_NULLABLE | RBT_COLUMN_DESCENDING;

    if ((column->flags & ~known_flags) != 0u ||
        (!key_column && (column->flags & RBT_COLUMN_DESCENDING) != 0u)) {
        return false;
    }
    if (column->type == RBT_TYPE_BOOL || column->type == RBT_TYPE_I64 ||
        column->type == RBT_TYPE_U64) {
        return column->max_size == 0u;
    }
    if (column->type == RBT_TYPE_BYTES || column->type == RBT_TYPE_UTF8) {
        return column->max_size != 0u && column->max_size <= RBT_MAX_FIELD_SIZE;
    }
    return false;
}

static int rbt_schema_measure(const struct rbt_schema *schema, uint32_t page_size,
                              uint32_t *out_maximum_key, uint32_t *out_leaf_capacity,
                              uint32_t *out_internal_capacity, uint32_t *out_inline_threshold) {
    uint64_t maximum_key = 0u;
    uint64_t maximum_row = 0u;
    uint64_t maximum_leaf_cell;
    uint32_t inline_threshold = page_size / 128u;
    size_t total_columns;
    size_t index;

    if (schema == NULL || schema->key_column_count == 0u ||
        schema->key_column_count > RBT_MAX_COLUMNS ||
        schema->value_column_count > RBT_MAX_COLUMNS ||
        schema->key_column_count > SIZE_MAX - schema->value_column_count) {
        return -EINVAL;
    }
    total_columns = schema->key_column_count + schema->value_column_count;
    if (total_columns > RBT_MAX_COLUMNS || schema->key_columns == NULL ||
        (schema->value_column_count != 0u && schema->value_columns == NULL)) {
        return -EINVAL;
    }
    if (inline_threshold < 8u) {
        inline_threshold = 8u;
    }
    if (inline_threshold > 512u) {
        inline_threshold = 512u;
    }
    for (index = 0u; index < total_columns; ++index) {
        const struct rbt_column *column =
            index < schema->key_column_count
                ? &schema->key_columns[index]
                : &schema->value_columns[index - schema->key_column_count];
        size_t other;
        uint64_t raw_size;

        if (!rbt_column_valid(column, index < schema->key_column_count)) {
            return -EINVAL;
        }
        for (other = 0u; other < index; ++other) {
            const struct rbt_column *previous =
                other < schema->key_column_count
                    ? &schema->key_columns[other]
                    : &schema->value_columns[other - schema->key_column_count];

            if (previous->id == column->id) {
                return -EINVAL;
            }
        }
        raw_size = rbt_scalar_size(column->type);
        if (raw_size == 0u) {
            raw_size = column->max_size;
        }
        maximum_row += raw_size;
        if (index < schema->key_column_count) {
            maximum_key += 1u;
            if (column->type == RBT_TYPE_BYTES || column->type == RBT_TYPE_UTF8) {
                maximum_key += (uint64_t)column->max_size * 2u + 2u;
            } else {
                maximum_key += raw_size;
            }
        }
    }
    if (maximum_row > RBT_MAX_ROW_SIZE || maximum_key > UINT32_MAX) {
        return -EINVAL;
    }
    *out_internal_capacity = (uint32_t)(((uint64_t)page_size - RBT_PAGE_HEADER_SIZE) /
                                        (RBT_SLOT_SIZE + sizeof(uint64_t) + maximum_key));
    if (*out_internal_capacity < 3u) {
        return -EINVAL;
    }
    maximum_leaf_cell = RBT_LEAF_CELL_HEADER_SIZE + maximum_key +
                        schema->value_column_count * RBT_VALUE_DESCRIPTOR_SIZE;
    for (index = 0u; index < schema->value_column_count; ++index) {
        const struct rbt_column *column = &schema->value_columns[index];
        uint64_t size = rbt_scalar_size(column->type);

        if (size == 0u) {
            size = column->max_size < inline_threshold ? column->max_size : inline_threshold;
        }
        maximum_leaf_cell += size;
    }
    *out_leaf_capacity = (uint32_t)(((uint64_t)page_size - RBT_PAGE_HEADER_SIZE) /
                                    (RBT_SLOT_SIZE + maximum_leaf_cell));
    if (*out_leaf_capacity < 3u) {
        return -EINVAL;
    }
    *out_maximum_key = (uint32_t)maximum_key;
    *out_inline_threshold = inline_threshold;
    return 0;
}

static int rbt_schema_encode(const struct rbt_schema *schema, unsigned char **out_data,
                             size_t *out_size) {
    size_t count = schema->key_column_count + schema->value_column_count;
    size_t size = RBT_SCHEMA_HEADER_SIZE + count * RBT_SCHEMA_COLUMN_SIZE;
    unsigned char *data = calloc(1u, size);
    size_t index;

    if (data == NULL) {
        return -ENOMEM;
    }
    memcpy(data, "RBTS", 4u);
    rbt_store_u32(data + 4u, RBT_FORMAT_VERSION);
    rbt_store_u64(data + 8u, schema->id);
    rbt_store_u32(data + 16u, (uint32_t)schema->key_column_count);
    rbt_store_u32(data + 20u, (uint32_t)schema->value_column_count);
    for (index = 0u; index < count; ++index) {
        const struct rbt_column *column =
            index < schema->key_column_count
                ? &schema->key_columns[index]
                : &schema->value_columns[index - schema->key_column_count];
        unsigned char *encoded = data + RBT_SCHEMA_HEADER_SIZE + index * RBT_SCHEMA_COLUMN_SIZE;

        rbt_store_u32(encoded, column->id);
        rbt_store_u32(encoded + 4u, (uint32_t)column->type);
        rbt_store_u32(encoded + 8u, column->flags);
        rbt_store_u32(encoded + 12u, column->max_size);
    }
    *out_data = data;
    *out_size = size;
    return 0;
}

static int rbt_schema_decode(const unsigned char *data, size_t size, struct rbt_schema *out_schema,
                             struct rbt_column **out_columns) {
    uint32_t key_count;
    uint32_t value_count;
    size_t count;
    struct rbt_column *columns;
    size_t index;

    if (size < RBT_SCHEMA_HEADER_SIZE || memcmp(data, "RBTS", 4u) != 0 ||
        rbt_load_u32(data + 4u) != RBT_FORMAT_VERSION) {
        return -EBADMSG;
    }
    key_count = rbt_load_u32(data + 16u);
    value_count = rbt_load_u32(data + 20u);
    count = (size_t)key_count + (size_t)value_count;
    if (key_count == 0u || count > RBT_MAX_COLUMNS ||
        size != RBT_SCHEMA_HEADER_SIZE + count * RBT_SCHEMA_COLUMN_SIZE) {
        return -EBADMSG;
    }
    columns = calloc(count, sizeof(*columns));
    if (columns == NULL) {
        return -ENOMEM;
    }
    for (index = 0u; index < count; ++index) {
        const unsigned char *encoded =
            data + RBT_SCHEMA_HEADER_SIZE + index * RBT_SCHEMA_COLUMN_SIZE;

        columns[index].id = rbt_load_u32(encoded);
        columns[index].type = (enum rbt_type)rbt_load_u32(encoded + 4u);
        columns[index].flags = rbt_load_u32(encoded + 8u);
        columns[index].max_size = rbt_load_u32(encoded + 12u);
    }
    out_schema->id = rbt_load_u64(data + 8u);
    out_schema->key_columns = columns;
    out_schema->key_column_count = key_count;
    out_schema->value_columns = columns + key_count;
    out_schema->value_column_count = value_count;
    *out_columns = columns;
    return 0;
}

static int rbt_value_validate(const struct rbt_column *column, const struct rbt_value *value) {
    if (value->type != column->type ||
        (value->is_null && (column->flags & RBT_COLUMN_NULLABLE) == 0u)) {
        return -EINVAL;
    }
    if (value->is_null) {
        return 0;
    }
    if (value->type == RBT_TYPE_BYTES || value->type == RBT_TYPE_UTF8) {
        if (value->as.bytes.size > column->max_size ||
            (value->as.bytes.size != 0u && value->as.bytes.data == NULL)) {
            return -EINVAL;
        }
        if (value->type == RBT_TYPE_UTF8 &&
            !rbt_utf8_valid(value->as.bytes.data, value->as.bytes.size)) {
            return -EINVAL;
        }
    }
    return 0;
}

static int rbt_record_validate(const struct rbt *tree, const struct rbt_record *record,
                               bool require_values) {
    size_t index;

    if (record == NULL || record->key_count != tree->schema.key_column_count ||
        record->key == NULL ||
        (require_values && (record->value_count != tree->schema.value_column_count ||
                            (record->value_count != 0u && record->value == NULL))) ||
        (!require_values && (record->value != NULL || record->value_count != 0u))) {
        return -EINVAL;
    }
    for (index = 0u; index < record->key_count; ++index) {
        if (rbt_value_validate(&tree->schema.key_columns[index], &record->key[index]) != 0) {
            return -EINVAL;
        }
    }
    if (require_values) {
        for (index = 0u; index < record->value_count; ++index) {
            if (rbt_value_validate(&tree->schema.value_columns[index], &record->value[index]) !=
                0) {
                return -EINVAL;
            }
        }
    }
    return 0;
}

static void rbt_store_big_u64(unsigned char data[8], uint64_t value) {
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        data[7u - index] = (unsigned char)(value >> (index * 8u));
    }
}

static uint64_t rbt_load_big_u64(const unsigned char data[8]) {
    uint64_t value = 0u;
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        value = (value << 8u) | data[index];
    }
    return value;
}

static int rbt_key_encode(const struct rbt *tree, const struct rbt_value *values,
                          unsigned char **out_key, uint32_t *out_size) {
    unsigned char *key = malloc(tree->maximum_key_size);
    size_t used = 0u;
    size_t index;

    if (key == NULL) {
        return -ENOMEM;
    }
    for (index = 0u; index < tree->schema.key_column_count; ++index) {
        const struct rbt_column *column = &tree->schema.key_columns[index];
        const struct rbt_value *value = &values[index];
        size_t begin = used;

        key[used++] = value->is_null ? 0u : 1u;
        if (!value->is_null) {
            if (column->type == RBT_TYPE_BOOL) {
                key[used++] = value->as.boolean ? 1u : 0u;
            } else if (column->type == RBT_TYPE_I64) {
                rbt_store_big_u64(key + used,
                                  (uint64_t)value->as.i64 ^ UINT64_C(0x8000000000000000));
                used += 8u;
            } else if (column->type == RBT_TYPE_U64) {
                rbt_store_big_u64(key + used, value->as.u64);
                used += 8u;
            } else {
                const unsigned char *bytes = value->as.bytes.data;
                size_t byte_index;

                for (byte_index = 0u; byte_index < value->as.bytes.size; ++byte_index) {
                    if (bytes[byte_index] == 0u) {
                        key[used++] = 0u;
                        key[used++] = 0xffu;
                    } else {
                        key[used++] = bytes[byte_index];
                    }
                }
                key[used++] = 0u;
                key[used++] = 0u;
            }
        }
        if ((column->flags & RBT_COLUMN_DESCENDING) != 0u) {
            size_t position;

            for (position = begin; position < used; ++position) {
                key[position] = (unsigned char)~key[position];
            }
        }
    }
    *out_key = key;
    *out_size = (uint32_t)used;
    return 0;
}

static int rbt_compare_keys(const unsigned char *left, uint32_t left_size,
                            const unsigned char *right, uint32_t right_size) {
    size_t common = left_size < right_size ? left_size : right_size;
    int result = memcmp(left, right, common);

    if (result != 0) {
        return result;
    }
    return left_size < right_size ? -1 : left_size > right_size ? 1 : 0;
}

static void rbt_row_release(struct rbt_row *row) {
    size_t index;

    if (row == NULL) {
        return;
    }
    for (index = 0u; index < row->key_count; ++index) {
        if (!row->key[index].is_null &&
            (row->key[index].type == RBT_TYPE_BYTES || row->key[index].type == RBT_TYPE_UTF8)) {
            free((void *)row->key[index].as.bytes.data);
        }
    }
    for (index = 0u; index < row->value_count; ++index) {
        if (!row->value[index].is_null &&
            (row->value[index].type == RBT_TYPE_BYTES || row->value[index].type == RBT_TYPE_UTF8)) {
            free((void *)row->value[index].as.bytes.data);
        }
    }
    free(row->key);
    free(row->value);
    free(row);
}

static bool rbt_values_equal(const struct rbt_value *left, const struct rbt_value *right,
                             size_t count) {
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (left[index].type != right[index].type || left[index].is_null != right[index].is_null) {
            return false;
        }
        if (left[index].is_null) {
            continue;
        }
        if (left[index].type == RBT_TYPE_BOOL) {
            if (left[index].as.boolean != right[index].as.boolean) {
                return false;
            }
        } else if (left[index].type == RBT_TYPE_I64) {
            if (left[index].as.i64 != right[index].as.i64) {
                return false;
            }
        } else if (left[index].type == RBT_TYPE_U64) {
            if (left[index].as.u64 != right[index].as.u64) {
                return false;
            }
        } else if (left[index].as.bytes.size != right[index].as.bytes.size ||
                   (left[index].as.bytes.size != 0u &&
                    memcmp(left[index].as.bytes.data, right[index].as.bytes.data,
                           left[index].as.bytes.size) != 0)) {
            return false;
        }
    }
    return true;
}

static int rbt_key_decode(const struct rbt *tree, const unsigned char *key, uint32_t key_size,
                          struct rbt_value *values) {
    size_t offset = 0u;
    size_t index;

    for (index = 0u; index < tree->schema.key_column_count; ++index) {
        const struct rbt_column *column = &tree->schema.key_columns[index];
        bool descending = (column->flags & RBT_COLUMN_DESCENDING) != 0u;
        unsigned char marker;

#define RBT_KEY_BYTE(position) (descending ? (unsigned char)~key[(position)] : key[(position)])
        if (offset >= key_size) {
            return -EBADMSG;
        }
        marker = RBT_KEY_BYTE(offset++);
        values[index].type = column->type;
        values[index].is_null = marker == 0u;
        if (marker > 1u || (values[index].is_null && (column->flags & RBT_COLUMN_NULLABLE) == 0u)) {
            return -EBADMSG;
        }
        if (values[index].is_null) {
            continue;
        }
        if (column->type == RBT_TYPE_BOOL) {
            unsigned char boolean;

            if (offset >= key_size || (boolean = RBT_KEY_BYTE(offset++)) > 1u) {
                return -EBADMSG;
            }
            values[index].as.boolean = boolean != 0u;
        } else if (column->type == RBT_TYPE_I64 || column->type == RBT_TYPE_U64) {
            unsigned char bytes[8];
            size_t part;
            uint64_t number;

            if ((size_t)key_size - offset < sizeof(bytes)) {
                return -EBADMSG;
            }
            for (part = 0u; part < sizeof(bytes); ++part) {
                bytes[part] = RBT_KEY_BYTE(offset++);
            }
            number = rbt_load_big_u64(bytes);
            if (column->type == RBT_TYPE_I64) {
                values[index].as.i64 = (int64_t)(number ^ UINT64_C(0x8000000000000000));
            } else {
                values[index].as.u64 = number;
            }
        } else {
            unsigned char *bytes = malloc(column->max_size == 0u ? 1u : column->max_size);
            size_t used = 0u;
            bool terminated = false;

            if (bytes == NULL) {
                return -ENOMEM;
            }
            while (offset < key_size) {
                unsigned char byte = RBT_KEY_BYTE(offset++);

                if (byte != 0u) {
                    if (used == column->max_size) {
                        free(bytes);
                        return -EBADMSG;
                    }
                    bytes[used++] = byte;
                    continue;
                }
                if (offset >= key_size) {
                    free(bytes);
                    return -EBADMSG;
                }
                byte = RBT_KEY_BYTE(offset++);
                if (byte == 0u) {
                    terminated = true;
                    break;
                }
                if (byte != 0xffu || used == column->max_size) {
                    free(bytes);
                    return -EBADMSG;
                }
                bytes[used++] = 0u;
            }
            if (!terminated || (column->type == RBT_TYPE_UTF8 && !rbt_utf8_valid(bytes, used))) {
                free(bytes);
                return -EBADMSG;
            }
            values[index].as.bytes.data = bytes;
            values[index].as.bytes.size = used;
        }
#undef RBT_KEY_BYTE
    }
    return offset == key_size ? 0 : -EBADMSG;
}

static int rbt_key_is_canonical(const struct rbt *tree, const unsigned char *key,
                                uint32_t key_size) {
    struct rbt_value *values = calloc(tree->schema.key_column_count, sizeof(*values));
    unsigned char *encoded = NULL;
    uint32_t encoded_size = 0u;
    size_t index;
    int result;

    if (values == NULL) {
        return -ENOMEM;
    }
    result = rbt_key_decode(tree, key, key_size, values);
    if (result == 0) {
        result = rbt_key_encode(tree, values, &encoded, &encoded_size);
    }
    if (result == 0 && (encoded_size != key_size || memcmp(encoded, key, key_size) != 0)) {
        result = -EBADMSG;
    }
    for (index = 0u; index < tree->schema.key_column_count; ++index) {
        if (!values[index].is_null &&
            (values[index].type == RBT_TYPE_BYTES || values[index].type == RBT_TYPE_UTF8)) {
            free((void *)values[index].as.bytes.data);
        }
    }
    free(encoded);
    free(values);
    return result;
}

static bool rbt_slots_valid(const struct rbt *tree, const unsigned char *page,
                            const struct rbt_page_header *header) {
    uint64_t expected_lower = RBT_PAGE_HEADER_SIZE + (uint64_t)header->count * RBT_SLOT_SIZE;
    uint32_t index;

    if (expected_lower != header->free_lower || expected_lower > header->free_upper) {
        return false;
    }
    for (index = 0u; index < header->count; ++index) {
        const unsigned char *slot = page + RBT_PAGE_HEADER_SIZE + (size_t)index * RBT_SLOT_SIZE;
        uint32_t offset = rbt_load_u32(slot);
        uint32_t length = rbt_load_u32(slot + 4u);
        uint32_t other;

        if (length == 0u || offset < header->free_upper || offset > tree->storage.page_size ||
            length > tree->storage.page_size - offset) {
            return false;
        }
        for (other = 0u; other < index; ++other) {
            const unsigned char *other_slot =
                page + RBT_PAGE_HEADER_SIZE + (size_t)other * RBT_SLOT_SIZE;
            uint32_t other_offset = rbt_load_u32(other_slot);
            uint32_t other_length = rbt_load_u32(other_slot + 4u);

            if (offset < other_offset + other_length && other_offset < offset + length) {
                return false;
            }
        }
    }
    return true;
}

static const unsigned char *rbt_slot_cell(const unsigned char *page, uint32_t index,
                                          uint32_t *out_length) {
    const unsigned char *slot = page + RBT_PAGE_HEADER_SIZE + (size_t)index * RBT_SLOT_SIZE;

    *out_length = rbt_load_u32(slot + 4u);
    return page + rbt_load_u32(slot);
}

static bool rbt_page_add_cell(unsigned char *page, const unsigned char *cell, uint32_t cell_size) {
    uint32_t count = rbt_load_u32(page + RBT_PAGE_COUNT_OFFSET);
    uint32_t lower = rbt_load_u32(page + RBT_PAGE_FREE_LOWER_OFFSET);
    uint32_t upper = rbt_load_u32(page + RBT_PAGE_FREE_UPPER_OFFSET);
    unsigned char *slot;

    if (cell_size > upper || upper - cell_size < lower + RBT_SLOT_SIZE) {
        return false;
    }
    upper -= cell_size;
    memcpy(page + upper, cell, cell_size);
    slot = page + lower;
    rbt_store_u32(slot, upper);
    rbt_store_u32(slot + 4u, cell_size);
    rbt_store_u32(page + RBT_PAGE_COUNT_OFFSET, count + 1u);
    rbt_store_u32(page + RBT_PAGE_FREE_LOWER_OFFSET, lower + RBT_SLOT_SIZE);
    rbt_store_u32(page + RBT_PAGE_FREE_UPPER_OFFSET, upper);
    return true;
}

static int rbt_read_overflow_from(struct rbt *tree, struct rbt_txn *transaction,
                                  uint64_t first_page, uint64_t column, uint32_t logical_size,
                                  unsigned char **out_data, unsigned char *ownership_state,
                                  uint64_t *inout_overflow_pages) {
    unsigned char *data = malloc(logical_size == 0u ? 1u : logical_size);
    unsigned char *page_buffer =
        transaction == NULL ? malloc((size_t)tree->storage.page_size) : NULL;
    uint64_t page_limit =
        transaction == NULL ? tree->metadata.next_page_id : transaction->metadata.next_page_id;
    uint64_t page_id = first_page;
    uint32_t copied = 0u;
    uint64_t visited = 0u;
    int result = 0;

    if (data == NULL || (transaction == NULL && page_buffer == NULL)) {
        free(data);
        free(page_buffer);
        return -ENOMEM;
    }
    while (copied < logical_size) {
        unsigned char *page = page_buffer;
        struct rbt_page_header header;

        if (page_id == 0u || page_id >= page_limit || visited++ >= page_limit ||
            (ownership_state != NULL && ownership_state[page_id] != RBT_VALIDATION_UNOWNED)) {
            result = -EBADMSG;
            break;
        }
        if (transaction == NULL) {
            result = rbt_read_typed_page(tree, page_id, RBT_PAGE_OVERFLOW, page, &header);
        } else {
            result =
                rbt_txn_load_typed_page(transaction, page_id, RBT_PAGE_OVERFLOW, &page, &header);
        }
        if (result != 0) {
            break;
        }
        if (header.level != 0u || header.link1 != 0u || header.aux != column ||
            header.count == 0u || header.count > tree->storage.page_size - RBT_PAGE_HEADER_SIZE ||
            header.free_lower != RBT_PAGE_HEADER_SIZE + header.count ||
            header.free_upper != tree->storage.page_size || header.count > logical_size - copied) {
            result = -EBADMSG;
            break;
        }
        if (ownership_state != NULL) {
            ownership_state[page_id] = RBT_VALIDATION_OVERFLOW;
        }
        if (inout_overflow_pages != NULL) {
            ++*inout_overflow_pages;
        }
        memcpy(data + copied, page + RBT_PAGE_HEADER_SIZE, header.count);
        copied += header.count;
        page_id = header.link0;
    }
    if (result == 0 && (copied != logical_size || page_id != 0u)) {
        result = -EBADMSG;
    }
    free(page_buffer);
    if (result != 0) {
        free(data);
        return result;
    }
    *out_data = data;
    return 0;
}

static int rbt_row_from_cell_source(struct rbt *tree, struct rbt_txn *transaction,
                                    const unsigned char *cell, uint32_t cell_size,
                                    struct rbt_row **out_row, unsigned char *ownership_state,
                                    uint64_t *inout_overflow_pages) {
    uint32_t key_size;
    uint16_t value_count;
    size_t directory_end;
    size_t expected_inline;
    struct rbt_row *row;
    size_t index;
    int result;

    if (cell_size < RBT_LEAF_CELL_HEADER_SIZE) {
        return -EBADMSG;
    }
    key_size = rbt_load_u32(cell);
    value_count = rbt_load_u16(cell + 4u);
    directory_end =
        RBT_LEAF_CELL_HEADER_SIZE + key_size + (size_t)value_count * RBT_VALUE_DESCRIPTOR_SIZE;
    if (key_size == 0u || key_size > tree->maximum_key_size ||
        value_count != tree->schema.value_column_count || directory_end > cell_size ||
        rbt_load_u16(cell + 6u) != 0u) {
        return -EBADMSG;
    }
    row = calloc(1u, sizeof(*row));
    if (row == NULL) {
        return -ENOMEM;
    }
    row->key_count = tree->schema.key_column_count;
    row->value_count = tree->schema.value_column_count;
    row->key = calloc(tree->schema.key_column_count, sizeof(*row->key));
    row->value = calloc(tree->schema.value_column_count, sizeof(*row->value));
    if (row->key == NULL || (tree->schema.value_column_count != 0u && row->value == NULL)) {
        rbt_row_release(row);
        return -ENOMEM;
    }
    result = rbt_key_decode(tree, cell + RBT_LEAF_CELL_HEADER_SIZE, key_size, row->key);
    if (result != 0) {
        rbt_row_release(row);
        return result;
    }
    {
        unsigned char *canonical = NULL;
        uint32_t canonical_size = 0u;

        result = rbt_key_encode(tree, row->key, &canonical, &canonical_size);
        if (result == 0 && (canonical_size != key_size ||
                            memcmp(canonical, cell + RBT_LEAF_CELL_HEADER_SIZE, key_size) != 0)) {
            result = -EBADMSG;
        }
        free(canonical);
        if (result != 0) {
            rbt_row_release(row);
            return result;
        }
    }
    expected_inline = directory_end;
    for (index = 0u; index < tree->schema.value_column_count; ++index) {
        const struct rbt_column *column = &tree->schema.value_columns[index];
        const unsigned char *descriptor =
            cell + RBT_LEAF_CELL_HEADER_SIZE + key_size + index * RBT_VALUE_DESCRIPTOR_SIZE;
        unsigned char kind = descriptor[0];
        uint32_t logical_size = rbt_load_u32(descriptor + 4u);
        uint64_t location = rbt_load_u64(descriptor + 8u);
        size_t scalar_size = rbt_scalar_size(column->type);

        row->value[index].type = column->type;
        if (descriptor[1] != 0u || descriptor[2] != 0u || descriptor[3] != 0u ||
            kind > RBT_VALUE_OVERFLOW) {
            result = -EBADMSG;
            break;
        }
        if (kind == RBT_VALUE_NULL) {
            if ((column->flags & RBT_COLUMN_NULLABLE) == 0u || logical_size != 0u ||
                location != 0u) {
                result = -EBADMSG;
                break;
            }
            row->value[index].is_null = true;
            continue;
        }
        row->value[index].is_null = false;
        if (scalar_size != 0u) {
            if (kind != RBT_VALUE_INLINE || logical_size != scalar_size || location > cell_size ||
                logical_size > cell_size - (size_t)location || location != expected_inline) {
                result = -EBADMSG;
                break;
            }
            if (column->type == RBT_TYPE_BOOL) {
                unsigned char boolean = cell[location];

                if (boolean > 1u) {
                    result = -EBADMSG;
                    break;
                }
                row->value[index].as.boolean = boolean != 0u;
            } else if (column->type == RBT_TYPE_I64) {
                row->value[index].as.i64 = (int64_t)rbt_load_u64(cell + location);
            } else {
                row->value[index].as.u64 = rbt_load_u64(cell + location);
            }
            expected_inline += logical_size;
            continue;
        }
        if (logical_size > column->max_size) {
            result = -EBADMSG;
            break;
        }
        if (kind == RBT_VALUE_INLINE) {
            void *copy = malloc(logical_size == 0u ? 1u : logical_size);

            if (copy == NULL) {
                result = -ENOMEM;
                break;
            }
            if (location > cell_size || logical_size > cell_size - (size_t)location ||
                location != expected_inline || logical_size > tree->inline_threshold) {
                free(copy);
                result = -EBADMSG;
                break;
            }
            memcpy(copy, cell + location, logical_size);
            row->value[index].as.bytes.data = copy;
            expected_inline += logical_size;
        } else {
            unsigned char *copy = NULL;

            if (kind != RBT_VALUE_OVERFLOW || logical_size <= tree->inline_threshold ||
                location == 0u) {
                result = -EBADMSG;
                break;
            }
            result = rbt_read_overflow_from(tree, transaction, location, index, logical_size, &copy,
                                            ownership_state, inout_overflow_pages);
            if (result != 0) {
                break;
            }
            row->value[index].as.bytes.data = copy;
        }
        row->value[index].as.bytes.size = logical_size;
        if (column->type == RBT_TYPE_UTF8 &&
            !rbt_utf8_valid(row->value[index].as.bytes.data, logical_size)) {
            result = -EBADMSG;
            break;
        }
    }
    if (result == 0 && expected_inline != cell_size) {
        result = -EBADMSG;
    }
    if (result != 0) {
        rbt_row_release(row);
        return result;
    }
    *out_row = row;
    return 0;
}

static int rbt_row_from_cell(struct rbt *tree, const unsigned char *cell, uint32_t cell_size,
                             struct rbt_row **out_row, unsigned char *ownership_state,
                             uint64_t *inout_overflow_pages) {
    return rbt_row_from_cell_source(tree, NULL, cell, cell_size, out_row, ownership_state,
                                    inout_overflow_pages);
}

static int rbt_internal_cell(const struct rbt *tree, const unsigned char *page, uint32_t index,
                             const unsigned char **out_key, uint32_t *out_key_size,
                             uint64_t *out_right_child) {
    uint32_t cell_size;
    const unsigned char *cell = rbt_slot_cell(page, index, &cell_size);

    if (cell_size <= sizeof(uint64_t) || cell_size - sizeof(uint64_t) > tree->maximum_key_size) {
        return -EBADMSG;
    }
    *out_right_child = rbt_load_u64(cell);
    *out_key = cell + sizeof(uint64_t);
    *out_key_size = cell_size - (uint32_t)sizeof(uint64_t);
    return 0;
}

static int rbt_find_leaf(struct rbt *tree, const unsigned char *key, uint32_t key_size,
                         unsigned char *page, struct rbt_page_header *out_header) {
    uint64_t page_id = tree->metadata.root_page_id;
    uint32_t level = tree->metadata.height - 1u;

    for (;;) {
        struct rbt_page_header header;
        int result = rbt_read_page(tree, page_id, page);

        if (result != 0) {
            return result;
        }
        result = rbt_decode_page_header(tree, page, page_id, &header);
        if (result != 0 || header.level != level) {
            return result != 0 ? result : -EBADMSG;
        }
        if (level == 0u) {
            if (header.type != RBT_PAGE_LEAF || header.count > tree->leaf_capacity ||
                !rbt_slots_valid(tree, page, &header)) {
                return -EBADMSG;
            }
            *out_header = header;
            return 0;
        }
        if (header.type != RBT_PAGE_INTERNAL || header.count == 0u ||
            header.count > tree->internal_capacity || header.link0 == 0u || header.link1 != 0u ||
            !rbt_slots_valid(tree, page, &header)) {
            return -EBADMSG;
        }
        {
            uint32_t first = 0u;
            uint32_t length = header.count;

            while (length != 0u) {
                uint32_t half = length / 2u;
                uint32_t middle = first + half;
                const unsigned char *separator;
                uint32_t separator_size;
                uint64_t child;

                result = rbt_internal_cell(tree, page, middle, &separator, &separator_size, &child);
                if (result != 0) {
                    return result;
                }
                if (rbt_compare_keys(separator, separator_size, key, key_size) <= 0) {
                    first = middle + 1u;
                    length -= half + 1u;
                } else {
                    length = half;
                }
            }
            if (first == 0u) {
                page_id = header.link0;
            } else {
                const unsigned char *ignored;
                uint32_t ignored_size;

                result =
                    rbt_internal_cell(tree, page, first - 1u, &ignored, &ignored_size, &page_id);
                if (result != 0) {
                    return result;
                }
            }
        }
        --level;
    }
}

static int rbt_leaf_lower_bound(const unsigned char *page, uint32_t count, const unsigned char *key,
                                uint32_t key_size, uint32_t *out_position) {
    uint32_t first = 0u;
    uint32_t length = count;

    while (length != 0u) {
        uint32_t half = length / 2u;
        uint32_t middle = first + half;
        uint32_t cell_size;
        const unsigned char *cell = rbt_slot_cell(page, middle, &cell_size);
        uint32_t stored_key_size;

        if (cell_size < RBT_LEAF_CELL_HEADER_SIZE || (stored_key_size = rbt_load_u32(cell)) == 0u ||
            stored_key_size > cell_size - RBT_LEAF_CELL_HEADER_SIZE) {
            return -EBADMSG;
        }
        if (rbt_compare_keys(cell + RBT_LEAF_CELL_HEADER_SIZE, stored_key_size, key, key_size) <
            0) {
            first = middle + 1u;
            length -= half + 1u;
        } else {
            length = half;
        }
    }
    *out_position = first;
    return 0;
}

static void rbt_metadata_encode(unsigned char *page, const struct rbt *tree,
                                const struct rbt_metadata *metadata) {
    rbt_page_initialize(page, tree->storage.page_size, 0u, RBT_PAGE_META, 0u, 0u);
    rbt_store_u32(page + RBT_PAGE_FREE_LOWER_OFFSET, RBT_META_USED_SIZE);
    rbt_store_u64(page + RBT_META_ROOT_OFFSET, metadata->root_page_id);
    rbt_store_u64(page + RBT_META_FREE_HEAD_OFFSET, metadata->free_head);
    rbt_store_u64(page + RBT_META_ITEM_COUNT_OFFSET, metadata->item_count);
    rbt_store_u64(page + RBT_META_NEXT_PAGE_OFFSET, metadata->next_page_id);
    rbt_store_u32(page + RBT_META_HEIGHT_OFFSET, metadata->height);
    rbt_store_u32(page + RBT_META_PAGE_SIZE_OFFSET, tree->storage.page_size);
    rbt_store_u64(page + RBT_META_SCHEMA_HEAD_OFFSET, metadata->schema_head);
    rbt_store_u64(page + RBT_META_SCHEMA_SIZE_OFFSET, metadata->schema_size);
    rbt_store_u64(page + RBT_META_SCHEMA_PAGE_COUNT_OFFSET, metadata->schema_page_count);
}

static void rbt_txn_destroy(struct rbt_txn *transaction) {
    size_t index;

    for (index = 0u; index < transaction->page_count; ++index) {
        free(transaction->pages[index].data);
    }
    free(transaction->page_index);
    free(transaction->pages);
}

static size_t rbt_txn_page_hash(uint64_t page_id) {
    page_id ^= page_id >> 30u;
    page_id *= UINT64_C(0xbf58476d1ce4e5b9);
    page_id ^= page_id >> 27u;
    page_id *= UINT64_C(0x94d049bb133111eb);
    page_id ^= page_id >> 31u;
    return (size_t)page_id;
}

static void rbt_txn_index_insert(size_t *page_index, size_t capacity, uint64_t page_id,
                                 size_t transaction_index) {
    size_t slot = rbt_txn_page_hash(page_id) & (capacity - 1u);

    while (page_index[slot] != 0u) {
        slot = (slot + 1u) & (capacity - 1u);
    }
    page_index[slot] = transaction_index + 1u;
}

static struct rbt_txn_page *rbt_txn_find_page(struct rbt_txn *transaction, uint64_t page_id) {
    size_t slot;
    size_t probes;

    if (transaction->page_index_capacity == 0u) {
        return NULL;
    }
    slot = rbt_txn_page_hash(page_id) & (transaction->page_index_capacity - 1u);
    for (probes = 0u; probes < transaction->page_index_capacity; ++probes) {
        size_t entry = transaction->page_index[slot];
        size_t transaction_index;

        if (entry == 0u) {
            return NULL;
        }
        transaction_index = entry - 1u;
        if (transaction_index < transaction->page_count &&
            transaction->pages[transaction_index].page_id == page_id) {
            return &transaction->pages[transaction_index];
        }
        slot = (slot + 1u) & (transaction->page_index_capacity - 1u);
    }
    return NULL;
}

static int rbt_txn_add_page(struct rbt_txn *transaction, uint64_t page_id, unsigned char *data,
                            struct rbt_txn_page **out_page) {
    struct rbt_txn_page *expanded_pages = NULL;
    size_t *expanded_index = NULL;
    size_t page_capacity = transaction->page_capacity;
    size_t page_index_capacity = transaction->page_index_capacity;
    size_t next_page_count;
    size_t index;

    if (transaction->page_count == SIZE_MAX || rbt_txn_find_page(transaction, page_id) != NULL) {
        return transaction->page_count == SIZE_MAX ? -ENOMEM : -EBADMSG;
    }
    next_page_count = transaction->page_count + 1u;
    if (transaction->page_count == transaction->page_capacity) {
        page_capacity = transaction->page_capacity == 0u ? 8u : transaction->page_capacity * 2u;
        if (page_capacity < transaction->page_capacity ||
            page_capacity > SIZE_MAX / sizeof(*expanded_pages)) {
            return -ENOMEM;
        }
        expanded_pages = calloc(page_capacity, sizeof(*expanded_pages));
        if (expanded_pages == NULL) {
            return -ENOMEM;
        }
        if (transaction->page_count != 0u) {
            memcpy(expanded_pages, transaction->pages,
                   transaction->page_count * sizeof(*expanded_pages));
        }
    }
    if (page_index_capacity == 0u) {
        page_index_capacity = 16u;
    }
    while (next_page_count > page_index_capacity / 2u) {
        if (page_index_capacity > SIZE_MAX / 2u) {
            free(expanded_pages);
            return -ENOMEM;
        }
        page_index_capacity *= 2u;
    }
    if (page_index_capacity != transaction->page_index_capacity) {
        if (page_index_capacity > SIZE_MAX / sizeof(*expanded_index)) {
            free(expanded_pages);
            return -ENOMEM;
        }
        expanded_index = calloc(page_index_capacity, sizeof(*expanded_index));
        if (expanded_index == NULL) {
            free(expanded_pages);
            return -ENOMEM;
        }
        for (index = 0u; index < transaction->page_count; ++index) {
            rbt_txn_index_insert(expanded_index, page_index_capacity,
                                 transaction->pages[index].page_id, index);
        }
    }
    if (expanded_pages != NULL) {
        free(transaction->pages);
        transaction->pages = expanded_pages;
        transaction->page_capacity = page_capacity;
    }
    if (expanded_index != NULL) {
        free(transaction->page_index);
        transaction->page_index = expanded_index;
        transaction->page_index_capacity = page_index_capacity;
    }
    *out_page = &transaction->pages[transaction->page_count];
    (*out_page)->data = data;
    (*out_page)->page_id = page_id;
    (*out_page)->dirty = false;
    rbt_txn_index_insert(transaction->page_index, transaction->page_index_capacity, page_id,
                         transaction->page_count);
    ++transaction->page_count;
    return 0;
}

static int rbt_txn_load_page(struct rbt_txn *transaction, uint64_t page_id,
                             unsigned char **out_page, struct rbt_page_header *out_header) {
    struct rbt_txn_page *transaction_page = rbt_txn_find_page(transaction, page_id);
    int result;

    if (page_id == 0u || page_id >= transaction->metadata.next_page_id) {
        return -EBADMSG;
    }
    if (transaction_page == NULL) {
        unsigned char *data;

        if (page_id >= transaction->tree->metadata.next_page_id) {
            return -EBADMSG;
        }
        data = malloc((size_t)transaction->tree->storage.page_size);
        if (data == NULL) {
            return -ENOMEM;
        }
        result = rbt_read_page(transaction->tree, page_id, data);
        if (result == 0) {
            result = rbt_txn_add_page(transaction, page_id, data, &transaction_page);
        }
        if (result != 0) {
            free(data);
            return result;
        }
    }
    if (transaction_page->dirty) {
        result = rbt_decode_page_header_fields(transaction->tree, transaction_page->data, page_id,
                                               out_header);
    } else {
        result =
            rbt_decode_page_header(transaction->tree, transaction_page->data, page_id, out_header);
    }
    if (result != 0) {
        return result;
    }
    *out_page = transaction_page->data;
    return 0;
}

static int rbt_txn_load_typed_page(struct rbt_txn *transaction, uint64_t page_id,
                                   uint8_t expected_type, unsigned char **out_page,
                                   struct rbt_page_header *out_header) {
    int result = rbt_txn_load_page(transaction, page_id, out_page, out_header);

    if (result != 0) {
        return result;
    }
    return out_header->type == expected_type ? 0 : -EBADMSG;
}

static int rbt_txn_mark_dirty(struct rbt_txn *transaction, uint64_t page_id) {
    struct rbt_txn_page *page = rbt_txn_find_page(transaction, page_id);

    if (page == NULL) {
        return -EBADMSG;
    }
    page->dirty = true;
    return 0;
}

static int rbt_txn_new_page(struct rbt_txn *transaction, uint64_t page_id,
                            unsigned char **out_page) {
    struct rbt_txn_page *page;
    unsigned char *data;
    int result;

    if (rbt_txn_find_page(transaction, page_id) != NULL) {
        return -EBADMSG;
    }
    data = malloc((size_t)transaction->tree->storage.page_size);
    if (data == NULL) {
        return -ENOMEM;
    }
    result = rbt_txn_add_page(transaction, page_id, data, &page);
    if (result != 0) {
        free(data);
        return result;
    }
    page->dirty = true;
    *out_page = page->data;
    return 0;
}

static int rbt_txn_allocate_page(struct rbt_txn *transaction, uint8_t type, uint32_t level,
                                 uint64_t *out_page_id, unsigned char **out_page) {
    uint64_t page_id;
    unsigned char *page;
    int result;

    if (transaction->metadata.free_head != 0u) {
        struct rbt_page_header header;

        page_id = transaction->metadata.free_head;
        result = rbt_txn_load_typed_page(transaction, page_id, RBT_PAGE_FREE, &page, &header);
        if (result != 0) {
            return result;
        }
        if (header.count != 0u || header.level != 0u || header.free_lower != RBT_PAGE_HEADER_SIZE ||
            header.free_upper != transaction->tree->storage.page_size || header.link1 != 0u ||
            header.aux != 0u ||
            (header.link0 != 0u &&
             (header.link0 >= transaction->metadata.next_page_id || header.link0 == page_id))) {
            return -EBADMSG;
        }
        transaction->metadata.free_head = header.link0;
        result = rbt_txn_mark_dirty(transaction, page_id);
        if (result != 0) {
            return result;
        }
    } else {
        if (transaction->metadata.next_page_id == UINT64_MAX) {
            return -ENOMEM;
        }
        page_id = transaction->metadata.next_page_id++;
        result = rbt_txn_new_page(transaction, page_id, &page);
        if (result != 0) {
            --transaction->metadata.next_page_id;
            return result;
        }
    }
    rbt_page_initialize(page, transaction->tree->storage.page_size, page_id, type, 0u, level);
    *out_page_id = page_id;
    *out_page = page;
    return 0;
}

static int rbt_txn_free_page(struct rbt_txn *transaction, uint64_t page_id) {
    unsigned char *page;
    struct rbt_page_header header;
    int result = rbt_txn_load_page(transaction, page_id, &page, &header);

    if (result != 0) {
        return result;
    }
    if (header.type == RBT_PAGE_META || header.type == RBT_PAGE_SCHEMA ||
        header.type == RBT_PAGE_FREE) {
        return -EBADMSG;
    }
    rbt_page_initialize(page, transaction->tree->storage.page_size, page_id, RBT_PAGE_FREE, 0u, 0u);
    rbt_store_u64(page + RBT_PAGE_LINK0_OFFSET, transaction->metadata.free_head);
    transaction->metadata.free_head = page_id;
    return rbt_txn_mark_dirty(transaction, page_id);
}

static int rbt_txn_commit(struct rbt_txn *transaction) {
    unsigned char *metadata_page;
    struct rbt_page_update *updates;
    size_t dirty_count = 0u;
    size_t update_index = 1u;
    size_t index;
    int result;

    for (index = 0u; index < transaction->page_count; ++index) {
        if (transaction->pages[index].dirty) {
            ++dirty_count;
        }
    }
    if (dirty_count == 0u || dirty_count == SIZE_MAX) {
        return dirty_count == 0u ? -EINVAL : -ENOMEM;
    }
    metadata_page = malloc((size_t)transaction->tree->storage.page_size);
    updates = malloc((dirty_count + 1u) * sizeof(*updates));
    if (metadata_page == NULL || updates == NULL) {
        free(metadata_page);
        free(updates);
        return -ENOMEM;
    }
    rbt_metadata_encode(metadata_page, transaction->tree, &transaction->metadata);
    rbt_page_checksum_store(metadata_page, transaction->tree->storage.page_size);
    updates[0].page_id = 0u;
    updates[0].data = metadata_page;
    for (index = 0u; index < transaction->page_count; ++index) {
        if (!transaction->pages[index].dirty) {
            continue;
        }
        rbt_page_checksum_store(transaction->pages[index].data,
                                transaction->tree->storage.page_size);
        updates[update_index].page_id = transaction->pages[index].page_id;
        updates[update_index].data = transaction->pages[index].data;
        ++update_index;
    }
    result = rbt_commit(transaction->tree, updates, dirty_count + 1u);
    if (result == 0) {
        transaction->tree->metadata = transaction->metadata;
    }
    free(updates);
    free(metadata_page);
    return result;
}

static void rbt_cell_images_destroy(struct rbt_cell_image *cells, uint32_t count) {
    uint32_t index;

    if (cells == NULL) {
        return;
    }
    for (index = 0u; index < count; ++index) {
        free(cells[index].data);
    }
    free(cells);
}

static int rbt_cell_image_copy(struct rbt_cell_image *destination, const unsigned char *data,
                               uint32_t size) {
    destination->data = malloc(size);
    if (destination->data == NULL) {
        return -ENOMEM;
    }
    memcpy(destination->data, data, size);
    destination->size = size;
    return 0;
}

static int rbt_leaf_collect(const struct rbt *tree, const unsigned char *page,
                            const struct rbt_page_header *header, uint32_t extra,
                            struct rbt_cell_image **out_cells) {
    struct rbt_cell_image *cells;
    uint32_t capacity;
    uint32_t index;

    if (header->type != RBT_PAGE_LEAF || header->level != 0u ||
        header->count > tree->leaf_capacity || !rbt_slots_valid(tree, page, header) ||
        extra > UINT32_MAX - header->count) {
        return -EBADMSG;
    }
    capacity = header->count + extra;
    cells = calloc(capacity == 0u ? 1u : capacity, sizeof(*cells));
    if (cells == NULL) {
        return -ENOMEM;
    }
    for (index = 0u; index < header->count; ++index) {
        uint32_t size;
        const unsigned char *cell = rbt_slot_cell(page, index, &size);
        int result = rbt_cell_image_copy(&cells[index], cell, size);

        if (result != 0) {
            rbt_cell_images_destroy(cells, index);
            return result;
        }
    }
    *out_cells = cells;
    return 0;
}

static int rbt_leaf_encode(const struct rbt *tree, unsigned char *page, uint64_t page_id,
                           const struct rbt_cell_image *cells, uint32_t count, uint64_t previous,
                           uint64_t next) {
    uint32_t index;

    if (count > tree->leaf_capacity) {
        return -EINVAL;
    }
    rbt_page_initialize(page, tree->storage.page_size, page_id, RBT_PAGE_LEAF, 0u, 0u);
    rbt_store_u64(page + RBT_PAGE_LINK0_OFFSET, next);
    rbt_store_u64(page + RBT_PAGE_LINK1_OFFSET, previous);
    for (index = 0u; index < count; ++index) {
        if (!rbt_page_add_cell(page, cells[index].data, cells[index].size)) {
            return -EINVAL;
        }
    }
    return 0;
}

static int rbt_leaf_cell_key(const struct rbt_cell_image *cell, const unsigned char **out_key,
                             uint32_t *out_key_size) {
    uint32_t key_size;

    if (cell->size < RBT_LEAF_CELL_HEADER_SIZE || (key_size = rbt_load_u32(cell->data)) == 0u ||
        key_size > cell->size - RBT_LEAF_CELL_HEADER_SIZE) {
        return -EBADMSG;
    }
    *out_key = cell->data + RBT_LEAF_CELL_HEADER_SIZE;
    *out_key_size = key_size;
    return 0;
}

static int rbt_txn_build_overflow(struct rbt_txn *transaction, uint64_t column,
                                  const unsigned char *data, uint32_t size, uint64_t *out_first) {
    uint32_t payload = transaction->tree->storage.page_size - RBT_PAGE_HEADER_SIZE;
    uint32_t offset = 0u;
    unsigned char *previous = NULL;

    *out_first = 0u;
    while (offset < size) {
        uint32_t chunk = size - offset < payload ? size - offset : payload;
        uint64_t page_id;
        unsigned char *page;
        int result = rbt_txn_allocate_page(transaction, RBT_PAGE_OVERFLOW, 0u, &page_id, &page);

        if (result != 0) {
            return result;
        }
        if (*out_first == 0u) {
            *out_first = page_id;
        }
        if (previous != NULL) {
            rbt_store_u64(previous + RBT_PAGE_LINK0_OFFSET, page_id);
        }
        rbt_store_u32(page + RBT_PAGE_COUNT_OFFSET, chunk);
        rbt_store_u32(page + RBT_PAGE_FREE_LOWER_OFFSET, RBT_PAGE_HEADER_SIZE + chunk);
        rbt_store_u64(page + RBT_PAGE_AUX_OFFSET, column);
        memcpy(page + RBT_PAGE_HEADER_SIZE, data + offset, chunk);
        previous = page;
        offset += chunk;
    }
    return 0;
}

static int rbt_txn_build_leaf_cell(struct rbt_txn *transaction, const unsigned char *key,
                                   uint32_t key_size, const struct rbt_record *record,
                                   const struct rbt_cell_image *existing_cell,
                                   const struct rbt_value *existing_values,
                                   struct rbt_cell_image *out_cell) {
    const struct rbt *tree = transaction->tree;
    size_t directory_size = tree->schema.value_column_count * RBT_VALUE_DESCRIPTOR_SIZE;
    size_t size = RBT_LEAF_CELL_HEADER_SIZE + key_size + directory_size;
    const unsigned char *existing_descriptors = NULL;
    size_t inline_offset;
    size_t index;
    unsigned char *cell;

    if (existing_cell != NULL) {
        if (existing_cell->data == NULL || existing_cell->size < RBT_LEAF_CELL_HEADER_SIZE) {
            return -EBADMSG;
        }
        uint32_t existing_key_size = rbt_load_u32(existing_cell->data);

        if (existing_key_size > existing_cell->size - RBT_LEAF_CELL_HEADER_SIZE ||
            RBT_LEAF_CELL_HEADER_SIZE + existing_key_size + directory_size > existing_cell->size) {
            return -EBADMSG;
        }
        existing_descriptors = existing_cell->data + RBT_LEAF_CELL_HEADER_SIZE + existing_key_size;
    }
    for (index = 0u; index < tree->schema.value_column_count; ++index) {
        const struct rbt_value *value = &record->value[index];
        size_t field_size = rbt_scalar_size(value->type);

        if (!value->is_null &&
            (field_size != 0u || value->as.bytes.size <= tree->inline_threshold)) {
            size += field_size != 0u ? field_size : value->as.bytes.size;
        }
    }
    if (size > UINT32_MAX) {
        return -ENOMEM;
    }
    cell = calloc(1u, size);
    if (cell == NULL) {
        return -ENOMEM;
    }
    rbt_store_u32(cell, key_size);
    rbt_store_u16(cell + 4u, (uint16_t)tree->schema.value_column_count);
    memcpy(cell + RBT_LEAF_CELL_HEADER_SIZE, key, key_size);
    inline_offset = RBT_LEAF_CELL_HEADER_SIZE + key_size + directory_size;
    for (index = 0u; index < tree->schema.value_column_count; ++index) {
        const struct rbt_value *value = &record->value[index];
        unsigned char *descriptor =
            cell + RBT_LEAF_CELL_HEADER_SIZE + key_size + index * RBT_VALUE_DESCRIPTOR_SIZE;
        size_t field_size = rbt_scalar_size(value->type);
        uint32_t logical_size;

        if (existing_values != NULL && rbt_values_equal(&existing_values[index], value, 1u) &&
            existing_descriptors[index * RBT_VALUE_DESCRIPTOR_SIZE] == RBT_VALUE_OVERFLOW) {
            memcpy(descriptor, existing_descriptors + index * RBT_VALUE_DESCRIPTOR_SIZE,
                   RBT_VALUE_DESCRIPTOR_SIZE);
            continue;
        }
        if (existing_descriptors != NULL &&
            existing_descriptors[index * RBT_VALUE_DESCRIPTOR_SIZE] == RBT_VALUE_OVERFLOW) {
            const unsigned char *existing_descriptor =
                existing_descriptors + index * RBT_VALUE_DESCRIPTOR_SIZE;
            int result = rbt_txn_free_overflow(transaction, rbt_load_u64(existing_descriptor + 8u),
                                               index, rbt_load_u32(existing_descriptor + 4u));

            if (result != 0) {
                free(cell);
                return result;
            }
        }
        if (value->is_null) {
            descriptor[0] = RBT_VALUE_NULL;
            continue;
        }
        logical_size = (uint32_t)(field_size != 0u ? field_size : value->as.bytes.size);
        rbt_store_u32(descriptor + 4u, logical_size);
        if (field_size != 0u || logical_size <= tree->inline_threshold) {
            descriptor[0] = RBT_VALUE_INLINE;
            rbt_store_u64(descriptor + 8u, (uint64_t)inline_offset);
            if (value->type == RBT_TYPE_BOOL) {
                cell[inline_offset] = value->as.boolean ? 1u : 0u;
            } else if (value->type == RBT_TYPE_I64) {
                rbt_store_u64(cell + inline_offset, (uint64_t)value->as.i64);
            } else if (value->type == RBT_TYPE_U64) {
                rbt_store_u64(cell + inline_offset, value->as.u64);
            } else {
                memcpy(cell + inline_offset, value->as.bytes.data, logical_size);
            }
            inline_offset += logical_size;
        } else {
            uint64_t first_page;
            int result = rbt_txn_build_overflow(transaction, index, value->as.bytes.data,
                                                logical_size, &first_page);

            if (result != 0) {
                free(cell);
                return result;
            }
            descriptor[0] = RBT_VALUE_OVERFLOW;
            rbt_store_u64(descriptor + 8u, first_page);
        }
    }
    out_cell->data = cell;
    out_cell->size = (uint32_t)size;
    return 0;
}

static int rbt_txn_free_overflow(struct rbt_txn *transaction, uint64_t first_page, uint64_t column,
                                 uint32_t logical_size) {
    uint64_t page_id = first_page;
    uint64_t visited = 0u;
    uint32_t consumed = 0u;

    while (consumed < logical_size) {
        unsigned char *page;
        struct rbt_page_header header;
        uint64_t next;
        int result;

        if (page_id == 0u || visited++ >= transaction->metadata.next_page_id) {
            return -EBADMSG;
        }
        result = rbt_txn_load_typed_page(transaction, page_id, RBT_PAGE_OVERFLOW, &page, &header);
        if (result != 0) {
            return result;
        }
        if (header.level != 0u || header.link1 != 0u || header.aux != column ||
            header.count == 0u || header.count > logical_size - consumed ||
            header.count > transaction->tree->storage.page_size - RBT_PAGE_HEADER_SIZE ||
            header.free_lower != RBT_PAGE_HEADER_SIZE + header.count ||
            header.free_upper != transaction->tree->storage.page_size) {
            return -EBADMSG;
        }
        next = header.link0;
        consumed += header.count;
        result = rbt_txn_free_page(transaction, page_id);
        if (result != 0) {
            return result;
        }
        page_id = next;
    }
    return consumed == logical_size && page_id == 0u ? 0 : -EBADMSG;
}

static int rbt_txn_free_cell_overflow(struct rbt_txn *transaction,
                                      const struct rbt_cell_image *cell) {
    const struct rbt *tree = transaction->tree;
    uint32_t key_size;
    uint16_t value_count;
    size_t directory_end;
    size_t index;

    if (cell->size < RBT_LEAF_CELL_HEADER_SIZE) {
        return -EBADMSG;
    }
    key_size = rbt_load_u32(cell->data);
    value_count = rbt_load_u16(cell->data + 4u);
    directory_end =
        RBT_LEAF_CELL_HEADER_SIZE + key_size + (size_t)value_count * RBT_VALUE_DESCRIPTOR_SIZE;
    if (key_size == 0u || value_count != tree->schema.value_column_count ||
        directory_end > cell->size) {
        return -EBADMSG;
    }
    for (index = 0u; index < tree->schema.value_column_count; ++index) {
        const unsigned char *descriptor =
            cell->data + RBT_LEAF_CELL_HEADER_SIZE + key_size + index * RBT_VALUE_DESCRIPTOR_SIZE;

        if (descriptor[0] == RBT_VALUE_OVERFLOW) {
            uint32_t logical_size = rbt_load_u32(descriptor + 4u);
            uint64_t first_page = rbt_load_u64(descriptor + 8u);
            int result = rbt_txn_free_overflow(transaction, first_page, index, logical_size);

            if (result != 0) {
                return result;
            }
        }
    }
    return 0;
}

static void rbt_internal_image_destroy(struct rbt_internal_image *image) {
    rbt_cell_images_destroy(image->keys, image->count);
    free(image->children);
    memset(image, 0, sizeof(*image));
}

static int rbt_internal_collect(const struct rbt *tree, const unsigned char *page,
                                const struct rbt_page_header *header, uint32_t extra,
                                struct rbt_internal_image *out_image) {
    uint32_t capacity;
    uint32_t index;

    memset(out_image, 0, sizeof(*out_image));
    if (header->type != RBT_PAGE_INTERNAL || header->count > tree->internal_capacity ||
        header->link0 == 0u || header->link1 != 0u || !rbt_slots_valid(tree, page, header) ||
        extra > UINT32_MAX - header->count) {
        return -EBADMSG;
    }
    capacity = header->count + extra;
    out_image->children = calloc((size_t)capacity + 1u, sizeof(*out_image->children));
    out_image->keys = calloc(capacity == 0u ? 1u : capacity, sizeof(*out_image->keys));
    if (out_image->children == NULL || out_image->keys == NULL) {
        rbt_internal_image_destroy(out_image);
        return -ENOMEM;
    }
    out_image->count = header->count;
    out_image->capacity = capacity;
    out_image->children[0] = header->link0;
    for (index = 0u; index < header->count; ++index) {
        const unsigned char *key;
        uint32_t key_size;
        uint64_t child;
        int result = rbt_internal_cell(tree, page, index, &key, &key_size, &child);

        if (result == 0) {
            result = rbt_cell_image_copy(&out_image->keys[index], key, key_size);
        }
        if (result != 0) {
            out_image->count = index;
            rbt_internal_image_destroy(out_image);
            return result;
        }
        out_image->children[index + 1u] = child;
    }
    return 0;
}

static int rbt_internal_encode(const struct rbt *tree, unsigned char *page, uint64_t page_id,
                               uint32_t level, const struct rbt_internal_image *image) {
    uint32_t index;

    if (image->count > tree->internal_capacity || image->children[0] == 0u) {
        return -EINVAL;
    }
    rbt_page_initialize(page, tree->storage.page_size, page_id, RBT_PAGE_INTERNAL, 0u, level);
    rbt_store_u64(page + RBT_PAGE_LINK0_OFFSET, image->children[0]);
    for (index = 0u; index < image->count; ++index) {
        uint32_t cell_size;
        unsigned char *cell;

        if (image->keys[index].size == 0u || image->keys[index].size > tree->maximum_key_size ||
            image->keys[index].size > UINT32_MAX - (uint32_t)sizeof(uint64_t) ||
            image->children[index + 1u] == 0u) {
            return -EINVAL;
        }
        cell_size = (uint32_t)sizeof(uint64_t) + image->keys[index].size;
        cell = malloc(cell_size);
        if (cell == NULL) {
            return -ENOMEM;
        }
        rbt_store_u64(cell, image->children[index + 1u]);
        memcpy(cell + sizeof(uint64_t), image->keys[index].data, image->keys[index].size);
        if (!rbt_page_add_cell(page, cell, cell_size)) {
            free(cell);
            return -EINVAL;
        }
        free(cell);
    }
    return 0;
}

static int rbt_internal_replace_key(struct rbt_internal_image *image, uint32_t index,
                                    const unsigned char *key, uint32_t key_size) {
    unsigned char *copy;

    if (index >= image->count) {
        return -EBADMSG;
    }
    copy = malloc(key_size);
    if (copy == NULL) {
        return -ENOMEM;
    }
    memcpy(copy, key, key_size);
    free(image->keys[index].data);
    image->keys[index].data = copy;
    image->keys[index].size = key_size;
    return 0;
}

static int rbt_internal_insert_image(struct rbt_internal_image *image, uint32_t child_index,
                                     const unsigned char *separator, uint32_t separator_size,
                                     uint64_t right_child) {
    unsigned char *copy;

    if (child_index > image->count || image->count == image->capacity || right_child == 0u) {
        return -EBADMSG;
    }
    copy = malloc(separator_size);
    if (copy == NULL) {
        return -ENOMEM;
    }
    memcpy(copy, separator, separator_size);
    memmove(&image->keys[child_index + 1u], &image->keys[child_index],
            (size_t)(image->count - child_index) * sizeof(*image->keys));
    memmove(&image->children[child_index + 2u], &image->children[child_index + 1u],
            (size_t)(image->count - child_index) * sizeof(*image->children));
    image->keys[child_index].data = copy;
    image->keys[child_index].size = separator_size;
    image->children[child_index + 1u] = right_child;
    ++image->count;
    return 0;
}

static int rbt_internal_prepend_image(struct rbt_internal_image *image,
                                      const unsigned char *separator, uint32_t separator_size,
                                      uint64_t left_child) {
    unsigned char *copy;

    if (image->count == image->capacity || left_child == 0u) {
        return -EBADMSG;
    }
    copy = malloc(separator_size);
    if (copy == NULL) {
        return -ENOMEM;
    }
    memcpy(copy, separator, separator_size);
    memmove(&image->keys[1], &image->keys[0], (size_t)image->count * sizeof(*image->keys));
    memmove(&image->children[1], &image->children[0],
            ((size_t)image->count + 1u) * sizeof(*image->children));
    image->keys[0].data = copy;
    image->keys[0].size = separator_size;
    image->children[0] = left_child;
    ++image->count;
    return 0;
}

static void rbt_internal_remove_image(struct rbt_internal_image *image, uint32_t key_index) {
    free(image->keys[key_index].data);
    memmove(&image->keys[key_index], &image->keys[key_index + 1u],
            (size_t)(image->count - key_index - 1u) * sizeof(*image->keys));
    memmove(&image->children[key_index + 1u], &image->children[key_index + 2u],
            (size_t)(image->count - key_index - 1u) * sizeof(*image->children));
    --image->count;
    memset(&image->keys[image->count], 0, sizeof(*image->keys));
}

static void rbt_internal_remove_first_child(struct rbt_internal_image *image) {
    free(image->keys[0].data);
    memmove(&image->keys[0], &image->keys[1], (size_t)(image->count - 1u) * sizeof(*image->keys));
    memmove(&image->children[0], &image->children[1],
            (size_t)image->count * sizeof(*image->children));
    --image->count;
    memset(&image->keys[image->count], 0, sizeof(*image->keys));
}

static int rbt_internal_append_image(struct rbt_internal_image *left,
                                     const unsigned char *separator, uint32_t separator_size,
                                     const struct rbt_internal_image *right) {
    uint32_t old_count = left->count;
    uint32_t index;
    int result;

    result =
        rbt_internal_insert_image(left, old_count, separator, separator_size, right->children[0]);
    if (result != 0) {
        return result;
    }
    for (index = 0u; index < right->count; ++index) {
        result = rbt_internal_insert_image(left, left->count, right->keys[index].data,
                                           right->keys[index].size, right->children[index + 1u]);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

static int rbt_internal_child_index(const struct rbt *tree, const unsigned char *page,
                                    uint32_t count, const unsigned char *key, uint32_t key_size,
                                    uint32_t *out_index) {
    uint32_t first = 0u;
    uint32_t length = count;

    while (length != 0u) {
        uint32_t half = length / 2u;
        uint32_t middle = first + half;
        const unsigned char *separator;
        uint32_t separator_size;
        uint64_t ignored;
        int result = rbt_internal_cell(tree, page, middle, &separator, &separator_size, &ignored);

        if (result != 0) {
            return result;
        }
        if (rbt_compare_keys(separator, separator_size, key, key_size) <= 0) {
            first = middle + 1u;
            length -= half + 1u;
        } else {
            length = half;
        }
    }
    *out_index = first;
    return 0;
}

static int rbt_txn_find_path(struct rbt_txn *transaction, const unsigned char *key,
                             uint32_t key_size, struct rbt_path *path, unsigned char **out_leaf,
                             struct rbt_page_header *out_header) {
    uint64_t page_id = transaction->metadata.root_page_id;
    uint32_t expected_level = transaction->metadata.height - 1u;

    memset(path, 0, sizeof(*path));
    for (;;) {
        unsigned char *page;
        struct rbt_page_header header;
        int result = rbt_txn_load_page(transaction, page_id, &page, &header);

        if (result != 0) {
            return result;
        }
        if (header.level != expected_level) {
            return -EBADMSG;
        }
        if (expected_level == 0u) {
            if (header.type != RBT_PAGE_LEAF || header.count > transaction->tree->leaf_capacity ||
                !rbt_slots_valid(transaction->tree, page, &header)) {
                return -EBADMSG;
            }
            path->leaf_page_id = page_id;
            *out_leaf = page;
            *out_header = header;
            return 0;
        }
        if (header.type != RBT_PAGE_INTERNAL || header.count == 0u ||
            header.count > transaction->tree->internal_capacity || header.link0 == 0u ||
            header.link1 != 0u || !rbt_slots_valid(transaction->tree, page, &header) ||
            path->depth >= sizeof(path->entries) / sizeof(path->entries[0])) {
            return -EBADMSG;
        }
        path->entries[path->depth].page_id = page_id;
        result = rbt_internal_child_index(transaction->tree, page, header.count, key, key_size,
                                          &path->entries[path->depth].child_index);
        if (result != 0) {
            return result;
        }
        if (path->entries[path->depth].child_index == 0u) {
            page_id = header.link0;
        } else {
            const unsigned char *ignored;
            uint32_t ignored_size;

            result = rbt_internal_cell(transaction->tree, page,
                                       path->entries[path->depth].child_index - 1u, &ignored,
                                       &ignored_size, &page_id);
            if (result != 0) {
                return result;
            }
        }
        ++path->depth;
        --expected_level;
    }
}

static int rbt_txn_propagate_minimum(struct rbt_txn *transaction, const struct rbt_path *path,
                                     const unsigned char *minimum, uint32_t minimum_size) {
    size_t depth = path->depth;

    while (depth != 0u) {
        const struct rbt_path_entry *entry = &path->entries[depth - 1u];

        if (entry->child_index != 0u) {
            unsigned char *parent;
            struct rbt_page_header header;
            struct rbt_internal_image image;
            uint32_t key_index = entry->child_index - 1u;
            int result = rbt_txn_load_typed_page(transaction, entry->page_id, RBT_PAGE_INTERNAL,
                                                 &parent, &header);

            if (result != 0) {
                return result;
            }
            result = rbt_internal_collect(transaction->tree, parent, &header, 0u, &image);
            if (result != 0) {
                return result;
            }
            if (key_index >= image.count) {
                rbt_internal_image_destroy(&image);
                return -EBADMSG;
            }
            if (rbt_compare_keys(image.keys[key_index].data, image.keys[key_index].size, minimum,
                                 minimum_size) != 0) {
                result = rbt_internal_replace_key(&image, key_index, minimum, minimum_size);
                if (result == 0) {
                    result = rbt_internal_encode(transaction->tree, parent, entry->page_id,
                                                 header.level, &image);
                }
                if (result == 0) {
                    result = rbt_txn_mark_dirty(transaction, entry->page_id);
                }
            }
            rbt_internal_image_destroy(&image);
            return result;
        }
        --depth;
    }
    return 0;
}

static int rbt_txn_split_internal(struct rbt_txn *transaction, uint64_t page_id,
                                  unsigned char *page, const struct rbt_page_header *header,
                                  uint32_t child_index, const unsigned char *separator,
                                  uint32_t separator_size, uint64_t right_child,
                                  struct rbt_cell_image *out_promoted,
                                  uint64_t *out_right_page_id) {
    struct rbt_internal_image image;
    struct rbt_internal_image left;
    struct rbt_internal_image right;
    uint32_t middle;
    uint32_t index;
    uint64_t right_page_id;
    unsigned char *right_page;
    int result;

    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    result = rbt_internal_collect(transaction->tree, page, header, 1u, &image);
    if (result != 0) {
        return result;
    }
    result = rbt_internal_insert_image(&image, child_index, separator, separator_size, right_child);
    if (result != 0) {
        rbt_internal_image_destroy(&image);
        return result;
    }
    middle = image.count / 2u;
    result = rbt_cell_image_copy(out_promoted, image.keys[middle].data, image.keys[middle].size);
    if (result != 0) {
        rbt_internal_image_destroy(&image);
        return result;
    }
    left.capacity = middle;
    left.count = middle;
    left.children = calloc((size_t)left.capacity + 1u, sizeof(*left.children));
    left.keys = calloc(left.capacity == 0u ? 1u : left.capacity, sizeof(*left.keys));
    right.count = image.count - middle - 1u;
    right.capacity = right.count;
    right.children = calloc((size_t)right.capacity + 1u, sizeof(*right.children));
    right.keys = calloc(right.capacity == 0u ? 1u : right.capacity, sizeof(*right.keys));
    if (left.children == NULL || left.keys == NULL || right.children == NULL ||
        right.keys == NULL) {
        result = -ENOMEM;
        goto cleanup;
    }
    for (index = 0u; index <= left.count; ++index) {
        left.children[index] = image.children[index];
    }
    for (index = 0u; index < left.count; ++index) {
        result =
            rbt_cell_image_copy(&left.keys[index], image.keys[index].data, image.keys[index].size);
        if (result != 0) {
            left.count = index;
            goto cleanup;
        }
    }
    for (index = 0u; index <= right.count; ++index) {
        right.children[index] = image.children[middle + 1u + index];
    }
    for (index = 0u; index < right.count; ++index) {
        result = rbt_cell_image_copy(&right.keys[index], image.keys[middle + 1u + index].data,
                                     image.keys[middle + 1u + index].size);
        if (result != 0) {
            right.count = index;
            goto cleanup;
        }
    }
    result = rbt_txn_allocate_page(transaction, RBT_PAGE_INTERNAL, header->level, &right_page_id,
                                   &right_page);
    if (result == 0) {
        result = rbt_internal_encode(transaction->tree, page, page_id, header->level, &left);
    }
    if (result == 0) {
        result = rbt_internal_encode(transaction->tree, right_page, right_page_id, header->level,
                                     &right);
    }
    if (result == 0) {
        result = rbt_txn_mark_dirty(transaction, page_id);
    }
    if (result == 0) {
        *out_right_page_id = right_page_id;
    }

cleanup:
    if (result != 0) {
        free(out_promoted->data);
        memset(out_promoted, 0, sizeof(*out_promoted));
    }
    rbt_internal_image_destroy(&right);
    rbt_internal_image_destroy(&left);
    rbt_internal_image_destroy(&image);
    return result;
}

static int rbt_txn_insert_parent(struct rbt_txn *transaction, const struct rbt_path *path,
                                 const unsigned char *separator, uint32_t separator_size,
                                 uint64_t right_child) {
    struct rbt_cell_image current_separator = {0};
    size_t depth = path->depth;
    int result;

    result = rbt_cell_image_copy(&current_separator, separator, separator_size);
    if (result != 0) {
        return result;
    }
    while (depth != 0u) {
        const struct rbt_path_entry *entry = &path->entries[depth - 1u];
        unsigned char *parent;
        struct rbt_page_header header;

        result = rbt_txn_load_typed_page(transaction, entry->page_id, RBT_PAGE_INTERNAL, &parent,
                                         &header);
        if (result != 0) {
            goto cleanup;
        }
        if (header.count > transaction->tree->internal_capacity ||
            entry->child_index > header.count) {
            result = -EBADMSG;
            goto cleanup;
        }
        if (header.count < transaction->tree->internal_capacity) {
            struct rbt_internal_image image;

            result = rbt_internal_collect(transaction->tree, parent, &header, 1u, &image);
            if (result == 0) {
                result =
                    rbt_internal_insert_image(&image, entry->child_index, current_separator.data,
                                              current_separator.size, right_child);
            }
            if (result == 0) {
                result = rbt_internal_encode(transaction->tree, parent, entry->page_id,
                                             header.level, &image);
            }
            if (result == 0) {
                result = rbt_txn_mark_dirty(transaction, entry->page_id);
            }
            rbt_internal_image_destroy(&image);
            goto cleanup;
        }
        {
            struct rbt_cell_image promoted = {0};

            result = rbt_txn_split_internal(transaction, entry->page_id, parent, &header,
                                            entry->child_index, current_separator.data,
                                            current_separator.size, right_child, &promoted,
                                            &right_child);
            if (result != 0) {
                goto cleanup;
            }
            free(current_separator.data);
            current_separator = promoted;
        }
        --depth;
    }
    {
        struct rbt_internal_image root;
        uint64_t root_page_id;
        unsigned char *root_page;

        if (transaction->metadata.height == UINT32_MAX) {
            result = -ENOMEM;
            goto cleanup;
        }
        memset(&root, 0, sizeof(root));
        root.capacity = 1u;
        root.children = calloc(2u, sizeof(*root.children));
        root.keys = calloc(1u, sizeof(*root.keys));
        if (root.children == NULL || root.keys == NULL) {
            rbt_internal_image_destroy(&root);
            result = -ENOMEM;
            goto cleanup;
        }
        root.children[0] = transaction->metadata.root_page_id;
        result = rbt_internal_insert_image(&root, 0u, current_separator.data,
                                           current_separator.size, right_child);
        if (result == 0) {
            result = rbt_txn_allocate_page(transaction, RBT_PAGE_INTERNAL,
                                           transaction->metadata.height, &root_page_id, &root_page);
        }
        if (result == 0) {
            result = rbt_internal_encode(transaction->tree, root_page, root_page_id,
                                         transaction->metadata.height, &root);
        }
        if (result == 0) {
            transaction->metadata.root_page_id = root_page_id;
            ++transaction->metadata.height;
        }
        rbt_internal_image_destroy(&root);
    }

cleanup:
    free(current_separator.data);
    return result;
}

static bool rbt_internal_header_valid(const struct rbt_txn *transaction,
                                      const struct rbt_page_header *header, uint32_t level) {
    return header->type == RBT_PAGE_INTERNAL && header->level == level &&
           header->count <= transaction->tree->internal_capacity && header->link0 != 0u &&
           header->link1 == 0u;
}

static bool rbt_leaf_header_valid(const struct rbt_txn *transaction,
                                  const struct rbt_page_header *header) {
    return header->type == RBT_PAGE_LEAF && header->level == 0u &&
           header->count <= transaction->tree->leaf_capacity;
}

static int rbt_txn_rebalance_internal(struct rbt_txn *transaction, const struct rbt_path *path,
                                      size_t node_position) {
    const struct rbt *tree = transaction->tree;
    uint32_t minimum_count = ((tree->internal_capacity + 2u) / 2u) - 1u;

    for (;;) {
        uint64_t node_page_id = path->entries[node_position].page_id;
        unsigned char *node;
        struct rbt_page_header node_header;
        int result = rbt_txn_load_typed_page(transaction, node_page_id, RBT_PAGE_INTERNAL, &node,
                                             &node_header);

        if (result != 0) {
            return result;
        }
        if (!rbt_internal_header_valid(transaction, &node_header, node_header.level)) {
            return -EBADMSG;
        }
        if (node_position == 0u) {
            if (node_header.count == 0u) {
                if (transaction->metadata.height <= 1u) {
                    return -EBADMSG;
                }
                transaction->metadata.root_page_id = node_header.link0;
                --transaction->metadata.height;
                return rbt_txn_free_page(transaction, node_page_id);
            }
            return 0;
        }
        if (node_header.count >= minimum_count) {
            return 0;
        }
        {
            const struct rbt_path_entry *grand_entry = &path->entries[node_position - 1u];
            uint32_t node_index = grand_entry->child_index;
            unsigned char *grand;
            struct rbt_page_header grand_header;
            struct rbt_internal_image grand_image;

            result = rbt_txn_load_typed_page(transaction, grand_entry->page_id, RBT_PAGE_INTERNAL,
                                             &grand, &grand_header);
            if (result != 0) {
                return result;
            }
            if (!rbt_internal_header_valid(transaction, &grand_header, node_header.level + 1u)) {
                return -EBADMSG;
            }
            result = rbt_internal_collect(tree, grand, &grand_header, 0u, &grand_image);
            if (result != 0) {
                return result;
            }
            if (node_index > grand_image.count ||
                grand_image.children[node_index] != node_page_id) {
                rbt_internal_image_destroy(&grand_image);
                return -EBADMSG;
            }
            if (node_index != 0u) {
                uint64_t left_page_id = grand_image.children[node_index - 1u];
                unsigned char *left;
                struct rbt_page_header left_header;
                struct rbt_internal_image left_image = {0};

                result = rbt_txn_load_typed_page(transaction, left_page_id, RBT_PAGE_INTERNAL,
                                                 &left, &left_header);
                if (result != 0) {
                    rbt_internal_image_destroy(&grand_image);
                    return result;
                }
                if (!rbt_internal_header_valid(transaction, &left_header, node_header.level) ||
                    left_header.count < minimum_count) {
                    rbt_internal_image_destroy(&grand_image);
                    return -EBADMSG;
                }
                if (left_header.count > minimum_count) {
                    struct rbt_internal_image node_image = {0};
                    uint32_t left_last = left_header.count - 1u;

                    result = rbt_internal_collect(tree, left, &left_header, 0u, &left_image);
                    if (result == 0) {
                        result = rbt_internal_collect(tree, node, &node_header, 1u, &node_image);
                    }
                    if (result == 0) {
                        result = rbt_internal_prepend_image(&node_image,
                                                            grand_image.keys[node_index - 1u].data,
                                                            grand_image.keys[node_index - 1u].size,
                                                            left_image.children[left_image.count]);
                    }
                    if (result == 0) {
                        result = rbt_internal_replace_key(&grand_image, node_index - 1u,
                                                          left_image.keys[left_last].data,
                                                          left_image.keys[left_last].size);
                    }
                    if (result == 0) {
                        rbt_internal_remove_image(&left_image, left_last);
                        result = rbt_internal_encode(tree, left, left_page_id, left_header.level,
                                                     &left_image);
                    }
                    if (result == 0) {
                        result = rbt_internal_encode(tree, node, node_page_id, node_header.level,
                                                     &node_image);
                    }
                    if (result == 0) {
                        result = rbt_internal_encode(tree, grand, grand_entry->page_id,
                                                     grand_header.level, &grand_image);
                    }
                    if (result == 0) {
                        result = rbt_txn_mark_dirty(transaction, left_page_id);
                    }
                    if (result == 0) {
                        result = rbt_txn_mark_dirty(transaction, node_page_id);
                    }
                    if (result == 0) {
                        result = rbt_txn_mark_dirty(transaction, grand_entry->page_id);
                    }
                    rbt_internal_image_destroy(&node_image);
                    rbt_internal_image_destroy(&left_image);
                    rbt_internal_image_destroy(&grand_image);
                    return result;
                }
            }
            if (node_index < grand_image.count) {
                uint64_t right_page_id = grand_image.children[node_index + 1u];
                unsigned char *right;
                struct rbt_page_header right_header;
                struct rbt_internal_image right_image = {0};

                result = rbt_txn_load_typed_page(transaction, right_page_id, RBT_PAGE_INTERNAL,
                                                 &right, &right_header);
                if (result != 0) {
                    rbt_internal_image_destroy(&grand_image);
                    return result;
                }
                if (!rbt_internal_header_valid(transaction, &right_header, node_header.level) ||
                    right_header.count < minimum_count) {
                    rbt_internal_image_destroy(&grand_image);
                    return -EBADMSG;
                }
                if (right_header.count > minimum_count) {
                    struct rbt_internal_image node_image = {0};

                    result = rbt_internal_collect(tree, node, &node_header, 1u, &node_image);
                    if (result == 0) {
                        result = rbt_internal_collect(tree, right, &right_header, 0u, &right_image);
                    }
                    if (result == 0) {
                        result = rbt_internal_insert_image(
                            &node_image, node_image.count, grand_image.keys[node_index].data,
                            grand_image.keys[node_index].size, right_image.children[0]);
                    }
                    if (result == 0) {
                        result = rbt_internal_replace_key(&grand_image, node_index,
                                                          right_image.keys[0].data,
                                                          right_image.keys[0].size);
                    }
                    if (result == 0) {
                        rbt_internal_remove_first_child(&right_image);
                        result = rbt_internal_encode(tree, node, node_page_id, node_header.level,
                                                     &node_image);
                    }
                    if (result == 0) {
                        result = rbt_internal_encode(tree, right, right_page_id, right_header.level,
                                                     &right_image);
                    }
                    if (result == 0) {
                        result = rbt_internal_encode(tree, grand, grand_entry->page_id,
                                                     grand_header.level, &grand_image);
                    }
                    if (result == 0) {
                        result = rbt_txn_mark_dirty(transaction, node_page_id);
                    }
                    if (result == 0) {
                        result = rbt_txn_mark_dirty(transaction, right_page_id);
                    }
                    if (result == 0) {
                        result = rbt_txn_mark_dirty(transaction, grand_entry->page_id);
                    }
                    rbt_internal_image_destroy(&right_image);
                    rbt_internal_image_destroy(&node_image);
                    rbt_internal_image_destroy(&grand_image);
                    return result;
                }
            }
            if (node_index != 0u) {
                uint64_t left_page_id = grand_image.children[node_index - 1u];
                unsigned char *left;
                struct rbt_page_header left_header;
                struct rbt_internal_image left_image = {0};
                struct rbt_internal_image node_image = {0};

                result = rbt_txn_load_typed_page(transaction, left_page_id, RBT_PAGE_INTERNAL,
                                                 &left, &left_header);
                if (result == 0) {
                    result = rbt_internal_collect(tree, left, &left_header, node_header.count + 1u,
                                                  &left_image);
                }
                if (result == 0) {
                    result = rbt_internal_collect(tree, node, &node_header, 0u, &node_image);
                }
                if (result == 0 &&
                    left_image.count + 1u + node_image.count > tree->internal_capacity) {
                    result = -EBADMSG;
                }
                if (result == 0) {
                    result = rbt_internal_append_image(
                        &left_image, grand_image.keys[node_index - 1u].data,
                        grand_image.keys[node_index - 1u].size, &node_image);
                }
                if (result == 0) {
                    result = rbt_internal_encode(tree, left, left_page_id, left_header.level,
                                                 &left_image);
                }
                if (result == 0) {
                    result = rbt_txn_mark_dirty(transaction, left_page_id);
                }
                if (result == 0) {
                    result = rbt_txn_free_page(transaction, node_page_id);
                }
                if (result == 0) {
                    rbt_internal_remove_image(&grand_image, node_index - 1u);
                    result = rbt_internal_encode(tree, grand, grand_entry->page_id,
                                                 grand_header.level, &grand_image);
                }
                if (result == 0) {
                    result = rbt_txn_mark_dirty(transaction, grand_entry->page_id);
                }
                rbt_internal_image_destroy(&node_image);
                rbt_internal_image_destroy(&left_image);
            } else {
                uint64_t right_page_id;
                unsigned char *right;
                struct rbt_page_header right_header;
                struct rbt_internal_image node_image = {0};
                struct rbt_internal_image right_image = {0};

                if (grand_image.count == 0u) {
                    rbt_internal_image_destroy(&grand_image);
                    return -EBADMSG;
                }
                right_page_id = grand_image.children[1];
                result = rbt_txn_load_typed_page(transaction, right_page_id, RBT_PAGE_INTERNAL,
                                                 &right, &right_header);
                if (result == 0) {
                    result = rbt_internal_collect(tree, node, &node_header, right_header.count + 1u,
                                                  &node_image);
                }
                if (result == 0) {
                    result = rbt_internal_collect(tree, right, &right_header, 0u, &right_image);
                }
                if (result == 0 &&
                    node_image.count + 1u + right_image.count > tree->internal_capacity) {
                    result = -EBADMSG;
                }
                if (result == 0) {
                    result = rbt_internal_append_image(&node_image, grand_image.keys[0].data,
                                                       grand_image.keys[0].size, &right_image);
                }
                if (result == 0) {
                    result = rbt_internal_encode(tree, node, node_page_id, node_header.level,
                                                 &node_image);
                }
                if (result == 0) {
                    result = rbt_txn_mark_dirty(transaction, node_page_id);
                }
                if (result == 0) {
                    result = rbt_txn_free_page(transaction, right_page_id);
                }
                if (result == 0) {
                    rbt_internal_remove_image(&grand_image, 0u);
                    result = rbt_internal_encode(tree, grand, grand_entry->page_id,
                                                 grand_header.level, &grand_image);
                }
                if (result == 0) {
                    result = rbt_txn_mark_dirty(transaction, grand_entry->page_id);
                }
                rbt_internal_image_destroy(&right_image);
                rbt_internal_image_destroy(&node_image);
            }
            rbt_internal_image_destroy(&grand_image);
            if (result != 0) {
                return result;
            }
        }
        --node_position;
    }
}

static int rbt_parent_replace_leaf_minimum(
    struct rbt_txn *transaction, const struct rbt_path_entry *parent_entry, unsigned char *parent,
    const struct rbt_page_header *parent_header, struct rbt_internal_image *parent_image,
    uint32_t leaf_index, const struct rbt_cell_image *minimum_cell) {
    const unsigned char *minimum;
    uint32_t minimum_size;
    int result;

    if (leaf_index == 0u) {
        return 0;
    }
    result = rbt_leaf_cell_key(minimum_cell, &minimum, &minimum_size);
    if (result == 0) {
        result = rbt_internal_replace_key(parent_image, leaf_index - 1u, minimum, minimum_size);
    }
    if (result == 0) {
        result = rbt_internal_encode(transaction->tree, parent, parent_entry->page_id,
                                     parent_header->level, parent_image);
    }
    if (result == 0) {
        result = rbt_txn_mark_dirty(transaction, parent_entry->page_id);
    }
    return result;
}

static int rbt_txn_rebalance_leaf(struct rbt_txn *transaction, const struct rbt_path *path) {
    const struct rbt *tree = transaction->tree;
    const struct rbt_path_entry *parent_entry = &path->entries[path->depth - 1u];
    uint32_t leaf_index = parent_entry->child_index;
    uint32_t minimum_count = (tree->leaf_capacity + 1u) / 2u;
    unsigned char *leaf;
    unsigned char *parent;
    struct rbt_page_header leaf_header;
    struct rbt_page_header parent_header;
    struct rbt_internal_image parent_image = {0};
    int result;

    result = rbt_txn_load_typed_page(transaction, path->leaf_page_id, RBT_PAGE_LEAF, &leaf,
                                     &leaf_header);
    if (result == 0) {
        result = rbt_txn_load_typed_page(transaction, parent_entry->page_id, RBT_PAGE_INTERNAL,
                                         &parent, &parent_header);
    }
    if (result != 0) {
        return result;
    }
    if (!rbt_leaf_header_valid(transaction, &leaf_header) ||
        !rbt_internal_header_valid(transaction, &parent_header, 1u)) {
        return -EBADMSG;
    }
    result = rbt_internal_collect(tree, parent, &parent_header, 0u, &parent_image);
    if (result != 0) {
        return result;
    }
    if (leaf_index > parent_image.count ||
        parent_image.children[leaf_index] != path->leaf_page_id) {
        result = -EBADMSG;
        goto cleanup;
    }
    if (leaf_index != 0u) {
        uint64_t left_page_id = parent_image.children[leaf_index - 1u];
        unsigned char *left;
        struct rbt_page_header left_header = {0};

        result =
            rbt_txn_load_typed_page(transaction, left_page_id, RBT_PAGE_LEAF, &left, &left_header);
        if (result != 0) {
            goto cleanup;
        }
        if (!rbt_leaf_header_valid(transaction, &left_header) ||
            left_header.count < minimum_count || left_header.link0 != path->leaf_page_id ||
            leaf_header.link1 != left_page_id) {
            result = -EBADMSG;
            goto cleanup;
        }
        if (left_header.count > minimum_count) {
            struct rbt_cell_image *left_cells = NULL;
            struct rbt_cell_image *leaf_cells = NULL;
            uint32_t left_count = left_header.count;
            uint32_t leaf_count = leaf_header.count;

            result = rbt_leaf_collect(tree, left, &left_header, 0u, &left_cells);
            if (result == 0) {
                result = rbt_leaf_collect(tree, leaf, &leaf_header, 1u, &leaf_cells);
            }
            if (result == 0) {
                memmove(&leaf_cells[1], &leaf_cells[0], (size_t)leaf_count * sizeof(*leaf_cells));
                leaf_cells[0] = left_cells[left_count - 1u];
                memset(&left_cells[left_count - 1u], 0, sizeof(*left_cells));
                --left_count;
                ++leaf_count;
                result = rbt_leaf_encode(tree, left, left_page_id, left_cells, left_count,
                                         left_header.link1, left_header.link0);
            }
            if (result == 0) {
                result = rbt_leaf_encode(tree, leaf, path->leaf_page_id, leaf_cells, leaf_count,
                                         leaf_header.link1, leaf_header.link0);
            }
            if (result == 0) {
                result = rbt_parent_replace_leaf_minimum(transaction, parent_entry, parent,
                                                         &parent_header, &parent_image, leaf_index,
                                                         &leaf_cells[0]);
            }
            if (result == 0) {
                result = rbt_txn_mark_dirty(transaction, left_page_id);
            }
            if (result == 0) {
                result = rbt_txn_mark_dirty(transaction, path->leaf_page_id);
            }
            rbt_cell_images_destroy(leaf_cells, leaf_count);
            rbt_cell_images_destroy(left_cells, left_count);
            goto cleanup;
        }
    }
    if (leaf_index < parent_image.count) {
        uint64_t right_page_id = parent_image.children[leaf_index + 1u];
        unsigned char *right;
        struct rbt_page_header right_header = {0};

        result = rbt_txn_load_typed_page(transaction, right_page_id, RBT_PAGE_LEAF, &right,
                                         &right_header);
        if (result != 0) {
            goto cleanup;
        }
        if (!rbt_leaf_header_valid(transaction, &right_header) ||
            right_header.count < minimum_count || leaf_header.link0 != right_page_id ||
            right_header.link1 != path->leaf_page_id) {
            result = -EBADMSG;
            goto cleanup;
        }
        if (right_header.count > minimum_count) {
            struct rbt_cell_image *leaf_cells = NULL;
            struct rbt_cell_image *right_cells = NULL;
            uint32_t leaf_count = leaf_header.count;
            uint32_t right_count = right_header.count;
            const unsigned char *right_minimum;
            uint32_t right_minimum_size;

            result = rbt_leaf_collect(tree, leaf, &leaf_header, 1u, &leaf_cells);
            if (result == 0) {
                result = rbt_leaf_collect(tree, right, &right_header, 0u, &right_cells);
            }
            if (result == 0) {
                leaf_cells[leaf_count++] = right_cells[0];
                memmove(&right_cells[0], &right_cells[1],
                        (size_t)(right_count - 1u) * sizeof(*right_cells));
                --right_count;
                memset(&right_cells[right_count], 0, sizeof(*right_cells));
                result = rbt_leaf_cell_key(&right_cells[0], &right_minimum, &right_minimum_size);
            }
            if (result == 0) {
                result = rbt_internal_replace_key(&parent_image, leaf_index, right_minimum,
                                                  right_minimum_size);
            }
            if (result == 0 && leaf_index != 0u) {
                result = rbt_parent_replace_leaf_minimum(transaction, parent_entry, parent,
                                                         &parent_header, &parent_image, leaf_index,
                                                         &leaf_cells[0]);
            } else if (result == 0) {
                result = rbt_internal_encode(tree, parent, parent_entry->page_id,
                                             parent_header.level, &parent_image);
                if (result == 0) {
                    result = rbt_txn_mark_dirty(transaction, parent_entry->page_id);
                }
            }
            if (result == 0) {
                result = rbt_leaf_encode(tree, leaf, path->leaf_page_id, leaf_cells, leaf_count,
                                         leaf_header.link1, leaf_header.link0);
            }
            if (result == 0) {
                result = rbt_leaf_encode(tree, right, right_page_id, right_cells, right_count,
                                         right_header.link1, right_header.link0);
            }
            if (result == 0) {
                result = rbt_txn_mark_dirty(transaction, path->leaf_page_id);
            }
            if (result == 0) {
                result = rbt_txn_mark_dirty(transaction, right_page_id);
            }
            if (result == 0 && leaf_index == 0u) {
                const unsigned char *minimum;
                uint32_t minimum_size;

                result = rbt_leaf_cell_key(&leaf_cells[0], &minimum, &minimum_size);
                if (result == 0) {
                    result = rbt_txn_propagate_minimum(transaction, path, minimum, minimum_size);
                }
            }
            rbt_cell_images_destroy(right_cells, right_count);
            rbt_cell_images_destroy(leaf_cells, leaf_count);
            goto cleanup;
        }
    }
    if (leaf_index != 0u) {
        uint64_t left_page_id = parent_image.children[leaf_index - 1u];
        unsigned char *left;
        struct rbt_page_header left_header = {0};
        struct rbt_cell_image *left_cells = NULL;
        struct rbt_cell_image *leaf_cells = NULL;
        uint32_t left_count;
        uint32_t leaf_count = leaf_header.count;
        uint32_t index;

        result =
            rbt_txn_load_typed_page(transaction, left_page_id, RBT_PAGE_LEAF, &left, &left_header);
        if (result == 0) {
            result = rbt_leaf_collect(tree, left, &left_header, leaf_count, &left_cells);
        }
        if (result == 0) {
            result = rbt_leaf_collect(tree, leaf, &leaf_header, 0u, &leaf_cells);
        }
        left_count = left_header.count;
        if (result == 0 && left_count + leaf_count > tree->leaf_capacity) {
            result = -EBADMSG;
        }
        for (index = 0u; result == 0 && index < leaf_count; ++index) {
            left_cells[left_count + index] = leaf_cells[index];
            memset(&leaf_cells[index], 0, sizeof(*leaf_cells));
        }
        if (result == 0) {
            left_count += leaf_count;
            result = rbt_leaf_encode(tree, left, left_page_id, left_cells, left_count,
                                     left_header.link1, leaf_header.link0);
        }
        if (result == 0 && leaf_header.link0 != 0u) {
            unsigned char *next;
            struct rbt_page_header next_header;

            result = rbt_txn_load_typed_page(transaction, leaf_header.link0, RBT_PAGE_LEAF, &next,
                                             &next_header);
            if (result == 0 && (!rbt_leaf_header_valid(transaction, &next_header) ||
                                next_header.link1 != path->leaf_page_id)) {
                result = -EBADMSG;
            }
            if (result == 0) {
                rbt_store_u64(next + RBT_PAGE_LINK1_OFFSET, left_page_id);
                result = rbt_txn_mark_dirty(transaction, leaf_header.link0);
            }
        }
        if (result == 0) {
            result = rbt_txn_mark_dirty(transaction, left_page_id);
        }
        if (result == 0) {
            result = rbt_txn_free_page(transaction, path->leaf_page_id);
        }
        if (result == 0) {
            rbt_internal_remove_image(&parent_image, leaf_index - 1u);
            result = rbt_internal_encode(tree, parent, parent_entry->page_id, parent_header.level,
                                         &parent_image);
        }
        if (result == 0) {
            result = rbt_txn_mark_dirty(transaction, parent_entry->page_id);
        }
        rbt_cell_images_destroy(leaf_cells, leaf_count);
        rbt_cell_images_destroy(left_cells, left_count);
    } else {
        uint64_t right_page_id;
        unsigned char *right;
        struct rbt_page_header right_header = {0};
        struct rbt_cell_image *leaf_cells = NULL;
        struct rbt_cell_image *right_cells = NULL;
        uint32_t leaf_count = leaf_header.count;
        uint32_t right_count;
        uint32_t index;

        if (parent_image.count == 0u) {
            result = -EBADMSG;
            goto cleanup;
        }
        right_page_id = parent_image.children[1];
        result = rbt_txn_load_typed_page(transaction, right_page_id, RBT_PAGE_LEAF, &right,
                                         &right_header);
        if (result == 0) {
            result = rbt_leaf_collect(tree, leaf, &leaf_header, right_header.count, &leaf_cells);
        }
        if (result == 0) {
            result = rbt_leaf_collect(tree, right, &right_header, 0u, &right_cells);
        }
        right_count = right_header.count;
        if (result == 0 && leaf_count + right_count > tree->leaf_capacity) {
            result = -EBADMSG;
        }
        for (index = 0u; result == 0 && index < right_count; ++index) {
            leaf_cells[leaf_count + index] = right_cells[index];
            memset(&right_cells[index], 0, sizeof(*right_cells));
        }
        if (result == 0) {
            leaf_count += right_count;
            result = rbt_leaf_encode(tree, leaf, path->leaf_page_id, leaf_cells, leaf_count,
                                     leaf_header.link1, right_header.link0);
        }
        if (result == 0 && right_header.link0 != 0u) {
            unsigned char *next;
            struct rbt_page_header next_header;

            result = rbt_txn_load_typed_page(transaction, right_header.link0, RBT_PAGE_LEAF, &next,
                                             &next_header);
            if (result == 0 && (!rbt_leaf_header_valid(transaction, &next_header) ||
                                next_header.link1 != right_page_id)) {
                result = -EBADMSG;
            }
            if (result == 0) {
                rbt_store_u64(next + RBT_PAGE_LINK1_OFFSET, path->leaf_page_id);
                result = rbt_txn_mark_dirty(transaction, right_header.link0);
            }
        }
        if (result == 0) {
            result = rbt_txn_mark_dirty(transaction, path->leaf_page_id);
        }
        if (result == 0) {
            result = rbt_txn_free_page(transaction, right_page_id);
        }
        if (result == 0) {
            rbt_internal_remove_image(&parent_image, 0u);
            result = rbt_internal_encode(tree, parent, parent_entry->page_id, parent_header.level,
                                         &parent_image);
        }
        if (result == 0) {
            result = rbt_txn_mark_dirty(transaction, parent_entry->page_id);
        }
        if (result == 0 && leaf_count != 0u) {
            const unsigned char *minimum;
            uint32_t minimum_size;

            result = rbt_leaf_cell_key(&leaf_cells[0], &minimum, &minimum_size);
            if (result == 0) {
                result = rbt_txn_propagate_minimum(transaction, path, minimum, minimum_size);
            }
        }
        rbt_cell_images_destroy(right_cells, right_count);
        rbt_cell_images_destroy(leaf_cells, leaf_count);
    }
    if (result == 0) {
        result = rbt_txn_rebalance_internal(transaction, path, path->depth - 1u);
    }

cleanup:
    rbt_internal_image_destroy(&parent_image);
    return result;
}

static int rbt_read_metadata(struct rbt *tree, uint64_t page_count,
                             struct rbt_metadata *out_metadata) {
    unsigned char *page;
    struct rbt_page_header header;
    int result;

    result = rbt_page_count_validate(page_count);
    if (result != 0) {
        return result;
    }
    if (page_count < 3u) {
        return -EBADMSG;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return -ENOMEM;
    }
    result = rbt_read_page(tree, 0u, page);
    if (result == 0) {
        result = rbt_decode_page_header(tree, page, 0u, &header);
    }
    if (result == 0 &&
        (header.type != RBT_PAGE_META || header.count != 0u || header.level != 0u ||
         header.free_lower != RBT_META_USED_SIZE || header.free_upper != tree->storage.page_size ||
         header.link0 != 0u || header.link1 != 0u || header.aux != 0u ||
         rbt_load_u32(page + RBT_META_PAGE_SIZE_OFFSET) != tree->storage.page_size)) {
        result = -EBADMSG;
    }
    if (result == 0) {
        uint64_t schema_payload = tree->storage.page_size - RBT_PAGE_HEADER_SIZE;
        uint64_t maximum_schema_size =
            RBT_SCHEMA_HEADER_SIZE + (uint64_t)RBT_MAX_COLUMNS * RBT_SCHEMA_COLUMN_SIZE;
        uint64_t expected_schema_pages;

        out_metadata->root_page_id = rbt_load_u64(page + RBT_META_ROOT_OFFSET);
        out_metadata->free_head = rbt_load_u64(page + RBT_META_FREE_HEAD_OFFSET);
        out_metadata->item_count = rbt_load_u64(page + RBT_META_ITEM_COUNT_OFFSET);
        out_metadata->next_page_id = rbt_load_u64(page + RBT_META_NEXT_PAGE_OFFSET);
        out_metadata->height = rbt_load_u32(page + RBT_META_HEIGHT_OFFSET);
        out_metadata->schema_head = rbt_load_u64(page + RBT_META_SCHEMA_HEAD_OFFSET);
        out_metadata->schema_size = rbt_load_u64(page + RBT_META_SCHEMA_SIZE_OFFSET);
        out_metadata->schema_page_count = rbt_load_u64(page + RBT_META_SCHEMA_PAGE_COUNT_OFFSET);
        expected_schema_pages = out_metadata->schema_size / schema_payload +
                                (out_metadata->schema_size % schema_payload != 0u);
        if (out_metadata->next_page_id != page_count || out_metadata->root_page_id == 0u ||
            out_metadata->root_page_id >= out_metadata->next_page_id ||
            out_metadata->height == 0u || out_metadata->height > 64u ||
            out_metadata->schema_head == 0u ||
            out_metadata->schema_head >= out_metadata->next_page_id ||
            out_metadata->schema_size < RBT_SCHEMA_HEADER_SIZE ||
            out_metadata->schema_size > maximum_schema_size ||
            out_metadata->schema_page_count == 0u ||
            out_metadata->schema_page_count != expected_schema_pages ||
            !rbt_allocation_count_fits(out_metadata->schema_page_count,
                                       sizeof(*tree->schema_pages)) ||
            out_metadata->schema_page_count > out_metadata->next_page_id - 2u ||
            (out_metadata->free_head != 0u &&
             out_metadata->free_head >= out_metadata->next_page_id)) {
            result = -EBADMSG;
        }
    }
    free(page);
    return result;
}

static int rbt_load_schema(struct rbt *tree) {
    unsigned char *encoded;
    unsigned char *page;
    uint64_t page_id = tree->metadata.schema_head;
    uint64_t remaining = tree->metadata.schema_size;
    uint64_t index;
    size_t offset = 0u;
    int result = 0;

    if (tree->metadata.schema_size > SIZE_MAX ||
        !rbt_allocation_count_fits(tree->metadata.schema_page_count, sizeof(*tree->schema_pages))) {
        return -ENOMEM;
    }
    encoded = malloc((size_t)tree->metadata.schema_size);
    page = malloc((size_t)tree->storage.page_size);
    tree->schema_pages =
        calloc((size_t)tree->metadata.schema_page_count, sizeof(*tree->schema_pages));
    if (encoded == NULL || page == NULL || tree->schema_pages == NULL) {
        free(encoded);
        free(page);
        return -ENOMEM;
    }
    for (index = 0u; index < tree->metadata.schema_page_count; ++index) {
        struct rbt_page_header header;
        uint64_t expected_chunk = remaining < tree->storage.page_size - RBT_PAGE_HEADER_SIZE
                                      ? remaining
                                      : tree->storage.page_size - RBT_PAGE_HEADER_SIZE;
        uint64_t prior;

        for (prior = 0u; prior < index; ++prior) {
            if (tree->schema_pages[prior] == page_id) {
                result = -EBADMSG;
                break;
            }
        }
        if (result != 0 || page_id == 0u || page_id >= tree->metadata.next_page_id) {
            result = -EBADMSG;
            break;
        }
        tree->schema_pages[index] = page_id;
        result = rbt_read_typed_page(tree, page_id, RBT_PAGE_SCHEMA, page, &header);
        if (result != 0) {
            break;
        }
        if (header.count != expected_chunk || header.level != 0u ||
            header.free_lower != RBT_PAGE_HEADER_SIZE + header.count ||
            header.free_upper != tree->storage.page_size || header.link1 != 0u ||
            header.aux != index ||
            ((index + 1u == tree->metadata.schema_page_count) != (header.link0 == 0u))) {
            result = -EBADMSG;
            break;
        }
        memcpy(encoded + offset, page + RBT_PAGE_HEADER_SIZE, header.count);
        offset += header.count;
        remaining -= header.count;
        page_id = header.link0;
    }
    if (result == 0 && (remaining != 0u || page_id != 0u)) {
        result = -EBADMSG;
    }
    if (result == 0) {
        result = rbt_schema_decode(encoded, (size_t)tree->metadata.schema_size, &tree->schema,
                                   &tree->columns);
    }
    free(encoded);
    free(page);
    return result;
}

static int rbt_configure_schema(struct rbt *tree) {
    return rbt_schema_measure(&tree->schema, tree->storage.page_size, &tree->maximum_key_size,
                              &tree->leaf_capacity, &tree->internal_capacity,
                              &tree->inline_threshold);
}

static int rbt_create_pages(struct rbt *tree, const unsigned char *schema_data,
                            size_t schema_size) {
    uint32_t payload = tree->storage.page_size - RBT_PAGE_HEADER_SIZE;
    uint64_t schema_pages = schema_size / payload + (schema_size % payload != 0u);
    uint64_t root_page_id = schema_pages + 1u;
    size_t update_count = (size_t)schema_pages + 2u;
    struct rbt_page_update *updates;
    unsigned char *pages;
    struct rbt_metadata metadata;
    uint64_t index;
    size_t offset = 0u;
    int result;

    if (update_count > SIZE_MAX / (size_t)tree->storage.page_size) {
        return -ENOMEM;
    }
    updates = calloc(update_count, sizeof(*updates));
    pages = calloc(update_count, (size_t)tree->storage.page_size);
    tree->schema_pages = calloc((size_t)schema_pages, sizeof(*tree->schema_pages));
    if (updates == NULL || pages == NULL || tree->schema_pages == NULL) {
        free(updates);
        free(pages);
        return -ENOMEM;
    }
    metadata.root_page_id = root_page_id;
    metadata.free_head = 0u;
    metadata.item_count = 0u;
    metadata.next_page_id = root_page_id + 1u;
    metadata.height = 1u;
    metadata.schema_head = 1u;
    metadata.schema_size = schema_size;
    metadata.schema_page_count = schema_pages;
    rbt_metadata_encode(pages, tree, &metadata);
    updates[0].page_id = 0u;
    updates[0].data = pages;
    for (index = 0u; index < schema_pages; ++index) {
        unsigned char *page = pages + ((size_t)index + 1u) * (size_t)tree->storage.page_size;
        uint32_t chunk =
            (uint32_t)(schema_size - offset < payload ? schema_size - offset : payload);
        uint64_t page_id = index + 1u;

        tree->schema_pages[index] = page_id;
        rbt_page_initialize(page, tree->storage.page_size, page_id, RBT_PAGE_SCHEMA, chunk, 0u);
        rbt_store_u32(page + RBT_PAGE_FREE_LOWER_OFFSET, RBT_PAGE_HEADER_SIZE + chunk);
        rbt_store_u64(page + RBT_PAGE_LINK0_OFFSET, index + 1u == schema_pages ? 0u : page_id + 1u);
        rbt_store_u64(page + RBT_PAGE_AUX_OFFSET, index);
        memcpy(page + RBT_PAGE_HEADER_SIZE, schema_data + offset, chunk);
        updates[index + 1u].page_id = page_id;
        updates[index + 1u].data = page;
        offset += chunk;
    }
    rbt_page_initialize(pages + (update_count - 1u) * (size_t)tree->storage.page_size,
                        tree->storage.page_size, root_page_id, RBT_PAGE_LEAF, 0u, 0u);
    updates[update_count - 1u].page_id = root_page_id;
    updates[update_count - 1u].data = pages + (update_count - 1u) * (size_t)tree->storage.page_size;
    for (index = 0u; index < update_count; ++index) {
        rbt_page_checksum_store(pages + (size_t)index * (size_t)tree->storage.page_size,
                                tree->storage.page_size);
    }
    result = rbt_commit(tree, updates, update_count);
    if (result == 0) {
        tree->metadata = metadata;
    }

    free(pages);
    free(updates);
    return result;
}

static void rbt_release(struct rbt *tree) {
    if (tree == NULL) {
        return;
    }
    free(tree->schema_pages);
    free(tree->columns);
    if (tree->storage_acquired) {
        tree->storage_acquired = false;
        tree->storage.release(tree->storage.context);
    }
    free(tree);
}

int rbt_create(const struct rbt_config *config, struct rbt **out_rbt) {
    struct rbt *tree;
    unsigned char *schema_data = NULL;
    size_t schema_size = 0u;
    uint64_t page_count = 0u;
    int result;

    if (out_rbt != NULL) {
        *out_rbt = NULL;
    }
    if (out_rbt == NULL || config == NULL || !rbt_storage_valid(config->storage) ||
        config->schema == NULL) {
        return -EINVAL;
    }
    tree = calloc(1u, sizeof(*tree));
    if (tree == NULL) {
        return -ENOMEM;
    }
    tree->storage = *config->storage;
    tree->schema = *config->schema;
    result = tree->storage.acquire(tree->storage.context);
    if (result == 0) {
        tree->storage_acquired = true;
        result = rbt_configure_schema(tree);
    }
    if (result == 0) {
        result = rbt_page_count(tree, &page_count);
    }
    if (result == 0 && page_count != 0u) {
        result = -EINVAL;
    }
    if (result == 0) {
        result = rbt_schema_encode(config->schema, &schema_data, &schema_size);
    }
    if (result == 0) {
        size_t count = config->schema->key_column_count + config->schema->value_column_count;

        tree->columns = malloc(count * sizeof(*tree->columns));
        if (tree->columns == NULL) {
            result = -ENOMEM;
        } else {
            memcpy(tree->columns, config->schema->key_columns,
                   config->schema->key_column_count * sizeof(*tree->columns));
            memcpy(tree->columns + config->schema->key_column_count, config->schema->value_columns,
                   config->schema->value_column_count * sizeof(*tree->columns));
            tree->schema.key_columns = tree->columns;
            tree->schema.value_columns = tree->columns + config->schema->key_column_count;
        }
    }
    if (result == 0) {
        result = rbt_create_pages(tree, schema_data, schema_size);
    }
    free(schema_data);
    if (result != 0) {
        rbt_release(tree);
        return result;
    }
    *out_rbt = tree;
    return 0;
}

int rbt_open(const struct rbt_storage *storage, struct rbt **out_rbt) {
    struct rbt *tree;
    uint64_t page_count = 0u;
    int result;

    if (out_rbt != NULL) {
        *out_rbt = NULL;
    }
    if (out_rbt == NULL || !rbt_storage_valid(storage)) {
        return -EINVAL;
    }
    tree = calloc(1u, sizeof(*tree));
    if (tree == NULL) {
        return -ENOMEM;
    }
    tree->storage = *storage;
    result = tree->storage.acquire(tree->storage.context);
    if (result == 0) {
        tree->storage_acquired = true;
        result = rbt_page_count(tree, &page_count);
    }
    if (result == 0) {
        result = rbt_read_metadata(tree, page_count, &tree->metadata);
    }
    if (result == 0) {
        result = rbt_load_schema(tree);
    }
    if (result == 0) {
        result = rbt_configure_schema(tree);
        if (result == -EINVAL) {
            result = -EBADMSG;
        }
    }
    if (result == 0) {
        result = rbt_validate(tree, NULL, NULL, 0u);
    }
    if (result != 0) {
        rbt_release(tree);
        return result;
    }
    *out_rbt = tree;
    return 0;
}

int rbt_destroy(struct rbt *tree) {
    if (tree == NULL) {
        return -EINVAL;
    }
    if (tree->scan_active) {
        return -EBUSY;
    }
    rbt_release(tree);
    return 0;
}

int rbt_get_schema(const struct rbt *tree, const struct rbt_schema **out_schema) {
    if (out_schema != NULL) {
        *out_schema = NULL;
    }
    if (tree == NULL || out_schema == NULL) {
        return -EINVAL;
    }
    *out_schema = &tree->schema;
    return 0;
}

int rbt_get(struct rbt *tree, const struct rbt_record *key_record, struct rbt_row **out_row) {
    unsigned char *encoded_key = NULL;
    uint32_t encoded_size = 0u;
    unsigned char *page = NULL;
    struct rbt_page_header header;
    uint32_t position;
    int result;

    if (out_row != NULL) {
        *out_row = NULL;
    }
    if (tree == NULL || out_row == NULL || rbt_record_validate(tree, key_record, false) != 0) {
        return -EINVAL;
    }
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    result = rbt_key_encode(tree, key_record->key, &encoded_key, &encoded_size);
    page = malloc((size_t)tree->storage.page_size);
    if (result == 0 && page == NULL) {
        result = -ENOMEM;
    }
    if (result == 0) {
        result = rbt_find_leaf(tree, encoded_key, encoded_size, page, &header);
    }
    if (result == 0) {
        result = rbt_leaf_lower_bound(page, header.count, encoded_key, encoded_size, &position);
    }
    if (result == 0) {
        if (position == header.count) {
            result = -ENOENT;
        } else {
            uint32_t cell_size;
            const unsigned char *cell = rbt_slot_cell(page, position, &cell_size);
            uint32_t stored_size = rbt_load_u32(cell);

            if (rbt_compare_keys(cell + RBT_LEAF_CELL_HEADER_SIZE, stored_size, encoded_key,
                                 encoded_size) != 0) {
                result = -ENOENT;
            } else {
                result = rbt_row_from_cell(tree, cell, cell_size, out_row, NULL, NULL);
            }
        }
    }
    free(page);
    free(encoded_key);
    return result;
}

int rbt_put(struct rbt *tree, const struct rbt_record *record, bool *out_inserted) {
    struct rbt_txn transaction;
    struct rbt_path path;
    unsigned char *encoded_key = NULL;
    uint32_t encoded_size = 0u;
    unsigned char *leaf;
    struct rbt_page_header leaf_header;
    struct rbt_cell_image *cells = NULL;
    uint32_t position;
    uint32_t cell_count = 0u;
    int result;

    if (out_inserted != NULL) {
        *out_inserted = false;
    }
    if (tree == NULL || out_inserted == NULL || rbt_record_validate(tree, record, true) != 0) {
        return -EINVAL;
    }
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    if (tree->scan_active) {
        return -EBUSY;
    }
    memset(&transaction, 0, sizeof(transaction));
    transaction.tree = tree;
    transaction.metadata = tree->metadata;
    result = rbt_key_encode(tree, record->key, &encoded_key, &encoded_size);
    if (result == 0) {
        result =
            rbt_txn_find_path(&transaction, encoded_key, encoded_size, &path, &leaf, &leaf_header);
    }
    if (result != 0) {
        free(encoded_key);
        rbt_txn_destroy(&transaction);
        return result;
    }
    result = rbt_leaf_lower_bound(leaf, leaf_header.count, encoded_key, encoded_size, &position);
    if (result != 0) {
        free(encoded_key);
        rbt_txn_destroy(&transaction);
        return result;
    }
    if (position < leaf_header.count) {
        uint32_t existing_size;
        const unsigned char *existing = rbt_slot_cell(leaf, position, &existing_size);
        uint32_t existing_key_size =
            existing_size < RBT_LEAF_CELL_HEADER_SIZE ? 0u : rbt_load_u32(existing);

        if (existing_key_size != 0u &&
            existing_key_size <= existing_size - RBT_LEAF_CELL_HEADER_SIZE &&
            rbt_compare_keys(existing + RBT_LEAF_CELL_HEADER_SIZE, existing_key_size, encoded_key,
                             encoded_size) == 0) {
            struct rbt_row *existing_row = NULL;
            struct rbt_cell_image replacement = {0};

            result = rbt_row_from_cell_source(tree, &transaction, existing, existing_size,
                                              &existing_row, NULL, NULL);
            if (result == 0 && rbt_values_equal(existing_row->value, record->value,
                                                tree->schema.value_column_count)) {
                rbt_row_release(existing_row);
                free(encoded_key);
                rbt_txn_destroy(&transaction);
                return 0;
            }
            if (result == 0) {
                result = rbt_leaf_collect(tree, leaf, &leaf_header, 0u, &cells);
                cell_count = leaf_header.count;
            }
            if (result == 0) {
                result =
                    rbt_txn_build_leaf_cell(&transaction, encoded_key, encoded_size, record,
                                            &cells[position], existing_row->value, &replacement);
            }
            rbt_row_release(existing_row);
            if (result == 0) {
                free(cells[position].data);
                cells[position] = replacement;
                memset(&replacement, 0, sizeof(replacement));
                result = rbt_leaf_encode(tree, leaf, path.leaf_page_id, cells, cell_count,
                                         leaf_header.link1, leaf_header.link0);
            }
            if (result == 0) {
                result = rbt_txn_mark_dirty(&transaction, path.leaf_page_id);
            }
            if (result == 0) {
                result = rbt_txn_commit(&transaction);
            }
            free(replacement.data);
            rbt_cell_images_destroy(cells, cell_count);
            free(encoded_key);
            rbt_txn_destroy(&transaction);
            return result;
        }
    }
    if (transaction.metadata.item_count == UINT64_MAX) {
        free(encoded_key);
        rbt_txn_destroy(&transaction);
        return -ENOMEM;
    }
    result = rbt_leaf_collect(tree, leaf, &leaf_header, 1u, &cells);
    cell_count = leaf_header.count;
    if (result == 0) {
        memmove(&cells[position + 1u], &cells[position],
                (size_t)(cell_count - position) * sizeof(*cells));
        memset(&cells[position], 0, sizeof(*cells));
        ++cell_count;
        result = rbt_txn_build_leaf_cell(&transaction, encoded_key, encoded_size, record, NULL,
                                         NULL, &cells[position]);
    }
    if (result == 0) {
        ++transaction.metadata.item_count;
        if (cell_count <= tree->leaf_capacity) {
            result = rbt_leaf_encode(tree, leaf, path.leaf_page_id, cells, cell_count,
                                     leaf_header.link1, leaf_header.link0);
            if (result == 0) {
                result = rbt_txn_mark_dirty(&transaction, path.leaf_page_id);
            }
            if (result == 0 && position == 0u) {
                const unsigned char *minimum;
                uint32_t minimum_size;

                result = rbt_leaf_cell_key(&cells[0], &minimum, &minimum_size);
                if (result == 0) {
                    result = rbt_txn_propagate_minimum(&transaction, &path, minimum, minimum_size);
                }
            }
        } else {
            uint32_t left_count = cell_count / 2u;
            uint32_t right_count = cell_count - left_count;
            uint64_t right_page_id;
            unsigned char *right_page;
            const unsigned char *separator;
            uint32_t separator_size;

            result =
                rbt_txn_allocate_page(&transaction, RBT_PAGE_LEAF, 0u, &right_page_id, &right_page);
            if (result == 0) {
                result = rbt_leaf_encode(tree, leaf, path.leaf_page_id, cells, left_count,
                                         leaf_header.link1, right_page_id);
            }
            if (result == 0) {
                result = rbt_leaf_encode(tree, right_page, right_page_id, cells + left_count,
                                         right_count, path.leaf_page_id, leaf_header.link0);
            }
            if (result == 0) {
                result = rbt_txn_mark_dirty(&transaction, path.leaf_page_id);
            }
            if (result == 0 && leaf_header.link0 != 0u) {
                unsigned char *next;
                struct rbt_page_header next_header;

                result = rbt_txn_load_typed_page(&transaction, leaf_header.link0, RBT_PAGE_LEAF,
                                                 &next, &next_header);
                if (result == 0 && (!rbt_leaf_header_valid(&transaction, &next_header) ||
                                    next_header.link1 != path.leaf_page_id)) {
                    result = -EBADMSG;
                }
                if (result == 0) {
                    rbt_store_u64(next + RBT_PAGE_LINK1_OFFSET, right_page_id);
                    result = rbt_txn_mark_dirty(&transaction, leaf_header.link0);
                }
            }
            if (result == 0 && position == 0u) {
                const unsigned char *minimum;
                uint32_t minimum_size;

                result = rbt_leaf_cell_key(&cells[0], &minimum, &minimum_size);
                if (result == 0) {
                    result = rbt_txn_propagate_minimum(&transaction, &path, minimum, minimum_size);
                }
            }
            if (result == 0) {
                result = rbt_leaf_cell_key(&cells[left_count], &separator, &separator_size);
            }
            if (result == 0) {
                result = rbt_txn_insert_parent(&transaction, &path, separator, separator_size,
                                               right_page_id);
            }
        }
    }
    if (result == 0) {
        result = rbt_txn_commit(&transaction);
    }
    if (result == 0) {
        *out_inserted = true;
    }
    rbt_cell_images_destroy(cells, cell_count);
    free(encoded_key);
    rbt_txn_destroy(&transaction);
    return result;
}

int rbt_delete(struct rbt *tree, const struct rbt_record *key_record, bool *out_deleted) {
    struct rbt_txn transaction;
    struct rbt_path path;
    unsigned char *encoded_key = NULL;
    uint32_t encoded_size = 0u;
    unsigned char *leaf;
    struct rbt_page_header leaf_header;
    struct rbt_cell_image *cells = NULL;
    uint32_t position;
    uint32_t cell_count = 0u;
    int result;

    if (out_deleted != NULL) {
        *out_deleted = false;
    }
    if (tree == NULL || out_deleted == NULL || rbt_record_validate(tree, key_record, false) != 0) {
        return -EINVAL;
    }
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    if (tree->scan_active) {
        return -EBUSY;
    }
    memset(&transaction, 0, sizeof(transaction));
    transaction.tree = tree;
    transaction.metadata = tree->metadata;
    result = rbt_key_encode(tree, key_record->key, &encoded_key, &encoded_size);
    if (result == 0) {
        result =
            rbt_txn_find_path(&transaction, encoded_key, encoded_size, &path, &leaf, &leaf_header);
    }
    if (result != 0) {
        free(encoded_key);
        rbt_txn_destroy(&transaction);
        return result;
    }
    result = rbt_leaf_lower_bound(leaf, leaf_header.count, encoded_key, encoded_size, &position);
    if (result == 0) {
        if (position == leaf_header.count) {
            result = -ENOENT;
        } else {
            uint32_t stored_size;
            const unsigned char *stored = rbt_slot_cell(leaf, position, &stored_size);
            uint32_t stored_key_size =
                stored_size < RBT_LEAF_CELL_HEADER_SIZE ? 0u : rbt_load_u32(stored);

            if (stored_key_size == 0u ||
                stored_key_size > stored_size - RBT_LEAF_CELL_HEADER_SIZE ||
                rbt_compare_keys(stored + RBT_LEAF_CELL_HEADER_SIZE, stored_key_size, encoded_key,
                                 encoded_size) != 0) {
                result = -ENOENT;
            } else {
                struct rbt_row *row = NULL;

                result = rbt_row_from_cell_source(tree, &transaction, stored, stored_size, &row,
                                                  NULL, NULL);
                rbt_row_release(row);
            }
        }
    }
    if (result == 0 && transaction.metadata.item_count == 0u) {
        result = -EBADMSG;
    }
    if (result == 0) {
        result = rbt_leaf_collect(tree, leaf, &leaf_header, 0u, &cells);
        cell_count = leaf_header.count;
    }
    if (result == 0) {
        result = rbt_txn_free_cell_overflow(&transaction, &cells[position]);
    }
    if (result == 0) {
        free(cells[position].data);
        memmove(&cells[position], &cells[position + 1u],
                (size_t)(cell_count - position - 1u) * sizeof(*cells));
        --cell_count;
        memset(&cells[cell_count], 0, sizeof(*cells));
        result = rbt_leaf_encode(tree, leaf, path.leaf_page_id, cells, cell_count,
                                 leaf_header.link1, leaf_header.link0);
    }
    if (result == 0) {
        result = rbt_txn_mark_dirty(&transaction, path.leaf_page_id);
    }
    if (result == 0) {
        --transaction.metadata.item_count;
    }
    if (result == 0 && position == 0u && cell_count != 0u) {
        const unsigned char *minimum;
        uint32_t minimum_size;

        result = rbt_leaf_cell_key(&cells[0], &minimum, &minimum_size);
        if (result == 0) {
            result = rbt_txn_propagate_minimum(&transaction, &path, minimum, minimum_size);
        }
    }
    if (result == 0 && path.depth != 0u && cell_count < (tree->leaf_capacity + 1u) / 2u) {
        result = rbt_txn_rebalance_leaf(&transaction, &path);
    }
    if (result == 0) {
        result = rbt_txn_commit(&transaction);
    }
    if (result == 0) {
        *out_deleted = true;
    }
    rbt_cell_images_destroy(cells, cell_count);
    free(encoded_key);
    rbt_txn_destroy(&transaction);
    return result;
}

int rbt_scan(struct rbt *tree, const struct rbt_record *begin, const struct rbt_record *end,
             rbt_scan_fn callback, void *context) {
    unsigned char *begin_key = NULL;
    unsigned char *end_key = NULL;
    unsigned char *page = NULL;
    struct rbt_page_header header;
    uint32_t begin_size = 0u;
    uint32_t end_size = 0u;
    uint64_t page_id = 0u;
    uint64_t previous = 0u;
    uint64_t visited = 0u;
    bool first_leaf = true;
    int result = 0;

    if (tree == NULL || callback == NULL ||
        (begin != NULL && rbt_record_validate(tree, begin, false) != 0) ||
        (end != NULL && rbt_record_validate(tree, end, false) != 0)) {
        return -EINVAL;
    }
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    if (tree->scan_active) {
        return -EBUSY;
    }
    if (begin != NULL) {
        result = rbt_key_encode(tree, begin->key, &begin_key, &begin_size);
    }
    if (result == 0 && end != NULL) {
        result = rbt_key_encode(tree, end->key, &end_key, &end_size);
    }
    if (result == 0 && begin != NULL && end != NULL &&
        rbt_compare_keys(begin_key, begin_size, end_key, end_size) >= 0) {
        free(begin_key);
        free(end_key);
        return 0;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (result == 0 && page == NULL) {
        result = -ENOMEM;
    }
    if (result == 0) {
        result = rbt_find_leaf(tree, begin == NULL ? (const unsigned char *)"" : begin_key,
                               begin == NULL ? 0u : begin_size, page, &header);
        if (result == 0) {
            page_id = rbt_load_u64(page + RBT_PAGE_ID_OFFSET);
        }
    }
    tree->scan_active = result == 0;
    while (result == 0 && page_id != 0u) {
        uint32_t index = 0u;

        if (visited++ >= tree->metadata.next_page_id || header.link1 != previous ||
            header.type != RBT_PAGE_LEAF || header.level != 0u ||
            header.count > tree->leaf_capacity || !rbt_slots_valid(tree, page, &header)) {
            result = -EBADMSG;
            break;
        }
        if (first_leaf && begin != NULL) {
            result = rbt_leaf_lower_bound(page, header.count, begin_key, begin_size, &index);
        }
        while (result == 0 && index < header.count) {
            uint32_t cell_size;
            const unsigned char *cell = rbt_slot_cell(page, index, &cell_size);
            uint32_t key_size = cell_size < RBT_LEAF_CELL_HEADER_SIZE ? 0u : rbt_load_u32(cell);
            struct rbt_row *row = NULL;
            enum rbt_scan_action action;

            if (key_size == 0u || key_size > cell_size - RBT_LEAF_CELL_HEADER_SIZE) {
                result = -EBADMSG;
                break;
            }
            if (end != NULL && rbt_compare_keys(cell + RBT_LEAF_CELL_HEADER_SIZE, key_size, end_key,
                                                end_size) >= 0) {
                page_id = 0u;
                break;
            }
            result = rbt_row_from_cell(tree, cell, cell_size, &row, NULL, NULL);
            if (result != 0) {
                break;
            }
            row->callback_lifetime = true;
            action = callback(context, row);
            row->callback_lifetime = false;
            rbt_row_release(row);
            if (action == RBT_SCAN_STOP) {
                page_id = 0u;
                break;
            }
            if (action != RBT_SCAN_CONTINUE) {
                result = -EINVAL;
                break;
            }
            ++index;
        }
        if (result != 0 || page_id == 0u || header.link0 == 0u) {
            break;
        }
        previous = page_id;
        page_id = header.link0;
        result = rbt_read_typed_page(tree, page_id, RBT_PAGE_LEAF, page, &header);
        first_leaf = false;
    }

    tree->scan_active = false;
    free(page);
    free(begin_key);
    free(end_key);
    return result;
}

int rbt_row_get_key(const struct rbt_row *row, size_t index, const struct rbt_value **out_value) {
    if (out_value != NULL) {
        *out_value = NULL;
    }
    if (row == NULL || out_value == NULL || index >= row->key_count) {
        return -EINVAL;
    }
    *out_value = &row->key[index];
    return 0;
}

int rbt_row_get_value(const struct rbt_row *row, size_t index, const struct rbt_value **out_value) {
    if (out_value != NULL) {
        *out_value = NULL;
    }
    if (row == NULL || out_value == NULL || index >= row->value_count) {
        return -EINVAL;
    }
    *out_value = &row->value[index];
    return 0;
}

int rbt_row_destroy(struct rbt_row *row) {
    if (row == NULL || row->callback_lifetime) {
        return -EINVAL;
    }
    rbt_row_release(row);
    return 0;
}

static void rbt_set_error(struct rbt_validation *validation, const char *format, ...) {
    va_list arguments;

    if (validation->error == NULL || validation->error_capacity == 0u ||
        validation->error[0] != '\0') {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(validation->error, validation->error_capacity, format, arguments);
    va_end(arguments);
}

static int rbt_validation_add_leaf(struct rbt_validation *validation, uint64_t page_id) {
    uint64_t *expanded;
    size_t capacity;

    if (validation->leaf_count == validation->leaf_capacity) {
        capacity = validation->leaf_capacity == 0u ? 16u : validation->leaf_capacity * 2u;
        if (capacity < validation->leaf_capacity || capacity > SIZE_MAX / sizeof(*expanded)) {
            return -ENOMEM;
        }
        expanded = realloc(validation->leaf_ids, capacity * sizeof(*expanded));
        if (expanded == NULL) {
            return -ENOMEM;
        }
        validation->leaf_ids = expanded;
        validation->leaf_capacity = capacity;
    }
    validation->leaf_ids[validation->leaf_count++] = page_id;
    return 0;
}

static int rbt_validate_node(struct rbt_validation *validation, uint64_t page_id,
                             uint32_t expected_level, bool root, const unsigned char *lower,
                             uint32_t lower_size, const unsigned char *upper, uint32_t upper_size,
                             unsigned char **out_minimum, uint32_t *out_minimum_size) {
    struct rbt *tree = validation->tree;
    unsigned char *page;
    struct rbt_page_header header;
    int result;
    uint32_t index;

    if (page_id == 0u || page_id >= tree->metadata.next_page_id ||
        validation->state[page_id] != RBT_VALIDATION_UNOWNED) {
        rbt_set_error(validation, "tree page has an invalid or shared reference");
        return -EBADMSG;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return -ENOMEM;
    }
    result = rbt_read_page(tree, page_id, page);
    if (result == 0) {
        result = rbt_decode_page_header(tree, page, page_id, &header);
    }
    if (result != 0 || header.level != expected_level || !rbt_slots_valid(tree, page, &header)) {
        rbt_set_error(validation, "invalid tree page header or slot layout");
        free(page);
        return result != 0 ? result : -EBADMSG;
    }
    validation->state[page_id] = RBT_VALIDATION_TREE;
    ++validation->tree_pages;
    if (expected_level == 0u) {
        uint32_t minimum_count = (tree->leaf_capacity + 1u) / 2u;

        if (header.type != RBT_PAGE_LEAF || header.count > tree->leaf_capacity ||
            (!root && header.count < minimum_count)) {
            rbt_set_error(validation, "invalid leaf occupancy");
            free(page);
            return -EBADMSG;
        }
        for (index = 0u; index < header.count; ++index) {
            uint32_t cell_size;
            const unsigned char *cell = rbt_slot_cell(page, index, &cell_size);
            uint32_t key_size;
            struct rbt_row *row = NULL;
            bool unordered = false;

            key_size = cell_size < RBT_LEAF_CELL_HEADER_SIZE ? 0u : rbt_load_u32(cell);
            if (index > 0u && key_size != 0u && key_size <= cell_size - RBT_LEAF_CELL_HEADER_SIZE) {
                uint32_t previous_size;
                const unsigned char *previous = rbt_slot_cell(page, index - 1u, &previous_size);
                uint32_t previous_key_size =
                    previous_size < RBT_LEAF_CELL_HEADER_SIZE ? 0u : rbt_load_u32(previous);

                unordered =
                    previous_key_size == 0u ||
                    previous_key_size > previous_size - RBT_LEAF_CELL_HEADER_SIZE ||
                    rbt_compare_keys(previous + RBT_LEAF_CELL_HEADER_SIZE, previous_key_size,
                                     cell + RBT_LEAF_CELL_HEADER_SIZE, key_size) >= 0;
            }
            if (key_size == 0u || key_size > cell_size - RBT_LEAF_CELL_HEADER_SIZE || unordered ||
                (lower != NULL && rbt_compare_keys(cell + RBT_LEAF_CELL_HEADER_SIZE, key_size,
                                                   lower, lower_size) < 0) ||
                (upper != NULL && rbt_compare_keys(cell + RBT_LEAF_CELL_HEADER_SIZE, key_size,
                                                   upper, upper_size) >= 0)) {
                rbt_set_error(validation, "leaf keys are malformed or unordered");
                free(page);
                return -EBADMSG;
            }
            result = rbt_row_from_cell(tree, cell, cell_size, &row, validation->state,
                                       &validation->overflow_pages);
            rbt_row_release(row);
            if (result != 0) {
                rbt_set_error(validation, "leaf row or overflow chain is invalid");
                free(page);
                return result;
            }
        }
        validation->items += header.count;
        result = rbt_validation_add_leaf(validation, page_id);
        if (result == 0 && header.count != 0u) {
            uint32_t cell_size;
            const unsigned char *cell = rbt_slot_cell(page, 0u, &cell_size);

            *out_minimum_size = rbt_load_u32(cell);
            *out_minimum = malloc(*out_minimum_size);
            if (*out_minimum == NULL) {
                result = -ENOMEM;
            } else {
                memcpy(*out_minimum, cell + RBT_LEAF_CELL_HEADER_SIZE, *out_minimum_size);
            }
        }
        free(page);
        return result;
    }
    {
        uint32_t minimum_count = ((tree->internal_capacity + 2u) / 2u) - 1u;
        unsigned char *first_minimum = NULL;
        uint32_t first_minimum_size = 0u;

        if (header.type != RBT_PAGE_INTERNAL || header.count == 0u ||
            header.count > tree->internal_capacity || (!root && header.count < minimum_count) ||
            header.link0 == 0u || header.link1 != 0u) {
            rbt_set_error(validation, "invalid internal occupancy");
            free(page);
            return -EBADMSG;
        }
        for (index = 0u; index < header.count; ++index) {
            const unsigned char *separator;
            uint32_t separator_size;
            uint64_t ignored;

            result = rbt_internal_cell(tree, page, index, &separator, &separator_size, &ignored);
            if (result == 0) {
                result = rbt_key_is_canonical(tree, separator, separator_size);
            }
            if (result == 0 && index > 0u) {
                const unsigned char *previous;
                uint32_t previous_size;

                result =
                    rbt_internal_cell(tree, page, index - 1u, &previous, &previous_size, &ignored);
                if (result == 0 &&
                    rbt_compare_keys(previous, previous_size, separator, separator_size) >= 0) {
                    result = -EBADMSG;
                }
            }
            if (result == 0 && lower != NULL &&
                rbt_compare_keys(separator, separator_size, lower, lower_size) < 0) {
                result = -EBADMSG;
            }
            if (result == 0 && upper != NULL &&
                rbt_compare_keys(separator, separator_size, upper, upper_size) >= 0) {
                result = -EBADMSG;
            }
            if (result != 0) {
                rbt_set_error(validation, "internal separator is noncanonical or unordered");
                free(page);
                return result;
            }
        }
        for (index = 0u; index <= header.count; ++index) {
            uint64_t child_id;
            const unsigned char *child_lower = lower;
            uint32_t child_lower_size = lower_size;
            const unsigned char *child_upper = upper;
            uint32_t child_upper_size = upper_size;
            unsigned char *child_minimum = NULL;
            uint32_t child_minimum_size = 0u;
            const unsigned char *separator = NULL;
            uint32_t separator_size = 0u;

            if (index == 0u) {
                child_id = header.link0;
            } else {
                result = rbt_internal_cell(tree, page, index - 1u, &separator, &separator_size,
                                           &child_id);
                if (result != 0) {
                    free(first_minimum);
                    free(page);
                    return result;
                }
                child_lower = separator;
                child_lower_size = separator_size;
            }
            if (index < header.count) {
                uint64_t ignored;

                result =
                    rbt_internal_cell(tree, page, index, &child_upper, &child_upper_size, &ignored);
                if (result != 0) {
                    free(first_minimum);
                    free(page);
                    return result;
                }
            }
            result = rbt_validate_node(validation, child_id, expected_level - 1u, false,
                                       child_lower, child_lower_size, child_upper, child_upper_size,
                                       &child_minimum, &child_minimum_size);
            if (result != 0) {
                free(first_minimum);
                free(page);
                return result;
            }
            if (index == 0u) {
                first_minimum = child_minimum;
                first_minimum_size = child_minimum_size;
            } else {
                if (child_minimum == NULL ||
                    rbt_compare_keys(separator, separator_size, child_minimum,
                                     child_minimum_size) != 0) {
                    free(child_minimum);
                    free(first_minimum);
                    rbt_set_error(validation, "separator differs from right-child minimum");
                    free(page);
                    return -EBADMSG;
                }
                free(child_minimum);
            }
        }
        *out_minimum = first_minimum;
        *out_minimum_size = first_minimum_size;
    }
    free(page);
    return 0;
}

static int rbt_validate_all(struct rbt *tree, struct rbt_stats *out_stats, char *error,
                            size_t error_capacity) {
    struct rbt_validation validation;
    uint64_t page_count;
    struct rbt_metadata metadata;
    unsigned char *minimum = NULL;
    uint32_t minimum_size = 0u;
    uint64_t free_pages = 0u;
    uint64_t page_id;
    unsigned char *page = NULL;
    int result;

    memset(&validation, 0, sizeof(validation));
    validation.tree = tree;
    validation.error = error;
    validation.error_capacity = error_capacity;
    result = rbt_page_count(tree, &page_count);
    if (result == 0) {
        result = rbt_read_metadata(tree, page_count, &metadata);
    }
    if (result != 0) {
        return result;
    }
    if (metadata.root_page_id != tree->metadata.root_page_id ||
        metadata.free_head != tree->metadata.free_head ||
        metadata.item_count != tree->metadata.item_count ||
        metadata.next_page_id != tree->metadata.next_page_id ||
        metadata.height != tree->metadata.height ||
        metadata.schema_head != tree->metadata.schema_head ||
        metadata.schema_size != tree->metadata.schema_size ||
        metadata.schema_page_count != tree->metadata.schema_page_count) {
        return -EBADMSG;
    }
    validation.state = calloc((size_t)page_count, 1u);
    page = malloc((size_t)tree->storage.page_size);
    if (validation.state == NULL || page == NULL) {
        free(validation.state);
        free(page);
        return -ENOMEM;
    }
    validation.state[0] = RBT_VALIDATION_METADATA;
    for (page_id = 0u; page_id < tree->metadata.schema_page_count; ++page_id) {
        uint64_t schema_page = tree->schema_pages[page_id];
        struct rbt_page_header header;
        uint64_t expected_next = page_id + 1u == tree->metadata.schema_page_count
                                     ? 0u
                                     : tree->schema_pages[page_id + 1u];
        uint64_t consumed = page_id * (tree->storage.page_size - RBT_PAGE_HEADER_SIZE);
        uint64_t remaining =
            consumed < tree->metadata.schema_size ? tree->metadata.schema_size - consumed : 0u;
        uint32_t expected_count =
            (uint32_t)(remaining < tree->storage.page_size - RBT_PAGE_HEADER_SIZE
                           ? remaining
                           : tree->storage.page_size - RBT_PAGE_HEADER_SIZE);

        if (schema_page == 0u || schema_page >= page_count ||
            validation.state[schema_page] != RBT_VALIDATION_UNOWNED ||
            rbt_read_typed_page(tree, schema_page, RBT_PAGE_SCHEMA, page, &header) != 0 ||
            header.aux != page_id || header.count != expected_count ||
            header.free_lower != RBT_PAGE_HEADER_SIZE + expected_count ||
            header.free_upper != tree->storage.page_size || header.link0 != expected_next ||
            header.link1 != 0u) {
            rbt_set_error(&validation, "schema chain ownership is invalid");
            result = -EBADMSG;
            goto cleanup;
        }
        validation.state[schema_page] = RBT_VALIDATION_SCHEMA;
    }
    result = rbt_validate_node(&validation, tree->metadata.root_page_id, tree->metadata.height - 1u,
                               true, NULL, 0u, NULL, 0u, &minimum, &minimum_size);
    free(minimum);
    if (result != 0) {
        goto cleanup;
    }
    for (page_id = 0u; page_id < validation.leaf_count; ++page_id) {
        struct rbt_page_header header;
        uint64_t expected_previous = page_id == 0u ? 0u : validation.leaf_ids[page_id - 1u];
        uint64_t expected_next =
            page_id + 1u == validation.leaf_count ? 0u : validation.leaf_ids[page_id + 1u];

        result =
            rbt_read_typed_page(tree, validation.leaf_ids[page_id], RBT_PAGE_LEAF, page, &header);
        if (result != 0 || header.link1 != expected_previous || header.link0 != expected_next) {
            rbt_set_error(&validation, "leaf links are inconsistent");
            result = result != 0 ? result : -EBADMSG;
            goto cleanup;
        }
    }
    page_id = tree->metadata.free_head;
    while (page_id != 0u) {
        struct rbt_page_header header;

        if (page_id >= page_count || validation.state[page_id] != RBT_VALIDATION_UNOWNED) {
            rbt_set_error(&validation, "freelist cycle or shared page");
            result = -EBADMSG;
            goto cleanup;
        }
        result = rbt_read_typed_page(tree, page_id, RBT_PAGE_FREE, page, &header);
        if (result != 0 || header.count != 0u || header.level != 0u ||
            header.free_lower != RBT_PAGE_HEADER_SIZE ||
            header.free_upper != tree->storage.page_size || header.link1 != 0u ||
            header.aux != 0u) {
            rbt_set_error(&validation, "invalid freelist page");
            result = result != 0 ? result : -EBADMSG;
            goto cleanup;
        }
        validation.state[page_id] = RBT_VALIDATION_FREELIST;
        ++free_pages;
        page_id = header.link0;
    }
    for (page_id = 0u; page_id < page_count; ++page_id) {
        if (validation.state[page_id] == RBT_VALIDATION_UNOWNED) {
            rbt_set_error(&validation, "allocated page has no unique owner");
            result = -EBADMSG;
            goto cleanup;
        }
    }
    if (validation.items != tree->metadata.item_count ||
        1u + tree->metadata.schema_page_count + validation.tree_pages + validation.overflow_pages +
                free_pages !=
            page_count) {
        rbt_set_error(&validation, "metadata counts do not match owned pages");
        result = -EBADMSG;
        goto cleanup;
    }
    if (out_stats != NULL) {
        out_stats->item_count = validation.items;
        out_stats->allocated_pages = page_count;
        out_stats->schema_pages = tree->metadata.schema_page_count;
        out_stats->tree_pages = validation.tree_pages;
        out_stats->overflow_pages = validation.overflow_pages;
        out_stats->free_pages = free_pages;
        out_stats->live_pages =
            1u + out_stats->schema_pages + out_stats->tree_pages + out_stats->overflow_pages;
        out_stats->height = tree->metadata.height;
        out_stats->page_size = tree->storage.page_size;
    }
    result = 0;

cleanup:
    free(page);
    free(validation.leaf_ids);
    free(validation.state);
    return result;
}

int rbt_validate(struct rbt *tree, struct rbt_stats *out_stats, char *error_out,
                 size_t error_capacity) {
    if (out_stats != NULL) {
        memset(out_stats, 0, sizeof(*out_stats));
    }
    if (error_out != NULL && error_capacity != 0u) {
        error_out[0] = '\0';
    }
    if (tree == NULL || (error_out == NULL && error_capacity != 0u)) {
        return -EINVAL;
    }
    if (tree->poisoned) {
        return -EOWNERDEAD;
    }
    if (tree->scan_active) {
        return -EBUSY;
    }
    return rbt_validate_all(tree, out_stats, error_out, error_capacity);
}

const char *rbt_strerror(int error) {
    unsigned int magnitude;

    if (error == 0) {
        return "success";
    }
    magnitude = error < 0 ? 0u - (unsigned int)error : (unsigned int)error;
    switch (magnitude) {
    case ENOENT:
        return "not found";
    case EINVAL:
        return "invalid argument";
    case ENOMEM:
        return "out of memory";
    case EIO:
        return "I/O error";
    case EBADMSG:
        return "corrupt data";
    case ENOTSUP:
        return "unsupported format";
    case EBUSY:
        return "busy";
    case EOWNERDEAD:
        return "commit outcome unknown";
    default:
        return "unknown error";
    }
}
