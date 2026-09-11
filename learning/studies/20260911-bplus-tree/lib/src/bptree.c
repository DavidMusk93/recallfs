#include "bptree_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bpt_metadata {
    uint64_t root_page_id;
    uint64_t free_head;
    uint64_t item_count;
    uint64_t next_page_id;
    uint32_t height;
} bpt_metadata;

typedef struct bpt_entry {
    uint64_t key;
    uint64_t value;
} bpt_entry;

typedef struct bpt_node_ref {
    uint64_t minimum_key;
    uint64_t maximum_key;
    bool has_keys;
} bpt_node_ref;

typedef struct bpt_node_header {
    uint8_t type;
    uint32_t count;
    uint32_t level;
    uint64_t next;
    uint64_t previous;
} bpt_node_header;

struct bpt_tree {
    bpt_storage storage;
    bpt_metadata metadata;
    bool poisoned;
};

typedef struct bpt_validation {
    bpt_tree *tree;
    bpt_metadata metadata;
    unsigned char *state;
    uint64_t *leaf_ids;
    uint64_t leaf_count;
    uint64_t live_pages;
    uint64_t item_count;
    char *error_out;
    size_t error_capacity;
} bpt_validation;

typedef struct bpt_txn_page {
    uint64_t page_id;
    unsigned char *data;
    bool dirty;
} bpt_txn_page;

typedef struct bpt_txn {
    bpt_tree *tree;
    bpt_metadata metadata;
    bpt_txn_page *pages;
    size_t page_count;
    size_t page_capacity;
} bpt_txn;

typedef struct bpt_path_entry {
    uint64_t page_id;
    uint32_t child_index;
} bpt_path_entry;

typedef struct bpt_path {
    bpt_path_entry entries[64];
    size_t depth;
    uint64_t leaf_page_id;
} bpt_path;

static bool bpt_storage_valid(const bpt_storage *storage) {
    return storage != NULL && bpt_page_size_valid(storage->page_size) &&
           storage->read_page != NULL && storage->page_count != NULL &&
           storage->commit_pages != NULL;
}

static bpt_status bpt_backend_status(bpt_tree *tree, bpt_status status) {
    if (status == BPT_RECOVERY_REQUIRED) {
        tree->poisoned = true;
    }
    return status;
}

static bpt_status bpt_read_page(bpt_tree *tree, uint64_t page_id, unsigned char *page) {
    bpt_status status;

    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    status = tree->storage.read_page(tree->storage.context, page_id, page);
    return bpt_backend_status(tree, status);
}

static bpt_status bpt_get_page_count(bpt_tree *tree, uint64_t *count_out) {
    bpt_status status;

    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    status = tree->storage.page_count(tree->storage.context, count_out);
    return bpt_backend_status(tree, status);
}

static bpt_status bpt_commit_pages(bpt_tree *tree, const bpt_page_update *updates,
                                   size_t update_count) {
    bpt_status status;

    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    status = tree->storage.commit_pages(tree->storage.context, updates, update_count);
    return bpt_backend_status(tree, status);
}

static void bpt_set_error(bpt_validation *validation, const char *format, ...) {
    va_list arguments;

    if (validation->error_out == NULL || validation->error_capacity == 0u ||
        validation->error_out[0] != '\0') {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(validation->error_out, validation->error_capacity, format, arguments);
    va_end(arguments);
}

static bpt_status bpt_decode_page_header_fields(const unsigned char *page,
                                                uint64_t expected_page_id,
                                                bpt_node_header *header_out) {
    uint16_t version;
    uint8_t type;

    if (bpt_load_u32(page + BPT_PAGE_MAGIC_OFFSET) != BPT_PAGE_MAGIC) {
        return BPT_CORRUPT;
    }
    version = bpt_load_u16(page + BPT_PAGE_VERSION_OFFSET);
    if (version != (uint16_t)BPTREE_FORMAT_VERSION) {
        return BPT_UNSUPPORTED;
    }
    if (page[BPT_PAGE_FLAGS_OFFSET] != 0u) {
        return BPT_UNSUPPORTED;
    }
    if (bpt_load_u64(page + BPT_PAGE_ID_OFFSET) != expected_page_id) {
        return BPT_CORRUPT;
    }
    type = page[BPT_PAGE_TYPE_OFFSET];
    if (type < (uint8_t)BPT_PAGE_LEAF || type > (uint8_t)BPT_PAGE_FREE) {
        return BPT_CORRUPT;
    }

    header_out->type = type;
    header_out->count = bpt_load_u32(page + BPT_PAGE_COUNT_OFFSET);
    header_out->level = bpt_load_u32(page + BPT_PAGE_LEVEL_OFFSET);
    header_out->next = bpt_load_u64(page + BPT_PAGE_NEXT_OFFSET);
    header_out->previous = bpt_load_u64(page + BPT_PAGE_PREVIOUS_OFFSET);
    return BPT_OK;
}

static bpt_status bpt_decode_page_header(bpt_tree *tree, const unsigned char *page,
                                         uint64_t expected_page_id, bpt_node_header *header_out) {
    if (!bpt_page_checksum_valid(page, tree->storage.page_size)) {
        return BPT_CORRUPT;
    }
    return bpt_decode_page_header_fields(page, expected_page_id, header_out);
}

static bpt_status bpt_read_node(bpt_tree *tree, const bpt_metadata *metadata, uint64_t page_id,
                                unsigned char *page, bpt_node_header *header_out) {
    bpt_status status;

    if (page_id == 0u || page_id >= metadata->next_page_id) {
        return BPT_CORRUPT;
    }
    status = bpt_read_page(tree, page_id, page);
    if (status != BPT_OK) {
        return status;
    }
    return bpt_decode_page_header(tree, page, page_id, header_out);
}

static bpt_status bpt_read_metadata(bpt_tree *tree, uint64_t page_count,
                                    bpt_metadata *metadata_out) {
    unsigned char *page;
    bpt_status status;
    uint16_t version;

    if (page_count < 2u) {
        return BPT_CORRUPT;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    status = bpt_read_page(tree, 0u, page);
    if (status != BPT_OK) {
        free(page);
        return status;
    }
    if (!bpt_page_checksum_valid(page, tree->storage.page_size) ||
        bpt_load_u32(page + BPT_PAGE_MAGIC_OFFSET) != BPT_PAGE_MAGIC ||
        page[BPT_PAGE_TYPE_OFFSET] != (unsigned char)BPT_PAGE_META ||
        bpt_load_u64(page + BPT_PAGE_ID_OFFSET) != 0u) {
        free(page);
        return BPT_CORRUPT;
    }
    version = bpt_load_u16(page + BPT_PAGE_VERSION_OFFSET);
    if (version != (uint16_t)BPTREE_FORMAT_VERSION || page[BPT_PAGE_FLAGS_OFFSET] != 0u) {
        free(page);
        return BPT_UNSUPPORTED;
    }
    if (bpt_load_u32(page + BPT_META_PAGE_SIZE_OFFSET) != tree->storage.page_size) {
        free(page);
        return BPT_CORRUPT;
    }

    metadata_out->root_page_id = bpt_load_u64(page + BPT_META_ROOT_OFFSET);
    metadata_out->free_head = bpt_load_u64(page + BPT_META_FREE_HEAD_OFFSET);
    metadata_out->item_count = bpt_load_u64(page + BPT_META_ITEM_COUNT_OFFSET);
    metadata_out->next_page_id = bpt_load_u64(page + BPT_META_NEXT_PAGE_OFFSET);
    metadata_out->height = bpt_load_u32(page + BPT_META_HEIGHT_OFFSET);
    free(page);

    if (metadata_out->next_page_id != page_count || metadata_out->root_page_id == 0u ||
        metadata_out->root_page_id >= metadata_out->next_page_id ||
        (metadata_out->free_head != 0u && metadata_out->free_head >= metadata_out->next_page_id) ||
        metadata_out->height == 0u || metadata_out->height > 64u) {
        return BPT_CORRUPT;
    }
    return BPT_OK;
}

static uint64_t bpt_leaf_key(const unsigned char *page, uint32_t index) {
    size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)index * 16u;

    return bpt_load_u64(page + offset);
}

static uint64_t bpt_leaf_value(const unsigned char *page, uint32_t index) {
    size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)index * 16u + 8u;

    return bpt_load_u64(page + offset);
}

static uint64_t bpt_internal_child(const unsigned char *page, uint32_t index) {
    size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)index * 16u;

    return bpt_load_u64(page + offset);
}

static uint64_t bpt_internal_key(const unsigned char *page, uint32_t index) {
    size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)index * 16u + 8u;

    return bpt_load_u64(page + offset);
}

static bpt_status bpt_validate_node(bpt_validation *validation, uint64_t page_id,
                                    uint32_t expected_level, bool is_root, bool has_lower,
                                    uint64_t lower, bool has_upper, uint64_t upper,
                                    bpt_node_ref *result_out) {
    bpt_tree *tree = validation->tree;
    unsigned char *page;
    bpt_node_header header;
    bpt_status status;
    uint32_t capacity;
    uint32_t minimum_count;
    uint32_t index;

    if (page_id == 0u || page_id >= validation->metadata.next_page_id) {
        bpt_set_error(validation, "page reference out of range");
        return BPT_CORRUPT;
    }
    if (validation->state[page_id] != 0u) {
        bpt_set_error(validation, "tree page cycle or duplicate reference");
        return BPT_CORRUPT;
    }
    validation->state[page_id] = 1u;
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    status = bpt_read_node(tree, &validation->metadata, page_id, page, &header);
    if (status != BPT_OK) {
        bpt_set_error(validation, "invalid tree page");
        free(page);
        return status;
    }
    if (header.level != expected_level) {
        bpt_set_error(validation, "node level mismatch");
        free(page);
        return BPT_CORRUPT;
    }

    if (expected_level == 0u) {
        capacity = bpt_leaf_capacity(tree->storage.page_size);
        minimum_count = (capacity + 1u) / 2u;
        if (header.type != (uint8_t)BPT_PAGE_LEAF || header.count > capacity ||
            (!is_root && header.count < minimum_count)) {
            bpt_set_error(validation, "invalid leaf header or occupancy");
            free(page);
            return BPT_CORRUPT;
        }
        for (index = 0u; index < header.count; ++index) {
            uint64_t key = bpt_leaf_key(page, index);

            if ((index > 0u && bpt_leaf_key(page, index - 1u) >= key) ||
                (has_lower && key < lower) || (has_upper && key >= upper)) {
                bpt_set_error(validation, "leaf keys are not ordered");
                free(page);
                return BPT_CORRUPT;
            }
        }
        if (UINT64_MAX - validation->item_count < header.count) {
            free(page);
            return BPT_CORRUPT;
        }
        validation->item_count += header.count;
        validation->leaf_ids[validation->leaf_count] = page_id;
        validation->leaf_count++;
        result_out->has_keys = header.count != 0u;
        result_out->minimum_key = header.count == 0u ? 0u : bpt_leaf_key(page, 0u);
        result_out->maximum_key = header.count == 0u ? 0u : bpt_leaf_key(page, header.count - 1u);
        validation->live_pages++;
        free(page);
        return BPT_OK;
    }

    capacity = bpt_internal_capacity(tree->storage.page_size);
    minimum_count = ((capacity + 2u) / 2u) - 1u;
    if (header.type != (uint8_t)BPT_PAGE_INTERNAL || header.count > capacity ||
        header.count == 0u || (!is_root && header.count < minimum_count) || header.next != 0u ||
        header.previous != 0u) {
        bpt_set_error(validation, "invalid internal header or occupancy");
        free(page);
        return BPT_CORRUPT;
    }
    for (index = 0u; index < header.count; ++index) {
        uint64_t separator = bpt_internal_key(page, index);

        if ((index > 0u && bpt_internal_key(page, index - 1u) >= separator) ||
            (has_lower && separator < lower) || (has_upper && separator >= upper)) {
            bpt_set_error(validation, "internal keys are not ordered");
            free(page);
            return BPT_CORRUPT;
        }
    }

    result_out->has_keys = false;
    for (index = 0u; index <= header.count; ++index) {
        uint64_t child_id = bpt_internal_child(page, index);
        bool child_has_lower = has_lower || index > 0u;
        bool child_has_upper = has_upper || index < header.count;
        uint64_t child_lower = index == 0u ? lower : bpt_internal_key(page, index - 1u);
        uint64_t child_upper = index == header.count ? upper : bpt_internal_key(page, index);
        bpt_node_ref child;

        status =
            bpt_validate_node(validation, child_id, expected_level - 1u, false, child_has_lower,
                              child_lower, child_has_upper, child_upper, &child);
        if (status != BPT_OK) {
            free(page);
            return status;
        }
        if (!child.has_keys) {
            bpt_set_error(validation, "empty non-root subtree");
            free(page);
            return BPT_CORRUPT;
        }
        if (index == 0u) {
            result_out->minimum_key = child.minimum_key;
            result_out->has_keys = true;
        }
        result_out->maximum_key = child.maximum_key;
        if (index < header.count && child.maximum_key >= bpt_internal_key(page, index)) {
            bpt_set_error(validation, "left child crosses separator");
            free(page);
            return BPT_CORRUPT;
        }
    }
    validation->live_pages++;
    free(page);
    return BPT_OK;
}

static bpt_status bpt_validate_leaf_links(bpt_validation *validation) {
    unsigned char *page;
    uint64_t index;

    page = malloc((size_t)validation->tree->storage.page_size);
    if (page == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    for (index = 0u; index < validation->leaf_count; ++index) {
        bpt_node_header header;
        bpt_status status = bpt_read_node(validation->tree, &validation->metadata,
                                          validation->leaf_ids[index], page, &header);
        uint64_t expected_previous = index == 0u ? 0u : validation->leaf_ids[index - 1u];
        uint64_t expected_next =
            index + 1u == validation->leaf_count ? 0u : validation->leaf_ids[index + 1u];

        if (status != BPT_OK) {
            free(page);
            return status;
        }
        if (header.type != (uint8_t)BPT_PAGE_LEAF || header.previous != expected_previous ||
            header.next != expected_next) {
            bpt_set_error(validation, "leaf links are inconsistent");
            free(page);
            return BPT_CORRUPT;
        }
    }
    free(page);
    return BPT_OK;
}

static bpt_status bpt_validate_freelist(bpt_validation *validation, uint64_t *free_pages_out) {
    unsigned char *page;
    uint64_t page_id = validation->metadata.free_head;
    uint64_t free_pages = 0u;

    page = malloc((size_t)validation->tree->storage.page_size);
    if (page == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    while (page_id != 0u) {
        bpt_node_header header;
        bpt_status status;

        if (page_id >= validation->metadata.next_page_id || validation->state[page_id] != 0u) {
            bpt_set_error(validation, "freelist cycle or invalid page");
            free(page);
            return BPT_CORRUPT;
        }
        status = bpt_read_node(validation->tree, &validation->metadata, page_id, page, &header);
        if (status != BPT_OK) {
            free(page);
            return status;
        }
        if (header.type != (uint8_t)BPT_PAGE_FREE || header.count != 0u || header.level != 0u ||
            header.previous != 0u) {
            bpt_set_error(validation, "invalid freelist page");
            free(page);
            return BPT_CORRUPT;
        }
        validation->state[page_id] = 2u;
        free_pages++;
        page_id = header.next;
    }
    free(page);
    *free_pages_out = free_pages;
    return BPT_OK;
}

static bpt_status bpt_validate_all(bpt_tree *tree, bpt_stats *stats_out, char *error_out,
                                   size_t error_capacity) {
    bpt_validation validation;
    bpt_node_ref root;
    uint64_t page_count = 0u;
    uint64_t free_pages = 0u;
    uint64_t page_id;
    bpt_status status;

    memset(&validation, 0, sizeof(validation));
    validation.tree = tree;
    validation.error_out = error_out;
    validation.error_capacity = error_capacity;
    status = bpt_get_page_count(tree, &page_count);
    if (status != BPT_OK) {
        return status;
    }
    status = bpt_read_metadata(tree, page_count, &validation.metadata);
    if (status != BPT_OK) {
        return status;
    }
    if (validation.metadata.next_page_id > (uint64_t)(SIZE_MAX / sizeof(*validation.leaf_ids))) {
        return BPT_OUT_OF_MEMORY;
    }
    validation.state = calloc((size_t)validation.metadata.next_page_id, sizeof(*validation.state));
    validation.leaf_ids =
        malloc((size_t)validation.metadata.next_page_id * sizeof(*validation.leaf_ids));
    if (validation.state == NULL || validation.leaf_ids == NULL) {
        free(validation.state);
        free(validation.leaf_ids);
        return BPT_OUT_OF_MEMORY;
    }
    validation.state[0] = 3u;
    status = bpt_validate_node(&validation, validation.metadata.root_page_id,
                               validation.metadata.height - 1u, true, false, 0u, false, 0u, &root);
    if (status == BPT_OK) {
        status = bpt_validate_leaf_links(&validation);
    }
    if (status == BPT_OK) {
        status = bpt_validate_freelist(&validation, &free_pages);
    }
    if (status == BPT_OK) {
        for (page_id = 1u; page_id < validation.metadata.next_page_id; ++page_id) {
            if (validation.state[page_id] == 0u) {
                bpt_set_error(&validation, "unowned allocated page");
                status = BPT_CORRUPT;
                break;
            }
        }
    }
    if (status == BPT_OK && validation.item_count != validation.metadata.item_count) {
        bpt_set_error(&validation, "metadata item count mismatch");
        status = BPT_CORRUPT;
    }
    if (status == BPT_OK) {
        tree->metadata = validation.metadata;
        if (stats_out != NULL) {
            stats_out->item_count = validation.metadata.item_count;
            stats_out->allocated_pages = validation.metadata.next_page_id;
            stats_out->live_pages = validation.live_pages + 1u;
            stats_out->free_pages = free_pages;
            stats_out->height = validation.metadata.height;
            stats_out->page_size = tree->storage.page_size;
            stats_out->leaf_capacity = bpt_leaf_capacity(tree->storage.page_size);
            stats_out->internal_capacity = bpt_internal_capacity(tree->storage.page_size);
        }
    }
    free(validation.state);
    free(validation.leaf_ids);
    return status;
}

static void bpt_page_initialize(unsigned char *page, uint64_t page_id, uint8_t type, uint32_t count,
                                uint32_t level) {
    bpt_store_u32(page + BPT_PAGE_MAGIC_OFFSET, BPT_PAGE_MAGIC);
    bpt_store_u16(page + BPT_PAGE_VERSION_OFFSET, (uint16_t)BPTREE_FORMAT_VERSION);
    page[BPT_PAGE_TYPE_OFFSET] = type;
    page[BPT_PAGE_FLAGS_OFFSET] = 0u;
    bpt_store_u64(page + BPT_PAGE_ID_OFFSET, page_id);
    bpt_store_u32(page + BPT_PAGE_COUNT_OFFSET, count);
    bpt_store_u32(page + BPT_PAGE_LEVEL_OFFSET, level);
}

static void bpt_metadata_encode(unsigned char *page, uint32_t page_size,
                                const bpt_metadata *metadata) {
    bpt_page_initialize(page, 0u, (uint8_t)BPT_PAGE_META, 0u, 0u);
    bpt_store_u64(page + BPT_META_ROOT_OFFSET, metadata->root_page_id);
    bpt_store_u64(page + BPT_META_FREE_HEAD_OFFSET, metadata->free_head);
    bpt_store_u64(page + BPT_META_ITEM_COUNT_OFFSET, metadata->item_count);
    bpt_store_u64(page + BPT_META_NEXT_PAGE_OFFSET, metadata->next_page_id);
    bpt_store_u32(page + BPT_META_HEIGHT_OFFSET, metadata->height);
    bpt_store_u32(page + BPT_META_PAGE_SIZE_OFFSET, page_size);
    bpt_page_checksum_store(page, page_size);
}

static void bpt_txn_destroy(bpt_txn *transaction) {
    size_t index;

    for (index = 0u; index < transaction->page_count; ++index) {
        free(transaction->pages[index].data);
    }
    free(transaction->pages);
}

static bpt_txn_page *bpt_txn_find_page(bpt_txn *transaction, uint64_t page_id) {
    size_t index;

    for (index = 0u; index < transaction->page_count; ++index) {
        if (transaction->pages[index].page_id == page_id) {
            return &transaction->pages[index];
        }
    }
    return NULL;
}

static bpt_status bpt_txn_reserve_page(bpt_txn *transaction, uint64_t page_id,
                                       bpt_txn_page **page_out) {
    bpt_txn_page *expanded;
    size_t new_capacity;

    if (transaction->page_count == transaction->page_capacity) {
        if (transaction->page_capacity == 0u) {
            new_capacity = 8u;
        } else {
            if (transaction->page_capacity > SIZE_MAX / (2u * sizeof(*transaction->pages))) {
                return BPT_OUT_OF_MEMORY;
            }
            new_capacity = transaction->page_capacity * 2u;
        }
        expanded = realloc(transaction->pages, new_capacity * sizeof(*expanded));
        if (expanded == NULL) {
            return BPT_OUT_OF_MEMORY;
        }
        transaction->pages = expanded;
        transaction->page_capacity = new_capacity;
    }
    *page_out = &transaction->pages[transaction->page_count];
    (*page_out)->data = malloc((size_t)transaction->tree->storage.page_size);
    if ((*page_out)->data == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    (*page_out)->page_id = page_id;
    (*page_out)->dirty = false;
    transaction->page_count++;
    return BPT_OK;
}

static bpt_status bpt_txn_load_node(bpt_txn *transaction, uint64_t page_id,
                                    unsigned char **page_out, bpt_node_header *header_out) {
    bpt_txn_page *transaction_page = bpt_txn_find_page(transaction, page_id);
    bpt_status status;

    if (page_id == 0u || page_id >= transaction->metadata.next_page_id) {
        return BPT_CORRUPT;
    }
    if (transaction_page == NULL) {
        if (page_id >= transaction->tree->metadata.next_page_id) {
            return BPT_CORRUPT;
        }
        status = bpt_txn_reserve_page(transaction, page_id, &transaction_page);
        if (status != BPT_OK) {
            return status;
        }
        status = bpt_read_page(transaction->tree, page_id, transaction_page->data);
        if (status != BPT_OK) {
            free(transaction_page->data);
            transaction->page_count--;
            return status;
        }
    }
    status = transaction_page->dirty
                 ? bpt_decode_page_header_fields(transaction_page->data, page_id, header_out)
                 : bpt_decode_page_header(transaction->tree, transaction_page->data, page_id,
                                          header_out);
    if (status != BPT_OK) {
        return status;
    }
    *page_out = transaction_page->data;
    return BPT_OK;
}

static bpt_status bpt_txn_new_page(bpt_txn *transaction, uint64_t page_id,
                                   unsigned char **page_out) {
    bpt_txn_page *transaction_page;
    bpt_status status;

    if (bpt_txn_find_page(transaction, page_id) != NULL) {
        return BPT_CORRUPT;
    }
    status = bpt_txn_reserve_page(transaction, page_id, &transaction_page);
    if (status != BPT_OK) {
        return status;
    }
    memset(transaction_page->data, 0, (size_t)transaction->tree->storage.page_size);
    transaction_page->dirty = true;
    *page_out = transaction_page->data;
    return BPT_OK;
}

static bpt_status bpt_txn_mark_dirty(bpt_txn *transaction, uint64_t page_id) {
    bpt_txn_page *transaction_page = bpt_txn_find_page(transaction, page_id);

    if (transaction_page == NULL) {
        return BPT_CORRUPT;
    }
    transaction_page->dirty = true;
    return BPT_OK;
}

static bpt_status bpt_txn_allocate_page(bpt_txn *transaction, uint8_t type, uint32_t level,
                                        uint64_t *page_id_out, unsigned char **page_out) {
    unsigned char *page;
    uint64_t page_id;
    bpt_status status;

    if (transaction->metadata.free_head != 0u) {
        bpt_node_header header;

        page_id = transaction->metadata.free_head;
        status = bpt_txn_load_node(transaction, page_id, &page, &header);
        if (status != BPT_OK) {
            return status;
        }
        if (header.type != (uint8_t)BPT_PAGE_FREE || header.count != 0u || header.level != 0u ||
            header.previous != 0u ||
            (header.next != 0u &&
             (header.next >= transaction->metadata.next_page_id || header.next == page_id))) {
            return BPT_CORRUPT;
        }
        transaction->metadata.free_head = header.next;
        memset(page, 0, (size_t)transaction->tree->storage.page_size);
        status = bpt_txn_mark_dirty(transaction, page_id);
        if (status != BPT_OK) {
            return status;
        }
    } else {
        if (transaction->metadata.next_page_id == UINT64_MAX) {
            return BPT_OUT_OF_MEMORY;
        }
        page_id = transaction->metadata.next_page_id;
        transaction->metadata.next_page_id++;
        status = bpt_txn_new_page(transaction, page_id, &page);
        if (status != BPT_OK) {
            return status;
        }
    }
    bpt_page_initialize(page, page_id, type, 0u, level);
    *page_id_out = page_id;
    *page_out = page;
    return BPT_OK;
}

static bpt_status bpt_txn_free_page(bpt_txn *transaction, uint64_t page_id) {
    unsigned char *page;
    bpt_node_header header;
    bpt_status status = bpt_txn_load_node(transaction, page_id, &page, &header);

    if (status != BPT_OK) {
        return status;
    }
    if (header.type == (uint8_t)BPT_PAGE_FREE) {
        return BPT_CORRUPT;
    }
    memset(page, 0, (size_t)transaction->tree->storage.page_size);
    bpt_page_initialize(page, page_id, (uint8_t)BPT_PAGE_FREE, 0u, 0u);
    bpt_store_u64(page + BPT_PAGE_NEXT_OFFSET, transaction->metadata.free_head);
    transaction->metadata.free_head = page_id;
    return bpt_txn_mark_dirty(transaction, page_id);
}

static bpt_status bpt_txn_commit(bpt_txn *transaction) {
    unsigned char *metadata_page;
    bpt_page_update *updates;
    size_t dirty_count = 0u;
    size_t update_index = 1u;
    size_t index;
    bpt_status status;

    for (index = 0u; index < transaction->page_count; ++index) {
        if (transaction->pages[index].dirty) {
            dirty_count++;
        }
    }
    if (dirty_count == SIZE_MAX) {
        return BPT_OUT_OF_MEMORY;
    }
    metadata_page = calloc(1u, (size_t)transaction->tree->storage.page_size);
    updates = malloc((dirty_count + 1u) * sizeof(*updates));
    if (metadata_page == NULL || updates == NULL) {
        free(metadata_page);
        free(updates);
        return BPT_OUT_OF_MEMORY;
    }
    bpt_metadata_encode(metadata_page, transaction->tree->storage.page_size,
                        &transaction->metadata);
    updates[0].page_id = 0u;
    updates[0].data = metadata_page;
    for (index = 0u; index < transaction->page_count; ++index) {
        if (!transaction->pages[index].dirty) {
            continue;
        }
        bpt_page_checksum_store(transaction->pages[index].data,
                                transaction->tree->storage.page_size);
        updates[update_index].page_id = transaction->pages[index].page_id;
        updates[update_index].data = transaction->pages[index].data;
        update_index++;
    }
    status = bpt_commit_pages(transaction->tree, updates, dirty_count + 1u);
    if (status == BPT_OK) {
        transaction->tree->metadata = transaction->metadata;
    }
    free(updates);
    free(metadata_page);
    return status;
}

static uint32_t bpt_leaf_lower_bound(const unsigned char *page, uint32_t count, uint64_t key) {
    uint32_t first = 0u;
    uint32_t length = count;

    while (length != 0u) {
        uint32_t half = length / 2u;
        uint32_t middle = first + half;

        if (bpt_leaf_key(page, middle) < key) {
            first = middle + 1u;
            length -= half + 1u;
        } else {
            length = half;
        }
    }
    return first;
}

static uint32_t bpt_internal_child_index(const unsigned char *page, uint32_t count, uint64_t key) {
    uint32_t first = 0u;
    uint32_t length = count;

    while (length != 0u) {
        uint32_t half = length / 2u;
        uint32_t middle = first + half;

        if (bpt_internal_key(page, middle) <= key) {
            first = middle + 1u;
            length -= half + 1u;
        } else {
            length = half;
        }
    }
    return first;
}

static bpt_status bpt_txn_find_path(bpt_txn *transaction, uint64_t key, bpt_path *path,
                                    unsigned char **leaf_out, bpt_node_header *leaf_header_out) {
    uint64_t page_id = transaction->metadata.root_page_id;
    uint32_t expected_level = transaction->metadata.height - 1u;

    memset(path, 0, sizeof(*path));
    for (;;) {
        unsigned char *page;
        bpt_node_header header;
        bpt_status status = bpt_txn_load_node(transaction, page_id, &page, &header);

        if (status != BPT_OK) {
            return status;
        }
        if (header.level != expected_level) {
            return BPT_CORRUPT;
        }
        if (expected_level == 0u) {
            if (header.type != (uint8_t)BPT_PAGE_LEAF ||
                header.count > bpt_leaf_capacity(transaction->tree->storage.page_size)) {
                return BPT_CORRUPT;
            }
            path->leaf_page_id = page_id;
            *leaf_out = page;
            *leaf_header_out = header;
            return BPT_OK;
        }
        if (header.type != (uint8_t)BPT_PAGE_INTERNAL || header.count == 0u ||
            header.count > bpt_internal_capacity(transaction->tree->storage.page_size) ||
            header.next != 0u || header.previous != 0u ||
            path->depth >= sizeof(path->entries) / sizeof(path->entries[0])) {
            return BPT_CORRUPT;
        }
        path->entries[path->depth].page_id = page_id;
        path->entries[path->depth].child_index = bpt_internal_child_index(page, header.count, key);
        page_id = bpt_internal_child(page, path->entries[path->depth].child_index);
        path->depth++;
        expected_level--;
    }
}

static bpt_status bpt_txn_propagate_minimum(bpt_txn *transaction, const bpt_path *path,
                                            uint64_t minimum_key) {
    size_t depth = path->depth;

    while (depth != 0u) {
        const bpt_path_entry *entry = &path->entries[depth - 1u];

        if (entry->child_index != 0u) {
            bpt_txn_page *parent = bpt_txn_find_page(transaction, entry->page_id);

            if (parent == NULL) {
                return BPT_CORRUPT;
            }
            bpt_store_u64(parent->data + BPT_PAGE_PAYLOAD_OFFSET +
                              (size_t)(entry->child_index - 1u) * 16u + 8u,
                          minimum_key);
            parent->dirty = true;
            return BPT_OK;
        }
        depth--;
    }
    return BPT_OK;
}

static void bpt_leaf_encode(unsigned char *page, uint32_t page_size, uint64_t page_id,
                            const bpt_entry *entries, uint32_t count, uint64_t previous,
                            uint64_t next) {
    uint32_t index;

    memset(page, 0, (size_t)page_size);
    bpt_page_initialize(page, page_id, (uint8_t)BPT_PAGE_LEAF, count, 0u);
    bpt_store_u64(page + BPT_PAGE_PREVIOUS_OFFSET, previous);
    bpt_store_u64(page + BPT_PAGE_NEXT_OFFSET, next);
    for (index = 0u; index < count; ++index) {
        size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)index * 16u;

        bpt_store_u64(page + offset, entries[index].key);
        bpt_store_u64(page + offset + 8u, entries[index].value);
    }
}

static void bpt_internal_encode(unsigned char *page, uint32_t page_size, uint64_t page_id,
                                uint32_t level, const uint64_t *children, const uint64_t *keys,
                                uint32_t count) {
    uint32_t index;

    memset(page, 0, (size_t)page_size);
    bpt_page_initialize(page, page_id, (uint8_t)BPT_PAGE_INTERNAL, count, level);
    for (index = 0u; index <= count; ++index) {
        size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)index * 16u;

        bpt_store_u64(page + offset, children[index]);
        if (index != 0u) {
            bpt_store_u64(page + offset - 8u, keys[index - 1u]);
        }
    }
}

static void bpt_internal_insert(unsigned char *page, uint32_t count, uint32_t child_index,
                                uint64_t separator, uint64_t right_child) {
    size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)child_index * 16u + 8u;
    size_t tail_size = (size_t)(count - child_index) * 16u;

    memmove(page + offset + 16u, page + offset, tail_size);
    bpt_store_u64(page + offset, separator);
    bpt_store_u64(page + offset + 8u, right_child);
    bpt_store_u32(page + BPT_PAGE_COUNT_OFFSET, count + 1u);
}

static void bpt_internal_remove(unsigned char *page, uint32_t count, uint32_t key_index) {
    size_t offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)key_index * 16u + 8u;
    size_t tail_size = (size_t)(count - key_index - 1u) * 16u;

    memmove(page + offset, page + offset + 16u, tail_size);
    memset(page + (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)(count - 1u) * 16u + 8u, 0, 16u);
    bpt_store_u32(page + BPT_PAGE_COUNT_OFFSET, count - 1u);
}

static bpt_status bpt_txn_split_internal(bpt_txn *transaction, uint64_t page_id,
                                         unsigned char *page, const bpt_node_header *header,
                                         uint32_t child_index, uint64_t separator,
                                         uint64_t right_child, uint64_t *promoted_out,
                                         uint64_t *right_page_id_out) {
    uint32_t total_count = header->count + 1u;
    uint32_t middle = total_count / 2u;
    uint32_t right_count = total_count - middle - 1u;
    uint64_t *children;
    uint64_t *keys;
    uint64_t new_page_id;
    unsigned char *new_page;
    uint32_t index;
    bpt_status status;

    children = malloc((size_t)(total_count + 1u) * sizeof(*children));
    keys = malloc((size_t)total_count * sizeof(*keys));
    if (children == NULL || keys == NULL) {
        free(children);
        free(keys);
        return BPT_OUT_OF_MEMORY;
    }
    for (index = 0u; index <= total_count; ++index) {
        if (index <= child_index) {
            children[index] = bpt_internal_child(page, index);
        } else if (index == child_index + 1u) {
            children[index] = right_child;
        } else {
            children[index] = bpt_internal_child(page, index - 1u);
        }
    }
    for (index = 0u; index < total_count; ++index) {
        if (index < child_index) {
            keys[index] = bpt_internal_key(page, index);
        } else if (index == child_index) {
            keys[index] = separator;
        } else {
            keys[index] = bpt_internal_key(page, index - 1u);
        }
    }
    status = bpt_txn_allocate_page(transaction, (uint8_t)BPT_PAGE_INTERNAL, header->level,
                                   &new_page_id, &new_page);
    if (status == BPT_OK) {
        *promoted_out = keys[middle];
        bpt_internal_encode(page, transaction->tree->storage.page_size, page_id, header->level,
                            children, keys, middle);
        bpt_internal_encode(new_page, transaction->tree->storage.page_size, new_page_id,
                            header->level, children + middle + 1u, keys + middle + 1u, right_count);
        status = bpt_txn_mark_dirty(transaction, page_id);
    }
    free(keys);
    free(children);
    if (status == BPT_OK) {
        *right_page_id_out = new_page_id;
    }
    return status;
}

static bpt_status bpt_txn_insert_parent(bpt_txn *transaction, const bpt_path *path,
                                        uint64_t separator, uint64_t right_child) {
    size_t depth = path->depth;

    while (depth != 0u) {
        const bpt_path_entry *entry = &path->entries[depth - 1u];
        unsigned char *parent;
        bpt_node_header header;
        bpt_status status = bpt_txn_load_node(transaction, entry->page_id, &parent, &header);

        if (status != BPT_OK) {
            return status;
        }
        if (header.type != (uint8_t)BPT_PAGE_INTERNAL ||
            header.count > bpt_internal_capacity(transaction->tree->storage.page_size) ||
            entry->child_index > header.count) {
            return BPT_CORRUPT;
        }
        if (header.count < bpt_internal_capacity(transaction->tree->storage.page_size)) {
            bpt_internal_insert(parent, header.count, entry->child_index, separator, right_child);
            return bpt_txn_mark_dirty(transaction, entry->page_id);
        }
        status =
            bpt_txn_split_internal(transaction, entry->page_id, parent, &header, entry->child_index,
                                   separator, right_child, &separator, &right_child);
        if (status != BPT_OK) {
            return status;
        }
        depth--;
    }
    {
        uint64_t root_page_id;
        unsigned char *root_page;
        uint64_t children[2];
        uint64_t keys[1];
        bpt_status status;

        if (transaction->metadata.height == UINT32_MAX) {
            return BPT_OUT_OF_MEMORY;
        }
        status = bpt_txn_allocate_page(transaction, (uint8_t)BPT_PAGE_INTERNAL,
                                       transaction->metadata.height, &root_page_id, &root_page);
        if (status != BPT_OK) {
            return status;
        }
        children[0] = transaction->metadata.root_page_id;
        children[1] = right_child;
        keys[0] = separator;
        bpt_internal_encode(root_page, transaction->tree->storage.page_size, root_page_id,
                            transaction->metadata.height, children, keys, 1u);
        transaction->metadata.root_page_id = root_page_id;
        transaction->metadata.height++;
    }
    return BPT_OK;
}

static bool bpt_internal_header_valid(const bpt_txn *transaction, const bpt_node_header *header,
                                      uint32_t level) {
    return header->type == (uint8_t)BPT_PAGE_INTERNAL && header->level == level &&
           header->count <= bpt_internal_capacity(transaction->tree->storage.page_size) &&
           header->next == 0u && header->previous == 0u;
}

static bpt_status bpt_txn_rebalance_internal(bpt_txn *transaction, const bpt_path *path,
                                             size_t node_position) {
    uint32_t minimum_count =
        ((bpt_internal_capacity(transaction->tree->storage.page_size) + 2u) / 2u) - 1u;

    for (;;) {
        uint64_t node_page_id = path->entries[node_position].page_id;
        unsigned char *node;
        bpt_node_header node_header;
        bpt_status status = bpt_txn_load_node(transaction, node_page_id, &node, &node_header);

        if (status != BPT_OK) {
            return status;
        }
        if (!bpt_internal_header_valid(transaction, &node_header, node_header.level)) {
            return BPT_CORRUPT;
        }
        if (node_position == 0u) {
            if (node_header.count == 0u) {
                uint64_t child_page_id = bpt_internal_child(node, 0u);

                if (transaction->metadata.height <= 1u) {
                    return BPT_CORRUPT;
                }
                transaction->metadata.root_page_id = child_page_id;
                transaction->metadata.height--;
                return bpt_txn_free_page(transaction, node_page_id);
            }
            return BPT_OK;
        }
        if (node_header.count >= minimum_count) {
            return BPT_OK;
        }
        {
            const bpt_path_entry *grand_entry = &path->entries[node_position - 1u];
            uint32_t node_index = grand_entry->child_index;
            unsigned char *grand;
            bpt_node_header grand_header;
            unsigned char *left = NULL;
            unsigned char *right = NULL;
            bpt_node_header left_header;
            bpt_node_header right_header;
            uint64_t left_page_id = 0u;
            uint64_t right_page_id = 0u;

            status = bpt_txn_load_node(transaction, grand_entry->page_id, &grand, &grand_header);
            if (status != BPT_OK) {
                return status;
            }
            if (!bpt_internal_header_valid(transaction, &grand_header, node_header.level + 1u) ||
                node_index > grand_header.count ||
                bpt_internal_child(grand, node_index) != node_page_id) {
                return BPT_CORRUPT;
            }
            if (node_index != 0u) {
                left_page_id = bpt_internal_child(grand, node_index - 1u);
                status = bpt_txn_load_node(transaction, left_page_id, &left, &left_header);
                if (status != BPT_OK) {
                    return status;
                }
                if (!bpt_internal_header_valid(transaction, &left_header, node_header.level) ||
                    left_header.count < minimum_count) {
                    return BPT_CORRUPT;
                }
                if (left_header.count > minimum_count) {
                    uint64_t old_separator = bpt_internal_key(grand, node_index - 1u);
                    uint64_t new_separator = bpt_internal_key(left, left_header.count - 1u);
                    uint64_t moved_child = bpt_internal_child(left, left_header.count);
                    size_t node_size = 8u + (size_t)node_header.count * 16u;

                    memmove(node + BPT_PAGE_PAYLOAD_OFFSET + 16u, node + BPT_PAGE_PAYLOAD_OFFSET,
                            node_size);
                    bpt_store_u64(node + BPT_PAGE_PAYLOAD_OFFSET, moved_child);
                    bpt_store_u64(node + BPT_PAGE_PAYLOAD_OFFSET + 8u, old_separator);
                    bpt_store_u32(node + BPT_PAGE_COUNT_OFFSET, node_header.count + 1u);
                    bpt_internal_remove(left, left_header.count, left_header.count - 1u);
                    bpt_store_u64(grand + BPT_PAGE_PAYLOAD_OFFSET +
                                      (size_t)(node_index - 1u) * 16u + 8u,
                                  new_separator);
                    status = bpt_txn_mark_dirty(transaction, node_page_id);
                    if (status == BPT_OK) {
                        status = bpt_txn_mark_dirty(transaction, left_page_id);
                    }
                    if (status == BPT_OK) {
                        status = bpt_txn_mark_dirty(transaction, grand_entry->page_id);
                    }
                    return status;
                }
            }
            if (node_index < grand_header.count) {
                right_page_id = bpt_internal_child(grand, node_index + 1u);
                status = bpt_txn_load_node(transaction, right_page_id, &right, &right_header);
                if (status != BPT_OK) {
                    return status;
                }
                if (!bpt_internal_header_valid(transaction, &right_header, node_header.level) ||
                    right_header.count < minimum_count) {
                    return BPT_CORRUPT;
                }
                if (right_header.count > minimum_count) {
                    uint64_t old_separator = bpt_internal_key(grand, node_index);
                    uint64_t new_separator = bpt_internal_key(right, 0u);
                    uint64_t moved_child = bpt_internal_child(right, 0u);
                    size_t right_size = 8u + (size_t)right_header.count * 16u;
                    bpt_internal_insert(node, node_header.count, node_header.count, old_separator,
                                        moved_child);
                    memmove(right + BPT_PAGE_PAYLOAD_OFFSET, right + BPT_PAGE_PAYLOAD_OFFSET + 16u,
                            right_size - 16u);
                    memset(right + BPT_PAGE_PAYLOAD_OFFSET + right_size - 16u, 0, 16u);
                    bpt_store_u32(right + BPT_PAGE_COUNT_OFFSET, right_header.count - 1u);
                    bpt_store_u64(grand + BPT_PAGE_PAYLOAD_OFFSET + (size_t)node_index * 16u + 8u,
                                  new_separator);
                    status = bpt_txn_mark_dirty(transaction, node_page_id);
                    if (status == BPT_OK) {
                        status = bpt_txn_mark_dirty(transaction, right_page_id);
                    }
                    if (status == BPT_OK) {
                        status = bpt_txn_mark_dirty(transaction, grand_entry->page_id);
                    }
                    return status;
                }
            }
            if (left != NULL) {
                uint64_t separator = bpt_internal_key(grand, node_index - 1u);
                uint32_t merged_count = left_header.count + 1u + node_header.count;
                size_t destination =
                    (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)(left_header.count + 1u) * 16u;
                size_t node_size = 8u + (size_t)node_header.count * 16u;

                if (merged_count > bpt_internal_capacity(transaction->tree->storage.page_size)) {
                    return BPT_CORRUPT;
                }
                bpt_store_u64(left + destination - 8u, separator);
                memcpy(left + destination, node + BPT_PAGE_PAYLOAD_OFFSET, node_size);
                bpt_store_u32(left + BPT_PAGE_COUNT_OFFSET, merged_count);
                status = bpt_txn_mark_dirty(transaction, left_page_id);
                if (status == BPT_OK) {
                    status = bpt_txn_free_page(transaction, node_page_id);
                }
                if (status == BPT_OK) {
                    bpt_internal_remove(grand, grand_header.count, node_index - 1u);
                    status = bpt_txn_mark_dirty(transaction, grand_entry->page_id);
                }
            } else {
                uint64_t separator;
                uint32_t merged_count;
                size_t destination;
                size_t right_size;

                if (right == NULL) {
                    return BPT_CORRUPT;
                }
                separator = bpt_internal_key(grand, node_index);
                merged_count = node_header.count + 1u + right_header.count;
                if (merged_count > bpt_internal_capacity(transaction->tree->storage.page_size)) {
                    return BPT_CORRUPT;
                }
                destination =
                    (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)(node_header.count + 1u) * 16u;
                right_size = 8u + (size_t)right_header.count * 16u;
                bpt_store_u64(node + destination - 8u, separator);
                memcpy(node + destination, right + BPT_PAGE_PAYLOAD_OFFSET, right_size);
                bpt_store_u32(node + BPT_PAGE_COUNT_OFFSET, merged_count);
                status = bpt_txn_mark_dirty(transaction, node_page_id);
                if (status == BPT_OK) {
                    status = bpt_txn_free_page(transaction, right_page_id);
                }
                if (status == BPT_OK) {
                    bpt_internal_remove(grand, grand_header.count, node_index);
                    status = bpt_txn_mark_dirty(transaction, grand_entry->page_id);
                }
            }
            if (status != BPT_OK) {
                return status;
            }
        }
        node_position--;
    }
}

static bool bpt_leaf_header_valid(const bpt_txn *transaction, const bpt_node_header *header) {
    return header->type == (uint8_t)BPT_PAGE_LEAF && header->level == 0u &&
           header->count <= bpt_leaf_capacity(transaction->tree->storage.page_size);
}

static bpt_status bpt_txn_rebalance_leaf(bpt_txn *transaction, const bpt_path *path) {
    const bpt_path_entry *parent_entry = &path->entries[path->depth - 1u];
    uint32_t leaf_index = parent_entry->child_index;
    uint32_t minimum_count = (bpt_leaf_capacity(transaction->tree->storage.page_size) + 1u) / 2u;
    unsigned char *leaf;
    unsigned char *parent;
    unsigned char *left = NULL;
    unsigned char *right = NULL;
    bpt_node_header leaf_header;
    bpt_node_header parent_header;
    bpt_node_header left_header;
    bpt_node_header right_header;
    uint64_t left_page_id = 0u;
    uint64_t right_page_id = 0u;
    bpt_status status;

    status = bpt_txn_load_node(transaction, path->leaf_page_id, &leaf, &leaf_header);
    if (status != BPT_OK) {
        return status;
    }
    status = bpt_txn_load_node(transaction, parent_entry->page_id, &parent, &parent_header);
    if (status != BPT_OK) {
        return status;
    }
    if (!bpt_leaf_header_valid(transaction, &leaf_header) ||
        parent_header.type != (uint8_t)BPT_PAGE_INTERNAL || parent_header.level != 1u ||
        parent_header.count > bpt_internal_capacity(transaction->tree->storage.page_size) ||
        leaf_index > parent_header.count ||
        bpt_internal_child(parent, leaf_index) != path->leaf_page_id) {
        return BPT_CORRUPT;
    }
    if (leaf_index != 0u) {
        left_page_id = bpt_internal_child(parent, leaf_index - 1u);
        status = bpt_txn_load_node(transaction, left_page_id, &left, &left_header);
        if (status != BPT_OK) {
            return status;
        }
        if (!bpt_leaf_header_valid(transaction, &left_header) ||
            left_header.count < minimum_count || left_header.next != path->leaf_page_id ||
            leaf_header.previous != left_page_id) {
            return BPT_CORRUPT;
        }
        if (left_header.count > minimum_count) {
            size_t leaf_size = (size_t)leaf_header.count * 16u;
            size_t left_offset =
                (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)(left_header.count - 1u) * 16u;

            memmove(leaf + BPT_PAGE_PAYLOAD_OFFSET + 16u, leaf + BPT_PAGE_PAYLOAD_OFFSET,
                    leaf_size);
            memcpy(leaf + BPT_PAGE_PAYLOAD_OFFSET, left + left_offset, 16u);
            memset(left + left_offset, 0, 16u);
            bpt_store_u32(leaf + BPT_PAGE_COUNT_OFFSET, leaf_header.count + 1u);
            bpt_store_u32(left + BPT_PAGE_COUNT_OFFSET, left_header.count - 1u);
            bpt_store_u64(parent + BPT_PAGE_PAYLOAD_OFFSET + (size_t)(leaf_index - 1u) * 16u + 8u,
                          bpt_leaf_key(leaf, 0u));
            status = bpt_txn_mark_dirty(transaction, path->leaf_page_id);
            if (status == BPT_OK) {
                status = bpt_txn_mark_dirty(transaction, left_page_id);
            }
            if (status == BPT_OK) {
                status = bpt_txn_mark_dirty(transaction, parent_entry->page_id);
            }
            return status;
        }
    }
    if (leaf_index < parent_header.count) {
        right_page_id = bpt_internal_child(parent, leaf_index + 1u);
        status = bpt_txn_load_node(transaction, right_page_id, &right, &right_header);
        if (status != BPT_OK) {
            return status;
        }
        if (!bpt_leaf_header_valid(transaction, &right_header) ||
            right_header.count < minimum_count || leaf_header.next != right_page_id ||
            right_header.previous != path->leaf_page_id) {
            return BPT_CORRUPT;
        }
        if (right_header.count > minimum_count) {
            size_t leaf_offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)leaf_header.count * 16u;
            size_t right_size = (size_t)right_header.count * 16u;

            memcpy(leaf + leaf_offset, right + BPT_PAGE_PAYLOAD_OFFSET, 16u);
            memmove(right + BPT_PAGE_PAYLOAD_OFFSET, right + BPT_PAGE_PAYLOAD_OFFSET + 16u,
                    right_size - 16u);
            memset(right + BPT_PAGE_PAYLOAD_OFFSET + right_size - 16u, 0, 16u);
            bpt_store_u32(leaf + BPT_PAGE_COUNT_OFFSET, leaf_header.count + 1u);
            bpt_store_u32(right + BPT_PAGE_COUNT_OFFSET, right_header.count - 1u);
            bpt_store_u64(parent + BPT_PAGE_PAYLOAD_OFFSET + (size_t)leaf_index * 16u + 8u,
                          bpt_leaf_key(right, 0u));
            status = bpt_txn_mark_dirty(transaction, path->leaf_page_id);
            if (status == BPT_OK) {
                status = bpt_txn_mark_dirty(transaction, right_page_id);
            }
            if (status == BPT_OK) {
                status = bpt_txn_mark_dirty(transaction, parent_entry->page_id);
            }
            return status;
        }
    }
    if (left != NULL) {
        uint32_t merged_count = left_header.count + leaf_header.count;
        uint64_t next_page_id = leaf_header.next;

        if (merged_count > bpt_leaf_capacity(transaction->tree->storage.page_size)) {
            return BPT_CORRUPT;
        }
        memcpy(left + BPT_PAGE_PAYLOAD_OFFSET + (size_t)left_header.count * 16u,
               leaf + BPT_PAGE_PAYLOAD_OFFSET, (size_t)leaf_header.count * 16u);
        bpt_store_u32(left + BPT_PAGE_COUNT_OFFSET, merged_count);
        bpt_store_u64(left + BPT_PAGE_NEXT_OFFSET, next_page_id);
        status = bpt_txn_mark_dirty(transaction, left_page_id);
        if (status == BPT_OK && next_page_id != 0u) {
            unsigned char *next;
            bpt_node_header next_header;

            status = bpt_txn_load_node(transaction, next_page_id, &next, &next_header);
            if (status == BPT_OK && (!bpt_leaf_header_valid(transaction, &next_header) ||
                                     next_header.previous != path->leaf_page_id)) {
                status = BPT_CORRUPT;
            }
            if (status == BPT_OK) {
                bpt_store_u64(next + BPT_PAGE_PREVIOUS_OFFSET, left_page_id);
                status = bpt_txn_mark_dirty(transaction, next_page_id);
            }
        }
        if (status == BPT_OK) {
            status = bpt_txn_free_page(transaction, path->leaf_page_id);
        }
        if (status == BPT_OK) {
            bpt_internal_remove(parent, parent_header.count, leaf_index - 1u);
            status = bpt_txn_mark_dirty(transaction, parent_entry->page_id);
        }
    } else {
        uint32_t merged_count;
        uint64_t next_page_id;

        if (right == NULL) {
            return BPT_CORRUPT;
        }
        merged_count = leaf_header.count + right_header.count;
        next_page_id = right_header.next;
        if (merged_count > bpt_leaf_capacity(transaction->tree->storage.page_size)) {
            return BPT_CORRUPT;
        }
        memcpy(leaf + BPT_PAGE_PAYLOAD_OFFSET + (size_t)leaf_header.count * 16u,
               right + BPT_PAGE_PAYLOAD_OFFSET, (size_t)right_header.count * 16u);
        bpt_store_u32(leaf + BPT_PAGE_COUNT_OFFSET, merged_count);
        bpt_store_u64(leaf + BPT_PAGE_NEXT_OFFSET, next_page_id);
        status = bpt_txn_mark_dirty(transaction, path->leaf_page_id);
        if (status == BPT_OK && next_page_id != 0u) {
            unsigned char *next;
            bpt_node_header next_header;

            status = bpt_txn_load_node(transaction, next_page_id, &next, &next_header);
            if (status == BPT_OK && (!bpt_leaf_header_valid(transaction, &next_header) ||
                                     next_header.previous != right_page_id)) {
                status = BPT_CORRUPT;
            }
            if (status == BPT_OK) {
                bpt_store_u64(next + BPT_PAGE_PREVIOUS_OFFSET, path->leaf_page_id);
                status = bpt_txn_mark_dirty(transaction, next_page_id);
            }
        }
        if (status == BPT_OK) {
            status = bpt_txn_free_page(transaction, right_page_id);
        }
        if (status == BPT_OK) {
            bpt_internal_remove(parent, parent_header.count, leaf_index);
            status = bpt_txn_mark_dirty(transaction, parent_entry->page_id);
        }
    }
    if (status != BPT_OK) {
        return status;
    }
    return bpt_txn_rebalance_internal(transaction, path, path->depth - 1u);
}

static bpt_status bpt_initialize_empty_tree(bpt_tree *tree) {
    bpt_metadata metadata;
    unsigned char *metadata_page;
    unsigned char *root_page;
    bpt_page_update updates[2];
    bpt_status status;

    metadata.root_page_id = 1u;
    metadata.free_head = 0u;
    metadata.item_count = 0u;
    metadata.next_page_id = 2u;
    metadata.height = 1u;
    metadata_page = calloc(1u, (size_t)tree->storage.page_size);
    root_page = calloc(1u, (size_t)tree->storage.page_size);
    if (metadata_page == NULL || root_page == NULL) {
        free(metadata_page);
        free(root_page);
        return BPT_OUT_OF_MEMORY;
    }
    bpt_metadata_encode(metadata_page, tree->storage.page_size, &metadata);
    bpt_page_initialize(root_page, metadata.root_page_id, (uint8_t)BPT_PAGE_LEAF, 0u, 0u);
    bpt_page_checksum_store(root_page, tree->storage.page_size);
    updates[0].page_id = 0u;
    updates[0].data = metadata_page;
    updates[1].page_id = metadata.root_page_id;
    updates[1].data = root_page;
    status = bpt_commit_pages(tree, updates, 2u);
    if (status == BPT_OK) {
        tree->metadata = metadata;
    }
    free(root_page);
    free(metadata_page);
    return status;
}

bpt_status bpt_tree_create(const bpt_storage *storage, bpt_tree **tree_out) {
    bpt_tree *tree;
    uint64_t page_count = 0u;
    bpt_status status;

    if (tree_out != NULL) {
        *tree_out = NULL;
    }
    if (!bpt_storage_valid(storage) || tree_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    status = storage->page_count(storage->context, &page_count);
    if (status != BPT_OK) {
        return status;
    }
    if (page_count != 0u) {
        return BPT_INVALID_ARGUMENT;
    }
    tree = calloc(1u, sizeof(*tree));
    if (tree == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    tree->storage = *storage;
    status = bpt_initialize_empty_tree(tree);
    if (status != BPT_OK) {
        free(tree);
        return status;
    }
    *tree_out = tree;
    return BPT_OK;
}

bpt_status bpt_tree_open(const bpt_storage *storage, bpt_tree **tree_out) {
    bpt_tree *tree;
    bpt_status status;

    if (tree_out != NULL) {
        *tree_out = NULL;
    }
    if (!bpt_storage_valid(storage) || tree_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    tree = calloc(1u, sizeof(*tree));
    if (tree == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    tree->storage = *storage;
    status = bpt_validate_all(tree, NULL, NULL, 0u);
    if (status != BPT_OK) {
        free(tree);
        return status;
    }
    *tree_out = tree;
    return BPT_OK;
}

void bpt_tree_close(bpt_tree *tree) {
    free(tree);
}

static bpt_status bpt_find_leaf(bpt_tree *tree, uint64_t key, unsigned char *page,
                                bpt_node_header *header_out) {
    uint64_t page_id = tree->metadata.root_page_id;
    uint32_t expected_level = tree->metadata.height - 1u;

    for (;;) {
        bpt_node_header header;
        bpt_status status = bpt_read_node(tree, &tree->metadata, page_id, page, &header);

        if (status != BPT_OK) {
            return status;
        }
        if (header.level != expected_level) {
            return BPT_CORRUPT;
        }
        if (expected_level == 0u) {
            if (header.type != (uint8_t)BPT_PAGE_LEAF ||
                header.count > bpt_leaf_capacity(tree->storage.page_size)) {
                return BPT_CORRUPT;
            }
            *header_out = header;
            return BPT_OK;
        }
        if (header.type != (uint8_t)BPT_PAGE_INTERNAL || header.count == 0u ||
            header.count > bpt_internal_capacity(tree->storage.page_size)) {
            return BPT_CORRUPT;
        }
        page_id = bpt_internal_child(page, bpt_internal_child_index(page, header.count, key));
        expected_level--;
    }
}

bpt_status bpt_tree_get(bpt_tree *tree, uint64_t key, uint64_t *value_out) {
    unsigned char *page;
    bpt_node_header header;
    uint32_t first;
    bpt_status status;

    if (tree == NULL || value_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    status = bpt_find_leaf(tree, key, page, &header);
    if (status != BPT_OK) {
        free(page);
        return status;
    }
    first = bpt_leaf_lower_bound(page, header.count, key);
    if (first == header.count || bpt_leaf_key(page, first) != key) {
        free(page);
        return BPT_NOT_FOUND;
    }
    *value_out = bpt_leaf_value(page, first);
    free(page);
    return BPT_OK;
}

bpt_status bpt_tree_put(bpt_tree *tree, uint64_t key, uint64_t value, bool *inserted_out) {
    bpt_txn transaction;
    bpt_path path;
    unsigned char *leaf;
    bpt_node_header leaf_header;
    uint32_t position;
    uint32_t capacity;
    bpt_status status;

    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    if (tree == NULL || inserted_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    memset(&transaction, 0, sizeof(transaction));
    transaction.tree = tree;
    transaction.metadata = tree->metadata;
    status = bpt_txn_find_path(&transaction, key, &path, &leaf, &leaf_header);
    if (status != BPT_OK) {
        bpt_txn_destroy(&transaction);
        return status;
    }
    position = bpt_leaf_lower_bound(leaf, leaf_header.count, key);
    if (position < leaf_header.count && bpt_leaf_key(leaf, position) == key) {
        size_t value_offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)position * 16u + 8u;

        if (bpt_leaf_value(leaf, position) == value) {
            bpt_txn_destroy(&transaction);
            return BPT_OK;
        }
        bpt_store_u64(leaf + value_offset, value);
        status = bpt_txn_mark_dirty(&transaction, path.leaf_page_id);
        if (status == BPT_OK) {
            status = bpt_txn_commit(&transaction);
        }
        bpt_txn_destroy(&transaction);
        return status;
    }
    if (transaction.metadata.item_count == UINT64_MAX) {
        bpt_txn_destroy(&transaction);
        return BPT_OUT_OF_MEMORY;
    }
    capacity = bpt_leaf_capacity(tree->storage.page_size);
    transaction.metadata.item_count++;
    if (leaf_header.count < capacity) {
        size_t entry_offset = (size_t)BPT_PAGE_PAYLOAD_OFFSET + (size_t)position * 16u;
        size_t tail_size = (size_t)(leaf_header.count - position) * 16u;

        memmove(leaf + entry_offset + 16u, leaf + entry_offset, tail_size);
        bpt_store_u64(leaf + entry_offset, key);
        bpt_store_u64(leaf + entry_offset + 8u, value);
        bpt_store_u32(leaf + BPT_PAGE_COUNT_OFFSET, leaf_header.count + 1u);
        status = bpt_txn_mark_dirty(&transaction, path.leaf_page_id);
        if (status == BPT_OK && position == 0u) {
            status = bpt_txn_propagate_minimum(&transaction, &path, key);
        }
        if (status == BPT_OK) {
            status = bpt_txn_commit(&transaction);
        }
    } else {
        uint32_t total_count = capacity + 1u;
        uint32_t left_count = total_count / 2u;
        uint32_t right_count;
        bpt_entry *entries = calloc((size_t)total_count, sizeof(*entries));
        uint64_t right_page_id = 0u;
        unsigned char *right_page = NULL;
        uint32_t index;

        if (entries == NULL) {
            bpt_txn_destroy(&transaction);
            return BPT_OUT_OF_MEMORY;
        }
        for (index = 0u; index < position; ++index) {
            entries[index].key = bpt_leaf_key(leaf, index);
            entries[index].value = bpt_leaf_value(leaf, index);
        }
        entries[position].key = key;
        entries[position].value = value;
        for (index = position; index < leaf_header.count; ++index) {
            entries[index + 1u].key = bpt_leaf_key(leaf, index);
            entries[index + 1u].value = bpt_leaf_value(leaf, index);
        }
        if (position >= left_count) {
            left_count++;
        }
        right_count = total_count - left_count;
        status = bpt_txn_allocate_page(&transaction, (uint8_t)BPT_PAGE_LEAF, 0u, &right_page_id,
                                       &right_page);
        if (status == BPT_OK) {
            bpt_leaf_encode(leaf, tree->storage.page_size, path.leaf_page_id, entries, left_count,
                            leaf_header.previous, right_page_id);
            bpt_leaf_encode(right_page, tree->storage.page_size, right_page_id,
                            entries + left_count, right_count, path.leaf_page_id, leaf_header.next);
            status = bpt_txn_mark_dirty(&transaction, path.leaf_page_id);
        }
        if (status == BPT_OK && leaf_header.next != 0u) {
            unsigned char *next_page;
            bpt_node_header next_header;

            status = bpt_txn_load_node(&transaction, leaf_header.next, &next_page, &next_header);
            if (status == BPT_OK && (!bpt_leaf_header_valid(&transaction, &next_header) ||
                                     next_header.previous != path.leaf_page_id)) {
                status = BPT_CORRUPT;
            }
            if (status == BPT_OK) {
                bpt_store_u64(next_page + BPT_PAGE_PREVIOUS_OFFSET, right_page_id);
                status = bpt_txn_mark_dirty(&transaction, leaf_header.next);
            }
        }
        if (status == BPT_OK && position == 0u) {
            status = bpt_txn_propagate_minimum(&transaction, &path, key);
        }
        if (status == BPT_OK) {
            status =
                bpt_txn_insert_parent(&transaction, &path, entries[left_count].key, right_page_id);
        }
        if (status == BPT_OK) {
            status = bpt_txn_commit(&transaction);
        }
        free(entries);
    }
    if (status == BPT_OK) {
        *inserted_out = true;
    }
    bpt_txn_destroy(&transaction);
    return status;
}

bpt_status bpt_tree_delete(bpt_tree *tree, uint64_t key, bool *removed_out) {
    bpt_txn transaction;
    bpt_path path;
    unsigned char *leaf;
    bpt_node_header leaf_header;
    uint32_t position;
    uint32_t new_count;
    bpt_status status;

    if (removed_out != NULL) {
        *removed_out = false;
    }
    if (tree == NULL || removed_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    memset(&transaction, 0, sizeof(transaction));
    transaction.tree = tree;
    transaction.metadata = tree->metadata;
    status = bpt_txn_find_path(&transaction, key, &path, &leaf, &leaf_header);
    if (status != BPT_OK) {
        bpt_txn_destroy(&transaction);
        return status;
    }
    position = bpt_leaf_lower_bound(leaf, leaf_header.count, key);
    if (position == leaf_header.count || bpt_leaf_key(leaf, position) != key) {
        bpt_txn_destroy(&transaction);
        return BPT_OK;
    }
    if (transaction.metadata.item_count == 0u) {
        bpt_txn_destroy(&transaction);
        return BPT_CORRUPT;
    }
    new_count = leaf_header.count - 1u;
    memmove(leaf + BPT_PAGE_PAYLOAD_OFFSET + (size_t)position * 16u,
            leaf + BPT_PAGE_PAYLOAD_OFFSET + (size_t)(position + 1u) * 16u,
            (size_t)(new_count - position) * 16u);
    memset(leaf + BPT_PAGE_PAYLOAD_OFFSET + (size_t)new_count * 16u, 0, 16u);
    bpt_store_u32(leaf + BPT_PAGE_COUNT_OFFSET, new_count);
    transaction.metadata.item_count--;
    status = bpt_txn_mark_dirty(&transaction, path.leaf_page_id);
    if (status == BPT_OK && position == 0u && new_count != 0u) {
        status = bpt_txn_propagate_minimum(&transaction, &path, bpt_leaf_key(leaf, 0u));
    }
    if (status == BPT_OK && path.depth != 0u &&
        new_count < (bpt_leaf_capacity(tree->storage.page_size) + 1u) / 2u) {
        status = bpt_txn_rebalance_leaf(&transaction, &path);
    }
    if (status == BPT_OK) {
        status = bpt_txn_commit(&transaction);
    }
    if (status == BPT_OK) {
        *removed_out = true;
    }
    bpt_txn_destroy(&transaction);
    return status;
}

bpt_status bpt_tree_scan(bpt_tree *tree, uint64_t begin_key, uint64_t end_key,
                         bpt_scan_callback callback, void *context) {
    unsigned char *page;
    bpt_node_header header;
    uint64_t page_id;
    uint64_t previous_page_id = 0u;
    uint64_t visited = 0u;
    bool first_leaf = true;
    bpt_status status;

    if (tree == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    if (begin_key >= end_key) {
        return BPT_OK;
    }
    if (callback == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    page = malloc((size_t)tree->storage.page_size);
    if (page == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    status = bpt_find_leaf(tree, begin_key, page, &header);
    if (status != BPT_OK) {
        free(page);
        return status;
    }
    page_id = bpt_load_u64(page + BPT_PAGE_ID_OFFSET);

    while (page_id != 0u) {
        uint32_t index = 0u;

        if (visited++ >= tree->metadata.next_page_id) {
            free(page);
            return BPT_CORRUPT;
        }
        if (!first_leaf) {
            status = bpt_read_node(tree, &tree->metadata, page_id, page, &header);
            if (status != BPT_OK) {
                free(page);
                return status;
            }
        }
        if (header.type != (uint8_t)BPT_PAGE_LEAF ||
            header.count > bpt_leaf_capacity(tree->storage.page_size) ||
            (!first_leaf && header.previous != previous_page_id)) {
            free(page);
            return BPT_CORRUPT;
        }
        if (first_leaf) {
            index = bpt_leaf_lower_bound(page, header.count, begin_key);
        }
        for (; index < header.count; ++index) {
            uint64_t key = bpt_leaf_key(page, index);

            if (key >= end_key) {
                free(page);
                return BPT_OK;
            }
            status = callback(context, key, bpt_leaf_value(page, index));
            if (status != BPT_OK) {
                free(page);
                return status;
            }
        }
        previous_page_id = page_id;
        page_id = header.next;
        first_leaf = false;
    }
    free(page);
    return BPT_OK;
}

bpt_status bpt_tree_validate(bpt_tree *tree, bpt_stats *stats_out, char *error_out,
                             size_t error_capacity) {
    if (stats_out != NULL) {
        memset(stats_out, 0, sizeof(*stats_out));
    }
    if (error_out != NULL && error_capacity != 0u) {
        error_out[0] = '\0';
    }
    if (tree == NULL || (error_out == NULL && error_capacity != 0u)) {
        return BPT_INVALID_ARGUMENT;
    }
    if (tree->poisoned) {
        return BPT_RECOVERY_REQUIRED;
    }
    return bpt_validate_all(tree, stats_out, error_out, error_capacity);
}

const char *bpt_status_string(bpt_status status) {
    switch (status) {
    case BPT_OK:
        return "ok";
    case BPT_NOT_FOUND:
        return "not found";
    case BPT_STOPPED:
        return "stopped";
    case BPT_INVALID_ARGUMENT:
        return "invalid argument";
    case BPT_OUT_OF_MEMORY:
        return "out of memory";
    case BPT_IO:
        return "I/O error";
    case BPT_CORRUPT:
        return "corrupt";
    case BPT_UNSUPPORTED:
        return "unsupported";
    case BPT_BUSY:
        return "busy";
    case BPT_RECOVERY_REQUIRED:
        return "recovery required";
    default:
        return "unknown status";
    }
}
