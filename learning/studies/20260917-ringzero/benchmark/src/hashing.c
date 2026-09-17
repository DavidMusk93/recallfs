#include "hashing.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t key_seed = UINT64_C(0x9e3779b97f4a7c15);
static const uint64_t node_seed_1 = UINT64_C(0x243f6a8885a308d3);
static const uint64_t node_seed_2 = UINT64_C(0x13198a2e03707344);

typedef struct {
    uint64_t token;
    rz_backend_id backend;
} ring_point;

struct rz_selector {
    rz_hash_kind kind;
    union {
        struct {
            rz_backend_id *items;
            size_t count;
        } list;
        struct {
            ring_point *points;
            size_t count;
        } ring;
        struct {
            rz_backend_id *slots;
            size_t count;
        } maglev;
    } state;
};

static uint64_t mix64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

uint64_t rz_hash_key(uint64_t key) { return mix64(key ^ key_seed); }

static int compare_backend(const void *left, const void *right) {
    rz_backend_id lhs = *(const rz_backend_id *)left;
    rz_backend_id rhs = *(const rz_backend_id *)right;
    return (lhs > rhs) - (lhs < rhs);
}

static int compare_ring_point(const void *left, const void *right) {
    const ring_point *lhs = left;
    const ring_point *rhs = right;
    if (lhs->token != rhs->token) {
        return (lhs->token > rhs->token) - (lhs->token < rhs->token);
    }
    return (lhs->backend > rhs->backend) - (lhs->backend < rhs->backend);
}

static int allocate_array(size_t count, size_t element_size, void **out) {
    if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size) {
        return EOVERFLOW;
    }
    *out = malloc(count * element_size);
    return *out == NULL ? ENOMEM : 0;
}

static int canonicalize_backends(const rz_backend_id *backends, size_t backend_count,
                                 rz_backend_id **canonical_out) {
    if (backends == NULL || canonical_out == NULL || backend_count == 0) {
        return EINVAL;
    }

    rz_backend_id *canonical = NULL;
    int status = allocate_array(backend_count, sizeof(*canonical), (void **)&canonical);
    if (status != 0) {
        return status;
    }
    memcpy(canonical, backends, backend_count * sizeof(*canonical));
    qsort(canonical, backend_count, sizeof(*canonical), compare_backend);
    for (size_t index = 1; index < backend_count; ++index) {
        if (canonical[index - 1] == canonical[index]) {
            free(canonical);
            return EEXIST;
        }
    }
    *canonical_out = canonical;
    return 0;
}

static bool has_duplicate_backends(const rz_backend_id *backends,
                                   size_t backend_count) {
    for (size_t left = 0; left < backend_count; ++left) {
        for (size_t right = left + 1; right < backend_count; ++right) {
            if (backends[left] == backends[right]) {
                return true;
            }
        }
    }
    return false;
}

static size_t greatest_common_divisor(size_t left, size_t right) {
    while (right != 0) {
        size_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static bool is_prime(size_t value) {
    if (value < 2) {
        return false;
    }
    if (value % 2 == 0) {
        return value == 2;
    }
    for (size_t divisor = 3; divisor <= value / divisor; divisor += 2) {
        if (value % divisor == 0) {
            return false;
        }
    }
    return true;
}

static size_t modular_add(size_t left, size_t right, size_t modulus) {
    return left >= modulus - right ? left - (modulus - right) : left + right;
}

int rz_maglev_build_with_parameters(const rz_backend_id *backends,
                                    const size_t *offsets, const size_t *skips,
                                    size_t backend_count, size_t table_size,
                                    rz_backend_id *table_out) {
    if (backends == NULL || offsets == NULL || skips == NULL || table_out == NULL ||
        backend_count == 0 || table_size < backend_count) {
        return EINVAL;
    }
    if (has_duplicate_backends(backends, backend_count)) {
        return EEXIST;
    }
    for (size_t index = 0; index < backend_count; ++index) {
        if (offsets[index] >= table_size || skips[index] == 0 ||
            skips[index] >= table_size ||
            greatest_common_divisor(skips[index], table_size) != 1) {
            return EINVAL;
        }
    }

    size_t *candidates = NULL;
    bool *occupied = NULL;
    int status =
        allocate_array(backend_count, sizeof(*candidates), (void **)&candidates);
    if (status != 0) {
        return status;
    }
    status = allocate_array(table_size, sizeof(*occupied), (void **)&occupied);
    if (status != 0) {
        free(candidates);
        return status;
    }
    memcpy(candidates, offsets, backend_count * sizeof(*candidates));
    memset(occupied, 0, table_size * sizeof(*occupied));

    size_t filled = 0;
    while (filled < table_size) {
        for (size_t backend = 0; backend < backend_count; ++backend) {
            size_t candidate = candidates[backend];
            while (occupied[candidate]) {
                candidate = modular_add(candidate, skips[backend], table_size);
            }
            table_out[candidate] = backends[backend];
            occupied[candidate] = true;
            candidates[backend] = modular_add(candidate, skips[backend], table_size);
            ++filled;
            if (filled == table_size) {
                break;
            }
        }
    }

    free(occupied);
    free(candidates);
    return 0;
}

int rz_maglev_build_ordered(const rz_backend_id *backends, size_t backend_count,
                            size_t table_size, rz_backend_id *table_out) {
    if (backends == NULL || table_out == NULL || backend_count == 0 ||
        table_size < backend_count || !is_prime(table_size)) {
        return EINVAL;
    }

    size_t *offsets = NULL;
    size_t *skips = NULL;
    int status = allocate_array(backend_count, sizeof(*offsets), (void **)&offsets);
    if (status != 0) {
        return status;
    }
    status = allocate_array(backend_count, sizeof(*skips), (void **)&skips);
    if (status != 0) {
        free(offsets);
        return status;
    }

    for (size_t index = 0; index < backend_count; ++index) {
        uint64_t identity = backends[index];
        offsets[index] = (size_t)(mix64(identity ^ node_seed_1) % table_size);
        skips[index] = (size_t)(mix64(identity ^ node_seed_2) % (table_size - 1)) + 1;
    }
    status = rz_maglev_build_with_parameters(backends, offsets, skips, backend_count,
                                             table_size, table_out);
    free(skips);
    free(offsets);
    return status;
}

static int build_ring(const rz_backend_id *backends, size_t backend_count,
                      uint32_t virtual_nodes, rz_selector *selector) {
    if (virtual_nodes == 0 || backend_count > SIZE_MAX / (size_t)virtual_nodes) {
        return EINVAL;
    }
    size_t point_count = backend_count * (size_t)virtual_nodes;
    ring_point *points = NULL;
    int status = allocate_array(point_count, sizeof(*points), (void **)&points);
    if (status != 0) {
        return status;
    }

    size_t point = 0;
    for (size_t backend = 0; backend < backend_count; ++backend) {
        for (uint32_t vnode = 0; vnode < virtual_nodes; ++vnode) {
            uint64_t identity = ((uint64_t)backends[backend] << 32) | (uint64_t)vnode;
            points[point++] = (ring_point){
                .token = mix64(identity ^ node_seed_1),
                .backend = backends[backend],
            };
        }
    }
    qsort(points, point_count, sizeof(*points), compare_ring_point);
    selector->state.ring.points = points;
    selector->state.ring.count = point_count;
    return 0;
}

static int build_maglev(const rz_backend_id *backends, size_t backend_count,
                        size_t table_size, rz_selector *selector) {
    if (table_size < backend_count || !is_prime(table_size)) {
        return EINVAL;
    }
    rz_backend_id *table = NULL;
    int status = allocate_array(table_size, sizeof(*table), (void **)&table);
    if (status != 0) {
        return status;
    }
    status = rz_maglev_build_ordered(backends, backend_count, table_size, table);
    if (status != 0) {
        free(table);
        return status;
    }
    selector->state.maglev.slots = table;
    selector->state.maglev.count = table_size;
    return 0;
}

int rz_selector_create(const rz_selector_config *config, rz_selector **selector_out) {
    if (selector_out == NULL) {
        return EINVAL;
    }
    *selector_out = NULL;
    if (config == NULL || config->backends == NULL || config->backend_count == 0 ||
        config->backend_count > INT32_MAX || config->kind < RZ_HASH_MODULO ||
        config->kind > RZ_HASH_MAGLEV) {
        return EINVAL;
    }

    rz_backend_id *backends = NULL;
    int status =
        canonicalize_backends(config->backends, config->backend_count, &backends);
    if (status != 0) {
        return status;
    }

    rz_selector *selector = calloc(1, sizeof(*selector));
    if (selector == NULL) {
        free(backends);
        return ENOMEM;
    }
    selector->kind = config->kind;

    switch (config->kind) {
    case RZ_HASH_MODULO:
    case RZ_HASH_RENDEZVOUS:
    case RZ_HASH_JUMP:
        selector->state.list.items = backends;
        selector->state.list.count = config->backend_count;
        backends = NULL;
        break;
    case RZ_HASH_RING:
        status = build_ring(backends, config->backend_count, config->ring_virtual_nodes,
                            selector);
        break;
    case RZ_HASH_MAGLEV:
        status = build_maglev(backends, config->backend_count,
                              config->maglev_table_size, selector);
        break;
    }

    free(backends);
    if (status != 0) {
        rz_selector_destroy(selector);
        return status;
    }
    *selector_out = selector;
    return 0;
}

void rz_selector_destroy(rz_selector *selector) {
    if (selector == NULL) {
        return;
    }
    switch (selector->kind) {
    case RZ_HASH_MODULO:
    case RZ_HASH_RENDEZVOUS:
    case RZ_HASH_JUMP:
        free(selector->state.list.items);
        break;
    case RZ_HASH_RING:
        free(selector->state.ring.points);
        break;
    case RZ_HASH_MAGLEV:
        free(selector->state.maglev.slots);
        break;
    }
    free(selector);
}

static int32_t jump_bucket(uint64_t key, int32_t bucket_count) {
    int64_t previous = -1;
    int64_t next = 0;
    while (next < bucket_count) {
        previous = next;
        key = key * UINT64_C(2862933555777941757) + 1;
        next = (int64_t)((double)(previous + 1) *
                         ((double)(UINT64_C(1) << 31) / (double)((key >> 33) + 1)));
    }
    return (int32_t)previous;
}

rz_backend_id rz_selector_select(const rz_selector *selector, uint64_t key_hash) {
    switch (selector->kind) {
    case RZ_HASH_MODULO:
        return selector->state.list.items[key_hash % selector->state.list.count];
    case RZ_HASH_RING: {
        size_t low = 0;
        size_t high = selector->state.ring.count;
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (selector->state.ring.points[middle].token < key_hash) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (low == selector->state.ring.count) {
            low = 0;
        }
        return selector->state.ring.points[low].backend;
    }
    case RZ_HASH_RENDEZVOUS: {
        rz_backend_id best_backend = selector->state.list.items[0];
        uint64_t node_hash = mix64((uint64_t)best_backend ^ node_seed_1);
        uint64_t best_score = mix64(key_hash ^ node_hash);
        for (size_t index = 1; index < selector->state.list.count; ++index) {
            rz_backend_id backend = selector->state.list.items[index];
            node_hash = mix64((uint64_t)backend ^ node_seed_1);
            uint64_t score = mix64(key_hash ^ node_hash);
            if (score > best_score || (score == best_score && backend > best_backend)) {
                best_backend = backend;
                best_score = score;
            }
        }
        return best_backend;
    }
    case RZ_HASH_JUMP:
        return selector->state.list
            .items[jump_bucket(key_hash, (int32_t)selector->state.list.count)];
    case RZ_HASH_MAGLEV:
        return selector->state.maglev.slots[key_hash % selector->state.maglev.count];
    }
    return 0;
}

size_t rz_selector_memory_bytes(const rz_selector *selector) {
    switch (selector->kind) {
    case RZ_HASH_MODULO:
    case RZ_HASH_RENDEZVOUS:
    case RZ_HASH_JUMP:
        return selector->state.list.count * sizeof(rz_backend_id);
    case RZ_HASH_RING:
        return selector->state.ring.count * sizeof(ring_point);
    case RZ_HASH_MAGLEV:
        return selector->state.maglev.count * sizeof(rz_backend_id);
    }
    return 0;
}

const rz_backend_id *rz_selector_maglev_table(const rz_selector *selector,
                                              size_t *table_size_out) {
    if (table_size_out != NULL) {
        *table_size_out = 0;
    }
    if (selector == NULL || selector->kind != RZ_HASH_MAGLEV) {
        return NULL;
    }
    if (table_size_out != NULL) {
        *table_size_out = selector->state.maglev.count;
    }
    return selector->state.maglev.slots;
}
