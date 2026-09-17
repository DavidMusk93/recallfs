#ifndef RINGZERO_HASHING_H
#define RINGZERO_HASHING_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t rz_backend_id;

typedef enum {
    RZ_HASH_MODULO = 0,
    RZ_HASH_RING = 1,
    RZ_HASH_RENDEZVOUS = 2,
    RZ_HASH_JUMP = 3,
    RZ_HASH_MAGLEV = 4,
} rz_hash_kind;

typedef struct {
    rz_hash_kind kind;
    const rz_backend_id *backends;
    size_t backend_count;
    size_t maglev_table_size;
    uint32_t ring_virtual_nodes;
} rz_selector_config;

typedef struct rz_selector rz_selector;

uint64_t rz_hash_key(uint64_t key);

int rz_selector_create(const rz_selector_config *config, rz_selector **selector_out);
void rz_selector_destroy(rz_selector *selector);

rz_backend_id rz_selector_select(const rz_selector *selector, uint64_t key_hash);
size_t rz_selector_memory_bytes(const rz_selector *selector);

const rz_backend_id *rz_selector_maglev_table(const rz_selector *selector,
                                              size_t *table_size_out);

int rz_maglev_build_ordered(const rz_backend_id *backends, size_t backend_count,
                            size_t table_size, rz_backend_id *table_out);

int rz_maglev_build_with_parameters(const rz_backend_id *backends,
                                    const size_t *offsets, const size_t *skips,
                                    size_t backend_count, size_t table_size,
                                    rz_backend_id *table_out);

#endif
