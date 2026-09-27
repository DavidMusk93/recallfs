#include "spsc_ring.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
  VARIANT_MUTEX = 0,
  VARIANT_SEQ_CST = 1,
  VARIANT_ACQUIRE_RELEASE = 2,
  VARIANT_CACHED = 3,
} variant;

typedef struct {
  spsc_ring *ring;
  spsc_producer producer;
  spsc_consumer consumer;
  variant implementation;
  size_t count;
  atomic_bool start;
  atomic_bool stop;
  atomic_bool failed;
  uint64_t checksum;
} concurrent_case;

static int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
      failures += 1;                                                           \
    }                                                                          \
  } while (0)

static uint64_t item_for(size_t sequence) {
  return ((uint64_t)sequence * UINT64_C(0x9e3779b97f4a7c15)) ^
         UINT64_C(0xd1b54a32d192ed03);
}

static bool try_push(concurrent_case *test, uint64_t value) {
  switch (test->implementation) {
  case VARIANT_MUTEX:
    return spsc_mutex_try_push(test->ring, value);
  case VARIANT_SEQ_CST:
    return spsc_seq_cst_try_push(test->ring, value);
  case VARIANT_ACQUIRE_RELEASE:
    return spsc_acquire_release_try_push(test->ring, value);
  case VARIANT_CACHED:
    return spsc_cached_try_push(&test->producer, value);
  }
  return false;
}

static bool try_pop(concurrent_case *test, uint64_t *value) {
  switch (test->implementation) {
  case VARIANT_MUTEX:
    return spsc_mutex_try_pop(test->ring, value);
  case VARIANT_SEQ_CST:
    return spsc_seq_cst_try_pop(test->ring, value);
  case VARIANT_ACQUIRE_RELEASE:
    return spsc_acquire_release_try_pop(test->ring, value);
  case VARIANT_CACHED:
    return spsc_cached_try_pop(&test->consumer, value);
  }
  return false;
}

static spsc_ring_storage storage_for(variant implementation) {
  return implementation == VARIANT_MUTEX ? SPSC_RING_MUTEX : SPSC_RING_ATOMIC;
}

static void *consume_sequence(void *argument) {
  concurrent_case *const test = argument;
  uint64_t checksum = 0U;

  while (!atomic_load_explicit(&test->start, memory_order_acquire)) {
  }
  for (size_t i = 0U; i < test->count; ++i) {
    uint64_t value = 0U;
    while (!try_pop(test, &value)) {
      if (atomic_load_explicit(&test->stop, memory_order_relaxed)) {
        return NULL;
      }
    }
    if (value != item_for(i)) {
      atomic_store_explicit(&test->failed, true, memory_order_relaxed);
      atomic_store_explicit(&test->stop, true, memory_order_release);
      return NULL;
    }
    checksum += value;
  }
  test->checksum = checksum;
  atomic_store_explicit(&test->stop, true, memory_order_release);
  return NULL;
}

static void test_creation_contract(void) {
  spsc_ring *ring = (spsc_ring *)(uintptr_t)1U;

  CHECK(spsc_ring_create(NULL, 8U, SPSC_RING_MUTEX) ==
        SPSC_INVALID_ARGUMENT);
  CHECK(spsc_ring_create(&ring, 1U, SPSC_RING_MUTEX) ==
        SPSC_INVALID_ARGUMENT);
  CHECK(ring == NULL);
  CHECK(spsc_ring_create(&ring, 8U, (spsc_ring_storage)99) ==
        SPSC_INVALID_ARGUMENT);
  CHECK(ring == NULL);
  CHECK(spsc_ring_create(&ring, SIZE_MAX / sizeof(uint64_t) + 1U,
                         SPSC_RING_MUTEX) == SPSC_SIZE_OVERFLOW);
  CHECK(ring == NULL);
  spsc_ring_destroy(NULL);
}

static void test_layout_contract(void) {
  spsc_ring *ring = NULL;
  spsc_layout layout = {0};

  CHECK(spsc_ring_create(&ring, 8U, SPSC_RING_ATOMIC) == SPSC_OK);
  if (ring == NULL) {
    return;
  }
  spsc_ring_describe_layout(ring, &layout);
  CHECK(layout.configured_cache_line == SPSC_CACHE_LINE_SIZE);
  CHECK(layout.ring_alignment >= SPSC_CACHE_LINE_SIZE);
  CHECK(layout.ring_size >= layout.tail_offset + sizeof(size_t));
  CHECK(layout.tail_offset >=
        layout.head_offset + layout.configured_cache_line);
  CHECK(layout.atomic_size_t_lock_free);
  CHECK(!spsc_ring_indexes_share_cache_line(
      ring, layout.configured_cache_line));
  CHECK(!spsc_ring_indexes_share_cache_line(NULL,
                                             layout.configured_cache_line));
  CHECK(!spsc_ring_indexes_share_cache_line(ring, 0U));
  CHECK(spsc_ring_capacity(ring) == 8U);
  CHECK(spsc_ring_usable_capacity(ring) == 7U);
  spsc_ring_destroy(ring);
}

static void test_sequential_wrap(variant implementation) {
  spsc_ring *ring = NULL;
  concurrent_case test = {0};

  CHECK(spsc_ring_create(&ring, 5U, storage_for(implementation)) == SPSC_OK);
  if (ring == NULL) {
    return;
  }
  test.ring = ring;
  test.implementation = implementation;
  spsc_producer_bind(&test.producer, ring);
  spsc_consumer_bind(&test.consumer, ring);

  for (size_t round = 0U; round < 1000U; ++round) {
    for (size_t i = 0U; i < 4U; ++i) {
      CHECK(try_push(&test, item_for(round * 4U + i)));
    }
    CHECK(!try_push(&test, UINT64_MAX));
    for (size_t i = 0U; i < 4U; ++i) {
      uint64_t value = 0U;
      CHECK(try_pop(&test, &value));
      CHECK(value == item_for(round * 4U + i));
    }
    {
      uint64_t value = 0U;
      CHECK(!try_pop(&test, &value));
    }
  }
  spsc_ring_destroy(ring);
}

static void test_concurrent_fifo(variant implementation, size_t capacity,
                                 size_t count) {
  concurrent_case test = {0};
  pthread_t consumer;
  uint64_t expected_checksum = 0U;
  int create_result = 0;
  int join_result = 0;

  CHECK(spsc_ring_create(&test.ring, capacity, storage_for(implementation)) ==
        SPSC_OK);
  if (test.ring == NULL) {
    return;
  }
  test.implementation = implementation;
  test.count = count;
  atomic_init(&test.start, false);
  atomic_init(&test.stop, false);
  atomic_init(&test.failed, false);
  spsc_producer_bind(&test.producer, test.ring);
  spsc_consumer_bind(&test.consumer, test.ring);

  create_result = pthread_create(&consumer, NULL, consume_sequence, &test);
  CHECK(create_result == 0);
  if (create_result != 0) {
    spsc_ring_destroy(test.ring);
    return;
  }

  atomic_store_explicit(&test.start, true, memory_order_release);
  for (size_t i = 0U; i < count; ++i) {
    const uint64_t value = item_for(i);
    while (!try_push(&test, value)) {
      if (atomic_load_explicit(&test.stop, memory_order_acquire)) {
        break;
      }
    }
    if (atomic_load_explicit(&test.failed, memory_order_relaxed)) {
      break;
    }
    expected_checksum += value;
  }
  join_result = pthread_join(consumer, NULL);
  CHECK(join_result == 0);
  CHECK(!atomic_load_explicit(&test.failed, memory_order_relaxed));
  CHECK(test.checksum == expected_checksum);
  spsc_ring_destroy(test.ring);
}

int main(void) {
  test_creation_contract();
  test_layout_contract();

  for (variant implementation = VARIANT_MUTEX;
       implementation <= VARIANT_CACHED;
       implementation = (variant)(implementation + 1)) {
    test_sequential_wrap(implementation);
    test_concurrent_fifo(implementation, 2U, 100000U);
    test_concurrent_fifo(implementation, 17U, 500000U);
  }

  if (failures != 0) {
    fprintf(stderr, "spsc_ring_test: %d checks failed\n", failures);
    return EXIT_FAILURE;
  }
  puts("spsc_ring_test: 14/14 suites passed");
  return EXIT_SUCCESS;
}
