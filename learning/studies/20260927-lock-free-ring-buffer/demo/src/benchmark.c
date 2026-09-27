#include "spsc_ring.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

typedef enum {
  BENCH_MUTEX = 0,
  BENCH_SEQ_CST = 1,
  BENCH_ACQUIRE_RELEASE = 2,
  BENCH_CACHED = 3,
  BENCH_VARIANT_COUNT = 4,
} bench_variant;

typedef struct {
  spsc_ring *ring;
  spsc_producer producer;
  spsc_consumer consumer;
  size_t count;
  atomic_bool ready;
  atomic_bool start;
  atomic_bool stop;
  atomic_bool failed;
  int consumer_qos_result;
  uint64_t checksum;
  uint64_t pop_retries;
} sample_context;

typedef struct {
  double seconds;
  double transfers_per_second;
  uint64_t checksum;
  uint64_t push_retries;
  uint64_t pop_retries;
  long voluntary_context_switches;
  long involuntary_context_switches;
  long minor_faults;
  bool indexes_share_hardware_cache_line;
} sample_result;

typedef struct {
  size_t count;
  size_t capacity;
  size_t samples;
  size_t hardware_cache_line;
  int variant_filter;
  bool self_check;
} options;

typedef int (*sample_runner)(size_t count, size_t capacity,
                             size_t hardware_cache_line, sample_result *result);

static _Atomic uint64_t observable_sink = 0U;

static const char *const variant_names[BENCH_VARIANT_COUNT] = {
    "mutex",
    "seq_cst",
    "acquire_release",
    "cached",
};

static uint64_t checksum_for_count(size_t count) {
  const uint64_t n = (uint64_t)count;

  if ((n & UINT64_C(1)) == 0U) {
    return (n / UINT64_C(2)) * (n - UINT64_C(1));
  }
  return n * ((n - UINT64_C(1)) / UINT64_C(2));
}

static double seconds_between(struct timespec begin, struct timespec end) {
  const double seconds = (double)(end.tv_sec - begin.tv_sec);
  const double nanoseconds = (double)(end.tv_nsec - begin.tv_nsec) * 1.0e-9;
  return seconds + nanoseconds;
}

static bool parse_size(const char *text, size_t minimum, size_t *out) {
  char *end = NULL;
  unsigned long long value = 0U;

  errno = 0;
  value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' ||
      value > (unsigned long long)SIZE_MAX || value < minimum) {
    return false;
  }
  *out = (size_t)value;
  return true;
}

static bool parse_variant(const char *text, int *out) {
  for (int implementation = BENCH_MUTEX; implementation < BENCH_VARIANT_COUNT;
       ++implementation) {
    if (strcmp(text, variant_names[implementation]) == 0) {
      *out = implementation;
      return true;
    }
  }
  return false;
}

static int compare_double(const void *left, const void *right) {
  const double a = *(const double *)left;
  const double b = *(const double *)right;
  return (a > b) - (a < b);
}

static double median(const double *values, size_t count) {
  double *copy = malloc(count * sizeof(*copy));
  double result = 0.0;

  if (copy == NULL) {
    return 0.0;
  }
  memcpy(copy, values, count * sizeof(*copy));
  qsort(copy, count, sizeof(*copy), compare_double);
  if ((count & 1U) != 0U) {
    result = copy[count / 2U];
  } else {
    result = (copy[count / 2U - 1U] + copy[count / 2U]) / 2.0;
  }
  free(copy);
  return result;
}

static void wait_for_start(sample_context *sample) {
  while (!atomic_load_explicit(&sample->start, memory_order_acquire)) {
  }
}

static int request_benchmark_qos(void) {
#if defined(__APPLE__)
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
  return 0;
#endif
}

static bool should_stop(sample_context *sample) {
  return atomic_load_explicit(&sample->stop, memory_order_acquire);
}

static void fail_sample(sample_context *sample) {
  atomic_store_explicit(&sample->failed, true, memory_order_relaxed);
  atomic_store_explicit(&sample->stop, true, memory_order_release);
}

#define DEFINE_SAMPLE_RUNNER(name, storage_kind, push_expression,              \
                             pop_expression)                                   \
  static void *consume_##name(void *argument) {                                \
    sample_context *const sample = argument;                                   \
    uint64_t checksum = 0U;                                                    \
    uint64_t retries = 0U;                                                     \
    sample->consumer_qos_result = request_benchmark_qos();                     \
    atomic_store_explicit(&sample->ready, true, memory_order_release);         \
    wait_for_start(sample);                                                    \
    for (size_t i = 0U; i < sample->count; ++i) {                              \
      uint64_t value = 0U;                                                     \
      while (!(pop_expression)) {                                              \
        retries += 1U;                                                         \
        if ((retries & UINT64_C(1023)) == 0U && should_stop(sample)) {         \
          return NULL;                                                         \
        }                                                                      \
      }                                                                        \
      if (value != (uint64_t)i) {                                              \
        fail_sample(sample);                                                   \
        return NULL;                                                           \
      }                                                                        \
      checksum += value;                                                       \
    }                                                                          \
    sample->checksum = checksum;                                               \
    sample->pop_retries = retries;                                             \
    atomic_store_explicit(&sample->stop, true, memory_order_release);          \
    return NULL;                                                               \
  }                                                                            \
                                                                               \
  static int run_##name(size_t count, size_t capacity,                         \
                        size_t hardware_cache_line, sample_result *result) {   \
    sample_context sample = {0};                                               \
    pthread_t consumer;                                                        \
    struct timespec begin = {0};                                               \
    struct timespec end = {0};                                                 \
    struct rusage usage_begin = {0};                                           \
    struct rusage usage_end = {0};                                             \
    const uint64_t expected_checksum = checksum_for_count(count);              \
    uint64_t push_retries = 0U;                                                \
    int thread_result = 0;                                                     \
    if (spsc_ring_create(&sample.ring, capacity, storage_kind) != SPSC_OK) {   \
      return 1;                                                                \
    }                                                                          \
    sample.count = count;                                                      \
    atomic_init(&sample.ready, false);                                         \
    atomic_init(&sample.start, false);                                         \
    atomic_init(&sample.stop, false);                                          \
    atomic_init(&sample.failed, false);                                        \
    spsc_producer_bind(&sample.producer, sample.ring);                         \
    spsc_consumer_bind(&sample.consumer, sample.ring);                         \
    thread_result = pthread_create(&consumer, NULL, consume_##name, &sample);  \
    if (thread_result != 0) {                                                  \
      spsc_ring_destroy(sample.ring);                                          \
      return 1;                                                                \
    }                                                                          \
    while (!atomic_load_explicit(&sample.ready, memory_order_acquire)) {       \
    }                                                                          \
    if (sample.consumer_qos_result != 0 || request_benchmark_qos() != 0) {     \
      fail_sample(&sample);                                                    \
      atomic_store_explicit(&sample.start, true, memory_order_release);        \
      (void)pthread_join(consumer, NULL);                                      \
      spsc_ring_destroy(sample.ring);                                          \
      return 1;                                                                \
    }                                                                          \
    (void)getrusage(RUSAGE_SELF, &usage_begin);                                \
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &begin);                          \
    atomic_store_explicit(&sample.start, true, memory_order_release);          \
    for (size_t i = 0U; i < count; ++i) {                                      \
      const uint64_t value = (uint64_t)i;                                      \
      while (!(push_expression)) {                                             \
        push_retries += 1U;                                                    \
        if ((push_retries & UINT64_C(1023)) == 0U && should_stop(&sample)) {   \
          break;                                                               \
        }                                                                      \
      }                                                                        \
      if (atomic_load_explicit(&sample.failed, memory_order_relaxed)) {        \
        break;                                                                 \
      }                                                                        \
    }                                                                          \
    thread_result = pthread_join(consumer, NULL);                              \
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);                            \
    (void)getrusage(RUSAGE_SELF, &usage_end);                                  \
    if (thread_result != 0 ||                                                  \
        atomic_load_explicit(&sample.failed, memory_order_relaxed) ||          \
        sample.checksum != expected_checksum) {                                \
      spsc_ring_destroy(sample.ring);                                          \
      return 1;                                                                \
    }                                                                          \
    result->seconds = seconds_between(begin, end);                             \
    result->transfers_per_second = (double)count / result->seconds;            \
    result->checksum = sample.checksum;                                        \
    result->push_retries = push_retries;                                       \
    result->pop_retries = sample.pop_retries;                                  \
    result->voluntary_context_switches =                                       \
        usage_end.ru_nvcsw - usage_begin.ru_nvcsw;                             \
    result->involuntary_context_switches =                                     \
        usage_end.ru_nivcsw - usage_begin.ru_nivcsw;                           \
    result->minor_faults = usage_end.ru_minflt - usage_begin.ru_minflt;        \
    result->indexes_share_hardware_cache_line =                                \
        spsc_ring_indexes_share_cache_line(sample.ring, hardware_cache_line);  \
    atomic_fetch_add_explicit(&observable_sink, sample.checksum | UINT64_C(1), \
                              memory_order_relaxed);                           \
    spsc_ring_destroy(sample.ring);                                            \
    return 0;                                                                  \
  }

DEFINE_SAMPLE_RUNNER(mutex, SPSC_RING_MUTEX,
                     spsc_mutex_try_push(sample.ring, value),
                     spsc_mutex_try_pop(sample->ring, &value))

DEFINE_SAMPLE_RUNNER(seq_cst, SPSC_RING_ATOMIC,
                     spsc_seq_cst_try_push(sample.ring, value),
                     spsc_seq_cst_try_pop(sample->ring, &value))

DEFINE_SAMPLE_RUNNER(acquire_release, SPSC_RING_ATOMIC,
                     spsc_acquire_release_try_push(sample.ring, value),
                     spsc_acquire_release_try_pop(sample->ring, &value))

DEFINE_SAMPLE_RUNNER(cached, SPSC_RING_ATOMIC,
                     spsc_cached_try_push(&sample.producer, value),
                     spsc_cached_try_pop(&sample->consumer, &value))

static const sample_runner runners[BENCH_VARIANT_COUNT] = {
    run_mutex,
    run_seq_cst,
    run_acquire_release,
    run_cached,
};

static void usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--count N] [--capacity N] [--samples N] "
          "[--hardware-cache-line N] [--variant NAME] [--self-check]\n",
          program);
}

static bool parse_options(int argc, char **argv, options *out) {
  *out = (options){
      .count = 10000000U,
      .capacity = 100000U,
      .samples = 11U,
      .hardware_cache_line = 0U,
      .variant_filter = -1,
      .self_check = false,
  };
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--self-check") == 0) {
      out->self_check = true;
      out->count = 10000U;
      out->capacity = 257U;
      out->samples = 1U;
    } else if (i + 1 < argc && strcmp(argv[i], "--count") == 0) {
      if (!parse_size(argv[++i], 1U, &out->count)) {
        return false;
      }
    } else if (i + 1 < argc && strcmp(argv[i], "--capacity") == 0) {
      if (!parse_size(argv[++i], 2U, &out->capacity)) {
        return false;
      }
    } else if (i + 1 < argc && strcmp(argv[i], "--samples") == 0) {
      if (!parse_size(argv[++i], 1U, &out->samples)) {
        return false;
      }
    } else if (i + 1 < argc && strcmp(argv[i], "--hardware-cache-line") == 0) {
      if (!parse_size(argv[++i], 1U, &out->hardware_cache_line)) {
        return false;
      }
    } else if (i + 1 < argc && strcmp(argv[i], "--variant") == 0) {
      if (!parse_variant(argv[++i], &out->variant_filter)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  options selected = {0};
  double *rates = NULL;
  spsc_ring *probe = NULL;
  spsc_layout layout = {0};

  if (!parse_options(argc, argv, &selected)) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  if (selected.samples > SIZE_MAX / BENCH_VARIANT_COUNT ||
      selected.samples * BENCH_VARIANT_COUNT > SIZE_MAX / sizeof(*rates)) {
    fputs("sample count is too large\n", stderr);
    return EXIT_FAILURE;
  }
  rates = calloc(selected.samples * BENCH_VARIANT_COUNT, sizeof(*rates));
  if (rates == NULL) {
    fputs("cannot allocate sample storage\n", stderr);
    return EXIT_FAILURE;
  }
  if (spsc_ring_create(&probe, selected.capacity, SPSC_RING_ATOMIC) !=
      SPSC_OK) {
    fputs("cannot create atomic ring\n", stderr);
    free(rates);
    return EXIT_FAILURE;
  }
  spsc_ring_describe_layout(probe, &layout);
  spsc_ring_destroy(probe);

  printf("# configured_cache_line=%zu\n", layout.configured_cache_line);
  printf("# ring_alignment=%zu\n", layout.ring_alignment);
  printf("# ring_size=%zu\n", layout.ring_size);
  printf("# head_offset=%zu\n", layout.head_offset);
  printf("# tail_offset=%zu\n", layout.tail_offset);
  printf("# atomic_size_t_lock_free=%d\n",
         layout.atomic_size_t_lock_free ? 1 : 0);
  printf("# count=%zu\n", selected.count);
  printf("# capacity=%zu\n", selected.capacity);
  printf("# hardware_cache_line=%zu\n", selected.hardware_cache_line);
#if defined(__APPLE__)
  puts("# scheduler=user_interactive_qos_unpinned");
#else
  puts("# scheduler=default_unpinned");
#endif
  puts("sample,order,variant,seconds,transfers_per_second,"
       "successful_api_calls_per_second,push_retries,pop_retries,"
       "voluntary_context_switches,involuntary_context_switches,"
       "minor_faults,indexes_share_hardware_cache_line,checksum");

  for (size_t sample = 0U; sample < selected.samples; ++sample) {
    const size_t variants_to_run =
        selected.variant_filter < 0 ? BENCH_VARIANT_COUNT : 1U;
    for (size_t order = 0U; order < variants_to_run; ++order) {
      const bench_variant implementation =
          selected.variant_filter < 0
              ? (bench_variant)((sample + order) % BENCH_VARIANT_COUNT)
              : (bench_variant)selected.variant_filter;
      sample_result result = {0};

      if (runners[implementation](selected.count, selected.capacity,
                                  selected.hardware_cache_line, &result) != 0) {
        fprintf(stderr, "benchmark failed: sample=%zu variant=%s\n", sample,
                variant_names[implementation]);
        free(rates);
        return EXIT_FAILURE;
      }
      rates[(size_t)implementation * selected.samples + sample] =
          result.transfers_per_second;
      printf("%zu,%zu,%s,%.9f,%.3f,%.3f,%llu,%llu,%ld,%ld,%ld,%d,%llu\n",
             sample, order, variant_names[implementation], result.seconds,
             result.transfers_per_second, result.transfers_per_second * 2.0,
             (unsigned long long)result.push_retries,
             (unsigned long long)result.pop_retries,
             result.voluntary_context_switches,
             result.involuntary_context_switches, result.minor_faults,
             result.indexes_share_hardware_cache_line ? 1 : 0,
             (unsigned long long)result.checksum);
    }
  }

  for (bench_variant implementation = BENCH_MUTEX;
       implementation < BENCH_VARIANT_COUNT;
       implementation = (bench_variant)(implementation + 1)) {
    if (selected.variant_filter >= 0 &&
        implementation != (bench_variant)selected.variant_filter) {
      continue;
    }
    const double value = median(
        &rates[(size_t)implementation * selected.samples], selected.samples);
    printf("SUMMARY,%s,median_transfers_per_second,%.3f,"
           "median_successful_api_calls_per_second,%.3f\n",
           variant_names[implementation], value, value * 2.0);
  }
  printf("# observable_sink=%llu\n",
         (unsigned long long)atomic_load_explicit(&observable_sink,
                                                  memory_order_relaxed));
  free(rates);

  if (selected.self_check) {
    if (selected.variant_filter < 0) {
      puts("benchmark self-check: 4/4 variants passed");
    } else {
      puts("benchmark self-check: 1/1 variant passed");
    }
  }
  return EXIT_SUCCESS;
}
