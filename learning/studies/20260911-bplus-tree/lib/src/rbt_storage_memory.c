#include "rbt_backends.h"
#include "rbt_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct rbt_mem {
    unsigned char *pages;
    uint64_t page_count;
    uint64_t capacity_pages;
    uint32_t page_size;
};

static int rbt_mem_page_count(void *context, uint64_t *out_count) {
    struct rbt_mem *memory = context;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (memory == NULL || out_count == NULL) {
        return -EINVAL;
    }
    *out_count = memory->page_count;
    return 0;
}

static int rbt_mem_read_page(void *context, uint64_t page_id, void *data_out) {
    struct rbt_mem *memory = context;

    if (memory == NULL || data_out == NULL) {
        return -EINVAL;
    }
    if (page_id >= memory->page_count) {
        return -EIO;
    }
    memcpy(data_out, memory->pages + (size_t)page_id * (size_t)memory->page_size,
           (size_t)memory->page_size);
    return 0;
}

static int rbt_mem_validate_updates(const struct rbt_mem *memory,
                                    const struct rbt_page_update *updates, size_t update_count,
                                    uint64_t *out_page_count) {
    uint64_t page_count;
    int result;

    result = rbt_page_updates_validate(updates, update_count, memory->page_count, NULL, NULL,
                                       &page_count);
    if (result != 0) {
        return result;
    }
    if (page_count > (uint64_t)(SIZE_MAX / (size_t)memory->page_size)) {
        return -EINVAL;
    }
    *out_page_count = page_count;
    return 0;
}

static int rbt_mem_commit_pages(void *context, const struct rbt_page_update *updates,
                                size_t update_count) {
    struct rbt_mem *memory = context;
    unsigned char *staged = NULL;
    unsigned char *expanded;
    uint64_t new_page_count;
    uint64_t new_capacity;
    uint64_t maximum_capacity;
    size_t index;
    int result;

    if (memory == NULL) {
        return -EINVAL;
    }
    result = rbt_mem_validate_updates(memory, updates, update_count, &new_page_count);
    if (result != 0 || update_count == 0u) {
        return result;
    }
    if (update_count > SIZE_MAX / (size_t)memory->page_size) {
        return -ENOMEM;
    }
    staged = malloc(update_count * (size_t)memory->page_size);
    if (staged == NULL) {
        return -ENOMEM;
    }
    for (index = 0u; index < update_count; ++index) {
        memcpy(staged + index * (size_t)memory->page_size, updates[index].data,
               (size_t)memory->page_size);
    }

    if (new_page_count > memory->capacity_pages) {
        maximum_capacity = (uint64_t)(SIZE_MAX / (size_t)memory->page_size);
        new_capacity = memory->capacity_pages == 0u ? 1u : memory->capacity_pages;
        while (new_capacity < new_page_count) {
            new_capacity =
                new_capacity > maximum_capacity / 2u ? maximum_capacity : new_capacity * 2u;
        }
        expanded = realloc(memory->pages, (size_t)new_capacity * (size_t)memory->page_size);
        if (expanded == NULL) {
            free(staged);
            return -ENOMEM;
        }
        memset(expanded + (size_t)memory->capacity_pages * (size_t)memory->page_size, 0,
               (size_t)(new_capacity - memory->capacity_pages) * (size_t)memory->page_size);
        memory->pages = expanded;
        memory->capacity_pages = new_capacity;
    }
    for (index = 0u; index < update_count; ++index) {
        memcpy(memory->pages + (size_t)updates[index].page_id * (size_t)memory->page_size,
               staged + index * (size_t)memory->page_size, (size_t)memory->page_size);
    }
    memory->page_count = new_page_count;
    free(staged);
    return 0;
}

int rbt_mem_create(uint32_t page_size, struct rbt_mem **out_mem) {
    struct rbt_mem *memory;

    if (out_mem != NULL) {
        *out_mem = NULL;
    }
    if (out_mem == NULL || !rbt_page_size_valid(page_size)) {
        return -EINVAL;
    }
    memory = calloc(1u, sizeof(*memory));
    if (memory == NULL) {
        return -ENOMEM;
    }
    memory->page_size = page_size;
    *out_mem = memory;
    return 0;
}

int rbt_mem_destroy(struct rbt_mem *memory) {
    if (memory == NULL) {
        return -EINVAL;
    }
    free(memory->pages);
    free(memory);
    return 0;
}

int rbt_mem_storage(struct rbt_mem *memory, struct rbt_storage *out_storage) {
    if (out_storage != NULL) {
        memset(out_storage, 0, sizeof(*out_storage));
    }
    if (memory == NULL || out_storage == NULL) {
        return -EINVAL;
    }
    out_storage->context = memory;
    out_storage->page_size = memory->page_size;
    out_storage->read_page = rbt_mem_read_page;
    out_storage->page_count = rbt_mem_page_count;
    out_storage->commit_pages = rbt_mem_commit_pages;
    return 0;
}
