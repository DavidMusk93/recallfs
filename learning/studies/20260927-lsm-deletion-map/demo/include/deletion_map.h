#ifndef RECALLFS_DELETION_MAP_H
#define RECALLFS_DELETION_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DM_MAX_RECORDS 128U
#define DM_MAX_VISIBLE_KEYS 128U
#define DM_MAX_RANGES 32U

enum dm_record_kind {
  DM_PUT = 1,
  DM_POINT_DELETE = 2,
};

enum dm_status {
  DM_OK = 0,
  DM_INVALID_ARGUMENT,
  DM_CAPACITY_EXCEEDED,
  DM_CONFLICTING_RECORD,
  DM_INCOMPLETE_VIEW,
  DM_STALE_TARGET,
};

enum dm_direction {
  DM_FORWARD = 1,
  DM_REVERSE = 2,
};

struct dm_record {
  uint64_t key;
  uint64_t sequence;
  int64_t value;
  uint32_t source_id;
  enum dm_record_kind kind;
};

/*
 * This proof is supplied by the LSM iterator integration. The deletion-map
 * builder cannot infer completeness from a stream that has already omitted a
 * file, prefix, timestamp, or failed I/O.
 */
struct dm_visibility_proof {
  bool all_sources;
  bool total_order;
  bool full_timestamp_history;
  bool io_complete;
  uint64_t source_epoch;
};

struct dm_visible_entry {
  uint64_t key;
  uint64_t sequence;
  int64_t value;
  enum dm_record_kind kind;
};

struct dm_snapshot_view {
  struct dm_visible_entry entries[DM_MAX_VISIBLE_KEYS];
  size_t count;
  uint64_t snapshot_sequence;
  struct dm_visibility_proof proof;
};

struct dm_range {
  uint64_t start_key;
  uint64_t end_key;
  uint64_t sequence;
};

struct dm_deletion_map {
  struct dm_range ranges[DM_MAX_RANGES];
  size_t count;
  uint64_t built_from_snapshot;
  uint64_t built_from_epoch;
};

struct dm_build_stats {
  size_t candidate_runs;
  size_t inserted_ranges;
  size_t below_threshold_runs;
  size_t unbounded_tail_runs;
};

struct dm_scan_output {
  uint64_t keys[DM_MAX_VISIBLE_KEYS];
  int64_t values[DM_MAX_VISIBLE_KEYS];
  size_t count;
};

/*
 * These counters describe the abstract merged-iterator work, not wall-clock
 * time. A range probe shadows point entries at sequence <= range.sequence.
 */
struct dm_scan_stats {
  size_t point_entries_examined;
  size_t point_entries_skipped;
  size_t range_entries_examined;
};

const char *dm_status_name(enum dm_status status);

enum dm_status dm_materialize_snapshot(
    const struct dm_record *records, size_t record_count,
    uint64_t snapshot_sequence, struct dm_visibility_proof proof,
    struct dm_snapshot_view *view);

enum dm_status dm_convert_tombstone_runs(
    const struct dm_snapshot_view *view, size_t min_tombstones,
    uint64_t target_epoch, struct dm_deletion_map *map,
    struct dm_build_stats *stats);

enum dm_status dm_scan_view(const struct dm_snapshot_view *view,
                            const struct dm_deletion_map *map,
                            enum dm_direction direction,
                            struct dm_scan_output *output,
                            struct dm_scan_stats *stats);

bool dm_outputs_equal(const struct dm_scan_output *left,
                      const struct dm_scan_output *right);

#endif
