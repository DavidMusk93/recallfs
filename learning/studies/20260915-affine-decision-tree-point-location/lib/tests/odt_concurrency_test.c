#include "odt_test_support.h"

#include "odt_internal.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

enum {
    READER_COUNT = 8,
    PHASE_REPETITIONS = 256,
    QUERY_COUNT = 12,
};

static const odt_domain test_domain = {-10.0, -8.0, 11.0, 9.0};
static const odt_site test_sites[] = {
    {-8.0, -6.0, 101}, {-4.5, 3.0, 102}, {-1.0, -2.0, 103}, {0.0, 7.5, 104},
    {2.0, 1.0, 105},   {5.5, -5.0, 106}, {8.0, 4.0, 107},   {10.0, -1.0, 108},
};
static const odt_point query_points[QUERY_COUNT] = {
    {-8.0, -6.0}, {-4.5, 3.0},  {-1.0, -2.0},  {0.0, 7.5},  {2.0, 1.0}, {5.5, -5.0},
    {8.0, 4.0},   {10.0, -1.0}, {-10.0, -8.0}, {12.0, 0.0}, {NAN, 1.0}, {0.0, -INFINITY},
};

typedef struct query_summary {
    odt_query_result results[QUERY_COUNT];
    odt_batch_stats stats;
    uint64_t checksum;
} query_summary;

typedef struct concurrency_state {
    _Atomic(odt_generation *) published;
    odt_generation *initial_generation;
    odt_generation *replacement_generation;
    query_summary expected;
    atomic_uint ready_readers;
    atomic_uint readers_at_replacement;
    atomic_bool start;
    atomic_bool replacement_published;
    atomic_uint active_readers;
} concurrency_state;

typedef struct reader_state {
    concurrency_state *shared;
    size_t reader_index;
    uint64_t checksum;
    bool failed;
} reader_state;

static uint64_t mix_checksum(uint64_t checksum, uint64_t value) {
    checksum ^= value;
    checksum *= UINT64_C(1099511628211);
    return checksum;
}

static uint64_t summary_checksum(const query_summary *summary) {
    uint64_t checksum = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0u; index < QUERY_COUNT; ++index) {
        checksum = mix_checksum(checksum, (uint64_t)(unsigned int)summary->results[index].kind);
        checksum = mix_checksum(checksum, (uint64_t)(unsigned int)summary->results[index].status);
        checksum = mix_checksum(checksum, (uint64_t)(uint32_t)summary->results[index].region_id);
    }
    checksum = mix_checksum(checksum, summary->stats.attempted);
    checksum = mix_checksum(checksum, summary->stats.region_count);
    checksum = mix_checksum(checksum, summary->stats.outside_count);
    checksum = mix_checksum(checksum, summary->stats.failed_count);
    checksum = mix_checksum(checksum, summary->stats.comparisons);
    return mix_checksum(checksum, summary->stats.exact_fallbacks);
}

static uint64_t generation_checksum(const odt_generation *generation) {
    const unsigned char *bytes = (const unsigned char *)generation;
    uint64_t checksum = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0u; index < generation->allocation_size; ++index) {
        checksum = mix_checksum(checksum, (uint64_t)bytes[index]);
    }
    return checksum;
}

static bool summaries_equal(const query_summary *first, const query_summary *second) {
    size_t index;

    for (index = 0u; index < QUERY_COUNT; ++index) {
        if (first->results[index].kind != second->results[index].kind ||
            first->results[index].status != second->results[index].status ||
            first->results[index].region_id != second->results[index].region_id) {
            return false;
        }
    }
    return first->stats.attempted == second->stats.attempted &&
           first->stats.region_count == second->stats.region_count &&
           first->stats.outside_count == second->stats.outside_count &&
           first->stats.failed_count == second->stats.failed_count &&
           first->stats.comparisons == second->stats.comparisons &&
           first->stats.exact_fallbacks == second->stats.exact_fallbacks &&
           first->checksum == second->checksum;
}

static bool evaluate_scalar(const odt_generation *generation, query_summary *out_summary) {
    size_t index;

    memset(out_summary, 0, sizeof(*out_summary));
    for (index = 0u; index < QUERY_COUNT; ++index) {
        odt_query_stats stats = {0u, 0u};
        odt_query_result result = {ODT_RESULT_ERROR, ODT_INTERNAL_ERROR, 0};
        odt_status status = odt_query(generation, &query_points[index], &result, &stats);

        if (status == ODT_INVALID_DATA) {
            result.kind = ODT_RESULT_ERROR;
            result.status = status;
            result.region_id = 0;
        } else if (status != ODT_OK) {
            return false;
        }
        out_summary->results[index] = result;
        out_summary->stats.attempted += 1u;
        out_summary->stats.comparisons += stats.comparisons;
        out_summary->stats.exact_fallbacks += stats.exact_fallbacks;
        if (result.kind == ODT_RESULT_REGION) {
            out_summary->stats.region_count += 1u;
        } else if (result.kind == ODT_RESULT_OUTSIDE) {
            out_summary->stats.outside_count += 1u;
        } else if (result.kind == ODT_RESULT_ERROR) {
            out_summary->stats.failed_count += 1u;
        } else {
            return false;
        }
    }
    out_summary->checksum = summary_checksum(out_summary);
    return true;
}

static bool evaluate_batch(const odt_generation *generation, query_summary *out_summary) {
    odt_status status;

    memset(out_summary, 0, sizeof(*out_summary));
    status = odt_query_batch(generation, QUERY_COUNT, query_points, sizeof(odt_point),
                             out_summary->results, sizeof(odt_query_result), &out_summary->stats);
    if (status != ODT_OK) {
        return false;
    }
    out_summary->checksum = summary_checksum(out_summary);
    return true;
}

static void *reader_main(void *argument) {
    reader_state *reader = argument;
    concurrency_state *shared = reader->shared;
    uint64_t checksum = UINT64_C(1469598103934665603);
    unsigned phase;

    atomic_fetch_add_explicit(&shared->ready_readers, 1u, memory_order_release);
    while (!atomic_load_explicit(&shared->start, memory_order_acquire)) {
        sched_yield();
    }
    atomic_fetch_add_explicit(&shared->active_readers, 1u, memory_order_relaxed);
    for (phase = 0u; phase < 2u; ++phase) {
        size_t repetition;

        for (repetition = 0u; repetition < PHASE_REPETITIONS; ++repetition) {
            odt_generation *generation =
                atomic_load_explicit(&shared->published, memory_order_acquire);
            query_summary actual;
            const bool used_batch = ((reader->reader_index + repetition) & 1u) != 0u;

            if ((phase == 0u && generation != shared->initial_generation) ||
                (phase == 1u && generation != shared->replacement_generation) ||
                !(used_batch ? evaluate_batch(generation, &actual)
                             : evaluate_scalar(generation, &actual)) ||
                !summaries_equal(&actual, &shared->expected)) {
                reader->failed = true;
                if (phase == 0u) {
                    atomic_fetch_add_explicit(&shared->readers_at_replacement, 1u,
                                              memory_order_release);
                    while (!atomic_load_explicit(&shared->replacement_published,
                                                 memory_order_acquire)) {
                        sched_yield();
                    }
                }
                atomic_fetch_sub_explicit(&shared->active_readers, 1u, memory_order_relaxed);
                return NULL;
            }
            checksum = mix_checksum(checksum, actual.checksum);
        }
        if (phase == 0u) {
            atomic_fetch_add_explicit(&shared->readers_at_replacement, 1u, memory_order_release);
            while (!atomic_load_explicit(&shared->replacement_published, memory_order_acquire)) {
                sched_yield();
            }
        }
    }
    reader->checksum = checksum;
    atomic_fetch_sub_explicit(&shared->active_readers, 1u, memory_order_release);
    return NULL;
}

static void test_concurrent_queries_and_application_quiescence(void) {
    odt_test_allocator_state allocator_state = {.live = true};
    odt_allocator allocator;
    concurrency_state shared;
    reader_state readers[READER_COUNT];
    pthread_t threads[READER_COUNT];
    odt_generation *retired;
    query_summary replacement_expected;
    uint64_t initial_hash;
    uint64_t replacement_hash;
    size_t index;

    memset(&shared, 0, sizeof(shared));
    odt_test_counting_allocator_init(&allocator, &allocator_state);
    ODT_TEST_STATUS(odt_build(&test_domain, test_sites, sizeof(test_sites) / sizeof(test_sites[0]),
                              NULL, NULL, &allocator, NULL, &shared.initial_generation),
                    ODT_OK);
    ODT_TEST_STATUS(odt_build(&test_domain, test_sites, sizeof(test_sites) / sizeof(test_sites[0]),
                              NULL, NULL, &allocator, NULL, &shared.replacement_generation),
                    ODT_OK);
    ODT_TEST_CHECK(allocator_state.live_allocations == 2u);
    ODT_TEST_CHECK(evaluate_scalar(shared.initial_generation, &shared.expected));
    ODT_TEST_CHECK(evaluate_batch(shared.replacement_generation, &replacement_expected));
    ODT_TEST_CHECK(summaries_equal(&shared.expected, &replacement_expected));
    initial_hash = generation_checksum(shared.initial_generation);
    replacement_hash = generation_checksum(shared.replacement_generation);

    atomic_init(&shared.published, shared.initial_generation);
    atomic_init(&shared.ready_readers, 0u);
    atomic_init(&shared.readers_at_replacement, 0u);
    atomic_init(&shared.start, false);
    atomic_init(&shared.replacement_published, false);
    atomic_init(&shared.active_readers, 0u);
    for (index = 0u; index < READER_COUNT; ++index) {
        readers[index].shared = &shared;
        readers[index].reader_index = index;
        readers[index].checksum = 0u;
        readers[index].failed = false;
        ODT_TEST_CHECK(pthread_create(&threads[index], NULL, reader_main, &readers[index]) == 0);
    }
    while (atomic_load_explicit(&shared.ready_readers, memory_order_acquire) != READER_COUNT) {
        sched_yield();
    }
    atomic_store_explicit(&shared.start, true, memory_order_release);
    while (atomic_load_explicit(&shared.readers_at_replacement, memory_order_acquire) !=
           READER_COUNT) {
        sched_yield();
    }

    retired = atomic_exchange_explicit(&shared.published, shared.replacement_generation,
                                       memory_order_acq_rel);
    ODT_TEST_CHECK(retired == shared.initial_generation);
    ODT_TEST_CHECK(atomic_load_explicit(&shared.active_readers, memory_order_acquire) ==
                   READER_COUNT);
    atomic_store_explicit(&shared.replacement_published, true, memory_order_release);

    for (index = 0u; index < READER_COUNT; ++index) {
        ODT_TEST_CHECK(pthread_join(threads[index], NULL) == 0);
        ODT_TEST_CHECK(!readers[index].failed);
        ODT_TEST_CHECK(readers[index].checksum == readers[0].checksum);
    }
    ODT_TEST_CHECK(atomic_load_explicit(&shared.active_readers, memory_order_acquire) == 0u);
    ODT_TEST_CHECK(generation_checksum(retired) == initial_hash);
    ODT_TEST_CHECK(generation_checksum(shared.replacement_generation) == replacement_hash);

    odt_generation_destroy(retired);
    ODT_TEST_CHECK(allocator_state.live_allocations == 1u);
    odt_generation_destroy(shared.replacement_generation);
    ODT_TEST_CHECK(allocator_state.live_allocations == 0u);
}

int main(void) {
    test_concurrent_queries_and_application_quiescence();
    return EXIT_SUCCESS;
}
