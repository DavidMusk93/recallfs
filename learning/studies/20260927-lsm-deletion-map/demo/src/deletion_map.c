#include "deletion_map.h"

#include <string.h>

static bool dm_valid_kind(enum dm_record_kind kind) {
  return kind == DM_PUT || kind == DM_POINT_DELETE;
}

static void dm_sort_keys(uint64_t *keys, size_t count) {
  size_t i;

  for (i = 1; i < count; ++i) {
    uint64_t key = keys[i];
    size_t j = i;

    while (j > 0 && keys[j - 1] > key) {
      keys[j] = keys[j - 1];
      --j;
    }
    keys[j] = key;
  }
}

static const struct dm_range *dm_covering_range(
    const struct dm_deletion_map *map, uint64_t key,
    uint64_t snapshot_sequence, size_t *range_index) {
  const struct dm_range *best = NULL;
  size_t best_index = 0;
  size_t i;

  if (map == NULL) {
    return NULL;
  }

  for (i = 0; i < map->count; ++i) {
    const struct dm_range *range = &map->ranges[i];

    if (range->sequence <= snapshot_sequence && range->start_key <= key &&
        key < range->end_key &&
        (best == NULL || best->sequence < range->sequence)) {
      best = range;
      best_index = i;
    }
  }

  if (best != NULL && range_index != NULL) {
    *range_index = best_index;
  }
  return best;
}

static enum dm_status dm_emit_visible(
    const struct dm_visible_entry *entry, struct dm_scan_output *output) {
  if (entry->kind != DM_PUT) {
    return DM_OK;
  }
  if (output->count == DM_MAX_VISIBLE_KEYS) {
    return DM_CAPACITY_EXCEEDED;
  }
  output->keys[output->count] = entry->key;
  output->values[output->count] = entry->value;
  ++output->count;
  return DM_OK;
}

const char *dm_status_name(enum dm_status status) {
  switch (status) {
    case DM_OK:
      return "ok";
    case DM_INVALID_ARGUMENT:
      return "invalid argument";
    case DM_CAPACITY_EXCEEDED:
      return "capacity exceeded";
    case DM_CONFLICTING_RECORD:
      return "conflicting record";
    case DM_INCOMPLETE_VIEW:
      return "incomplete view";
    case DM_STALE_TARGET:
      return "stale target";
  }
  return "unknown status";
}

enum dm_status dm_materialize_snapshot(
    const struct dm_record *records, size_t record_count,
    uint64_t snapshot_sequence, struct dm_visibility_proof proof,
    struct dm_snapshot_view *view) {
  uint64_t keys[DM_MAX_RECORDS];
  size_t key_count = 0;
  size_t i;

  if (view == NULL || (records == NULL && record_count != 0)) {
    return DM_INVALID_ARGUMENT;
  }
  memset(view, 0, sizeof(*view));
  view->snapshot_sequence = snapshot_sequence;
  view->proof = proof;

  if (record_count > DM_MAX_RECORDS) {
    return DM_CAPACITY_EXCEEDED;
  }

  for (i = 0; i < record_count; ++i) {
    size_t j;
    bool seen = false;

    if (!dm_valid_kind(records[i].kind)) {
      return DM_INVALID_ARGUMENT;
    }
    if (records[i].sequence > snapshot_sequence) {
      continue;
    }
    for (j = 0; j < key_count; ++j) {
      if (keys[j] == records[i].key) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      keys[key_count] = records[i].key;
      ++key_count;
    }
  }
  dm_sort_keys(keys, key_count);

  for (i = 0; i < key_count; ++i) {
    const struct dm_record *best = NULL;
    size_t j;

    for (j = 0; j < record_count; ++j) {
      const struct dm_record *candidate = &records[j];

      if (candidate->key != keys[i] ||
          candidate->sequence > snapshot_sequence) {
        continue;
      }
      if (best == NULL || best->sequence < candidate->sequence) {
        best = candidate;
      } else if (best->sequence == candidate->sequence &&
                 (best->kind != candidate->kind ||
                  (best->kind == DM_PUT && best->value != candidate->value))) {
        return DM_CONFLICTING_RECORD;
      }
    }

    if (best == NULL) {
      continue;
    }
    if (view->count == DM_MAX_VISIBLE_KEYS) {
      return DM_CAPACITY_EXCEEDED;
    }
    view->entries[view->count].key = best->key;
    view->entries[view->count].sequence = best->sequence;
    view->entries[view->count].value = best->value;
    view->entries[view->count].kind = best->kind;
    ++view->count;
  }

  return DM_OK;
}

enum dm_status dm_convert_tombstone_runs(
    const struct dm_snapshot_view *view, size_t min_tombstones,
    uint64_t target_epoch, struct dm_deletion_map *map,
    struct dm_build_stats *stats) {
  size_t run_start = 0;
  size_t run_length = 0;
  size_t i;

  if (view == NULL || map == NULL || stats == NULL) {
    return DM_INVALID_ARGUMENT;
  }
  memset(map, 0, sizeof(*map));
  memset(stats, 0, sizeof(*stats));
  map->built_from_snapshot = view->snapshot_sequence;
  map->built_from_epoch = view->proof.source_epoch;

  if (min_tombstones == 0) {
    return DM_OK;
  }
  if (!view->proof.all_sources || !view->proof.total_order ||
      !view->proof.full_timestamp_history || !view->proof.io_complete) {
    return DM_INCOMPLETE_VIEW;
  }
  if (target_epoch != view->proof.source_epoch) {
    return DM_STALE_TARGET;
  }

  for (i = 0; i < view->count; ++i) {
    const struct dm_visible_entry *entry = &view->entries[i];

    if (entry->kind == DM_POINT_DELETE) {
      if (run_length == 0) {
        run_start = i;
      }
      ++run_length;
      continue;
    }

    if (run_length != 0) {
      ++stats->candidate_runs;
      if (run_length >= min_tombstones) {
        struct dm_range *range;

        if (map->count == DM_MAX_RANGES) {
          map->count = 0;
          stats->inserted_ranges = 0;
          return DM_CAPACITY_EXCEEDED;
        }
        range = &map->ranges[map->count];
        range->start_key = view->entries[run_start].key;
        range->end_key = entry->key;
        range->sequence = view->snapshot_sequence;
        ++map->count;
        ++stats->inserted_ranges;
      } else {
        ++stats->below_threshold_runs;
      }
      run_length = 0;
    }
  }

  /*
   * A generic byte-key API has no universal successor for the last key and an
   * unbounded range needs a separate representation. Keep the tail as point
   * tombstones unless the integration owns a trustworthy exclusive bound.
   */
  if (run_length != 0) {
    ++stats->candidate_runs;
    ++stats->unbounded_tail_runs;
  }
  return DM_OK;
}

enum dm_status dm_scan_view(const struct dm_snapshot_view *view,
                            const struct dm_deletion_map *map,
                            enum dm_direction direction,
                            struct dm_scan_output *output,
                            struct dm_scan_stats *stats) {
  bool counted_ranges[DM_MAX_RANGES] = {false};
  size_t step;

  if (view == NULL || output == NULL || stats == NULL ||
      (direction != DM_FORWARD && direction != DM_REVERSE) ||
      (map != NULL && map->count > DM_MAX_RANGES)) {
    return DM_INVALID_ARGUMENT;
  }
  memset(output, 0, sizeof(*output));
  memset(stats, 0, sizeof(*stats));

  for (step = 0; step < view->count; ++step) {
    size_t index =
        direction == DM_FORWARD ? step : (view->count - 1U - step);
    const struct dm_visible_entry *entry = &view->entries[index];
    size_t range_index = 0;
    const struct dm_range *range = dm_covering_range(
        map, entry->key, view->snapshot_sequence, &range_index);

    if (range != NULL && range->sequence >= entry->sequence) {
      ++stats->point_entries_skipped;
      if (!counted_ranges[range_index]) {
        counted_ranges[range_index] = true;
        ++stats->range_entries_examined;
      }
      continue;
    }

    ++stats->point_entries_examined;
    {
      enum dm_status status = dm_emit_visible(entry, output);
      if (status != DM_OK) {
        return status;
      }
    }
  }

  return DM_OK;
}

bool dm_outputs_equal(const struct dm_scan_output *left,
                      const struct dm_scan_output *right) {
  size_t i;

  if (left == NULL || right == NULL || left->count != right->count) {
    return false;
  }
  for (i = 0; i < left->count; ++i) {
    if (left->keys[i] != right->keys[i] ||
        left->values[i] != right->values[i]) {
      return false;
    }
  }
  return true;
}
