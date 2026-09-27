#include "deletion_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,      \
              #condition);                                                     \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (false)

static const struct dm_record kSafeRecords[] = {
    {.key = 10, .sequence = 10, .value = 100, .source_id = 1, .kind = DM_PUT},
    {.key = 20, .sequence = 10, .value = 200, .source_id = 1, .kind = DM_PUT},
    {.key = 30, .sequence = 10, .value = 300, .source_id = 1, .kind = DM_PUT},
    {.key = 40, .sequence = 10, .value = 400, .source_id = 1, .kind = DM_PUT},
    {.key = 10, .sequence = 30, .source_id = 0, .kind = DM_POINT_DELETE},
    {.key = 20, .sequence = 30, .source_id = 0, .kind = DM_POINT_DELETE},
    {.key = 30, .sequence = 30, .source_id = 0, .kind = DM_POINT_DELETE},
};

static struct dm_visibility_proof complete_proof(void) {
  const struct dm_visibility_proof proof = {
      .all_sources = true,
      .total_order = true,
      .full_timestamp_history = true,
      .io_complete = true,
      .source_epoch = 7,
  };
  return proof;
}

static struct dm_scan_output oracle_point_only(
    const struct dm_record *records, size_t record_count, uint64_t snapshot,
    enum dm_direction direction) {
  struct dm_scan_output output;
  int64_t key;
  int64_t begin = direction == DM_FORWARD ? 0 : 50;
  int64_t end = direction == DM_FORWARD ? 51 : -1;
  int64_t delta = direction == DM_FORWARD ? 1 : -1;

  memset(&output, 0, sizeof(output));
  for (key = begin; key != end; key += delta) {
    const struct dm_record *best = NULL;
    size_t i;

    for (i = 0; i < record_count; ++i) {
      if (records[i].key == (uint64_t)key &&
          records[i].sequence <= snapshot &&
          (best == NULL || best->sequence < records[i].sequence)) {
        best = &records[i];
      }
    }
    if (best != NULL && best->kind == DM_PUT) {
      CHECK(output.count < DM_MAX_VISIBLE_KEYS);
      output.keys[output.count] = best->key;
      output.values[output.count] = best->value;
      ++output.count;
    }
  }
  return output;
}

static void test_safe_conversion(void) {
  struct dm_snapshot_view view;
  struct dm_deletion_map map;
  struct dm_build_stats build_stats;
  struct dm_scan_output baseline;
  struct dm_scan_output mapped;
  struct dm_scan_output reverse;
  struct dm_scan_stats baseline_stats;
  struct dm_scan_stats mapped_stats;
  struct dm_scan_stats reverse_stats;

  CHECK(dm_materialize_snapshot(kSafeRecords, ARRAY_LEN(kSafeRecords), 30,
                                complete_proof(), &view) == DM_OK);
  CHECK(view.count == 4);
  CHECK(dm_convert_tombstone_runs(&view, 3, 7, &map, &build_stats) == DM_OK);
  CHECK(map.count == 1);
  CHECK(map.ranges[0].start_key == 10);
  CHECK(map.ranges[0].end_key == 40);
  CHECK(map.ranges[0].sequence == 30);
  CHECK(build_stats.candidate_runs == 1);
  CHECK(build_stats.inserted_ranges == 1);

  CHECK(dm_scan_view(&view, NULL, DM_FORWARD, &baseline, &baseline_stats) ==
        DM_OK);
  CHECK(dm_scan_view(&view, &map, DM_FORWARD, &mapped, &mapped_stats) ==
        DM_OK);
  CHECK(dm_scan_view(&view, &map, DM_REVERSE, &reverse, &reverse_stats) ==
        DM_OK);
  CHECK(dm_outputs_equal(&baseline, &mapped));
  CHECK(mapped.count == 1 && mapped.keys[0] == 40);
  CHECK(reverse.count == 1 && reverse.keys[0] == 40);
  CHECK(baseline_stats.point_entries_examined == 4);
  CHECK(mapped_stats.point_entries_examined == 1);
  CHECK(mapped_stats.point_entries_skipped == 3);
  CHECK(mapped_stats.range_entries_examined == 1);
  CHECK(reverse_stats.point_entries_skipped == 3);
}

static void test_incomplete_and_stale_views_are_rejected(void) {
  struct dm_visibility_proof proof = complete_proof();
  struct dm_snapshot_view view;
  struct dm_deletion_map map;
  struct dm_build_stats stats;

  proof.all_sources = false;
  CHECK(dm_materialize_snapshot(kSafeRecords, ARRAY_LEN(kSafeRecords), 30,
                                proof, &view) == DM_OK);
  CHECK(dm_convert_tombstone_runs(&view, 3, 7, &map, &stats) ==
        DM_INCOMPLETE_VIEW);
  CHECK(map.count == 0);

  proof = complete_proof();
  CHECK(dm_materialize_snapshot(kSafeRecords, ARRAY_LEN(kSafeRecords), 30,
                                proof, &view) == DM_OK);
  CHECK(dm_convert_tombstone_runs(&view, 3, 8, &map, &stats) ==
        DM_STALE_TARGET);
  CHECK(map.count == 0);
}

static void test_partial_level_conversion_is_corrupting(void) {
  const struct dm_record records[] = {
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
  const struct dm_deletion_map unsafe_map = {
      .ranges = {{.start_key = 10, .end_key = 40, .sequence = 30}},
      .count = 1,
      .built_from_snapshot = 30,
      .built_from_epoch = 7,
  };
  struct dm_snapshot_view view;
  struct dm_scan_output expected;
  struct dm_scan_output actual;
  struct dm_scan_stats stats;

  CHECK(dm_materialize_snapshot(records, ARRAY_LEN(records), 30,
                                complete_proof(), &view) == DM_OK);
  expected = oracle_point_only(records, ARRAY_LEN(records), 30, DM_FORWARD);
  CHECK(expected.count == 4);
  CHECK(expected.keys[0] == 15 && expected.keys[1] == 25 &&
        expected.keys[2] == 35 && expected.keys[3] == 40);
  CHECK(dm_scan_view(&view, &unsafe_map, DM_FORWARD, &actual, &stats) ==
        DM_OK);
  CHECK(actual.count == 1 && actual.keys[0] == 40);
  CHECK(!dm_outputs_equal(&expected, &actual));
}

static void test_snapshot_and_newer_write_semantics(void) {
  struct dm_record records[ARRAY_LEN(kSafeRecords) + 1U];
  struct dm_snapshot_view conversion_view;
  struct dm_snapshot_view read_view;
  struct dm_deletion_map map;
  struct dm_build_stats build_stats;
  uint64_t snapshot;
  size_t i;

  for (i = 0; i < ARRAY_LEN(kSafeRecords); ++i) {
    records[i] = kSafeRecords[i];
  }
  records[ARRAY_LEN(kSafeRecords)] =
      (struct dm_record){.key = 20,
                         .sequence = 35,
                         .value = 205,
                         .source_id = 0,
                         .kind = DM_PUT};

  CHECK(dm_materialize_snapshot(records, ARRAY_LEN(records), 30,
                                complete_proof(), &conversion_view) == DM_OK);
  CHECK(dm_convert_tombstone_runs(&conversion_view, 3, 7, &map,
                                  &build_stats) == DM_OK);

  for (snapshot = 0; snapshot <= 40; ++snapshot) {
    enum dm_direction directions[] = {DM_FORWARD, DM_REVERSE};
    size_t d;

    CHECK(dm_materialize_snapshot(records, ARRAY_LEN(records), snapshot,
                                  complete_proof(), &read_view) == DM_OK);
    for (d = 0; d < ARRAY_LEN(directions); ++d) {
      struct dm_scan_output expected =
          oracle_point_only(records, ARRAY_LEN(records), snapshot,
                            directions[d]);
      struct dm_scan_output actual;
      struct dm_scan_stats stats;

      CHECK(dm_scan_view(&read_view, &map, directions[d], &actual, &stats) ==
            DM_OK);
      CHECK(dm_outputs_equal(&expected, &actual));
    }
  }
}

static void test_threshold_and_unbounded_tail(void) {
  const struct dm_record tail_records[] = {
      {.key = 10, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 20, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
      {.key = 30, .sequence = 30, .source_id = 0,
       .kind = DM_POINT_DELETE},
  };
  struct dm_snapshot_view view;
  struct dm_deletion_map map;
  struct dm_build_stats stats;

  CHECK(dm_materialize_snapshot(kSafeRecords, ARRAY_LEN(kSafeRecords), 30,
                                complete_proof(), &view) == DM_OK);
  CHECK(dm_convert_tombstone_runs(&view, 4, 7, &map, &stats) == DM_OK);
  CHECK(map.count == 0);
  CHECK(stats.below_threshold_runs == 1);

  CHECK(dm_materialize_snapshot(tail_records, ARRAY_LEN(tail_records), 30,
                                complete_proof(), &view) == DM_OK);
  CHECK(dm_convert_tombstone_runs(&view, 3, 7, &map, &stats) == DM_OK);
  CHECK(map.count == 0);
  CHECK(stats.unbounded_tail_runs == 1);
}

static void test_conflicting_records_are_rejected(void) {
  const struct dm_record records[] = {
      {.key = 10, .sequence = 7, .value = 1, .kind = DM_PUT},
      {.key = 10, .sequence = 7, .kind = DM_POINT_DELETE},
  };
  struct dm_snapshot_view view;

  CHECK(dm_materialize_snapshot(records, ARRAY_LEN(records), 7,
                                complete_proof(), &view) ==
        DM_CONFLICTING_RECORD);
}

static void test_capacity_failure_publishes_nothing(void) {
  struct dm_record records[(DM_MAX_RANGES + 1U) * 2U];
  struct dm_snapshot_view view;
  struct dm_deletion_map map;
  struct dm_build_stats stats;
  size_t i;

  for (i = 0; i < DM_MAX_RANGES + 1U; ++i) {
    records[i * 2U] =
        (struct dm_record){.key = (uint64_t)(i * 2U),
                           .sequence = 1,
                           .kind = DM_POINT_DELETE};
    records[i * 2U + 1U] =
        (struct dm_record){.key = (uint64_t)(i * 2U + 1U),
                           .sequence = 1,
                           .value = (int64_t)i,
                           .kind = DM_PUT};
  }

  CHECK(dm_materialize_snapshot(records, ARRAY_LEN(records), 1,
                                complete_proof(), &view) == DM_OK);
  CHECK(dm_convert_tombstone_runs(&view, 1, 7, &map, &stats) ==
        DM_CAPACITY_EXCEEDED);
  CHECK(map.count == 0);
  CHECK(stats.inserted_ranges == 0);
}

int main(void) {
  test_safe_conversion();
  test_incomplete_and_stale_views_are_rejected();
  test_partial_level_conversion_is_corrupting();
  test_snapshot_and_newer_write_semantics();
  test_threshold_and_unbounded_tail();
  test_conflicting_records_are_rejected();
  test_capacity_failure_publishes_nothing();
  puts("deletion-map correctness passed: 7 suites");
  return 0;
}
