#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int persist_step;

bool persist_wal_through(lsn_t lsn)
{
    assert(lsn == 77);
    assert(persist_step++ == 0);
    return true;
}

bool append_page(const struct page_image *page, disk_off_t *new_off)
{
    assert(page->len == 128);
    assert(persist_step++ == 1);
    *new_off = 8192;
    return true;
}

bool persist_page_range(disk_off_t off, uint32_t len)
{
    assert(off == 8192);
    assert(len == 128);
    assert(persist_step++ == 2);
    return true;
}

bool append_mapping_wal(const struct map_update *u, lsn_t *record_lsn)
{
    assert(u->pid == 9);
    assert(u->old_off == 4096);
    assert(u->new_off == 8192);
    assert(persist_step++ == 3);
    *record_lsn = 88;
    return true;
}

bool persist_mapping_wal(lsn_t record_lsn)
{
    assert(record_lsn == 88);
    assert(persist_step++ == 4);
    return true;
}

void publish_mapping(const struct map_update *u)
{
    assert(u->new_off == 8192);
    assert(persist_step++ == 5);
}

void retire_old_extent_after_epoch(disk_off_t old_off)
{
    assert(old_off == 4096);
    assert(persist_step++ == 6);
}

static bool profile_matches(const struct device_profile *p,
                            const char *model,
                            const char *firmware,
                            uint64_t namespace_bytes)
{
    return strcmp(p->model, model) == 0 &&
           strcmp(p->firmware, firmware) == 0 &&
           p->namespace_bytes == namespace_bytes;
}

static bool near(double actual, double expected)
{
    return fabs(actual - expected) < 0.000001;
}

static void test_waf(void)
{
    struct write_counters counters = {
        .user_page_bytes = 100,
        .host_data_bytes = 200,
        .nand_bytes = 472,
        .wal_bytes = 25,
        .operations = 4,
    };
    struct waf result;

    assert(compute_page_waf(&counters, &result));
    assert(near(result.db, 2.0));
    assert(near(result.ssd, 2.36));
    assert(near(result.total, 4.72));
    assert(near(total_waf(2.0, 2.36), 4.72));
    assert(near(expected_physical, 1103.7));
}

static void test_persist_order(void)
{
    const unsigned char data[128] = {0};
    const struct page_image page = {
        .data = data,
        .len = sizeof(data),
        .page_lsn = 77,
    };

    persist_step = 0;
    assert(flush_page_out_of_place(9, 4096, &page));
    assert(persist_step == 7);
}

static void test_page_packing(void)
{
    struct slot slots[MAX_SLOTS] = {{0}};
    size_t count = 0;
    struct packed_ref first;
    struct packed_ref second;
    struct packed_ref third;

    assert(pack_one(slots, &count, 3000, &first) == 0);
    assert(pack_one(slots, &count, 1000, &second) == 0);
    assert(pack_one(slots, &count, 200, &third) == 0);
    assert(count == 2);
    assert(first.slot_index == 0 && first.offset_in_slot == 0);
    assert(second.slot_index == 0 && second.offset_in_slot == 3000);
    assert(third.slot_index == 1 && third.offset_in_slot == 0);
    assert(slot_disk_offset(8192, 2) == 16384);
    assert(read_offset_for_page(16384 + 123) == 16384);
}

static void test_deathtime_and_gc(void)
{
    struct death_history history = {0};

    record_persist(&history, 10, false);
    record_persist(&history, 20, false);
    record_persist(&history, 40, false);
    record_persist(&history, 70, false);
    record_persist(&history, 90, true);
    assert(history.count == 4);
    assert(estimate_death_lsn(&history, 100) == 120);
    assert(near(gc_waf(0.75), 4.0));

    struct zone zones[] = {
        {.id = 1, .average_edt = 80, .free_bytes = 4096, .state = 1},
        {.id = 2, .average_edt = 130, .free_bytes = 4096, .state = 1},
        {.id = 3, .average_edt = 109, .free_bytes = 0, .state = 1},
    };
    assert(choose_zone(zones, 3, 110, 4096)->id == 2);
}

static void test_device_layout(void)
{
    const struct device_caps caps = {
        .mode = SSD_FDP,
        .fdp_ru_bytes = 8496ULL * 1024 * 1024,
        .fdp_ruh_count = 4,
    };
    struct db_layout layout;

    assert(make_layout(&caps, &layout));
    assert(layout.zone_bytes == caps.fdp_ru_bytes);
    assert(layout.max_open_zones == 4);
    assert(layout.use_placement_hints);
    assert(!layout.use_nowa);
    assert(placement_id(5, 4) == 1);
    assert(fdp_layout_is_valid(caps.fdp_ru_bytes,
                               caps.fdp_ru_bytes, 4, 4));
    assert(!fdp_layout_is_valid(caps.fdp_ru_bytes,
                                caps.fdp_ru_bytes, 5, 4));

    struct active_group group = {
        .zone_id = {10, 11, 12},
        .rewrite_generation = {3, 1, 2},
        .count = 3,
        .all_zones_full = false,
    };
    assert(!may_open_next_group(&group));
    assert(find_under_rewritten_zone(&group) == 1);

    const struct device_profile profile = {
        .model = "ExampleSSD",
        .firmware = "1.2.3",
        .namespace_bytes = 1000,
    };
    assert(profile_matches(&profile, "ExampleSSD", "1.2.3", 1000));
    assert(!profile_matches(&profile, "ExampleSSD", "1.2.4", 1000));
}

static void test_resource_and_feature_guards(void)
{
    struct frame_budget budget;
    atomic_init(&budget.clean_frames, 5);
    budget.gc_reserve = 2;
    assert(gc_may_pin_clean_frame(&budget));
    assert(atomic_load(&budget.clean_frames) == 4);
    assert(!checkpoint_should_preclean(&budget));

    const struct run_state ready = {
        .device_capacity_bytes = 100,
        .cumulative_host_bytes = 400,
        .stable_window_seconds = 3600,
        .recent_ssd_waf_cv = 0.04,
    };
    assert(reached_measurement_state(&ready));

    struct run_state not_ready = ready;
    not_ready.cumulative_host_bytes = 399;
    assert(!reached_measurement_state(&not_ready));

    const struct storage_features valid = {
        .out_of_place = true,
        .page_compression = true,
        .page_packing = true,
        .gdt_placement = true,
        .gdt_gc = true,
        .fdp = true,
    };
    assert(valid_features(&valid));

    struct storage_features invalid = valid;
    invalid.out_of_place = false;
    assert(!valid_features(&invalid));
}

int main(void)
{
    test_waf();
    test_persist_order();
    test_page_packing();
    test_deathtime_and_gc();
    test_device_layout();
    test_resource_and_feature_guards();
    puts("FIL-C tests passed: 6 suites");
    return 0;
}
