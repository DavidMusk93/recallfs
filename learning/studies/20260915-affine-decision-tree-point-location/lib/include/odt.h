#ifndef ODT_H
#define ODT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODT_VERSION_MAJOR 1
#define ODT_VERSION_MINOR 0
#define ODT_VERSION_PATCH 0
#define ODT_VERSION_STRING "1.0.0"

#define ODT_ABI_VERSION UINT32_C(1)
#define ODT_FORMAT_VERSION UINT32_C(1)

#define ODT_ALLOCATOR_VERSION UINT32_C(1)
#define ODT_LIMITS_VERSION UINT32_C(1)
#define ODT_BUILD_OPTIONS_VERSION UINT32_C(1)
#define ODT_ENCODE_OPTIONS_VERSION UINT32_C(1)
#define ODT_LOAD_LIMITS_VERSION UINT32_C(1)

/*
 * Source and binary compatibility are preserved within one major release.
 * Minor releases may append fields to versioned configuration structures and
 * append enum values, but do not reorder fields or change existing semantics.
 * Callers must use the matching initializer and preserve struct_size and
 * struct_version. The binary persistence format is versioned independently.
 */

typedef enum odt_status {
    ODT_OK = 0,
    ODT_INVALID_ARGUMENT = 1,
    ODT_INVALID_DATA = 2,
    ODT_LIMIT_EXCEEDED = 3,
    ODT_UNSUPPORTED_NUMERIC_ENVIRONMENT = 4,
    ODT_OUT_OF_MEMORY = 5,
    ODT_IO_ERROR = 6,
    ODT_COMMIT_UNKNOWN = 7,
    ODT_CORRUPT_DATA = 8,
    ODT_UNSUPPORTED_FORMAT = 9,
    ODT_INTERNAL_ERROR = 10
} odt_status;

/*
 * Returns an immutable process-lifetime string for every known status and
 * "unknown status" for other values. This function is thread-safe.
 */
const char *odt_status_string(odt_status status);

typedef void *(*odt_allocate_fn)(void *context, size_t size, size_t alignment);
typedef void (*odt_deallocate_fn)(void *context, void *pointer, size_t size, size_t alignment);

/*
 * Allocation requests always have nonzero size and power-of-two alignment.
 * allocate returns suitably aligned uninitialized storage or null. deallocate
 * receives the exact pointer, requested size, and requested alignment from the
 * matching successful allocation and is never called with a null pointer.
 *
 * The table is copied by every operation that retains allocations. context is
 * borrowed, is never freed by odt, and must remain valid through destruction
 * of every generation created with it. Callback synchronization is the
 * caller's responsibility when one allocator is used by concurrent calls.
 */
typedef struct odt_allocator {
    uint32_t struct_size;
    uint32_t struct_version;
    void *context;
    odt_allocate_fn allocate;
    odt_deallocate_fn deallocate;
} odt_allocator;

typedef struct odt_limits {
    uint32_t struct_size;
    uint32_t struct_version;
    size_t max_sites;
    size_t max_polygon_fragments;
    size_t max_internal_nodes;
    uint32_t max_depth;
    uint32_t reserved_0;
    size_t max_build_bytes;
    uint64_t max_build_work;
} odt_limits;

typedef struct odt_build_options {
    uint32_t struct_size;
    uint32_t struct_version;
} odt_build_options;

typedef struct odt_encode_options {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t format_version;
    uint32_t reserved_0;
} odt_encode_options;

typedef struct odt_load_limits {
    uint32_t struct_size;
    uint32_t struct_version;
    uint64_t max_encoded_bytes;
    size_t max_sites;
    size_t max_internal_nodes;
    size_t max_leaves;
    uint32_t max_depth;
    uint32_t reserved_0;
    size_t max_allocation_bytes;
} odt_load_limits;

/*
 * Initializers overwrite the complete destination with supported conservative
 * defaults. They return ODT_INVALID_ARGUMENT for a null destination and do not
 * access any shared mutable state. odt_allocator_init installs the default
 * aligned allocator; callers may replace the context and both callbacks.
 */
odt_status odt_allocator_init(odt_allocator *out_allocator);
odt_status odt_limits_init(odt_limits *out_limits);
odt_status odt_build_options_init(odt_build_options *out_options);
odt_status odt_encode_options_init(odt_encode_options *out_options);
odt_status odt_load_limits_init(odt_load_limits *out_limits);

typedef struct odt_domain {
    double min_x;
    double min_y;
    double max_x;
    double max_y;
} odt_domain;

typedef struct odt_site {
    double x;
    double y;
    int32_t region_id;
} odt_site;

typedef struct odt_point {
    double x;
    double y;
} odt_point;

typedef struct odt_build_stats {
    uint64_t site_count;
    uint64_t node_count;
    uint64_t leaf_count;
    uint64_t fragment_count;
    uint64_t build_work;
    uint64_t peak_build_bytes;
    uint64_t build_duration_ns;
    uint32_t maximum_depth;
    uint32_t reserved_0;
} odt_build_stats;

typedef enum odt_result_kind {
    ODT_RESULT_REGION = 1,
    ODT_RESULT_OUTSIDE = 2,
    ODT_RESULT_ERROR = 3
} odt_result_kind;

/*
 * REGION carries ODT_OK and a caller-supplied region ID. OUTSIDE carries
 * ODT_OK and region_id == 0. ERROR carries a non-ODT_OK point status and
 * region_id == 0. The kind, not the region_id value, selects the
 * interpretation.
 */
typedef struct odt_query_result {
    odt_result_kind kind;
    odt_status status;
    int32_t region_id;
} odt_query_result;

typedef struct odt_query_stats {
    uint64_t comparisons;
    uint64_t exact_fallbacks;
} odt_query_stats;

typedef struct odt_batch_stats {
    uint64_t attempted;
    uint64_t region_count;
    uint64_t outside_count;
    uint64_t failed_count;
    uint64_t comparisons;
    uint64_t exact_fallbacks;
} odt_batch_stats;

typedef struct odt_generation odt_generation;

/*
 * One-shot construction. domain and sites are borrowed for this call. options,
 * limits, and allocator are also call-borrowed and may be null to select their
 * initializer defaults. Site data and the allocator table are copied.
 *
 * out_generation is required and is set to null before any other validation.
 * On success it receives one unpublished, immutable, library-owned generation.
 * Optional out_stats is written only on success and otherwise remains
 * unchanged. No input pointer except allocator.context is retained.
 *
 * Build calls are independent and may run concurrently when their allocator
 * callbacks are safe for that use. Publication is entirely caller-owned.
 */
odt_status odt_build(const odt_domain *domain, const odt_site *sites, size_t site_count,
                     const odt_build_options *options, const odt_limits *limits,
                     const odt_allocator *allocator, odt_build_stats *out_stats,
                     odt_generation **out_generation);

/*
 * generation and point are borrowed for the call and no pointers are retained.
 * On failure, out_result and optional out_stats remain unchanged. On success,
 * both are fully overwritten. Query performs no allocation or mutation.
 *
 * Scalar and batch queries, and metadata accessors, may execute concurrently
 * against the same live generation without external locking.
 */
odt_status odt_query(const odt_generation *generation, const odt_point *point,
                     odt_query_result *out_result, odt_query_stats *out_stats);

/*
 * points and out_results are byte-strided arrays borrowed for this call.
 * Nonzero count requires nonnull arrays, point_stride >= sizeof(odt_point),
 * result_stride >= sizeof(odt_query_result), representable ranges, and
 * non-overlapping input/output ranges. Optional out_stats must not overlap
 * either array. Invalid envelopes fail before any result or out_stats mutation.
 *
 * Zero count succeeds, permits null arrays and zero strides, and writes zero
 * aggregate statistics when out_stats is nonnull. After a nonempty envelope
 * is accepted, every result slot is written exactly once. Per-point numeric
 * failures use ODT_RESULT_ERROR while the batch call returns ODT_OK.
 */
odt_status odt_query_batch(const odt_generation *generation, size_t count, const void *points,
                           size_t point_stride, void *out_results, size_t result_stride,
                           odt_batch_stats *out_stats);

/*
 * Metadata accessors borrow one live generation, retain nothing, and overwrite
 * their required output only on success. They may run concurrently with other
 * metadata accessors and queries against that generation.
 */
odt_status odt_generation_get_domain(const odt_generation *generation, odt_domain *out_domain);
odt_status odt_generation_get_site_count(const odt_generation *generation,
                                         uint64_t *out_site_count);
odt_status odt_generation_get_node_count(const odt_generation *generation,
                                         uint64_t *out_node_count);
odt_status odt_generation_get_leaf_count(const odt_generation *generation,
                                         uint64_t *out_leaf_count);
odt_status odt_generation_get_maximum_depth(const odt_generation *generation,
                                            uint32_t *out_maximum_depth);
odt_status odt_generation_get_encoded_size(const odt_generation *generation,
                                           uint64_t *out_encoded_size);
odt_status odt_generation_get_build_stats(const odt_generation *generation,
                                          odt_build_stats *out_stats);

/*
 * A sink callback returns zero after consuming the complete call-borrowed
 * buffer, or nonzero on failure. odt never retains the callback or buffer.
 */
typedef int (*odt_sink_write_fn)(void *context, const void *data, size_t size);

typedef struct odt_sink {
    void *context;
    odt_sink_write_fn write;
} odt_sink;

/*
 * Encodes one borrowed live generation through a borrowed sink. options may be
 * null for defaults. Callback failure maps to ODT_IO_ERROR. Optional
 * out_encoded_size is written only after complete success. Encoding is not
 * supported concurrently with another operation on the same generation in v1.
 */
odt_status odt_encode(const odt_generation *generation, const odt_encode_options *options,
                      const odt_sink *sink, uint64_t *out_encoded_size);

/*
 * A source callback returns zero after filling exactly size bytes, or nonzero
 * on failure. The source promises that [0, encoded_size) is one immutable
 * snapshot throughout odt_load. The callback and buffers are call-borrowed and
 * never retained.
 */
typedef int (*odt_source_read_at_fn)(void *context, uint64_t offset, void *data_out, size_t size);

typedef struct odt_source {
    void *context;
    uint64_t encoded_size;
    odt_source_read_at_fn read_at;
} odt_source;

/*
 * limits and allocator may be null for defaults. out_generation is required
 * and is set to null before other validation. A complete validated generation
 * is returned only on success; callback failure maps to ODT_IO_ERROR. The
 * source is not retained. Independent loads may run concurrently subject to
 * allocator callback synchronization.
 */
odt_status odt_load(const odt_source *source, const odt_load_limits *limits,
                    const odt_allocator *allocator, odt_generation **out_generation);

/*
 * File operations borrow path for the call and support local POSIX filesystems
 * in v1. Atomic save preserves the old destination on pre-rename failure. A
 * post-rename directory-sync failure returns ODT_COMMIT_UNKNOWN because the
 * new file may be visible without known crash durability. options, limits, and
 * allocator may be null for defaults. Load clears out_generation before other
 * validation and publishes no partial generation.
 *
 * File operations are not supported concurrently with another operation on
 * the same generation. Different generations and paths are independent.
 */
odt_status odt_save_file_atomic(const odt_generation *generation, const char *path,
                                const odt_encode_options *options);
odt_status odt_load_file(const char *path, const odt_load_limits *limits,
                         const odt_allocator *allocator, odt_generation **out_generation);

/*
 * Releases the generation with the copied allocator family and exact original
 * allocation layout. Null is accepted as a no-op. The allocator context must
 * still be live. The application must prove quiescence before calling destroy:
 * concurrent destroy, query during destroy, and use after destroy are invalid.
 * odt provides no publication, reference-counting, or reclamation primitive.
 */
void odt_generation_destroy(odt_generation *generation);

#ifdef __cplusplus
}
#endif

#endif
