#include "btree_backends.h"
#include "btree_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct btree_mem {
    unsigned char *pages;
    uint64_t page_count;
    uint64_t capacity_pages;
    uint32_t page_size;
};

static btree_status memory_growth_capacity(const btree_mem *backend, uint64_t required_pages,
                                           uint64_t *capacity_out) {
    uint64_t capacity = backend->capacity_pages;
    uint64_t max_capacity = (uint64_t)(SIZE_MAX / (size_t)backend->page_size);

    if (required_pages > max_capacity || capacity > max_capacity) {
        return BTREE_OUT_OF_MEMORY;
    }
    if (capacity == 0u) {
        capacity = 1u;
    }
    while (capacity < required_pages) {
        if (capacity > max_capacity / 2u) {
            capacity = max_capacity;
        } else {
            capacity *= 2u;
        }
    }

    *capacity_out = capacity;
    return BTREE_OK;
}

static btree_status memory_page_count(void *context, uint64_t *count_out) {
    btree_mem *backend = context;

    if (backend == NULL || count_out == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    *count_out = backend->page_count;
    return BTREE_OK;
}

static btree_status memory_read_page(void *context, uint64_t page_id, void *data_out) {
    btree_mem *backend = context;

    if (backend == NULL || data_out == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    if (page_id >= backend->page_count) {
        return BTREE_IO;
    }
    memcpy(data_out, backend->pages + (size_t)page_id * (size_t)backend->page_size,
           (size_t)backend->page_size);
    return BTREE_OK;
}

static btree_status memory_validate_updates(const btree_mem *backend,
                                            const btree_page_update *updates, size_t update_count,
                                            uint64_t *new_page_count_out) {
    uint64_t new_page_count = backend->page_count;
    uint64_t new_id_count = 0u;
    size_t index;

    if (update_count != 0u && updates == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    for (index = 0u; index < update_count; ++index) {
        size_t other;

        if (updates[index].data == NULL) {
            return BTREE_INVALID_ARGUMENT;
        }
        if (updates[index].page_id == UINT64_MAX) {
            return BTREE_OUT_OF_MEMORY;
        }
        for (other = 0u; other < index; ++other) {
            if (updates[other].page_id == updates[index].page_id) {
                return BTREE_INVALID_ARGUMENT;
            }
        }
        if (updates[index].page_id >= backend->page_count) {
            new_id_count++;
        }
        if (updates[index].page_id + 1u > new_page_count) {
            new_page_count = updates[index].page_id + 1u;
        }
    }

    if (new_page_count > (uint64_t)(SIZE_MAX / backend->page_size)) {
        return BTREE_OUT_OF_MEMORY;
    }
    if (new_page_count - backend->page_count != new_id_count) {
        return BTREE_INVALID_ARGUMENT;
    }

    *new_page_count_out = new_page_count;
    return BTREE_OK;
}

static btree_status memory_commit_pages(void *context, const btree_page_update *updates,
                                        size_t update_count) {
    btree_mem *backend = context;
    unsigned char *staged;
    uint64_t new_page_count;
    size_t staged_size;
    size_t index;
    btree_status status;

    if (backend == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    status = memory_validate_updates(backend, updates, update_count, &new_page_count);
    if (status != BTREE_OK || update_count == 0u) {
        return status;
    }

    if (update_count > SIZE_MAX / (size_t)backend->page_size) {
        return BTREE_OUT_OF_MEMORY;
    }
    staged_size = update_count * (size_t)backend->page_size;
    staged = malloc(staged_size);
    if (staged == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    for (index = 0u; index < update_count; ++index) {
        memcpy(staged + index * (size_t)backend->page_size, updates[index].data,
               (size_t)backend->page_size);
    }

    if (new_page_count > backend->capacity_pages) {
        uint64_t new_capacity;
        size_t old_capacity_size;
        size_t new_capacity_size;
        unsigned char *expanded;

        status = memory_growth_capacity(backend, new_page_count, &new_capacity);
        if (status != BTREE_OK) {
            free(staged);
            return status;
        }
        old_capacity_size = (size_t)backend->capacity_pages * (size_t)backend->page_size;
        new_capacity_size = (size_t)new_capacity * (size_t)backend->page_size;
        expanded = realloc(backend->pages, new_capacity_size);

        if (expanded == NULL) {
            free(staged);
            return BTREE_OUT_OF_MEMORY;
        }
        memset(expanded + old_capacity_size, 0, new_capacity_size - old_capacity_size);
        backend->pages = expanded;
        backend->capacity_pages = new_capacity;
    }

    for (index = 0u; index < update_count; ++index) {
        memcpy(backend->pages + (size_t)updates[index].page_id * (size_t)backend->page_size,
               staged + index * (size_t)backend->page_size, (size_t)backend->page_size);
    }
    backend->page_count = new_page_count;
    free(staged);
    return BTREE_OK;
}

btree_status btree_mem_create(uint32_t page_size, btree_mem **backend_out) {
    btree_mem *backend;

    if (backend_out != NULL) {
        *backend_out = NULL;
    }
    if (!btree_page_size_valid(page_size) || backend_out == NULL) {
        return BTREE_INVALID_ARGUMENT;
    }
    backend = calloc(1u, sizeof(*backend));
    if (backend == NULL) {
        return BTREE_OUT_OF_MEMORY;
    }
    backend->page_size = page_size;
    *backend_out = backend;
    return BTREE_OK;
}

void btree_mem_destroy(btree_mem *backend) {
    if (backend == NULL) {
        return;
    }
    free(backend->pages);
    free(backend);
}

btree_storage btree_mem_storage(btree_mem *backend) {
    btree_storage storage;

    memset(&storage, 0, sizeof(storage));
    if (backend == NULL) {
        return storage;
    }
    storage.context = backend;
    storage.page_size = backend->page_size;
    storage.read_page = memory_read_page;
    storage.page_count = memory_page_count;
    storage.commit_pages = memory_commit_pages;
    return storage;
}
