#ifndef RECALLFS_SPSC_RING_H
#define RECALLFS_SPSC_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SPSC_CACHE_LINE_SIZE
#define SPSC_CACHE_LINE_SIZE 64
#endif

typedef enum {
  SPSC_RING_MUTEX = 0,
  SPSC_RING_ATOMIC = 1,
} spsc_ring_storage;

typedef enum {
  SPSC_OK = 0,
  SPSC_INVALID_ARGUMENT = 1,
  SPSC_SIZE_OVERFLOW = 2,
  SPSC_ALLOCATION_FAILED = 3,
  SPSC_MUTEX_FAILED = 4,
  SPSC_ATOMIC_NOT_LOCK_FREE = 5,
} spsc_status;

typedef struct spsc_ring spsc_ring;

typedef struct {
  spsc_ring *ring;
  size_t cached_tail;
} spsc_producer;

typedef struct {
  spsc_ring *ring;
  size_t cached_head;
} spsc_consumer;

typedef struct {
  size_t configured_cache_line;
  size_t ring_alignment;
  size_t ring_size;
  size_t head_offset;
  size_t tail_offset;
  bool atomic_size_t_lock_free;
} spsc_layout;

/*
 * The ring stores uint64_t values and reserves one slot, so usable capacity is
 * capacity - 1. Creation rejects atomic storage when atomic_size_t is not
 * lock-free. Initialization and destruction require a quiescent ring.
 */
spsc_status spsc_ring_create(spsc_ring **out, size_t capacity,
                             spsc_ring_storage storage);
void spsc_ring_destroy(spsc_ring *ring);

size_t spsc_ring_capacity(const spsc_ring *ring);
size_t spsc_ring_usable_capacity(const spsc_ring *ring);
void spsc_ring_describe_layout(const spsc_ring *ring, spsc_layout *out);
bool spsc_ring_indexes_share_cache_line(const spsc_ring *ring,
                                        size_t cache_line_size);

void spsc_producer_bind(spsc_producer *producer, spsc_ring *ring);
void spsc_consumer_bind(spsc_consumer *consumer, spsc_ring *ring);

/* Do not mix operation families on one ring. Mutex operations are MPMC-safe. */
bool spsc_mutex_try_push(spsc_ring *ring, uint64_t value);
bool spsc_mutex_try_pop(spsc_ring *ring, uint64_t *value);

/* The atomic operation families require exactly one producer and one consumer.
 */
bool spsc_seq_cst_try_push(spsc_ring *ring, uint64_t value);
bool spsc_seq_cst_try_pop(spsc_ring *ring, uint64_t *value);

bool spsc_acquire_release_try_push(spsc_ring *ring, uint64_t value);
bool spsc_acquire_release_try_pop(spsc_ring *ring, uint64_t *value);

/*
 * One producer thread owns one producer endpoint, and one consumer thread owns
 * one consumer endpoint. Sharing either endpoint introduces a data race.
 */
bool spsc_cached_try_push(spsc_producer *producer, uint64_t value);
bool spsc_cached_try_pop(spsc_consumer *consumer, uint64_t *value);

#endif
