#include "btree_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct btree_metadata {
    uint64_t root_page_id;
    uint64_t free_head;
    uint64_t item_count;
    uint64_t next_page_id;
    uint32_t height;
    btree_schema schema;
} btree_metadata;

typedef struct btree_node_ref {
    unsigned char *minimum_key;
    unsigned char *maximum_key;
    bool has_keys;
} btree_node_ref;

typedef struct btree_node_header {
    uint8_t type;
    uint32_t count;
    uint32_t level;
    uint64_t next;
    uint64_t previous;
} btree_node_header;

struct btree {
    btree_storage storage;
    btree_metadata metadata;
    btree_schema schema;
    btree_compare_fn compare;
    void *compare_context;
    size_t leaf_stride;
    size_t internal_stride;
    uint32_t leaf_capacity;
    uint32_t internal_capacity;
    bool poisoned;
    bool scan_active;
};

typedef struct btree_validation {
    btree *tree;
    btree_metadata metadata;
    unsigned char *state;
    uint64_t *leaf_ids;
    uint64_t leaf_count;
    uint64_t live_pages;
    uint64_t item_count;
    char *error_out;
    size_t error_capacity;
} btree_validation;

typedef struct btree_txn_page {
    uint64_t page_id;
    unsigned char *data;
    bool dirty;
} btree_txn_page;

typedef struct btree_txn {
    btree *tree;
    btree_metadata metadata;
    btree_txn_page *pages;
    size_t page_count;
    size_t page_capacity;
} btree_txn;

typedef struct btree_path_entry {
    uint64_t page_id;
    uint32_t child_index;
} btree_path_entry;

typedef struct btree_path {
    btree_path_entry entries[64];
    size_t depth;
    uint64_t leaf_page_id;
} btree_path;

static bool btree_storage_valid(const btree_storage *storage) {
    return storage != NULL && btree_page_size_valid(storage->page_size) &&
           storage->read_page != NULL && storage->page_count != NULL &&
           storage->commit_pages != NULL;
}

static bool btree_options_valid(const btree_storage *storage, const btree_options *options,
                                size_t *leaf_stride_out, size_t *internal_stride_out,
                                uint32_t *leaf_capacity_out, uint32_t *internal_capacity_out) {
    if (options == NULL ||
        !btree_schema_layout(storage->page_size, options->key_size, options->value_size,
                             leaf_stride_out, internal_stride_out, leaf_capacity_out,
                             internal_capacity_out)) {
        return false;
    }
    if (options->compare == NULL) {
        return options->comparator_id == BTREE_COMPARATOR_LEXICOGRAPHIC;
    }
    return options->comparator_id >= BTREE_COMPARATOR_USER_MIN;
}

static void btree_configure(btree *tree, const btree_storage *storage, const btree_options *options,
                            size_t leaf_stride, size_t internal_stride, uint32_t leaf_capacity,
                            uint32_t internal_capacity) {
    tree->storage = *storage;
    tree->schema.key_size = options->key_size;
    tree->schema.value_size = options->value_size;
    tree->schema.comparator_id = options->comparator_id;
    tree->compare = options->compare;
    tree->compare_context = options->compare_context;
    tree->leaf_stride = leaf_stride;
    tree->internal_stride = internal_stride;
    tree->leaf_capacity = leaf_capacity;
    tree->internal_capacity = internal_capacity;
}

static int btree_compare_keys(const btree *tree, const void *left_key, const void *right_key) {
    if (tree->compare != NULL) {
        return tree->compare(tree->compare_context, left_key, right_key,
                             (size_t)tree->schema.key_size);
    }
    return memcmp(left_key, right_key, (size_t)tree->schema.key_size);
}

static unsigned char *btree_leaf_key(const btree *tree, unsigned char *page, uint32_t index) {
    return page + (size_t)BTREE_PAGE_PAYLOAD_OFFSET + (size_t)index * tree->leaf_stride;
}

static const unsigned char *btree_leaf_key_const(const btree *tree, const unsigned char *page,
                                                 uint32_t index) {
    return page + (size_t)BTREE_PAGE_PAYLOAD_OFFSET + (size_t)index * tree->leaf_stride;
}

static unsigned char *btree_leaf_value(const btree *tree, unsigned char *page, uint32_t index) {
    return btree_leaf_key(tree, page, index) + tree->schema.key_size;
}

static const unsigned char *btree_leaf_value_const(const btree *tree, const unsigned char *page,
                                                   uint32_t index) {
    return btree_leaf_key_const(tree, page, index) + tree->schema.key_size;
}

static uint64_t btree_internal_child(const btree *tree, const unsigned char *page, uint32_t index) {
    size_t offset = (size_t)BTREE_PAGE_PAYLOAD_OFFSET + (size_t)index * tree->internal_stride;

    return btree_load_u64(page + offset);
}

static unsigned char *btree_internal_key(const btree *tree, unsigned char *page, uint32_t index) {
    return page + (size_t)BTREE_PAGE_PAYLOAD_OFFSET + sizeof(uint64_t) +
           (size_t)index * tree->internal_stride;
}

static const unsigned char *btree_internal_key_const(const btree *tree, const unsigned char *page,
                                                     uint32_t index) {
    return page + (size_t)BTREE_PAGE_PAYLOAD_OFFSET + sizeof(uint64_t) +
           (size_t)index * tree->internal_stride;
}

static btree_status btree_backend_status(btree *tree, btree_status status) {
    if (status == BTREE_RECOVERY_REQUIRED) {
        tree->poisoned = true;
    }
    return status;
}

static btree_status btree_read_page(btree *tree, uint64_t page_id, unsigned char *page) {
    btree_status status;

    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    status = tree->storage.read_page(tree->storage.context, page_id, page);
    return btree_backend_status(tree, status);
}

static btree_status btree_get_page_count(btree *tree, uint64_t *count_out) {
    btree_status status;

    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    status = tree->storage.page_count(tree->storage.context, count_out);
    return btree_backend_status(tree, status);
}

static btree_status btree_commit_pages(btree *tree, const btree_page_update *updates,
                                       size_t update_count) {
    btree_status status;

    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    status = tree->storage.commit_pages(tree->storage.context, updates, update_count);
    return btree_backend_status(tree, status);
}

static void btree_set_error(btree_validation *validation, const char *format, ...) {
    va_list arguments;

    if (validation->error_out == NULL || validation->error_capacity == 0u ||
        validation->error_out[0] != '\0') {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(validation->error_out, validation->error_capacity, format, arguments);
    va_end(arguments);
}

static btree_status btree_decode_page_header_fields(const unsigned char *page,
                                                    uint64_t expected_page_id,
                                                    btree_node_header *header_out) {
    uint16_t version;
    uint8_t type;

    if (btree_load_u32(page + BTREE_PAGE_MAGIC_OFFSET) != BTREE_PAGE_MAGIC) {
        return BTREE_CORRUPT;
    }
    version = btree_load_u16(page + BTREE_PAGE_VERSION_OFFSET);
    if (version != (uint16_t)BTREE_FORMAT_VERSION) {
        return BTREE_UNSUPPORTED;
    }
    if (page[BTREE_PAGE_FLAGS_OFFSET] != 0u) {
        return BTREE_UNSUPPORTED;
    }
    if (btree_load_u64(page + BTREE_PAGE_ID_OFFSET) != expected_page_id) {
        return BTREE_CORRUPT;
    }
    type = page[BTREE_PAGE_TYPE_OFFSET];
    if (type < (uint8_t)BTREE_PAGE_LEAF || type > (uint8_t)BTREE_PAGE_FREE) {
        return BTREE_CORRUPT;
    }

    header_out->type = type;
    header_out->count = btree_load_u32(page + BTREE_PAGE_COUNT_OFFSET);
    header_out->level = btree_load_u32(page + BTREE_PAGE_LEVEL_OFFSET);
    header_out->next = btree_load_u64(page + BTREE_PAGE_NEXT_OFFSET);
    header_out->previous = btree_load_u64(page + BTREE_PAGE_PREVIOUS_OFFSET);
    return BTREE_OK;
}

static btree_status btree_decode_page_header(btree *tree, const unsigned char *page,
                                             uint64_t expected_page_id,
                                             btree_node_header *header_out) {
    if (!btree_page_checksum_valid(page, tree->storage.page_size)) {
        return BTREE_CORRUPT;
    }
    return btree_decode_page_header_fields(page, expected_page_id, header_out);
}

static btree_status btree_read_node(btree *tree, const btree_metadata *metadata, uint64_t page_id,
                                    unsigned char *page, btree_node_header *header_out) {
    btree_status status;

    if (page_id == 0u || page_id >= metadata->next_page_id) {
        return BTREE_CORRUPT;
    }
    status = btree_read_page(tree, page_id, page);
    if (status != BTREE_OK) {
        return status;
    }
    return btree_decode_page_header(tree, page, page_id, header_out);
}

static btree_status btree_read_metadata(btree *tree, uint64_t page_count, bool require_schema_match,
                                        btree_metadata *metadata_out) {
    unsigned char *page;
    btree_status status;
    uint16_t version;
    size_t leaf_stride;
    size_t internal_stride;
    uint32_t leaf_capacity;
    uint32_t internal_capacity;

    if (page_count < 2u) {
        return BTREE_CORRUPT;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    status = btree_read_page(tree, 0u, page);
    if (status != BTREE_OK) {
        free(page);
        return status;
    }
    if (!btree_page_checksum_valid(page, tree->storage.page_size) ||
        btree_load_u32(page + BTREE_PAGE_MAGIC_OFFSET) != BTREE_PAGE_MAGIC ||
        page[BTREE_PAGE_TYPE_OFFSET] != (unsigned char)BTREE_PAGE_META ||
        btree_load_u64(page + BTREE_PAGE_ID_OFFSET) != 0u) {
        free(page);
        return BTREE_CORRUPT;
    }
    version = btree_load_u16(page + BTREE_PAGE_VERSION_OFFSET);
    if (version != (uint16_t)BTREE_FORMAT_VERSION || page[BTREE_PAGE_FLAGS_OFFSET] != 0u) {
        free(page);
        return BTREE_UNSUPPORTED;
    }
    if (btree_load_u32(page + BTREE_META_PAGE_SIZE_OFFSET) != tree->storage.page_size) {
        free(page);
        return BTREE_CORRUPT;
    }

    metadata_out->root_page_id = btree_load_u64(page + BTREE_META_ROOT_OFFSET);
    metadata_out->free_head = btree_load_u64(page + BTREE_META_FREE_HEAD_OFFSET);
    metadata_out->item_count = btree_load_u64(page + BTREE_META_ITEM_COUNT_OFFSET);
    metadata_out->next_page_id = btree_load_u64(page + BTREE_META_NEXT_PAGE_OFFSET);
    metadata_out->height = btree_load_u32(page + BTREE_META_HEIGHT_OFFSET);
    metadata_out->schema.key_size = btree_load_u32(page + BTREE_META_KEY_SIZE_OFFSET);
    metadata_out->schema.value_size = btree_load_u32(page + BTREE_META_VALUE_SIZE_OFFSET);
    metadata_out->schema.comparator_id = btree_load_u64(page + BTREE_META_COMPARATOR_ID_OFFSET);
    free(page);

    if (metadata_out->next_page_id != page_count || metadata_out->root_page_id == 0u ||
        metadata_out->root_page_id >= metadata_out->next_page_id ||
        (metadata_out->free_head != 0u && metadata_out->free_head >= metadata_out->next_page_id) ||
        metadata_out->height == 0u || metadata_out->height > 64u) {
        return BTREE_CORRUPT;
    }
    if (!btree_schema_layout(tree->storage.page_size, metadata_out->schema.key_size,
                             metadata_out->schema.value_size, &leaf_stride, &internal_stride,
                             &leaf_capacity, &internal_capacity) ||
        (metadata_out->schema.comparator_id != BTREE_COMPARATOR_LEXICOGRAPHIC &&
         metadata_out->schema.comparator_id < BTREE_COMPARATOR_USER_MIN)) {
        return BTREE_CORRUPT;
    }
    if (require_schema_match &&
        (metadata_out->schema.key_size != tree->schema.key_size ||
         metadata_out->schema.value_size != tree->schema.value_size ||
         metadata_out->schema.comparator_id != tree->schema.comparator_id)) {
        return BTREE_SCHEMA_MISMATCH;
    }
    return BTREE_OK;
}

static btree_status btree_validate_node(btree_validation *validation, uint64_t page_id,
                                        uint32_t expected_level, bool is_root, bool has_lower,
                                        const unsigned char *lower, bool has_upper,
                                        const unsigned char *upper, btree_node_ref *result_out) {
    btree *tree = validation->tree;
    unsigned char *page;
    btree_node_header header;
    btree_status status;
    uint32_t capacity;
    uint32_t minimum_count;
    uint32_t index;

    if (page_id == 0u || page_id >= validation->metadata.next_page_id) {
        btree_set_error(validation, "page reference out of range");
        return BTREE_CORRUPT;
    }
    if (validation->state[page_id] != 0u) {
        btree_set_error(validation, "tree page cycle or duplicate reference");
        return BTREE_CORRUPT;
    }
    validation->state[page_id] = 1u;
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    status = btree_read_node(tree, &validation->metadata, page_id, page, &header);
    if (status != BTREE_OK) {
        btree_set_error(validation, "invalid tree page");
        free(page);
        return status;
    }
    if (header.level != expected_level) {
        btree_set_error(validation, "node level mismatch");
        free(page);
        return BTREE_CORRUPT;
    }

    if (expected_level == 0u) {
        capacity = tree->leaf_capacity;
        minimum_count = (capacity + 1u) / 2u;
        if (header.type != (uint8_t)BTREE_PAGE_LEAF || header.count > capacity ||
            (!is_root && header.count < minimum_count)) {
            btree_set_error(validation, "invalid leaf header or occupancy");
            free(page);
            return BTREE_CORRUPT;
        }
        for (index = 0u; index < header.count; ++index) {
            const unsigned char *key = btree_leaf_key_const(tree, page, index);

            if ((index > 0u && btree_compare_keys(
                                   tree, btree_leaf_key_const(tree, page, index - 1u), key) >= 0) ||
                (has_lower && btree_compare_keys(tree, key, lower) < 0) ||
                (has_upper && btree_compare_keys(tree, key, upper) >= 0)) {
                btree_set_error(validation, "leaf keys are not ordered");
                free(page);
                return BTREE_CORRUPT;
            }
        }
        if (UINT64_MAX - validation->item_count < header.count) {
            free(page);
            return BTREE_CORRUPT;
        }
        validation->item_count += header.count;
        validation->leaf_ids[validation->leaf_count] = page_id;
        validation->leaf_count++;
        result_out->has_keys = header.count != 0u;
        if (result_out->has_keys) {
            memcpy(result_out->minimum_key, btree_leaf_key_const(tree, page, 0u),
                   (size_t)tree->schema.key_size);
            memcpy(result_out->maximum_key, btree_leaf_key_const(tree, page, header.count - 1u),
                   (size_t)tree->schema.key_size);
        }
        validation->live_pages++;
        free(page);
        return BTREE_OK;
    }

    capacity = tree->internal_capacity;
    minimum_count = ((capacity + 2u) / 2u) - 1u;
    if (header.type != (uint8_t)BTREE_PAGE_INTERNAL || header.count > capacity ||
        header.count == 0u || (!is_root && header.count < minimum_count) || header.next != 0u ||
        header.previous != 0u) {
        btree_set_error(validation, "invalid internal header or occupancy");
        free(page);
        return BTREE_CORRUPT;
    }
    for (index = 0u; index < header.count; ++index) {
        const unsigned char *separator = btree_internal_key_const(tree, page, index);

        if ((index > 0u &&
             btree_compare_keys(tree, btree_internal_key_const(tree, page, index - 1u),
                                separator) >= 0) ||
            (has_lower && btree_compare_keys(tree, separator, lower) < 0) ||
            (has_upper && btree_compare_keys(tree, separator, upper) >= 0)) {
            btree_set_error(validation, "internal keys are not ordered");
            free(page);
            return BTREE_CORRUPT;
        }
    }

    result_out->has_keys = false;
    {
        unsigned char *child_keys = malloc((size_t)tree->schema.key_size * 2u);
        btree_node_ref child;

        if (child_keys == NULL) {
            free(page);
            return BTREE_OUT_OF_MEMORY;
        }
        child.minimum_key = child_keys;
        child.maximum_key = child_keys + tree->schema.key_size;
        for (index = 0u; index <= header.count; ++index) {
            uint64_t child_id = btree_internal_child(tree, page, index);
            bool child_has_lower = has_lower || index > 0u;
            bool child_has_upper = has_upper || index < header.count;
            const unsigned char *child_lower =
                index == 0u ? lower : btree_internal_key_const(tree, page, index - 1u);
            const unsigned char *child_upper =
                index == header.count ? upper : btree_internal_key_const(tree, page, index);

            status = btree_validate_node(validation, child_id, expected_level - 1u, false,
                                         child_has_lower, child_lower, child_has_upper, child_upper,
                                         &child);
            if (status != BTREE_OK) {
                free(child_keys);
                free(page);
                return status;
            }
            if (!child.has_keys) {
                btree_set_error(validation, "empty non-root subtree");
                free(child_keys);
                free(page);
                return BTREE_CORRUPT;
            }
            if (index == 0u) {
                memcpy(result_out->minimum_key, child.minimum_key, (size_t)tree->schema.key_size);
                result_out->has_keys = true;
            }
            memcpy(result_out->maximum_key, child.maximum_key, (size_t)tree->schema.key_size);
            if (index > 0u &&
                btree_compare_keys(tree, child.minimum_key,
                                   btree_internal_key_const(tree, page, index - 1u)) != 0) {
                btree_set_error(validation, "separator does not match right child minimum");
                free(child_keys);
                free(page);
                return BTREE_CORRUPT;
            }
            if (index < header.count &&
                btree_compare_keys(tree, child.maximum_key,
                                   btree_internal_key_const(tree, page, index)) >= 0) {
                btree_set_error(validation, "left child crosses separator");
                free(child_keys);
                free(page);
                return BTREE_CORRUPT;
            }
        }
        free(child_keys);
    }
    validation->live_pages++;
    free(page);
    return BTREE_OK;
}

static btree_status btree_validate_leaf_links(btree_validation *validation) {
    unsigned char *page;
    uint64_t index;

    page = malloc((size_t)validation->tree->storage.page_size);
    if (page == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    for (index = 0u; index < validation->leaf_count; ++index) {
        btree_node_header header;
        btree_status status = btree_read_node(validation->tree, &validation->metadata,
                                              validation->leaf_ids[index], page, &header);
        uint64_t expected_previous = index == 0u ? 0u : validation->leaf_ids[index - 1u];
        uint64_t expected_next =
            index + 1u == validation->leaf_count ? 0u : validation->leaf_ids[index + 1u];

        if (status != BTREE_OK) {
            free(page);
            return status;
        }
        if (header.type != (uint8_t)BTREE_PAGE_LEAF || header.previous != expected_previous ||
            header.next != expected_next) {
            btree_set_error(validation, "leaf links are inconsistent");
            free(page);
            return BTREE_CORRUPT;
        }
    }
    free(page);
    return BTREE_OK;
}

static btree_status btree_validate_freelist(btree_validation *validation,
                                            uint64_t *free_pages_out) {
    unsigned char *page;
    uint64_t page_id = validation->metadata.free_head;
    uint64_t free_pages = 0u;

    page = malloc((size_t)validation->tree->storage.page_size);
    if (page == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    while (page_id != 0u) {
        btree_node_header header;
        btree_status status;

        if (page_id >= validation->metadata.next_page_id || validation->state[page_id] != 0u) {
            btree_set_error(validation, "freelist cycle or invalid page");
            free(page);
            return BTREE_CORRUPT;
        }
        status = btree_read_node(validation->tree, &validation->metadata, page_id, page, &header);
        if (status != BTREE_OK) {
            free(page);
            return status;
        }
        if (header.type != (uint8_t)BTREE_PAGE_FREE || header.count != 0u || header.level != 0u ||
            header.previous != 0u) {
            btree_set_error(validation, "invalid freelist page");
            free(page);
            return BTREE_CORRUPT;
        }
        validation->state[page_id] = 2u;
        free_pages++;
        page_id = header.next;
    }
    free(page);
    *free_pages_out = free_pages;
    return BTREE_OK;
}

static btree_status btree_validate_all(btree *tree, btree_stats *stats_out, char *error_out,
                                       size_t error_capacity) {
    btree_validation validation;
    btree_node_ref root;
    unsigned char *root_keys = NULL;
    uint64_t page_count = 0u;
    uint64_t free_pages = 0u;
    uint64_t page_id;
    btree_status status;

    memset(&validation, 0, sizeof(validation));
    validation.tree = tree;
    validation.error_out = error_out;
    validation.error_capacity = error_capacity;
    status = btree_get_page_count(tree, &page_count);
    if (status != BTREE_OK) {
        return status;
    }
    status = btree_read_metadata(tree, page_count, true, &validation.metadata);
    if (status != BTREE_OK) {
        return status;
    }
    if (validation.metadata.next_page_id > (uint64_t)(SIZE_MAX / sizeof(*validation.leaf_ids))) {
        return BTREE_OUT_OF_MEMORY;
    }
    validation.state = calloc((size_t)validation.metadata.next_page_id, sizeof(*validation.state));
    validation.leaf_ids =
        malloc((size_t)validation.metadata.next_page_id * sizeof(*validation.leaf_ids));
    root_keys = malloc((size_t)tree->schema.key_size * 2u);
    if (validation.state == NULL || validation.leaf_ids == NULL || root_keys == NULL) {
        free(validation.state);
        free(validation.leaf_ids);
        free(root_keys);
        return BTREE_OUT_OF_MEMORY;
    }
    root.minimum_key = root_keys;
    root.maximum_key = root_keys + tree->schema.key_size;
    validation.state[0] = 3u;
    status =
        btree_validate_node(&validation, validation.metadata.root_page_id,
                            validation.metadata.height - 1u, true, false, NULL, false, NULL, &root);
    if (status == BTREE_OK) {
        status = btree_validate_leaf_links(&validation);
    }
    if (status == BTREE_OK) {
        status = btree_validate_freelist(&validation, &free_pages);
    }
    if (status == BTREE_OK) {
        for (page_id = 1u; page_id < validation.metadata.next_page_id; ++page_id) {
            if (validation.state[page_id] == 0u) {
                btree_set_error(&validation, "unowned allocated page");
                status = BTREE_CORRUPT;
                break;
            }
        }
    }
    if (status == BTREE_OK && validation.item_count != validation.metadata.item_count) {
        btree_set_error(&validation, "metadata item count mismatch");
        status = BTREE_CORRUPT;
    }
    if (status == BTREE_OK) {
        tree->metadata = validation.metadata;
        if (stats_out != NULL) {
            stats_out->item_count = validation.metadata.item_count;
            stats_out->allocated_pages = validation.metadata.next_page_id;
            stats_out->live_pages = validation.live_pages + 1u;
            stats_out->free_pages = free_pages;
            stats_out->comparator_id = tree->schema.comparator_id;
            stats_out->height = validation.metadata.height;
            stats_out->page_size = tree->storage.page_size;
            stats_out->key_size = tree->schema.key_size;
            stats_out->value_size = tree->schema.value_size;
            stats_out->leaf_capacity = tree->leaf_capacity;
            stats_out->internal_capacity = tree->internal_capacity;
        }
    }
    free(validation.state);
    free(validation.leaf_ids);
    free(root_keys);
    return status;
}

static void btree_page_initialize(unsigned char *page, uint64_t page_id, uint8_t type,
                                  uint32_t count, uint32_t level) {
    btree_store_u32(page + BTREE_PAGE_MAGIC_OFFSET, BTREE_PAGE_MAGIC);
    btree_store_u16(page + BTREE_PAGE_VERSION_OFFSET, (uint16_t)BTREE_FORMAT_VERSION);
    page[BTREE_PAGE_TYPE_OFFSET] = type;
    page[BTREE_PAGE_FLAGS_OFFSET] = 0u;
    btree_store_u64(page + BTREE_PAGE_ID_OFFSET, page_id);
    btree_store_u32(page + BTREE_PAGE_COUNT_OFFSET, count);
    btree_store_u32(page + BTREE_PAGE_LEVEL_OFFSET, level);
}

static void btree_metadata_encode(unsigned char *page, uint32_t page_size,
                                  const btree_metadata *metadata) {
    btree_page_initialize(page, 0u, (uint8_t)BTREE_PAGE_META, 0u, 0u);
    btree_store_u64(page + BTREE_META_ROOT_OFFSET, metadata->root_page_id);
    btree_store_u64(page + BTREE_META_FREE_HEAD_OFFSET, metadata->free_head);
    btree_store_u64(page + BTREE_META_ITEM_COUNT_OFFSET, metadata->item_count);
    btree_store_u64(page + BTREE_META_NEXT_PAGE_OFFSET, metadata->next_page_id);
    btree_store_u32(page + BTREE_META_HEIGHT_OFFSET, metadata->height);
    btree_store_u32(page + BTREE_META_PAGE_SIZE_OFFSET, page_size);
    btree_store_u32(page + BTREE_META_KEY_SIZE_OFFSET, metadata->schema.key_size);
    btree_store_u32(page + BTREE_META_VALUE_SIZE_OFFSET, metadata->schema.value_size);
    btree_store_u64(page + BTREE_META_COMPARATOR_ID_OFFSET, metadata->schema.comparator_id);
    btree_page_checksum_store(page, page_size);
}

static void btree_txn_destroy(btree_txn *transaction) {
    size_t index;

    for (index = 0u; index < transaction->page_count; ++index) {
        free(transaction->pages[index].data);
    }
    free(transaction->pages);
}

static btree_txn_page *btree_txn_find_page(btree_txn *transaction, uint64_t page_id) {
    size_t index;

    for (index = 0u; index < transaction->page_count; ++index) {
        if (transaction->pages[index].page_id == page_id) {
            return &transaction->pages[index];
        }
    }
    return NULL;
}

static btree_status btree_txn_reserve_page(btree_txn *transaction, uint64_t page_id,
                                           btree_txn_page **page_out) {
    btree_txn_page *expanded;
    size_t new_capacity;

    if (transaction->page_count == transaction->page_capacity) {
        if (transaction->page_capacity == 0u) {
            new_capacity = 8u;
        } else {
            if (transaction->page_capacity > SIZE_MAX / (2u * sizeof(*transaction->pages))) {
                return BTREE_OUT_OF_MEMORY;
            }
            new_capacity = transaction->page_capacity * 2u;
        }
        expanded = realloc(transaction->pages, new_capacity * sizeof(*expanded));
        if (expanded == NULL) {
            return BTREE_OUT_OF_MEMORY;
        }
        transaction->pages = expanded;
        transaction->page_capacity = new_capacity;
    }
    *page_out = &transaction->pages[transaction->page_count];
    (*page_out)->data = malloc((size_t)transaction->tree->storage.page_size);
    if ((*page_out)->data == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    (*page_out)->page_id = page_id;
    (*page_out)->dirty = false;
    transaction->page_count++;
    return BTREE_OK;
}

static btree_status btree_txn_load_node(btree_txn *transaction, uint64_t page_id,
                                        unsigned char **page_out, btree_node_header *header_out) {
    btree_txn_page *transaction_page = btree_txn_find_page(transaction, page_id);
    btree_status status;

    if (page_id == 0u || page_id >= transaction->metadata.next_page_id) {
        return BTREE_CORRUPT;
    }
    if (transaction_page == NULL) {
        if (page_id >= transaction->tree->metadata.next_page_id) {
            return BTREE_CORRUPT;
        }
        status = btree_txn_reserve_page(transaction, page_id, &transaction_page);
        if (status != BTREE_OK) {
            return status;
        }
        status = btree_read_page(transaction->tree, page_id, transaction_page->data);
        if (status != BTREE_OK) {
            free(transaction_page->data);
            transaction->page_count--;
            return status;
        }
    }
    status = transaction_page->dirty
                 ? btree_decode_page_header_fields(transaction_page->data, page_id, header_out)
                 : btree_decode_page_header(transaction->tree, transaction_page->data, page_id,
                                            header_out);
    if (status != BTREE_OK) {
        return status;
    }
    *page_out = transaction_page->data;
    return BTREE_OK;
}

static btree_status btree_txn_new_page(btree_txn *transaction, uint64_t page_id,
                                       unsigned char **page_out) {
    btree_txn_page *transaction_page;
    btree_status status;

    if (btree_txn_find_page(transaction, page_id) != NULL) {
        return BTREE_CORRUPT;
    }
    status = btree_txn_reserve_page(transaction, page_id, &transaction_page);
    if (status != BTREE_OK) {
        return status;
    }
    memset(transaction_page->data, 0, (size_t)transaction->tree->storage.page_size);
    transaction_page->dirty = true;
    *page_out = transaction_page->data;
    return BTREE_OK;
}

static btree_status btree_txn_mark_dirty(btree_txn *transaction, uint64_t page_id) {
    btree_txn_page *transaction_page = btree_txn_find_page(transaction, page_id);

    if (transaction_page == NULL) {
        return BTREE_CORRUPT;
    }
    transaction_page->dirty = true;
    return BTREE_OK;
}

static btree_status btree_txn_allocate_page(btree_txn *transaction, uint8_t type, uint32_t level,
                                            uint64_t *page_id_out, unsigned char **page_out) {
    unsigned char *page;
    uint64_t page_id;
    btree_status status;

    if (transaction->metadata.free_head != 0u) {
        btree_node_header header;

        page_id = transaction->metadata.free_head;
        status = btree_txn_load_node(transaction, page_id, &page, &header);
        if (status != BTREE_OK) {
            return status;
        }
        if (header.type != (uint8_t)BTREE_PAGE_FREE || header.count != 0u || header.level != 0u ||
            header.previous != 0u ||
            (header.next != 0u &&
             (header.next >= transaction->metadata.next_page_id || header.next == page_id))) {
            return BTREE_CORRUPT;
        }
        transaction->metadata.free_head = header.next;
        memset(page, 0, (size_t)transaction->tree->storage.page_size);
        status = btree_txn_mark_dirty(transaction, page_id);
        if (status != BTREE_OK) {
            return status;
        }
    } else {
        if (transaction->metadata.next_page_id == UINT64_MAX) {
            return BTREE_OUT_OF_MEMORY;
        }
        page_id = transaction->metadata.next_page_id;
        transaction->metadata.next_page_id++;
        status = btree_txn_new_page(transaction, page_id, &page);
        if (status != BTREE_OK) {
            return status;
        }
    }
    btree_page_initialize(page, page_id, type, 0u, level);
    *page_id_out = page_id;
    *page_out = page;
    return BTREE_OK;
}

static btree_status btree_txn_free_page(btree_txn *transaction, uint64_t page_id) {
    unsigned char *page;
    btree_node_header header;
    btree_status status = btree_txn_load_node(transaction, page_id, &page, &header);

    if (status != BTREE_OK) {
        return status;
    }
    if (header.type == (uint8_t)BTREE_PAGE_FREE) {
        return BTREE_CORRUPT;
    }
    memset(page, 0, (size_t)transaction->tree->storage.page_size);
    btree_page_initialize(page, page_id, (uint8_t)BTREE_PAGE_FREE, 0u, 0u);
    btree_store_u64(page + BTREE_PAGE_NEXT_OFFSET, transaction->metadata.free_head);
    transaction->metadata.free_head = page_id;
    return btree_txn_mark_dirty(transaction, page_id);
}

static btree_status btree_txn_commit(btree_txn *transaction) {
    unsigned char *metadata_page;
    btree_page_update *updates;
    size_t dirty_count = 0u;
    size_t update_index = 1u;
    size_t index;
    btree_status status;

    for (index = 0u; index < transaction->page_count; ++index) {
        if (transaction->pages[index].dirty) {
            dirty_count++;
        }
    }
    if (dirty_count == SIZE_MAX) {
        return BTREE_OUT_OF_MEMORY;
    }
    metadata_page = calloc(1u, (size_t)transaction->tree->storage.page_size);
    updates = malloc((dirty_count + 1u) * sizeof(*updates));
    if (metadata_page == NULL || updates == NULL) {
        free(metadata_page);
        free(updates);
        return BTREE_OUT_OF_MEMORY;
    }
    btree_metadata_encode(metadata_page, transaction->tree->storage.page_size,
                          &transaction->metadata);
    updates[0].page_id = 0u;
    updates[0].data = metadata_page;
    for (index = 0u; index < transaction->page_count; ++index) {
        if (!transaction->pages[index].dirty) {
            continue;
        }
        btree_page_checksum_store(transaction->pages[index].data,
                                  transaction->tree->storage.page_size);
        updates[update_index].page_id = transaction->pages[index].page_id;
        updates[update_index].data = transaction->pages[index].data;
        update_index++;
    }
    status = btree_commit_pages(transaction->tree, updates, dirty_count + 1u);
    if (status == BTREE_OK) {
        transaction->tree->metadata = transaction->metadata;
    }
    free(updates);
    free(metadata_page);
    return status;
}

static uint32_t btree_leaf_lower_bound(const btree *tree, const unsigned char *page, uint32_t count,
                                       const void *key) {
    uint32_t first = 0u;
    uint32_t length = count;

    while (length != 0u) {
        uint32_t half = length / 2u;
        uint32_t middle = first + half;

        if (btree_compare_keys(tree, btree_leaf_key_const(tree, page, middle), key) < 0) {
            first = middle + 1u;
            length -= half + 1u;
        } else {
            length = half;
        }
    }
    return first;
}

static uint32_t btree_internal_child_index(const btree *tree, const unsigned char *page,
                                           uint32_t count, const void *key) {
    uint32_t first = 0u;
    uint32_t length = count;

    while (length != 0u) {
        uint32_t half = length / 2u;
        uint32_t middle = first + half;

        if (btree_compare_keys(tree, btree_internal_key_const(tree, page, middle), key) <= 0) {
            first = middle + 1u;
            length -= half + 1u;
        } else {
            length = half;
        }
    }
    return first;
}

static btree_status btree_txn_find_path(btree_txn *transaction, const void *key, btree_path *path,
                                        unsigned char **leaf_out,
                                        btree_node_header *leaf_header_out) {
    uint64_t page_id = transaction->metadata.root_page_id;
    uint32_t expected_level = transaction->metadata.height - 1u;

    memset(path, 0, sizeof(*path));
    for (;;) {
        unsigned char *page;
        btree_node_header header;
        btree_status status = btree_txn_load_node(transaction, page_id, &page, &header);

        if (status != BTREE_OK) {
            return status;
        }
        if (header.level != expected_level) {
            return BTREE_CORRUPT;
        }
        if (expected_level == 0u) {
            if (header.type != (uint8_t)BTREE_PAGE_LEAF ||
                header.count > transaction->tree->leaf_capacity) {
                return BTREE_CORRUPT;
            }
            path->leaf_page_id = page_id;
            *leaf_out = page;
            *leaf_header_out = header;
            return BTREE_OK;
        }
        if (header.type != (uint8_t)BTREE_PAGE_INTERNAL || header.count == 0u ||
            header.count > transaction->tree->internal_capacity || header.next != 0u ||
            header.previous != 0u ||
            path->depth >= sizeof(path->entries) / sizeof(path->entries[0])) {
            return BTREE_CORRUPT;
        }
        path->entries[path->depth].page_id = page_id;
        path->entries[path->depth].child_index =
            btree_internal_child_index(transaction->tree, page, header.count, key);
        page_id =
            btree_internal_child(transaction->tree, page, path->entries[path->depth].child_index);
        path->depth++;
        expected_level--;
    }
}

static btree_status btree_txn_propagate_minimum(btree_txn *transaction, const btree_path *path,
                                                const void *minimum_key) {
    size_t depth = path->depth;

    while (depth != 0u) {
        const btree_path_entry *entry = &path->entries[depth - 1u];

        if (entry->child_index != 0u) {
            btree_txn_page *parent = btree_txn_find_page(transaction, entry->page_id);

            if (parent == NULL) {
                return BTREE_CORRUPT;
            }
            memcpy(btree_internal_key(transaction->tree, parent->data, entry->child_index - 1u),
                   minimum_key, (size_t)transaction->tree->schema.key_size);
            parent->dirty = true;
            return BTREE_OK;
        }
        depth--;
    }
    return BTREE_OK;
}

static void btree_leaf_encode(const btree *tree, unsigned char *page, uint64_t page_id,
                              const unsigned char *entries, uint32_t count, uint64_t previous,
                              uint64_t next) {
    memset(page, 0, (size_t)tree->storage.page_size);
    btree_page_initialize(page, page_id, (uint8_t)BTREE_PAGE_LEAF, count, 0u);
    btree_store_u64(page + BTREE_PAGE_PREVIOUS_OFFSET, previous);
    btree_store_u64(page + BTREE_PAGE_NEXT_OFFSET, next);
    memcpy(page + BTREE_PAGE_PAYLOAD_OFFSET, entries, (size_t)count * tree->leaf_stride);
}

static void btree_internal_encode(const btree *tree, unsigned char *page, uint64_t page_id,
                                  uint32_t level, const uint64_t *children,
                                  const unsigned char *keys, uint32_t count) {
    uint32_t index;

    memset(page, 0, (size_t)tree->storage.page_size);
    btree_page_initialize(page, page_id, (uint8_t)BTREE_PAGE_INTERNAL, count, level);
    btree_store_u64(page + BTREE_PAGE_PAYLOAD_OFFSET, children[0]);
    for (index = 0u; index <= count; ++index) {
        if (index != 0u) {
            memcpy(btree_internal_key(tree, page, index - 1u),
                   keys + (size_t)(index - 1u) * tree->schema.key_size,
                   (size_t)tree->schema.key_size);
            btree_store_u64(page + (size_t)BTREE_PAGE_PAYLOAD_OFFSET +
                                (size_t)index * tree->internal_stride,
                            children[index]);
        }
    }
}

static void btree_internal_insert(const btree *tree, unsigned char *page, uint32_t count,
                                  uint32_t child_index, const void *separator,
                                  uint64_t right_child) {
    size_t offset = (size_t)BTREE_PAGE_PAYLOAD_OFFSET + sizeof(uint64_t) +
                    (size_t)child_index * tree->internal_stride;
    size_t tail_size = (size_t)(count - child_index) * tree->internal_stride;

    memmove(page + offset + tree->internal_stride, page + offset, tail_size);
    memcpy(page + offset, separator, (size_t)tree->schema.key_size);
    btree_store_u64(page + offset + tree->schema.key_size, right_child);
    btree_store_u32(page + BTREE_PAGE_COUNT_OFFSET, count + 1u);
}

static void btree_internal_remove(const btree *tree, unsigned char *page, uint32_t count,
                                  uint32_t key_index) {
    size_t offset = (size_t)BTREE_PAGE_PAYLOAD_OFFSET + sizeof(uint64_t) +
                    (size_t)key_index * tree->internal_stride;
    size_t tail_size = (size_t)(count - key_index - 1u) * tree->internal_stride;

    memmove(page + offset, page + offset + tree->internal_stride, tail_size);
    memset(page + (size_t)BTREE_PAGE_PAYLOAD_OFFSET + sizeof(uint64_t) +
               (size_t)(count - 1u) * tree->internal_stride,
           0, tree->internal_stride);
    btree_store_u32(page + BTREE_PAGE_COUNT_OFFSET, count - 1u);
}

static btree_status btree_txn_split_internal(btree_txn *transaction, uint64_t page_id,
                                             unsigned char *page, const btree_node_header *header,
                                             uint32_t child_index, const void *separator,
                                             uint64_t right_child, void *promoted_out,
                                             uint64_t *right_page_id_out) {
    btree *tree = transaction->tree;
    uint32_t total_count = header->count + 1u;
    uint32_t middle = total_count / 2u;
    uint32_t right_count = total_count - middle - 1u;
    uint64_t *children;
    unsigned char *keys;
    uint64_t new_page_id;
    unsigned char *new_page;
    uint32_t index;
    btree_status status;

    children = malloc((size_t)(total_count + 1u) * sizeof(*children));
    keys = malloc((size_t)total_count * tree->schema.key_size);
    if (children == NULL || keys == NULL) {
        free(children);
        free(keys);
        return BTREE_OUT_OF_MEMORY;
    }
    for (index = 0u; index <= total_count; ++index) {
        if (index <= child_index) {
            children[index] = btree_internal_child(tree, page, index);
        } else if (index == child_index + 1u) {
            children[index] = right_child;
        } else {
            children[index] = btree_internal_child(tree, page, index - 1u);
        }
    }
    for (index = 0u; index < total_count; ++index) {
        if (index < child_index) {
            memcpy(keys + (size_t)index * tree->schema.key_size,
                   btree_internal_key_const(tree, page, index), (size_t)tree->schema.key_size);
        } else if (index == child_index) {
            memcpy(keys + (size_t)index * tree->schema.key_size, separator,
                   (size_t)tree->schema.key_size);
        } else {
            memcpy(keys + (size_t)index * tree->schema.key_size,
                   btree_internal_key_const(tree, page, index - 1u), (size_t)tree->schema.key_size);
        }
    }
    status = btree_txn_allocate_page(transaction, (uint8_t)BTREE_PAGE_INTERNAL, header->level,
                                     &new_page_id, &new_page);
    if (status == BTREE_OK) {
        memcpy(promoted_out, keys + (size_t)middle * tree->schema.key_size,
               (size_t)tree->schema.key_size);
        btree_internal_encode(tree, page, page_id, header->level, children, keys, middle);
        btree_internal_encode(tree, new_page, new_page_id, header->level, children + middle + 1u,
                              keys + (size_t)(middle + 1u) * tree->schema.key_size, right_count);
        status = btree_txn_mark_dirty(transaction, page_id);
    }
    free(keys);
    free(children);
    if (status == BTREE_OK) {
        *right_page_id_out = new_page_id;
    }
    return status;
}

static btree_status btree_txn_insert_parent(btree_txn *transaction, const btree_path *path,
                                            const void *separator, uint64_t right_child) {
    btree *tree = transaction->tree;
    size_t depth = path->depth;
    unsigned char *current_separator = malloc((size_t)tree->schema.key_size);
    btree_status status = BTREE_OK;

    if (current_separator == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    memcpy(current_separator, separator, (size_t)tree->schema.key_size);
    while (depth != 0u) {
        const btree_path_entry *entry = &path->entries[depth - 1u];
        unsigned char *parent;
        btree_node_header header;

        status = btree_txn_load_node(transaction, entry->page_id, &parent, &header);
        if (status != BTREE_OK) {
            goto cleanup;
        }
        if (header.type != (uint8_t)BTREE_PAGE_INTERNAL || header.count > tree->internal_capacity ||
            entry->child_index > header.count) {
            status = BTREE_CORRUPT;
            goto cleanup;
        }
        if (header.count < tree->internal_capacity) {
            btree_internal_insert(tree, parent, header.count, entry->child_index, current_separator,
                                  right_child);
            status = btree_txn_mark_dirty(transaction, entry->page_id);
            goto cleanup;
        }
        status = btree_txn_split_internal(transaction, entry->page_id, parent, &header,
                                          entry->child_index, current_separator, right_child,
                                          current_separator, &right_child);
        if (status != BTREE_OK) {
            goto cleanup;
        }
        depth--;
    }
    {
        uint64_t root_page_id;
        unsigned char *root_page;
        uint64_t children[2];

        if (transaction->metadata.height == UINT32_MAX) {
            status = BTREE_OUT_OF_MEMORY;
            goto cleanup;
        }
        status = btree_txn_allocate_page(transaction, (uint8_t)BTREE_PAGE_INTERNAL,
                                         transaction->metadata.height, &root_page_id, &root_page);
        if (status != BTREE_OK) {
            goto cleanup;
        }
        children[0] = transaction->metadata.root_page_id;
        children[1] = right_child;
        btree_internal_encode(tree, root_page, root_page_id, transaction->metadata.height, children,
                              current_separator, 1u);
        transaction->metadata.root_page_id = root_page_id;
        transaction->metadata.height++;
    }

cleanup:
    free(current_separator);
    return status;
}

static bool btree_internal_header_valid(const btree_txn *transaction,
                                        const btree_node_header *header, uint32_t level) {
    return header->type == (uint8_t)BTREE_PAGE_INTERNAL && header->level == level &&
           header->count <= transaction->tree->internal_capacity && header->next == 0u &&
           header->previous == 0u;
}

static btree_status btree_txn_rebalance_internal(btree_txn *transaction, const btree_path *path,
                                                 size_t node_position) {
    btree *tree = transaction->tree;
    uint32_t minimum_count = ((tree->internal_capacity + 2u) / 2u) - 1u;

    for (;;) {
        uint64_t node_page_id = path->entries[node_position].page_id;
        unsigned char *node;
        btree_node_header node_header;
        btree_status status = btree_txn_load_node(transaction, node_page_id, &node, &node_header);

        if (status != BTREE_OK) {
            return status;
        }
        if (!btree_internal_header_valid(transaction, &node_header, node_header.level)) {
            return BTREE_CORRUPT;
        }
        if (node_position == 0u) {
            if (node_header.count == 0u) {
                uint64_t child_page_id = btree_internal_child(tree, node, 0u);

                if (transaction->metadata.height <= 1u) {
                    return BTREE_CORRUPT;
                }
                transaction->metadata.root_page_id = child_page_id;
                transaction->metadata.height--;
                return btree_txn_free_page(transaction, node_page_id);
            }
            return BTREE_OK;
        }
        if (node_header.count >= minimum_count) {
            return BTREE_OK;
        }
        {
            const btree_path_entry *grand_entry = &path->entries[node_position - 1u];
            uint32_t node_index = grand_entry->child_index;
            unsigned char *grand;
            btree_node_header grand_header;
            unsigned char *left = NULL;
            unsigned char *right = NULL;
            btree_node_header left_header;
            btree_node_header right_header;
            uint64_t left_page_id = 0u;
            uint64_t right_page_id = 0u;

            status = btree_txn_load_node(transaction, grand_entry->page_id, &grand, &grand_header);
            if (status != BTREE_OK) {
                return status;
            }
            if (!btree_internal_header_valid(transaction, &grand_header, node_header.level + 1u) ||
                node_index > grand_header.count ||
                btree_internal_child(tree, grand, node_index) != node_page_id) {
                return BTREE_CORRUPT;
            }
            if (node_index != 0u) {
                left_page_id = btree_internal_child(tree, grand, node_index - 1u);
                status = btree_txn_load_node(transaction, left_page_id, &left, &left_header);
                if (status != BTREE_OK) {
                    return status;
                }
                if (!btree_internal_header_valid(transaction, &left_header, node_header.level) ||
                    left_header.count < minimum_count) {
                    return BTREE_CORRUPT;
                }
                if (left_header.count > minimum_count) {
                    const unsigned char *old_separator =
                        btree_internal_key_const(tree, grand, node_index - 1u);
                    uint64_t moved_child = btree_internal_child(tree, left, left_header.count);
                    size_t node_size =
                        sizeof(uint64_t) + (size_t)node_header.count * tree->internal_stride;

                    memmove(node + BTREE_PAGE_PAYLOAD_OFFSET + tree->internal_stride,
                            node + BTREE_PAGE_PAYLOAD_OFFSET, node_size);
                    btree_store_u64(node + BTREE_PAGE_PAYLOAD_OFFSET, moved_child);
                    memcpy(btree_internal_key(tree, node, 0u), old_separator,
                           (size_t)tree->schema.key_size);
                    btree_store_u32(node + BTREE_PAGE_COUNT_OFFSET, node_header.count + 1u);
                    memcpy(btree_internal_key(tree, grand, node_index - 1u),
                           btree_internal_key_const(tree, left, left_header.count - 1u),
                           (size_t)tree->schema.key_size);
                    btree_internal_remove(tree, left, left_header.count, left_header.count - 1u);
                    status = btree_txn_mark_dirty(transaction, node_page_id);
                    if (status == BTREE_OK) {
                        status = btree_txn_mark_dirty(transaction, left_page_id);
                    }
                    if (status == BTREE_OK) {
                        status = btree_txn_mark_dirty(transaction, grand_entry->page_id);
                    }
                    return status;
                }
            }
            if (node_index < grand_header.count) {
                right_page_id = btree_internal_child(tree, grand, node_index + 1u);
                status = btree_txn_load_node(transaction, right_page_id, &right, &right_header);
                if (status != BTREE_OK) {
                    return status;
                }
                if (!btree_internal_header_valid(transaction, &right_header, node_header.level) ||
                    right_header.count < minimum_count) {
                    return BTREE_CORRUPT;
                }
                if (right_header.count > minimum_count) {
                    const unsigned char *old_separator =
                        btree_internal_key_const(tree, grand, node_index);
                    uint64_t moved_child = btree_internal_child(tree, right, 0u);
                    size_t right_size =
                        sizeof(uint64_t) + (size_t)right_header.count * tree->internal_stride;

                    btree_internal_insert(tree, node, node_header.count, node_header.count,
                                          old_separator, moved_child);
                    memcpy(btree_internal_key(tree, grand, node_index),
                           btree_internal_key_const(tree, right, 0u),
                           (size_t)tree->schema.key_size);
                    memmove(right + BTREE_PAGE_PAYLOAD_OFFSET,
                            right + BTREE_PAGE_PAYLOAD_OFFSET + tree->internal_stride,
                            right_size - tree->internal_stride);
                    memset(right + BTREE_PAGE_PAYLOAD_OFFSET + right_size - tree->internal_stride,
                           0, tree->internal_stride);
                    btree_store_u32(right + BTREE_PAGE_COUNT_OFFSET, right_header.count - 1u);
                    status = btree_txn_mark_dirty(transaction, node_page_id);
                    if (status == BTREE_OK) {
                        status = btree_txn_mark_dirty(transaction, right_page_id);
                    }
                    if (status == BTREE_OK) {
                        status = btree_txn_mark_dirty(transaction, grand_entry->page_id);
                    }
                    return status;
                }
            }
            if (left != NULL) {
                const unsigned char *separator =
                    btree_internal_key_const(tree, grand, node_index - 1u);
                uint32_t merged_count = left_header.count + 1u + node_header.count;
                size_t destination = (size_t)BTREE_PAGE_PAYLOAD_OFFSET +
                                     (size_t)(left_header.count + 1u) * tree->internal_stride;
                size_t node_size =
                    sizeof(uint64_t) + (size_t)node_header.count * tree->internal_stride;

                if (merged_count > tree->internal_capacity) {
                    return BTREE_CORRUPT;
                }
                memcpy(left + destination - tree->schema.key_size, separator,
                       (size_t)tree->schema.key_size);
                memcpy(left + destination, node + BTREE_PAGE_PAYLOAD_OFFSET, node_size);
                btree_store_u32(left + BTREE_PAGE_COUNT_OFFSET, merged_count);
                status = btree_txn_mark_dirty(transaction, left_page_id);
                if (status == BTREE_OK) {
                    status = btree_txn_free_page(transaction, node_page_id);
                }
                if (status == BTREE_OK) {
                    btree_internal_remove(tree, grand, grand_header.count, node_index - 1u);
                    status = btree_txn_mark_dirty(transaction, grand_entry->page_id);
                }
            } else {
                const unsigned char *separator;
                uint32_t merged_count;
                size_t destination;
                size_t right_size;

                if (right == NULL) {
                    return BTREE_CORRUPT;
                }
                separator = btree_internal_key_const(tree, grand, node_index);
                merged_count = node_header.count + 1u + right_header.count;
                if (merged_count > tree->internal_capacity) {
                    return BTREE_CORRUPT;
                }
                destination = (size_t)BTREE_PAGE_PAYLOAD_OFFSET +
                              (size_t)(node_header.count + 1u) * tree->internal_stride;
                right_size = sizeof(uint64_t) + (size_t)right_header.count * tree->internal_stride;
                memcpy(node + destination - tree->schema.key_size, separator,
                       (size_t)tree->schema.key_size);
                memcpy(node + destination, right + BTREE_PAGE_PAYLOAD_OFFSET, right_size);
                btree_store_u32(node + BTREE_PAGE_COUNT_OFFSET, merged_count);
                status = btree_txn_mark_dirty(transaction, node_page_id);
                if (status == BTREE_OK) {
                    status = btree_txn_free_page(transaction, right_page_id);
                }
                if (status == BTREE_OK) {
                    btree_internal_remove(tree, grand, grand_header.count, node_index);
                    status = btree_txn_mark_dirty(transaction, grand_entry->page_id);
                }
            }
            if (status != BTREE_OK) {
                return status;
            }
        }
        node_position--;
    }
}

static bool btree_leaf_header_valid(const btree_txn *transaction, const btree_node_header *header) {
    return header->type == (uint8_t)BTREE_PAGE_LEAF && header->level == 0u &&
           header->count <= transaction->tree->leaf_capacity;
}

static btree_status btree_txn_rebalance_leaf(btree_txn *transaction, const btree_path *path) {
    btree *tree = transaction->tree;
    const btree_path_entry *parent_entry = &path->entries[path->depth - 1u];
    uint32_t leaf_index = parent_entry->child_index;
    uint32_t minimum_count = (tree->leaf_capacity + 1u) / 2u;
    unsigned char *leaf;
    unsigned char *parent;
    unsigned char *left = NULL;
    unsigned char *right = NULL;
    btree_node_header leaf_header;
    btree_node_header parent_header;
    btree_node_header left_header;
    btree_node_header right_header;
    uint64_t left_page_id = 0u;
    uint64_t right_page_id = 0u;
    btree_status status;

    status = btree_txn_load_node(transaction, path->leaf_page_id, &leaf, &leaf_header);
    if (status != BTREE_OK) {
        return status;
    }
    status = btree_txn_load_node(transaction, parent_entry->page_id, &parent, &parent_header);
    if (status != BTREE_OK) {
        return status;
    }
    if (!btree_leaf_header_valid(transaction, &leaf_header) ||
        parent_header.type != (uint8_t)BTREE_PAGE_INTERNAL || parent_header.level != 1u ||
        parent_header.count > tree->internal_capacity || leaf_index > parent_header.count ||
        btree_internal_child(tree, parent, leaf_index) != path->leaf_page_id) {
        return BTREE_CORRUPT;
    }
    if (leaf_index != 0u) {
        left_page_id = btree_internal_child(tree, parent, leaf_index - 1u);
        status = btree_txn_load_node(transaction, left_page_id, &left, &left_header);
        if (status != BTREE_OK) {
            return status;
        }
        if (!btree_leaf_header_valid(transaction, &left_header) ||
            left_header.count < minimum_count || left_header.next != path->leaf_page_id ||
            leaf_header.previous != left_page_id) {
            return BTREE_CORRUPT;
        }
        if (left_header.count > minimum_count) {
            size_t leaf_size = (size_t)leaf_header.count * tree->leaf_stride;
            size_t left_offset = (size_t)BTREE_PAGE_PAYLOAD_OFFSET +
                                 (size_t)(left_header.count - 1u) * tree->leaf_stride;

            memmove(leaf + BTREE_PAGE_PAYLOAD_OFFSET + tree->leaf_stride,
                    leaf + BTREE_PAGE_PAYLOAD_OFFSET, leaf_size);
            memcpy(leaf + BTREE_PAGE_PAYLOAD_OFFSET, left + left_offset, tree->leaf_stride);
            memset(left + left_offset, 0, tree->leaf_stride);
            btree_store_u32(leaf + BTREE_PAGE_COUNT_OFFSET, leaf_header.count + 1u);
            btree_store_u32(left + BTREE_PAGE_COUNT_OFFSET, left_header.count - 1u);
            memcpy(btree_internal_key(tree, parent, leaf_index - 1u),
                   btree_leaf_key_const(tree, leaf, 0u), (size_t)tree->schema.key_size);
            status = btree_txn_mark_dirty(transaction, path->leaf_page_id);
            if (status == BTREE_OK) {
                status = btree_txn_mark_dirty(transaction, left_page_id);
            }
            if (status == BTREE_OK) {
                status = btree_txn_mark_dirty(transaction, parent_entry->page_id);
            }
            return status;
        }
    }
    if (leaf_index < parent_header.count) {
        right_page_id = btree_internal_child(tree, parent, leaf_index + 1u);
        status = btree_txn_load_node(transaction, right_page_id, &right, &right_header);
        if (status != BTREE_OK) {
            return status;
        }
        if (!btree_leaf_header_valid(transaction, &right_header) ||
            right_header.count < minimum_count || leaf_header.next != right_page_id ||
            right_header.previous != path->leaf_page_id) {
            return BTREE_CORRUPT;
        }
        if (right_header.count > minimum_count) {
            size_t leaf_offset =
                (size_t)BTREE_PAGE_PAYLOAD_OFFSET + (size_t)leaf_header.count * tree->leaf_stride;
            size_t right_size = (size_t)right_header.count * tree->leaf_stride;

            memcpy(leaf + leaf_offset, right + BTREE_PAGE_PAYLOAD_OFFSET, tree->leaf_stride);
            memmove(right + BTREE_PAGE_PAYLOAD_OFFSET,
                    right + BTREE_PAGE_PAYLOAD_OFFSET + tree->leaf_stride,
                    right_size - tree->leaf_stride);
            memset(right + BTREE_PAGE_PAYLOAD_OFFSET + right_size - tree->leaf_stride, 0,
                   tree->leaf_stride);
            btree_store_u32(leaf + BTREE_PAGE_COUNT_OFFSET, leaf_header.count + 1u);
            btree_store_u32(right + BTREE_PAGE_COUNT_OFFSET, right_header.count - 1u);
            memcpy(btree_internal_key(tree, parent, leaf_index),
                   btree_leaf_key_const(tree, right, 0u), (size_t)tree->schema.key_size);
            status = btree_txn_mark_dirty(transaction, path->leaf_page_id);
            if (status == BTREE_OK) {
                status = btree_txn_mark_dirty(transaction, right_page_id);
            }
            if (status == BTREE_OK) {
                status = btree_txn_mark_dirty(transaction, parent_entry->page_id);
            }
            return status;
        }
    }
    if (left != NULL) {
        uint32_t merged_count = left_header.count + leaf_header.count;
        uint64_t next_page_id = leaf_header.next;

        if (merged_count > tree->leaf_capacity) {
            return BTREE_CORRUPT;
        }
        memcpy(left + BTREE_PAGE_PAYLOAD_OFFSET + (size_t)left_header.count * tree->leaf_stride,
               leaf + BTREE_PAGE_PAYLOAD_OFFSET, (size_t)leaf_header.count * tree->leaf_stride);
        btree_store_u32(left + BTREE_PAGE_COUNT_OFFSET, merged_count);
        btree_store_u64(left + BTREE_PAGE_NEXT_OFFSET, next_page_id);
        status = btree_txn_mark_dirty(transaction, left_page_id);
        if (status == BTREE_OK && next_page_id != 0u) {
            unsigned char *next;
            btree_node_header next_header;

            status = btree_txn_load_node(transaction, next_page_id, &next, &next_header);
            if (status == BTREE_OK && (!btree_leaf_header_valid(transaction, &next_header) ||
                                       next_header.previous != path->leaf_page_id)) {
                status = BTREE_CORRUPT;
            }
            if (status == BTREE_OK) {
                btree_store_u64(next + BTREE_PAGE_PREVIOUS_OFFSET, left_page_id);
                status = btree_txn_mark_dirty(transaction, next_page_id);
            }
        }
        if (status == BTREE_OK) {
            status = btree_txn_free_page(transaction, path->leaf_page_id);
        }
        if (status == BTREE_OK) {
            btree_internal_remove(tree, parent, parent_header.count, leaf_index - 1u);
            status = btree_txn_mark_dirty(transaction, parent_entry->page_id);
        }
    } else {
        uint32_t merged_count;
        uint64_t next_page_id;

        if (right == NULL) {
            return BTREE_CORRUPT;
        }
        merged_count = leaf_header.count + right_header.count;
        next_page_id = right_header.next;
        if (merged_count > tree->leaf_capacity) {
            return BTREE_CORRUPT;
        }
        memcpy(leaf + BTREE_PAGE_PAYLOAD_OFFSET + (size_t)leaf_header.count * tree->leaf_stride,
               right + BTREE_PAGE_PAYLOAD_OFFSET, (size_t)right_header.count * tree->leaf_stride);
        btree_store_u32(leaf + BTREE_PAGE_COUNT_OFFSET, merged_count);
        btree_store_u64(leaf + BTREE_PAGE_NEXT_OFFSET, next_page_id);
        status = btree_txn_mark_dirty(transaction, path->leaf_page_id);
        if (status == BTREE_OK && next_page_id != 0u) {
            unsigned char *next;
            btree_node_header next_header;

            status = btree_txn_load_node(transaction, next_page_id, &next, &next_header);
            if (status == BTREE_OK && (!btree_leaf_header_valid(transaction, &next_header) ||
                                       next_header.previous != right_page_id)) {
                status = BTREE_CORRUPT;
            }
            if (status == BTREE_OK) {
                btree_store_u64(next + BTREE_PAGE_PREVIOUS_OFFSET, path->leaf_page_id);
                status = btree_txn_mark_dirty(transaction, next_page_id);
            }
        }
        if (status == BTREE_OK) {
            status = btree_txn_free_page(transaction, right_page_id);
        }
        if (status == BTREE_OK) {
            btree_internal_remove(tree, parent, parent_header.count, leaf_index);
            status = btree_txn_mark_dirty(transaction, parent_entry->page_id);
        }
    }
    if (status != BTREE_OK) {
        return status;
    }
    return btree_txn_rebalance_internal(transaction, path, path->depth - 1u);
}

static btree_status btree_initialize_empty_tree(btree *tree) {
    btree_metadata metadata;
    unsigned char *metadata_page;
    unsigned char *root_page;
    btree_page_update updates[2];
    btree_status status;

    metadata.root_page_id = 1u;
    metadata.free_head = 0u;
    metadata.item_count = 0u;
    metadata.next_page_id = 2u;
    metadata.height = 1u;
    metadata.schema = tree->schema;
    metadata_page = calloc(1u, (size_t)tree->storage.page_size);
    root_page = calloc(1u, (size_t)tree->storage.page_size);
    if (metadata_page == NULL || root_page == NULL) {
        free(metadata_page);
        free(root_page);
        return BTREE_OUT_OF_MEMORY;
    }
    btree_metadata_encode(metadata_page, tree->storage.page_size, &metadata);
    btree_page_initialize(root_page, metadata.root_page_id, (uint8_t)BTREE_PAGE_LEAF, 0u, 0u);
    btree_page_checksum_store(root_page, tree->storage.page_size);
    updates[0].page_id = 0u;
    updates[0].data = metadata_page;
    updates[1].page_id = metadata.root_page_id;
    updates[1].data = root_page;
    status = btree_commit_pages(tree, updates, 2u);
    if (status == BTREE_OK) {
        tree->metadata = metadata;
    }
    free(root_page);
    free(metadata_page);
    return status;
}

void btree_options_init(btree_options *options, uint32_t key_size, uint32_t value_size) {
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->key_size = key_size;
    options->value_size = value_size;
    options->comparator_id = BTREE_COMPARATOR_LEXICOGRAPHIC;
}

btree_status btree_create(const btree_storage *storage, const btree_options *options,
                          btree **tree_out) {
    btree *tree;
    size_t leaf_stride;
    size_t internal_stride;
    uint32_t leaf_capacity;
    uint32_t internal_capacity;
    uint64_t page_count = 0u;
    btree_status status;

    if (tree_out != NULL) {
        *tree_out = NULL;
    }
    if (!btree_storage_valid(storage) || tree_out == NULL ||
        !btree_options_valid(storage, options, &leaf_stride, &internal_stride, &leaf_capacity,
                             &internal_capacity)) {
        return BTREE_INVALID_ARGUMENT;
    }
    status = storage->page_count(storage->context, &page_count);
    if (status != BTREE_OK) {
        return status;
    }
    if (page_count != 0u) {
        return BTREE_INVALID_ARGUMENT;
    }
    tree = calloc(1u, sizeof(*tree));
    if (tree == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    btree_configure(tree, storage, options, leaf_stride, internal_stride, leaf_capacity,
                    internal_capacity);
    status = btree_initialize_empty_tree(tree);
    if (status != BTREE_OK) {
        free(tree);
        return status;
    }
    *tree_out = tree;
    return BTREE_OK;
}

btree_status btree_open(const btree_storage *storage, const btree_options *options,
                        btree **tree_out) {
    btree *tree;
    size_t leaf_stride;
    size_t internal_stride;
    uint32_t leaf_capacity;
    uint32_t internal_capacity;
    btree_status status;

    if (tree_out != NULL) {
        *tree_out = NULL;
    }
    if (!btree_storage_valid(storage) || tree_out == NULL ||
        !btree_options_valid(storage, options, &leaf_stride, &internal_stride, &leaf_capacity,
                             &internal_capacity)) {
        return BTREE_INVALID_ARGUMENT;
    }
    tree = calloc(1u, sizeof(*tree));
    if (tree == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    btree_configure(tree, storage, options, leaf_stride, internal_stride, leaf_capacity,
                    internal_capacity);
    status = btree_validate_all(tree, NULL, NULL, 0u);
    if (status != BTREE_OK) {
        free(tree);
        return status;
    }
    *tree_out = tree;
    return BTREE_OK;
}

btree_status btree_read_schema(const btree_storage *storage, btree_schema *schema_out) {
    btree tree;
    btree_metadata metadata;
    uint64_t page_count = 0u;
    btree_status status;

    if (schema_out != NULL) {
        memset(schema_out, 0, sizeof(*schema_out));
    }
    if (!btree_storage_valid(storage) || schema_out == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    memset(&tree, 0, sizeof(tree));
    tree.storage = *storage;
    status = btree_get_page_count(&tree, &page_count);
    if (status != BTREE_OK) {
        return status;
    }
    status = btree_read_metadata(&tree, page_count, false, &metadata);
    if (status != BTREE_OK) {
        return status;
    }
    *schema_out = metadata.schema;
    return BTREE_OK;
}

void btree_close(btree *tree) {
    free(tree);
}

static btree_status btree_find_leaf(btree *tree, const void *key, unsigned char *page,
                                    btree_node_header *header_out) {
    uint64_t page_id = tree->metadata.root_page_id;
    uint32_t expected_level = tree->metadata.height - 1u;

    for (;;) {
        btree_node_header header;
        btree_status status = btree_read_node(tree, &tree->metadata, page_id, page, &header);

        if (status != BTREE_OK) {
            return status;
        }
        if (header.level != expected_level) {
            return BTREE_CORRUPT;
        }
        if (expected_level == 0u) {
            if (header.type != (uint8_t)BTREE_PAGE_LEAF || header.count > tree->leaf_capacity) {
                return BTREE_CORRUPT;
            }
            *header_out = header;
            return BTREE_OK;
        }
        if (header.type != (uint8_t)BTREE_PAGE_INTERNAL || header.count == 0u ||
            header.count > tree->internal_capacity || header.next != 0u || header.previous != 0u) {
            return BTREE_CORRUPT;
        }
        page_id = btree_internal_child(
            tree, page,
            key == NULL ? 0u : btree_internal_child_index(tree, page, header.count, key));
        expected_level--;
    }
}

btree_status btree_get(btree *tree, const void *key, void *value_out) {
    unsigned char *page;
    btree_node_header header;
    uint32_t first;
    btree_status status;

    if (tree == NULL || key == NULL || value_out == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    status = btree_find_leaf(tree, key, page, &header);
    if (status != BTREE_OK) {
        free(page);
        return status;
    }
    first = btree_leaf_lower_bound(tree, page, header.count, key);
    if (first == header.count ||
        btree_compare_keys(tree, btree_leaf_key_const(tree, page, first), key) != 0) {
        free(page);
        return BTREE_NOT_FOUND;
    }
    memcpy(value_out, btree_leaf_value_const(tree, page, first), (size_t)tree->schema.value_size);
    free(page);
    return BTREE_OK;
}

btree_status btree_put(btree *tree, const void *key, const void *value, bool *inserted_out) {
    btree_txn transaction;
    btree_path path;
    unsigned char *leaf;
    btree_node_header leaf_header;
    uint32_t position;
    uint32_t capacity;
    btree_status status;

    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    if (tree == NULL || inserted_out == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (tree->scan_active) {
        return BTREE_BUSY;
    }
    if (key == NULL || value == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    memset(&transaction, 0, sizeof(transaction));
    transaction.tree = tree;
    transaction.metadata = tree->metadata;
    status = btree_txn_find_path(&transaction, key, &path, &leaf, &leaf_header);
    if (status != BTREE_OK) {
        btree_txn_destroy(&transaction);
        return status;
    }
    position = btree_leaf_lower_bound(tree, leaf, leaf_header.count, key);
    if (position < leaf_header.count &&
        btree_compare_keys(tree, btree_leaf_key_const(tree, leaf, position), key) == 0) {
        unsigned char *stored_value = btree_leaf_value(tree, leaf, position);

        if (memcmp(stored_value, value, (size_t)tree->schema.value_size) == 0) {
            btree_txn_destroy(&transaction);
            return BTREE_OK;
        }
        memcpy(stored_value, value, (size_t)tree->schema.value_size);
        status = btree_txn_mark_dirty(&transaction, path.leaf_page_id);
        if (status == BTREE_OK) {
            status = btree_txn_commit(&transaction);
        }
        btree_txn_destroy(&transaction);
        return status;
    }
    if (transaction.metadata.item_count == UINT64_MAX) {
        btree_txn_destroy(&transaction);
        return BTREE_OUT_OF_MEMORY;
    }
    capacity = tree->leaf_capacity;
    transaction.metadata.item_count++;
    if (leaf_header.count < capacity) {
        size_t entry_offset =
            (size_t)BTREE_PAGE_PAYLOAD_OFFSET + (size_t)position * tree->leaf_stride;
        size_t tail_size = (size_t)(leaf_header.count - position) * tree->leaf_stride;

        memmove(leaf + entry_offset + tree->leaf_stride, leaf + entry_offset, tail_size);
        memcpy(leaf + entry_offset, key, (size_t)tree->schema.key_size);
        memcpy(leaf + entry_offset + tree->schema.key_size, value, (size_t)tree->schema.value_size);
        btree_store_u32(leaf + BTREE_PAGE_COUNT_OFFSET, leaf_header.count + 1u);
        status = btree_txn_mark_dirty(&transaction, path.leaf_page_id);
        if (status == BTREE_OK && position == 0u) {
            status = btree_txn_propagate_minimum(&transaction, &path, key);
        }
        if (status == BTREE_OK) {
            status = btree_txn_commit(&transaction);
        }
    } else {
        uint32_t total_count = capacity + 1u;
        uint32_t left_count = total_count / 2u;
        uint32_t right_count;
        unsigned char *entries = malloc((size_t)total_count * tree->leaf_stride);
        unsigned char *new_entry;
        uint64_t right_page_id = 0u;
        unsigned char *right_page = NULL;

        if (entries == NULL) {
            btree_txn_destroy(&transaction);
            return BTREE_OUT_OF_MEMORY;
        }
        memcpy(entries, leaf + BTREE_PAGE_PAYLOAD_OFFSET, (size_t)position * tree->leaf_stride);
        new_entry = entries + (size_t)position * tree->leaf_stride;
        memcpy(new_entry, key, (size_t)tree->schema.key_size);
        memcpy(new_entry + tree->schema.key_size, value, (size_t)tree->schema.value_size);
        memcpy(new_entry + tree->leaf_stride,
               leaf + BTREE_PAGE_PAYLOAD_OFFSET + (size_t)position * tree->leaf_stride,
               (size_t)(leaf_header.count - position) * tree->leaf_stride);
        if (position >= left_count) {
            left_count++;
        }
        right_count = total_count - left_count;
        status = btree_txn_allocate_page(&transaction, (uint8_t)BTREE_PAGE_LEAF, 0u, &right_page_id,
                                         &right_page);
        if (status == BTREE_OK) {
            btree_leaf_encode(tree, leaf, path.leaf_page_id, entries, left_count,
                              leaf_header.previous, right_page_id);
            btree_leaf_encode(tree, right_page, right_page_id,
                              entries + (size_t)left_count * tree->leaf_stride, right_count,
                              path.leaf_page_id, leaf_header.next);
            status = btree_txn_mark_dirty(&transaction, path.leaf_page_id);
        }
        if (status == BTREE_OK && leaf_header.next != 0u) {
            unsigned char *next_page;
            btree_node_header next_header;

            status = btree_txn_load_node(&transaction, leaf_header.next, &next_page, &next_header);
            if (status == BTREE_OK && (!btree_leaf_header_valid(&transaction, &next_header) ||
                                       next_header.previous != path.leaf_page_id)) {
                status = BTREE_CORRUPT;
            }
            if (status == BTREE_OK) {
                btree_store_u64(next_page + BTREE_PAGE_PREVIOUS_OFFSET, right_page_id);
                status = btree_txn_mark_dirty(&transaction, leaf_header.next);
            }
        }
        if (status == BTREE_OK && position == 0u) {
            status = btree_txn_propagate_minimum(&transaction, &path, key);
        }
        if (status == BTREE_OK) {
            status = btree_txn_insert_parent(&transaction, &path,
                                             entries + (size_t)left_count * tree->leaf_stride,
                                             right_page_id);
        }
        if (status == BTREE_OK) {
            status = btree_txn_commit(&transaction);
        }
        free(entries);
    }
    if (status == BTREE_OK) {
        *inserted_out = true;
    }
    btree_txn_destroy(&transaction);
    return status;
}

btree_status btree_delete(btree *tree, const void *key, bool *removed_out) {
    btree_txn transaction;
    btree_path path;
    unsigned char *leaf;
    btree_node_header leaf_header;
    uint32_t position;
    uint32_t new_count;
    btree_status status;

    if (removed_out != NULL) {
        *removed_out = false;
    }
    if (tree == NULL || removed_out == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (tree->scan_active) {
        return BTREE_BUSY;
    }
    if (key == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    memset(&transaction, 0, sizeof(transaction));
    transaction.tree = tree;
    transaction.metadata = tree->metadata;
    status = btree_txn_find_path(&transaction, key, &path, &leaf, &leaf_header);
    if (status != BTREE_OK) {
        btree_txn_destroy(&transaction);
        return status;
    }
    position = btree_leaf_lower_bound(tree, leaf, leaf_header.count, key);
    if (position == leaf_header.count ||
        btree_compare_keys(tree, btree_leaf_key_const(tree, leaf, position), key) != 0) {
        btree_txn_destroy(&transaction);
        return BTREE_OK;
    }
    if (transaction.metadata.item_count == 0u) {
        btree_txn_destroy(&transaction);
        return BTREE_CORRUPT;
    }
    new_count = leaf_header.count - 1u;
    memmove(leaf + BTREE_PAGE_PAYLOAD_OFFSET + (size_t)position * tree->leaf_stride,
            leaf + BTREE_PAGE_PAYLOAD_OFFSET + (size_t)(position + 1u) * tree->leaf_stride,
            (size_t)(new_count - position) * tree->leaf_stride);
    memset(leaf + BTREE_PAGE_PAYLOAD_OFFSET + (size_t)new_count * tree->leaf_stride, 0,
           tree->leaf_stride);
    btree_store_u32(leaf + BTREE_PAGE_COUNT_OFFSET, new_count);
    transaction.metadata.item_count--;
    status = btree_txn_mark_dirty(&transaction, path.leaf_page_id);
    if (status == BTREE_OK && position == 0u && new_count != 0u) {
        status =
            btree_txn_propagate_minimum(&transaction, &path, btree_leaf_key_const(tree, leaf, 0u));
    }
    if (status == BTREE_OK && path.depth != 0u && new_count < (tree->leaf_capacity + 1u) / 2u) {
        status = btree_txn_rebalance_leaf(&transaction, &path);
    }
    if (status == BTREE_OK) {
        status = btree_txn_commit(&transaction);
    }
    if (status == BTREE_OK) {
        *removed_out = true;
    }
    btree_txn_destroy(&transaction);
    return status;
}

btree_status btree_scan(btree *tree, const void *begin_key, const void *end_key,
                        btree_scan_fn callback, void *context) {
    unsigned char *page = NULL;
    btree_node_header header;
    uint64_t page_id;
    uint64_t previous_page_id = 0u;
    uint64_t visited = 0u;
    bool first_leaf = true;
    btree_status status;

    if (tree == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (tree->scan_active) {
        return BTREE_BUSY;
    }
    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    if (callback == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (begin_key != NULL && end_key != NULL && btree_compare_keys(tree, begin_key, end_key) >= 0) {
        return BTREE_OK;
    }
    tree->scan_active = true;
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        status = BTREE_OUT_OF_MEMORY;
        goto cleanup;
    }
    status = btree_find_leaf(tree, begin_key, page, &header);
    if (status != BTREE_OK) {
        goto cleanup;
    }
    page_id = btree_load_u64(page + BTREE_PAGE_ID_OFFSET);

    while (page_id != 0u) {
        uint32_t index = 0u;

        if (visited++ >= tree->metadata.next_page_id) {
            status = BTREE_CORRUPT;
            goto cleanup;
        }
        if (!first_leaf) {
            status = btree_read_node(tree, &tree->metadata, page_id, page, &header);
            if (status != BTREE_OK) {
                goto cleanup;
            }
        }
        if (header.type != (uint8_t)BTREE_PAGE_LEAF || header.count > tree->leaf_capacity ||
            (!first_leaf && header.previous != previous_page_id)) {
            status = BTREE_CORRUPT;
            goto cleanup;
        }
        if (first_leaf && begin_key != NULL) {
            index = btree_leaf_lower_bound(tree, page, header.count, begin_key);
        }
        for (; index < header.count; ++index) {
            const unsigned char *key = btree_leaf_key_const(tree, page, index);
            btree_scan_action action;

            if (end_key != NULL && btree_compare_keys(tree, key, end_key) >= 0) {
                status = BTREE_OK;
                goto cleanup;
            }
            action = callback(context, key, btree_leaf_value_const(tree, page, index));
            if (action == BTREE_SCAN_STOP) {
                status = BTREE_STOPPED;
                goto cleanup;
            }
        }
        previous_page_id = page_id;
        page_id = header.next;
        first_leaf = false;
    }
    status = BTREE_OK;

cleanup:
    free(page);
    tree->scan_active = false;
    return status;
}

btree_status btree_validate(btree *tree, btree_stats *stats_out, char *error_out,
                            size_t error_capacity) {
    if (stats_out != NULL) {
        memset(stats_out, 0, sizeof(*stats_out));
    }
    if (error_out != NULL && error_capacity != 0u) {
        error_out[0] = '\0';
    }
    if (tree == NULL || (error_out == NULL && error_capacity != 0u)) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BTREE_RECOVERY_REQUIRED;
    }
    return btree_validate_all(tree, stats_out, error_out, error_capacity);
}

const char *btree_status_string(btree_status status) {
    switch (status) {
    case BTREE_OK:
        return "ok";
    case BTREE_NOT_FOUND:
        return "not found";
    case BTREE_STOPPED:
        return "stopped";
    case BTREE_INVALID_ARGUMENT:
        return "invalid argument";
    case BTREE_OUT_OF_MEMORY:
        return "out of memory";
    case BTREE_IO:
        return "I/O error";
    case BTREE_CORRUPT:
        return "corrupt";
    case BTREE_UNSUPPORTED:
        return "unsupported";
    case BTREE_BUSY:
        return "busy";
    case BTREE_RECOVERY_REQUIRED:
        return "recovery required";
    case BTREE_SCHEMA_MISMATCH:
        return "schema mismatch";
    default:
        return "unknown status";
    }
}
