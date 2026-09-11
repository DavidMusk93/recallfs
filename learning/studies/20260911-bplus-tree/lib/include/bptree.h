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
    BPT_RECOVERY_REQUIRED = 9
} bpt_status;

typedef struct bpt_page_update {
    uint64_t page_id;
    const void *data;
} bpt_page_update;

/*
 * read_page copies exactly page_size bytes into data_out.
 * page_count returns the number of addressable logical pages.
 * commit_pages atomically publishes the complete update set. A custom provider
 * must return BPT_RECOVERY_REQUIRED whenever failure can leave an uncertain
 * visible state. The tree never retains data pointers after a callback.
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

bpt_status bpt_tree_create(const bpt_storage *storage, bpt_tree **tree_out);
bpt_status bpt_tree_open(const bpt_storage *storage, bpt_tree **tree_out);
void bpt_tree_close(bpt_tree *tree);

bpt_status bpt_tree_get(bpt_tree *tree, uint64_t key, uint64_t *value_out);
bpt_status bpt_tree_put(bpt_tree *tree, uint64_t key, uint64_t value, bool *inserted_out);
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
