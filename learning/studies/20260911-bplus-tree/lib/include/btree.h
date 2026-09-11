#ifndef BTREE_H
#define BTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BTREE_FORMAT_VERSION 2u
#define BTREE_MIN_PAGE_SIZE 512u
#define BTREE_MAX_PAGE_SIZE 65536u
#define BTREE_COMPARATOR_LEXICOGRAPHIC UINT64_C(1)
#define BTREE_COMPARATOR_USER_MIN UINT64_C(1024)

typedef enum btree_status {
    BTREE_OK = 0,
    BTREE_NOT_FOUND = 1,
    BTREE_STOPPED = 2,
    BTREE_INVALID_ARGUMENT = 3,
    BTREE_OUT_OF_MEMORY = 4,
    BTREE_IO = 5,
    BTREE_CORRUPT = 6,
    BTREE_UNSUPPORTED = 7,
    BTREE_BUSY = 8,
    /*
     * Commit-unknown: a mutation may or may not have been published. This
     * permanently poisons the current tree handle.
     */
    BTREE_RECOVERY_REQUIRED = 9,
    BTREE_SCHEMA_MISMATCH = 10
} btree_status;

typedef int (*btree_compare_fn)(void *context, const void *left_key, const void *right_key,
                                size_t key_size);

typedef struct btree_schema {
    uint32_t key_size;
    uint32_t value_size;
    uint64_t comparator_id;
} btree_schema;

typedef struct btree_options {
    uint32_t key_size;
    uint32_t value_size;
    uint64_t comparator_id;
    btree_compare_fn compare;
    void *compare_context;
} btree_options;

typedef struct btree_page_update {
    uint64_t page_id;
    const void *data;
} btree_page_update;

/*
 * The tree copies btree_storage by value during create/open, but context is
 * borrowed. The context and its provider/backend must outlive every tree that
 * uses it. btree_close never closes or destroys storage.
 *
 * read_page synchronously copies exactly page_size bytes into caller-owned
 * data_out; ownership is not transferred.
 * page_count returns the number of addressable logical pages.
 * commit_pages synchronously and atomically publishes the complete update set.
 * The updates array and every page buffer referenced by it are caller-owned,
 * valid only for the duration of the callback, and must not be retained.
 *
 * A custom provider must return BTREE_RECOVERY_REQUIRED whenever failure can
 * leave an uncertain visible state.
 */
typedef struct btree_storage {
    void *context;
    uint32_t page_size;
    btree_status (*read_page)(void *context, uint64_t page_id, void *data_out);
    btree_status (*page_count)(void *context, uint64_t *count_out);
    btree_status (*commit_pages)(void *context, const btree_page_update *updates,
                                 size_t update_count);
} btree_storage;

typedef struct btree btree;

typedef enum btree_scan_action { BTREE_SCAN_CONTINUE = 0, BTREE_SCAN_STOP = 1 } btree_scan_action;

/*
 * key and value point into an operation-owned page and are valid only for the
 * duration of the callback. Their sizes are fixed by the tree schema.
 */
typedef btree_scan_action (*btree_scan_fn)(void *context, const void *key, const void *value);

typedef struct btree_stats {
    uint64_t item_count;
    uint64_t allocated_pages;
    uint64_t live_pages;
    uint64_t free_pages;
    uint64_t comparator_id;
    uint32_t height;
    uint32_t page_size;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t leaf_capacity;
    uint32_t internal_capacity;
} btree_stats;

/*
 * Initialize options for unsigned-byte lexicographic ordering. Keys and values
 * are fixed-width byte sequences copied by the tree.
 */
void btree_options_init(btree_options *options, uint32_t key_size, uint32_t value_size);

/*
 * A custom comparator must define a deterministic strict total order and use a
 * stable comparator_id >= BTREE_COMPARATOR_USER_MIN. The comparator and its
 * borrowed context must remain valid for the tree lifetime. Reopen requires
 * the same key size, value size, comparator ID, and comparator semantics.
 */
btree_status btree_create(const btree_storage *storage, const btree_options *options,
                          btree **tree_out);
btree_status btree_open(const btree_storage *storage, const btree_options *options,
                        btree **tree_out);
btree_status btree_read_schema(const btree_storage *storage, btree_schema *schema_out);
void btree_close(btree *tree);

btree_status btree_get(btree *tree, const void *key, void *value_out);
/* inserted_out is valid only when btree_put returns BTREE_OK. */
btree_status btree_put(btree *tree, const void *key, const void *value, bool *inserted_out);
/* removed_out is valid only when btree_delete returns BTREE_OK. */
btree_status btree_delete(btree *tree, const void *key, bool *removed_out);

/*
 * Scan [begin_key, end_key). A NULL bound is unbounded. The callback may return
 * BTREE_SCAN_STOP, causing btree_scan to return BTREE_STOPPED. Put, delete, and
 * nested scan calls on the same tree return BTREE_BUSY while a callback runs.
 */
btree_status btree_scan(btree *tree, const void *begin_key, const void *end_key,
                        btree_scan_fn callback, void *context);

btree_status btree_validate(btree *tree, btree_stats *stats_out, char *error_out,
                            size_t error_capacity);

const char *btree_status_string(btree_status status);

#ifdef __cplusplus
}
#endif

#endif
