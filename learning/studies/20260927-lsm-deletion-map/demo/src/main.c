#include "deletion_map.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

static void require_ok(enum dm_status status, const char *operation) {
  if (status != DM_OK) {
    fprintf(stderr, "%s failed: %s\n", operation, dm_status_name(status));
    exit(EXIT_FAILURE);
  }
}

static void print_output(const char *label,
                         const struct dm_scan_output *output) {
  size_t i;

  printf("%s=[", label);
  for (i = 0; i < output->count; ++i) {
    printf("%s%" PRIu64, i == 0 ? "" : ",", output->keys[i]);
  }
  puts("]");
}

int main(void) {
  const struct dm_visibility_proof complete = {
      .all_sources = true,
      .total_order = true,
      .full_timestamp_history = true,
      .io_complete = true,
      .source_epoch = 7,
  };
  const struct dm_record safe_records[] = {
      {.key = 10, .sequence = 10, .value = 100, .source_id = 1,
       .kind = DM_PUT},
      {.key = 20, .sequence = 10, .value = 200, .source_id = 1,
       .kind = DM_PUT},
      {.key = 30, .sequence = 10, .value = 300, .source_id = 1,
       .kind = DM_PUT},
      {.key = 40, .sequence = 10, .value = 400, .source_id = 1,
       .kind = DM_PUT},
      {.key = 10, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 20, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 30, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
  };
  const struct dm_record unsafe_records[] = {
      {.key = 10, .sequence = 10, .value = 100, .source_id = 1,
       .kind = DM_PUT},
      {.key = 15, .sequence = 10, .value = 150, .source_id = 1,
       .kind = DM_PUT},
      {.key = 20, .sequence = 10, .value = 200, .source_id = 1,
       .kind = DM_PUT},
      {.key = 25, .sequence = 10, .value = 250, .source_id = 1,
       .kind = DM_PUT},
      {.key = 30, .sequence = 10, .value = 300, .source_id = 1,
       .kind = DM_PUT},
      {.key = 35, .sequence = 10, .value = 350, .source_id = 1,
       .kind = DM_PUT},
      {.key = 40, .sequence = 10, .value = 400, .source_id = 1,
       .kind = DM_PUT},
      {.key = 10, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 20, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 30, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
  };
  const struct dm_record partial_records[] = {
      {.key = 10, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 20, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 30, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 40, .sequence = 30, .value = 400, .source_id = 0,
       .kind = DM_PUT},
  };
  struct dm_record future_records[ARRAY_LEN(safe_records) + 1U];
  struct dm_snapshot_view safe_view;
  struct dm_snapshot_view unsafe_view;
  struct dm_snapshot_view partial_view;
  struct dm_snapshot_view old_view;
  struct dm_snapshot_view future_view;
  struct dm_deletion_map map;
  struct dm_deletion_map naive_map = {
      .ranges = {{.start_key = 10, .end_key = 40, .sequence = 30}},
      .count = 1,
      .built_from_snapshot = 30,
      .built_from_epoch = 7,
  };
  struct dm_build_stats build_stats;
  struct dm_scan_output baseline;
  struct dm_scan_output accelerated;
  struct dm_scan_output unsafe_baseline;
  struct dm_scan_output unsafe_result;
  struct dm_scan_output old_result;
  struct dm_scan_output future_result;
  struct dm_scan_stats baseline_stats;
  struct dm_scan_stats accelerated_stats;
  struct dm_scan_stats ignored_stats;
  struct dm_visibility_proof partial = complete;
  enum dm_status status;
  size_t i;

  require_ok(dm_materialize_snapshot(safe_records, ARRAY_LEN(safe_records), 30,
                                     complete, &safe_view),
             "materialize safe snapshot");
  require_ok(dm_convert_tombstone_runs(&safe_view, 3, 7, &map, &build_stats),
             "convert safe tombstone run");
  require_ok(dm_scan_view(&safe_view, NULL, DM_FORWARD, &baseline,
                          &baseline_stats),
             "baseline scan");
  require_ok(dm_scan_view(&safe_view, &map, DM_FORWARD, &accelerated,
                          &accelerated_stats),
             "accelerated scan");

  printf("safe conversion: [%" PRIu64 ",%" PRIu64 ")@%" PRIu64 "\n",
         map.ranges[0].start_key, map.ranges[0].end_key,
         map.ranges[0].sequence);
  print_output("baseline", &baseline);
  print_output("mapped", &accelerated);
  printf("abstract steps: %zu -> %zu (%zu point entries skipped)\n",
         baseline_stats.point_entries_examined +
             baseline_stats.range_entries_examined,
         accelerated_stats.point_entries_examined +
             accelerated_stats.range_entries_examined,
         accelerated_stats.point_entries_skipped);

  partial.all_sources = false;
  require_ok(dm_materialize_snapshot(partial_records,
                                     ARRAY_LEN(partial_records), 30, partial,
                                     &partial_view),
             "materialize partial snapshot");
  status =
      dm_convert_tombstone_runs(&partial_view, 3, 7, &map, &build_stats);
  printf("partial-level conversion: %s\n", dm_status_name(status));

  require_ok(dm_materialize_snapshot(unsafe_records, ARRAY_LEN(unsafe_records),
                                     30, complete, &unsafe_view),
             "materialize unsafe example");
  require_ok(dm_scan_view(&unsafe_view, NULL, DM_FORWARD, &unsafe_baseline,
                          &ignored_stats),
             "unsafe baseline scan");
  require_ok(dm_scan_view(&unsafe_view, &naive_map, DM_FORWARD, &unsafe_result,
                          &ignored_stats),
             "unsafe mapped scan");
  print_output("partial-view baseline", &unsafe_baseline);
  print_output("naive range result", &unsafe_result);
  printf("naive range verdict: %s\n",
         dm_outputs_equal(&unsafe_baseline, &unsafe_result)
             ? "unexpected match"
             : "CORRUPTION detected by oracle");

  for (i = 0; i < ARRAY_LEN(safe_records); ++i) {
    future_records[i] = safe_records[i];
  }
  future_records[ARRAY_LEN(safe_records)] =
      (struct dm_record){.key = 20,
                         .sequence = 35,
                         .value = 205,
                         .source_id = 0,
                         .kind = DM_PUT};

  require_ok(dm_materialize_snapshot(future_records,
                                     ARRAY_LEN(future_records), 20, complete,
                                     &old_view),
             "materialize old snapshot");
  require_ok(dm_scan_view(&old_view, &naive_map, DM_FORWARD, &old_result,
                          &ignored_stats),
             "scan old snapshot");
  print_output("snapshot@20", &old_result);

  require_ok(dm_materialize_snapshot(future_records,
                                     ARRAY_LEN(future_records), 35, complete,
                                     &future_view),
             "materialize future snapshot");
  require_ok(dm_scan_view(&future_view, &naive_map, DM_FORWARD, &future_result,
                          &ignored_stats),
             "scan future snapshot");
  print_output("snapshot@35", &future_result);

  return EXIT_SUCCESS;
}
