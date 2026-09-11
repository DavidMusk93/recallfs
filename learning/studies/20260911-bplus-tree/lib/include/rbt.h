#ifndef RBT_H
#define RBT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBT_VERSION_MAJOR 0
#define RBT_VERSION_MINOR 1
#define RBT_VERSION_PATCH 0

#define RBT_FORMAT_VERSION UINT32_C(1)
#define RBT_MIN_PAGE_SIZE UINT32_C(512)
#define RBT_MAX_PAGE_SIZE UINT32_C(65536)
#define RBT_MAX_COLUMNS UINT32_C(64)
#define RBT_MAX_FIELD_SIZE (UINT32_C(1024) * UINT32_C(1024))
#define RBT_MAX_ROW_SIZE (UINT32_C(4) * UINT32_C(1024) * UINT32_C(1024))

enum rbt_type {
    RBT_TYPE_BOOL = 1,
    RBT_TYPE_I64 = 2,
    RBT_TYPE_U64 = 3,
    RBT_TYPE_BYTES = 4,
    RBT_TYPE_UTF8 = 5,
};

enum rbt_column_flags {
    RBT_COLUMN_NULLABLE = 1u << 0,
    RBT_COLUMN_DESCENDING = 1u << 1,
};

struct rbt_column {
    uint32_t id;
    enum rbt_type type;
    uint32_t flags;
    uint32_t max_size;
};

/*
 * Column arrays are borrowed by rbt_create() for the duration of the
 * call. Create validates, copies, and persists the complete schema. Schemas
 * returned by rbt_get_schema() are borrowed from the tree.
 *
 * Fixed scalar columns (BOOL, I64, and U64) require max_size == 0. BYTES and
 * UTF8 columns require 0 < max_size <= RBT_MAX_FIELD_SIZE. Column IDs must be
 * unique across both arrays. A key must contain at least one column, and the
 * total number of key and value columns must not exceed RBT_MAX_COLUMNS.
 */
struct rbt_schema {
    uint64_t id;
    const struct rbt_column *key_columns;
    size_t key_column_count;
    const struct rbt_column *value_columns;
    size_t value_column_count;
};

struct rbt_bytes {
    const void *data;
    size_t size;
};

struct rbt_value {
    enum rbt_type type;
    bool is_null;
    union {
        bool boolean;
        int64_t i64;
        uint64_t u64;
        struct rbt_bytes bytes;
    } as;
};

/*
 * Records borrow both arrays and all byte payloads for the duration of the
 * operation. Get, delete, and scan bounds require value == NULL and
 * value_count == 0. Put requires both counts to match the persisted schema.
 */
struct rbt_record {
    const struct rbt_value *key;
    size_t key_count;
    const struct rbt_value *value;
    size_t value_count;
};

struct rbt_page_update {
    uint64_t page_id;
    const void *data;
};

/*
 * The tree copies this callback table by value; context remains borrowed and
 * must outlive every tree using it. read_page copies exactly page_size bytes
 * into caller-owned memory. page_count writes the number of addressable pages.
 *
 * commit_pages synchronously and atomically publishes the complete page set.
 * Every page ID occurs at most once. The updates array and page buffers remain
 * caller-owned and are callback-lifetime only. Returning 0 means the complete
 * set is durable and visible. A provider must return -EOWNERDEAD when failure
 * leaves publication unknown; the tree then permanently poisons that handle.
 */
struct rbt_storage {
    void *context;
    uint32_t page_size;
    int (*read_page)(void *context, uint64_t page_id, void *data_out);
    int (*page_count)(void *context, uint64_t *out_count);
    int (*commit_pages)(void *context, const struct rbt_page_update *updates, size_t update_count);
};

struct rbt_config {
    const struct rbt_storage *storage;
    const struct rbt_schema *schema;
};

struct rbt;
struct rbt_row;

enum rbt_scan_action {
    RBT_SCAN_CONTINUE = 0,
    RBT_SCAN_STOP = 1,
};

/*
 * The row and all views obtained from it are valid only during the callback.
 * rbt_row_destroy() rejects callback-lifetime rows with -EINVAL. Returning
 * RBT_SCAN_STOP requests successful early termination; rbt_scan() returns 0.
 */
typedef enum rbt_scan_action (*rbt_scan_fn)(void *context, const struct rbt_row *row);

/*
 * allocated_pages == live_pages + free_pages. live_pages consists of one
 * metadata page plus schema_pages, tree_pages, and overflow_pages.
 */
struct rbt_stats {
    uint64_t item_count;
    uint64_t allocated_pages;
    uint64_t live_pages;
    uint64_t free_pages;
    uint64_t schema_pages;
    uint64_t tree_pages;
    uint64_t overflow_pages;
    uint32_t height;
    uint32_t page_size;
};

/*
 * Public operations return 0 on success or a negative errno value. Defined
 * failures include -ENOENT, -EINVAL, -ENOMEM, -EIO, -EBADMSG, -ENOTSUP,
 * -EBUSY, and -EOWNERDEAD. Every required out parameter is initialized to a
 * null, false, or zero state before validation can fail.
 */
int rbt_create(const struct rbt_config *config, struct rbt **out_rbt);
/* Open loads the persisted schema and accepts no caller-supplied schema. */
int rbt_open(const struct rbt_storage *storage, struct rbt **out_rbt);
int rbt_destroy(struct rbt *rbt);
int rbt_get_schema(const struct rbt *rbt, const struct rbt_schema **out_schema);

int rbt_get(struct rbt *rbt, const struct rbt_record *key, struct rbt_row **out_row);
int rbt_put(struct rbt *rbt, const struct rbt_record *record, bool *out_inserted);
int rbt_delete(struct rbt *rbt, const struct rbt_record *key, bool *out_deleted);
int rbt_scan(struct rbt *rbt, const struct rbt_record *begin, const struct rbt_record *end,
             rbt_scan_fn callback, void *context);

/*
 * Values returned through these accessors are borrowed from the row. Rows
 * returned by rbt_get() are owned and released with rbt_row_destroy().
 */
int rbt_row_get_key(const struct rbt_row *row, size_t index, const struct rbt_value **out_value);
int rbt_row_get_value(const struct rbt_row *row, size_t index, const struct rbt_value **out_value);
int rbt_row_destroy(struct rbt_row *row);

int rbt_validate(struct rbt *rbt, struct rbt_stats *out_stats, char *error_out,
                 size_t error_capacity);
const char *rbt_strerror(int error);

#ifdef __cplusplus
}
#endif

#endif
