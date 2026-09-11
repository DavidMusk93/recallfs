#include "rbt_internal.h"

#include <errno.h>
#include <stdlib.h>

static int rbt_page_id_compare(const void *left, const void *right) {
    uint64_t left_id = *(const uint64_t *)left;
    uint64_t right_id = *(const uint64_t *)right;

    return left_id < right_id ? -1 : left_id > right_id ? 1 : 0;
}

int rbt_page_updates_validate(const struct rbt_page_update *updates, size_t update_count,
                              uint64_t current_page_count, rbt_page_update_check_fn check_page_id,
                              const void *check_context, uint64_t *out_final_page_count) {
    uint64_t *page_ids = NULL;
    uint64_t final_page_count = current_page_count;
    uint64_t new_page_count = 0u;
    size_t index;

    if (out_final_page_count == NULL || (update_count != 0u && updates == NULL)) {
        return -EINVAL;
    }
    if (update_count > SIZE_MAX / sizeof(*page_ids)) {
        return -ENOMEM;
    }
    if (update_count != 0u) {
        page_ids = malloc(update_count * sizeof(*page_ids));
        if (page_ids == NULL) {
            return -ENOMEM;
        }
    }
    for (index = 0u; index < update_count; ++index) {
        int result;

        if (updates[index].data == NULL) {
            free(page_ids);
            return -EINVAL;
        }
        if (updates[index].page_id == UINT64_MAX) {
            free(page_ids);
            return -ENOMEM;
        }
        result = check_page_id == NULL ? 0 : check_page_id(updates[index].page_id, check_context);
        if (result != 0) {
            free(page_ids);
            return result;
        }
        page_ids[index] = updates[index].page_id;
        if (page_ids[index] >= current_page_count) {
            ++new_page_count;
        }
        if (page_ids[index] + 1u > final_page_count) {
            final_page_count = page_ids[index] + 1u;
        }
    }
    if (update_count > 1u) {
        qsort(page_ids, update_count, sizeof(*page_ids), rbt_page_id_compare);
    }
    for (index = 1u; index < update_count; ++index) {
        if (page_ids[index - 1u] == page_ids[index]) {
            free(page_ids);
            return -EINVAL;
        }
    }
    free(page_ids);
    if (final_page_count - current_page_count != new_page_count) {
        return -EINVAL;
    }
    *out_final_page_count = final_page_count;
    return 0;
}
