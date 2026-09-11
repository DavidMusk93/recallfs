#ifndef BPTREE_H
#define BPTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BPTREE_FORMAT_VERSION 1u
#define BPTREE_MIN_PAGE_SIZE 512u
#define BPTREE_MAX_PAGE_SIZE 65536u

typedef enum bpt_status {
    BPT_OK = 0,
    BPT_NOT_FOUND = 1,
    BPT_STOPPED = 2,
    BPT_INVALID_ARGUMENT = 3,
    BPT_OUT_OF_MEMORY = 4,
    BPT_IO = 5,
    BPT_CORRUPT = 6,
    BPT_UNSUPPORTED = 7,
    BPT_BUSY = 8,
    /*
     * Commit-unknown: a mutation may or may not have been published. This
     * permanently poisons the current tree handle.
     */
    BPT_RECOVERY_REQUIRED = 9
} bpt_status;

typedef struct bpt_page_update {
    uint64_t page_id;
    const void *data;
} bpt_page_update;

/*
 * The tree copies bpt_storage by value during create/open, but context is
 * borrowed. The context and its provider/backend must outlive every tree that
 * uses it. bpt_tree_close never closes or destroys storage.
 *
 * read_page synchronously copies exactly page_size bytes into caller-owned
 * data_out; ownership is not transferred.
 * page_count returns the number of addressable logical pages.
 * commit_pages synchronously and atomically publishes the complete update set.
 * The updates array and every page buffer referenced by it are caller-owned,
 * valid only for the duration of the callback, and must not be retained.
 *
 * A custom provider must return BPT_RECOVERY_REQUIRED whenever failure can
 * leave an uncertain visible state.
 */
typedef struct bpt_storage {
    void *context;
    uint32_t page_size;
    bpt_status (*read_page)(void *context, uint64_t page_id, void *data_out);
    bpt_status (*page_count)(void *context, uint64_t *count_out);
    bpt_status (*commit_pages)(void *context, const bpt_page_update *updates, size_t update_count);
} bpt_storage;

typedef struct bpt_tree bpt_tree;

typedef bpt_status (*bpt_scan_callback)(void *context, uint64_t key, uint64_t value);

typedef struct bpt_stats {
    uint64_t item_count;
    uint64_t allocated_pages;
    uint64_t live_pages;
    uint64_t free_pages;
    uint32_t height;
    uint32_t page_size;
    uint32_t leaf_capacity;
    uint32_t internal_capacity;
} bpt_stats;

/*
 * A tree and its borrowed storage context are single-threaded. Serialize all
 * calls that use either one. Put, delete, and nested scan calls made from a
 * scan callback return BPT_BUSY. Do not begin a second mutation until the first
 * returns.
 *
 * When a mutation returns BPT_RECOVERY_REQUIRED, its commit outcome is unknown
 * and the current tree handle is permanently poisoned. Subsequent
 * status-returning calls on that handle return BPT_RECOVERY_REQUIRED. For the
 * file backend, close the tree, close the backend, reopen the backend, reopen
 * the tree, and inspect persisted state before deciding whether to retry.
 */
bpt_status bpt_tree_create(const bpt_storage *storage, bpt_tree **tree_out);
bpt_status bpt_tree_open(const bpt_storage *storage, bpt_tree **tree_out);
void bpt_tree_close(bpt_tree *tree);

bpt_status bpt_tree_get(bpt_tree *tree, uint64_t key, uint64_t *value_out);
/* inserted_out is valid only when bpt_tree_put returns BPT_OK. */
bpt_status bpt_tree_put(bpt_tree *tree, uint64_t key, uint64_t value, bool *inserted_out);
/* removed_out is valid only when bpt_tree_delete returns BPT_OK. */
bpt_status bpt_tree_delete(bpt_tree *tree, uint64_t key, bool *removed_out);
bpt_status bpt_tree_scan(bpt_tree *tree, uint64_t begin_key, uint64_t end_key,
                         bpt_scan_callback callback, void *context);

bpt_status bpt_tree_validate(bpt_tree *tree, bpt_stats *stats_out, char *error_out,
                             size_t error_capacity);

const char *bpt_status_string(bpt_status status);

#ifdef __cplusplus
}
#endif

#endif
