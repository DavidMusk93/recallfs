#include "bptree_backends.h"
#include "bptree_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct bpt_memory_backend {
    unsigned char *pages;
    uint64_t page_count;
    uint32_t page_size;
};

static bpt_status memory_page_count(void *context, uint64_t *count_out) {
    bpt_memory_backend *backend = context;

    if (backend == NULL || count_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    *count_out = backend->page_count;
    return BPT_OK;
}

static bpt_status memory_read_page(void *context, uint64_t page_id, void *data_out) {
    bpt_memory_backend *backend = context;

    if (backend == NULL || data_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    if (page_id >= backend->page_count) {
        return BPT_IO;
    }
    memcpy(data_out, backend->pages + (size_t)page_id * (size_t)backend->page_size,
           (size_t)backend->page_size);
    return BPT_OK;
}

static bpt_status memory_validate_updates(const bpt_memory_backend *backend,
                                          const bpt_page_update *updates, size_t update_count,
                                          uint64_t *new_page_count_out) {
    uint64_t new_page_count = backend->page_count;
    uint64_t new_id_count = 0u;
    size_t index;

    if (update_count != 0u && updates == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    for (index = 0u; index < update_count; ++index) {
        size_t other;

        if (updates[index].data == NULL) {
            return BPT_INVALID_ARGUMENT;
        }
        if (updates[index].page_id == UINT64_MAX) {
            return BPT_OUT_OF_MEMORY;
        }
        for (other = 0u; other < index; ++other) {
            if (updates[other].page_id == updates[index].page_id) {
                return BPT_INVALID_ARGUMENT;
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
        return BPT_OUT_OF_MEMORY;
    }
    if (new_page_count - backend->page_count != new_id_count) {
        return BPT_INVALID_ARGUMENT;
    }

    *new_page_count_out = new_page_count;
    return BPT_OK;
}

static bpt_status memory_commit_pages(void *context, const bpt_page_update *updates,
                                      size_t update_count) {
    bpt_memory_backend *backend = context;
    unsigned char *staged;
    uint64_t new_page_count;
    size_t staged_size;
    size_t index;
    bpt_status status;

    if (backend == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    status = memory_validate_updates(backend, updates, update_count, &new_page_count);
    if (status != BPT_OK || update_count == 0u) {
        return status;
    }

    if (update_count > SIZE_MAX / (size_t)backend->page_size) {
        return BPT_OUT_OF_MEMORY;
    }
    staged_size = update_count * (size_t)backend->page_size;
    staged = malloc(staged_size);
    if (staged == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    for (index = 0u; index < update_count; ++index) {
        memcpy(staged + index * (size_t)backend->page_size, updates[index].data,
               (size_t)backend->page_size);
    }

    if (new_page_count > backend->page_count) {
        size_t old_size = (size_t)backend->page_count * (size_t)backend->page_size;
        size_t new_size = (size_t)new_page_count * (size_t)backend->page_size;
        unsigned char *expanded = realloc(backend->pages, new_size);

        if (expanded == NULL) {
            free(staged);
            return BPT_OUT_OF_MEMORY;
        }
        backend->pages = expanded;
        memset(backend->pages + old_size, 0, new_size - old_size);
    }

    for (index = 0u; index < update_count; ++index) {
        memcpy(backend->pages + (size_t)updates[index].page_id * (size_t)backend->page_size,
               staged + index * (size_t)backend->page_size, (size_t)backend->page_size);
    }
    backend->page_count = new_page_count;
    free(staged);
    return BPT_OK;
}

bpt_status bpt_memory_backend_create(uint32_t page_size, bpt_memory_backend **backend_out) {
    bpt_memory_backend *backend;

    if (backend_out != NULL) {
        *backend_out = NULL;
    }
    if (!bpt_page_size_valid(page_size) || backend_out == NULL) {
        return BPT_INVALID_ARGUMENT;
    }
    backend = calloc(1u, sizeof(*backend));
    if (backend == NULL) {
        return BPT_OUT_OF_MEMORY;
    }
    backend->page_size = page_size;
    *backend_out = backend;
    return BPT_OK;
}

void bpt_memory_backend_destroy(bpt_memory_backend *backend) {
    if (backend == NULL) {
        return;
    }
    free(backend->pages);
    free(backend);
}

bpt_storage bpt_memory_backend_storage(bpt_memory_backend *backend) {
    bpt_storage storage;

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
