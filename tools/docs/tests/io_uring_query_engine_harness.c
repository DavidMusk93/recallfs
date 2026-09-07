#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static bool near(double actual, double expected)
{
    return fabs(actual - expected) < 0.000001;
}

static void test_direct_read_planning(void)
{
    struct direct_read_plan plan;
    uint64_t rounded;

    assert(round_up_u64(8192, 4096, &rounded));
    assert(rounded == 8192);
    assert(plan_direct_read(5000, 1000, 10000, 4096, &plan));
    assert(plan.io_offset == 4096);
    assert(plan.io_length == 4096);
    assert(plan.payload_offset == 904);
    assert(plan.payload_length == 1000);
    assert(plan.minimum_completion == 1904);

    assert(plan_direct_read(9000, 1000, 10000, 4096, &plan));
    assert(plan.io_offset == 8192);
    assert(plan.io_length == 4096);
    assert(plan.minimum_completion == 1808);

    assert(!plan_direct_read(0, 4096, 4096, 0, &plan));
    assert(!plan_direct_read(0, 4096, 4096, 4096, NULL));
    assert(!plan_direct_read(9000, 1001, 10000, 4096, &plan));
    assert(!plan_direct_read(0, 0, 10000, 4096, &plan));
    assert(!plan_direct_read(UINT64_MAX - 10, 20, UINT64_MAX, 4096,
                             &plan));
    assert(!plan_direct_read(0, (uint64_t)UINT32_MAX + 1,
                             (uint64_t)UINT32_MAX + 1, 4096, &plan));
    assert(!round_up_u64(UINT64_MAX - 1, 4096, &rounded));
}

static void test_chunking_and_window(void)
{
    struct chunk_cursor cursor;
    struct io_chunk chunk;
    struct io_window window = {.limit = 2, .in_flight = 0};
    uint32_t count = 0;

    assert(chunk_cursor_init(&cursor, 8192, 16384, 4096, 512));
    while (next_chunk(&cursor, &chunk)) {
        assert(chunk.offset == 8192 + (uint64_t)count * 4096);
        assert(chunk.length == 4096);
        count++;
    }
    assert(count == 4);
    assert(!chunk_cursor_init(&cursor, 1, 4096, 4096, 512));
    assert(!chunk_cursor_init(&cursor, 0, 4097, 4096, 512));

    assert(chunk_cursor_init(&cursor, 0, 4608, 4096, 512));
    assert(next_chunk(&cursor, &chunk));
    assert(chunk.offset == 0 && chunk.length == 4096);
    assert(next_chunk(&cursor, &chunk));
    assert(chunk.offset == 4096 && chunk.length == 512);
    assert(!next_chunk(&cursor, &chunk));

    assert(reserve_io(&window));
    assert(reserve_io(&window));
    assert(!reserve_io(&window));
    assert(release_io(&window));
    assert(window.in_flight == 1);
    assert(release_io(&window));
    assert(!release_io(&window));
    assert(!reserve_io(NULL));
    window.limit = 0;
    assert(!reserve_io(&window));
}

static void test_completion_state_machine(void)
{
    struct direct_read_plan tail_plan;
    struct io_slot slot = {
        .index = 7,
        .generation = 0,
        .state = SLOT_FREE
    };
    uint64_t failed_token = begin_io(&slot, 4096, 4096, 4096, 4096);
    uint64_t active_token;

    assert(failed_token != 0);
    assert(apply_read_cqe(&slot, failed_token, -EIO) == CQE_FAILED);
    assert(slot.error_number == EIO);

    slot.state = SLOT_FREE;
    active_token = begin_io(&slot, 8192, 8192, 4096, 4096);
    assert(active_token != failed_token);
    assert(apply_read_cqe(&slot, failed_token, 4096) == CQE_STALE);
    assert(apply_read_cqe(&slot, active_token, 4096) == CQE_RESUBMIT);
    assert(slot.completed_bytes == 4096);
    assert(apply_read_cqe(&slot, active_token, 4096) == CQE_COMPLETE);
    assert(slot.state == SLOT_COMPLETE);
    assert(slot.completed_bytes == slot.submitted_bytes);
    assert(apply_read_cqe(&slot, active_token, 1) == CQE_STALE);

    slot.state = SLOT_FREE;
    active_token = begin_io(&slot, 4096, 4096, 4096, 4096);
    assert(apply_read_cqe(&slot, active_token, 4096) == CQE_COMPLETE);

    assert(plan_direct_read(9000, 1000, 10000, 4096, &tail_plan));
    slot.state = SLOT_FREE;
    active_token = begin_io(&slot, tail_plan.io_length,
                            tail_plan.minimum_completion, 4096, 4096);
    assert(active_token != 0);
    assert(slot.submitted_bytes == 4096);
    assert(slot.minimum_completion == 1808);
    assert(apply_read_cqe(&slot, active_token, 1808) == CQE_COMPLETE);
    assert(slot.completed_bytes == tail_plan.minimum_completion);
    assert(slot.completed_bytes < slot.submitted_bytes);

    slot.state = SLOT_FREE;
    active_token = begin_io(&slot, 4096, 4096, 4096, 4096);
    assert(apply_read_cqe(&slot, active_token, 1024) == CQE_FAILED);
    assert(slot.error_number == EIO);
    assert(slot.completed_bytes == 0);

    slot.state = SLOT_FREE;
    active_token = begin_io(&slot, 4096, 4096, 4096, 4096);
    assert(apply_read_cqe(&slot, active_token, 4097) == CQE_FAILED);
    assert(slot.error_number == EOVERFLOW);

    slot.state = SLOT_FREE;
    active_token = begin_io(&slot, 4096, 4096, 4096, 4096);
    assert(apply_read_cqe(&slot, active_token, 0) == CQE_FAILED);
    assert(slot.error_number == EIO);

    slot.state = SLOT_FREE;
    assert(begin_io(NULL, 4096, 4096, 4096, 4096) == 0);
    assert(begin_io(&slot, 0, 4096, 4096, 4096) == 0);
    assert(begin_io(&slot, 4096, 0, 4096, 4096) == 0);
    assert(begin_io(&slot, 4096, 4097, 4096, 4096) == 0);
    assert(begin_io(&slot, 4096, 4096, 0, 4096) == 0);
    assert(begin_io(&slot, 4096, 4096, 4096, 0) == 0);
    assert(begin_io(&slot, 4097, 4096, 4096, 4096) == 0);
    assert(slot.state == SLOT_FREE);

    active_token = begin_io(&slot, 8192, 8192, 4096, 8192);
    assert(active_token != 0);
    assert(apply_read_cqe(&slot, active_token, 4096) == CQE_FAILED);
    assert(slot.error_number == EIO);
    assert(slot.state == SLOT_FAILED);
}

static void test_buffer_lifecycle(void)
{
    static const bool allowed[BUFFER_STATE_COUNT][BUFFER_STATE_COUNT] = {
        [BUFFER_FREE][BUFFER_PREWARMING] = true,
        [BUFFER_PREWARMING][BUFFER_READY_FOR_IO] = true,
        [BUFFER_READY_FOR_IO][BUFFER_IN_IO] = true,
        [BUFFER_IN_IO][BUFFER_DECODING] = true,
        [BUFFER_DECODING][BUFFER_FREE] = true
    };
    enum buffer_state current;
    enum buffer_state next;

    for (current = BUFFER_FREE; current < BUFFER_STATE_COUNT; current++) {
        assert(buffer_may_be_reused(current) ==
               (current == BUFFER_FREE));
        for (next = BUFFER_FREE; next < BUFFER_STATE_COUNT; next++) {
            enum buffer_state state = current;
            bool transitioned = transition_buffer(&state, next);

            assert(transitioned == allowed[current][next]);
            assert(state == (transitioned ? next : current));
        }
    }

    current = BUFFER_STATE_COUNT;
    assert(!transition_buffer(&current, BUFFER_FREE));
    current = BUFFER_FREE;
    assert(!transition_buffer(&current, BUFFER_STATE_COUNT));
    assert(!transition_buffer(NULL, BUFFER_PREWARMING));
    assert(!buffer_may_be_reused(BUFFER_STATE_COUNT));
}

static void test_fair_scheduling(void)
{
    struct fair_queue queue = {
        .query = {
            {.pending_chunks = 2},
            {.pending_chunks = 1},
            {.pending_chunks = 0}
        },
        .count = 3,
        .next_index = 0
    };

    assert(pick_next_query(&queue) == 0);
    assert(pick_next_query(&queue) == 1);
    assert(pick_next_query(&queue) == 0);
    assert(pick_next_query(&queue) == -1);
}

static void test_cache_budget(void)
{
    struct memory_budget budget = {
        .physical_bytes = 100,
        .process_base_bytes = 10,
        .ring_bytes = 20,
        .l1_bytes = 20,
        .l2_bytes = 20,
        .decode_bytes = 10,
        .required_headroom_bytes = 20
    };
    uint64_t committed;

    assert(cache_budget_fits(&budget, &committed));
    assert(committed == 100);
    assert(near(cache_hit_rate(75, 25), 0.75));
    assert(near(cache_hit_rate(0, 0), 0.0));

    budget.l2_bytes = 21;
    assert(!cache_budget_fits(&budget, &committed));
    budget.l2_bytes = UINT64_MAX;
    assert(!cache_budget_fits(&budget, &committed));
}

static void test_metrics_and_identity(void)
{
    struct benchmark_identity baseline = {
        .query_mix_id = "query-mix:v1:uniform-11d",
        .dataset_layout_id = "dataset-layout:v3:arrow-columnar",
        .host_storage_profile_id = "host-storage:v2:32nvme-raid0",
        .io_path = IO_PATH_MMAP,
        .device_count = 32,
        .query_concurrency = 12,
        .pod_count = 1,
        .queue_depth = 64,
        .dataset_bytes = 13ULL * 1024 * 1024 * 1024,
        .cache_bytes = 450ULL * 1024 * 1024 * 1024,
        .huge_tlb = true,
        .native_process = false
    };
    struct benchmark_identity candidate = baseline;

    assert(near(reduction_fraction(128957.0, 3647.0),
                0.971718966632));
    assert(near(reduction_fraction(118.0, 48.3),
                0.590677966102));
    assert(near(gib_per_second(20ULL * 1024 * 1024 * 1024,
                               1000000000ULL),
                20.0));

    candidate.io_path = IO_PATH_URING_DIRECT;
    assert(same_workload(&baseline, &candidate));
    assert(isolates_io_path(&baseline, &candidate));

    candidate = baseline;
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.io_path = IO_PATH_BUFFERED_READ;
    assert(isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.query_mix_id = "query-mix:v2:skewed-11d";
    assert(!same_workload(&baseline, &candidate));
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.dataset_layout_id = "dataset-layout:v4:repartitioned";
    assert(!same_workload(&baseline, &candidate));
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.host_storage_profile_id = "host-storage:v3:2nvme-lvm";
    assert(same_workload(&baseline, &candidate));
    assert(!isolates_io_path(&baseline, &candidate));

    candidate = baseline;
    candidate.query_mix_id = "";
    assert(!same_workload(&baseline, &candidate));
    candidate = baseline;
    candidate.dataset_layout_id = NULL;
    assert(!same_workload(&baseline, &candidate));
    candidate = baseline;
    candidate.host_storage_profile_id = "";
    assert(!isolates_io_path(&baseline, &candidate));

    candidate = baseline;
    candidate.query_concurrency++;
    assert(!same_workload(&baseline, &candidate));
    candidate = baseline;
    candidate.dataset_bytes++;
    assert(!same_workload(&baseline, &candidate));
    candidate = baseline;
    candidate.device_count--;
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.pod_count = 4;
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.queue_depth++;
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.cache_bytes++;
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.huge_tlb = false;
    assert(!isolates_io_path(&baseline, &candidate));
    candidate = baseline;
    candidate.native_process = true;
    assert(!isolates_io_path(&baseline, &candidate));
}

static void assert_rollout_rejected(const struct rollout_sample *baseline,
                                    const struct rollout_sample *candidate)
{
    assert(!rollout_is_better(baseline, candidate, 650, 0.75));
}

struct fake_populate_context {
    enum prewarm_result result;
    size_t calls;
    void *address;
    size_t length;
};

static enum prewarm_result fake_populate_write(void *address, size_t length,
                                                void *opaque)
{
    struct fake_populate_context *context = opaque;

    assert(context != NULL);
    context->calls++;
    context->address = address;
    context->length = length;
    return context->result;
}

static void test_rollout_and_prewarm_guard(void)
{
    const struct rollout_sample baseline = {
        .p95_ms = 118000,
        .p99_ms = 154000,
        .error_rate = 0.01,
        .cache_hit_rate = 0.0,
        .rss_bytes = 700,
        .minor_faults = 1000000,
        .major_faults = 128957,
        .completed_samples = 1000,
        .telemetry_complete = true
    };
    struct rollout_sample candidate = {
        .p95_ms = 48300,
        .p99_ms = 92500,
        .error_rate = 0.001,
        .cache_hit_rate = 0.76,
        .rss_bytes = 600,
        .minor_faults = 8600000,
        .major_faults = 3647,
        .completed_samples = 1000,
        .telemetry_complete = true
    };
    struct rollout_sample invalid;
    struct fake_populate_context context;
    unsigned char page = 0;

    assert(valid_rollout_sample(&baseline));
    assert(valid_rollout_sample(&candidate));
    assert(rollout_is_better(&baseline, &candidate, 650, 0.75));

    invalid = candidate;
    invalid.p95_ms = baseline.p95_ms;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.p99_ms = baseline.p99_ms + 1.0;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.error_rate = baseline.error_rate + 0.001;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    candidate.cache_hit_rate = 0.70;
    assert_rollout_rejected(&baseline, &candidate);
    candidate.cache_hit_rate = 0.76;
    invalid = candidate;
    invalid.rss_bytes = 651;
    assert_rollout_rejected(&baseline, &invalid);

    invalid = candidate;
    invalid.telemetry_complete = false;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.completed_samples = 0;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.p95_ms = 0.0;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.p95_ms = NAN;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.p99_ms = INFINITY;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.p99_ms = invalid.p95_ms - 1.0;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.error_rate = -0.001;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.error_rate = 1.001;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.error_rate = NAN;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.cache_hit_rate = -0.001;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.cache_hit_rate = 1.001;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.cache_hit_rate = NAN;
    assert_rollout_rejected(&baseline, &invalid);
    invalid = candidate;
    invalid.rss_bytes = 0;
    assert_rollout_rejected(&baseline, &invalid);

    invalid = baseline;
    invalid.completed_samples = 0;
    assert_rollout_rejected(&invalid, &candidate);
    assert(!rollout_is_better(NULL, &candidate, 650, 0.75));
    assert(!rollout_is_better(&baseline, NULL, 650, 0.75));
    assert(!rollout_is_better(&baseline, &candidate, 0, 0.75));
    assert(!rollout_is_better(&baseline, &candidate, 650, -0.01));
    assert(!rollout_is_better(&baseline, &candidate, 650, 1.01));
    assert(!rollout_is_better(&baseline, &candidate, 650, NAN));

    context = (struct fake_populate_context) {
        .result = PREWARM_OK
    };
    assert(prewarm_for_write_with(&page, sizeof(page), fake_populate_write,
                                  &context) == PREWARM_OK);
    assert(context.calls == 1);
    assert(context.address == &page);
    assert(context.length == sizeof(page));

    context = (struct fake_populate_context) {
        .result = PREWARM_FAILED
    };
    assert(prewarm_for_write_with(&page, sizeof(page), fake_populate_write,
                                  &context) == PREWARM_FAILED);
    assert(context.calls == 1);
    context = (struct fake_populate_context) {
        .result = PREWARM_UNSUPPORTED
    };
    assert(prewarm_for_write_with(&page, sizeof(page), fake_populate_write,
                                  &context) == PREWARM_UNSUPPORTED);
    assert(context.calls == 1);
    context = (struct fake_populate_context) {
        .result = (enum prewarm_result)99
    };
    assert(prewarm_for_write_with(&page, sizeof(page), fake_populate_write,
                                  &context) == PREWARM_FAILED);
    assert(context.calls == 1);

    assert(prewarm_for_write_with(&page, sizeof(page), NULL, NULL) ==
           PREWARM_UNSUPPORTED);
    context.calls = 0;
    assert(prewarm_for_write_with(NULL, sizeof(page), fake_populate_write,
                                  &context) == PREWARM_FAILED);
    assert(prewarm_for_write_with(&page, 0, fake_populate_write,
                                  &context) == PREWARM_FAILED);
    assert(context.calls == 0);
    assert(prewarm_for_write(NULL, 4096) == PREWARM_FAILED);
}

int main(void)
{
    typedef void (*test_function)(void);
    const test_function tests[] = {
        test_direct_read_planning,
        test_chunking_and_window,
        test_completion_state_machine,
        test_buffer_lifecycle,
        test_fair_scheduling,
        test_cache_budget,
        test_metrics_and_identity,
        test_rollout_and_prewarm_guard
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++)
        tests[index]();
    printf("FIL-C tests passed: %zu suites\n",
           sizeof(tests) / sizeof(tests[0]));
    return 0;
}
