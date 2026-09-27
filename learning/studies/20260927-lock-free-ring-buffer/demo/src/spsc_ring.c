#define _POSIX_C_SOURCE 200112L

#include "spsc_ring.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

_Static_assert(SPSC_CACHE_LINE_SIZE >= sizeof(atomic_size_t),
               "cache line must hold an atomic index");
_Static_assert((SPSC_CACHE_LINE_SIZE & (SPSC_CACHE_LINE_SIZE - 1U)) == 0U,
               "cache line must be a power of two");
_Static_assert((SPSC_CACHE_LINE_SIZE % sizeof(void *)) == 0U,
               "cache line must satisfy posix_memalign");

struct spsc_ring {
  uint64_t *slots;
  size_t capacity;
  spsc_ring_storage storage;
  pthread_mutex_t mutex;
  size_t mutex_head;
  size_t mutex_tail;
  alignas(SPSC_CACHE_LINE_SIZE) atomic_size_t atomic_head;
  alignas(SPSC_CACHE_LINE_SIZE) atomic_size_t atomic_tail;
};

static size_t next_index(const spsc_ring *ring, size_t index) {
  const size_t next = index + 1U;
  return next == ring->capacity ? 0U : next;
}

static size_t load_head_quiescent(spsc_ring *ring) {
  if (ring->storage == SPSC_RING_MUTEX) {
    return ring->mutex_head;
  }
  return atomic_load_explicit(&ring->atomic_head, memory_order_relaxed);
}

static size_t load_tail_quiescent(spsc_ring *ring) {
  if (ring->storage == SPSC_RING_MUTEX) {
    return ring->mutex_tail;
  }
  return atomic_load_explicit(&ring->atomic_tail, memory_order_relaxed);
}

spsc_status spsc_ring_create(spsc_ring **out, size_t capacity,
                             spsc_ring_storage storage) {
  spsc_ring *ring = NULL;

  if (out == NULL) {
    return SPSC_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (capacity < 2U ||
      (storage != SPSC_RING_MUTEX && storage != SPSC_RING_ATOMIC)) {
    return SPSC_INVALID_ARGUMENT;
  }
  if (capacity > SIZE_MAX / sizeof(*ring->slots)) {
    return SPSC_SIZE_OVERFLOW;
  }
  if (posix_memalign((void **)&ring, SPSC_CACHE_LINE_SIZE, sizeof(*ring)) != 0) {
    return SPSC_ALLOCATION_FAILED;
  }

  ring->slots = malloc(capacity * sizeof(*ring->slots));
  if (ring->slots == NULL) {
    free(ring);
    return SPSC_ALLOCATION_FAILED;
  }
  ring->capacity = capacity;
  ring->storage = storage;
  ring->mutex_head = 0U;
  ring->mutex_tail = 0U;
  atomic_init(&ring->atomic_head, 0U);
  atomic_init(&ring->atomic_tail, 0U);

  if (storage == SPSC_RING_ATOMIC &&
      (!atomic_is_lock_free(&ring->atomic_head) ||
       !atomic_is_lock_free(&ring->atomic_tail))) {
    free(ring->slots);
    free(ring);
    return SPSC_ATOMIC_NOT_LOCK_FREE;
  }
  if (storage == SPSC_RING_MUTEX &&
      pthread_mutex_init(&ring->mutex, NULL) != 0) {
    free(ring->slots);
    free(ring);
    return SPSC_MUTEX_FAILED;
  }

  *out = ring;
  return SPSC_OK;
}

void spsc_ring_destroy(spsc_ring *ring) {
  if (ring == NULL) {
    return;
  }
  if (ring->storage == SPSC_RING_MUTEX) {
    (void)pthread_mutex_destroy(&ring->mutex);
  }
  free(ring->slots);
  free(ring);
}

size_t spsc_ring_capacity(const spsc_ring *ring) {
  return ring == NULL ? 0U : ring->capacity;
}

size_t spsc_ring_usable_capacity(const spsc_ring *ring) {
  return ring == NULL ? 0U : ring->capacity - 1U;
}

void spsc_ring_describe_layout(const spsc_ring *ring, spsc_layout *out) {
  if (ring == NULL || out == NULL) {
    return;
  }
  out->configured_cache_line = SPSC_CACHE_LINE_SIZE;
  out->ring_alignment = alignof(spsc_ring);
  out->ring_size = sizeof(*ring);
  out->head_offset =
      (size_t)((const unsigned char *)&ring->atomic_head -
               (const unsigned char *)ring);
  out->tail_offset =
      (size_t)((const unsigned char *)&ring->atomic_tail -
               (const unsigned char *)ring);
  out->atomic_size_t_lock_free =
      atomic_is_lock_free(&ring->atomic_head) &&
      atomic_is_lock_free(&ring->atomic_tail);
}

bool spsc_ring_indexes_share_cache_line(const spsc_ring *ring,
                                        size_t cache_line_size) {
  uintptr_t head = 0U;
  uintptr_t tail = 0U;

  if (ring == NULL || cache_line_size == 0U) {
    return false;
  }
  head = (uintptr_t)&ring->atomic_head;
  tail = (uintptr_t)&ring->atomic_tail;
  return head / cache_line_size == tail / cache_line_size;
}

void spsc_producer_bind(spsc_producer *producer, spsc_ring *ring) {
  if (producer == NULL) {
    return;
  }
  producer->ring = ring;
  producer->cached_tail = ring == NULL ? 0U : load_tail_quiescent(ring);
}

void spsc_consumer_bind(spsc_consumer *consumer, spsc_ring *ring) {
  if (consumer == NULL) {
    return;
  }
  consumer->ring = ring;
  consumer->cached_head = ring == NULL ? 0U : load_head_quiescent(ring);
}

bool spsc_mutex_try_push(spsc_ring *ring, uint64_t value) {
  size_t next = 0U;

  (void)pthread_mutex_lock(&ring->mutex);
  next = next_index(ring, ring->mutex_head);
  if (next == ring->mutex_tail) {
    (void)pthread_mutex_unlock(&ring->mutex);
    return false;
  }
  ring->slots[ring->mutex_head] = value;
  ring->mutex_head = next;
  (void)pthread_mutex_unlock(&ring->mutex);
  return true;
}

bool spsc_mutex_try_pop(spsc_ring *ring, uint64_t *value) {
  (void)pthread_mutex_lock(&ring->mutex);
  if (ring->mutex_tail == ring->mutex_head) {
    (void)pthread_mutex_unlock(&ring->mutex);
    return false;
  }
  *value = ring->slots[ring->mutex_tail];
  ring->mutex_tail = next_index(ring, ring->mutex_tail);
  (void)pthread_mutex_unlock(&ring->mutex);
  return true;
}

bool spsc_seq_cst_try_push(spsc_ring *ring, uint64_t value) {
  const size_t head =
      atomic_load_explicit(&ring->atomic_head, memory_order_seq_cst);
  const size_t next = next_index(ring, head);

  if (next ==
      atomic_load_explicit(&ring->atomic_tail, memory_order_seq_cst)) {
    return false;
  }
  ring->slots[head] = value;
  atomic_store_explicit(&ring->atomic_head, next, memory_order_seq_cst);
  return true;
}

bool spsc_seq_cst_try_pop(spsc_ring *ring, uint64_t *value) {
  const size_t tail =
      atomic_load_explicit(&ring->atomic_tail, memory_order_seq_cst);

  if (tail ==
      atomic_load_explicit(&ring->atomic_head, memory_order_seq_cst)) {
    return false;
  }
  *value = ring->slots[tail];
  atomic_store_explicit(&ring->atomic_tail, next_index(ring, tail),
                        memory_order_seq_cst);
  return true;
}

bool spsc_acquire_release_try_push(spsc_ring *ring, uint64_t value) {
  const size_t head =
      atomic_load_explicit(&ring->atomic_head, memory_order_relaxed);
  const size_t next = next_index(ring, head);

  if (next == atomic_load_explicit(&ring->atomic_tail, memory_order_acquire)) {
    return false;
  }
  ring->slots[head] = value;
  atomic_store_explicit(&ring->atomic_head, next, memory_order_release);
  return true;
}

bool spsc_acquire_release_try_pop(spsc_ring *ring, uint64_t *value) {
  const size_t tail =
      atomic_load_explicit(&ring->atomic_tail, memory_order_relaxed);

  if (tail == atomic_load_explicit(&ring->atomic_head, memory_order_acquire)) {
    return false;
  }
  *value = ring->slots[tail];
  atomic_store_explicit(&ring->atomic_tail, next_index(ring, tail),
                        memory_order_release);
  return true;
}

bool spsc_cached_try_push(spsc_producer *producer, uint64_t value) {
  spsc_ring *const ring = producer->ring;
  const size_t head =
      atomic_load_explicit(&ring->atomic_head, memory_order_relaxed);
  const size_t next = next_index(ring, head);

  if (next == producer->cached_tail) {
    producer->cached_tail =
        atomic_load_explicit(&ring->atomic_tail, memory_order_acquire);
    if (next == producer->cached_tail) {
      return false;
    }
  }
  ring->slots[head] = value;
  atomic_store_explicit(&ring->atomic_head, next, memory_order_release);
  return true;
}

bool spsc_cached_try_pop(spsc_consumer *consumer, uint64_t *value) {
  spsc_ring *const ring = consumer->ring;
  const size_t tail =
      atomic_load_explicit(&ring->atomic_tail, memory_order_relaxed);

  if (tail == consumer->cached_head) {
    consumer->cached_head =
        atomic_load_explicit(&ring->atomic_head, memory_order_acquire);
    if (tail == consumer->cached_head) {
      return false;
    }
  }
  *value = ring->slots[tail];
  atomic_store_explicit(&ring->atomic_tail, next_index(ring, tail),
                        memory_order_release);
  return true;
}
