#ifndef RBT_BACKENDS_H
#define RBT_BACKENDS_H

#include "rbt.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rbt_mem;
struct rbt_file;

/*
 * A backend owns the context referenced by its storage value and must outlive
 * every tree borrowing that storage. Destroying or closing an attached backend
 * returns -EBUSY. Required out parameters are initialized defensively before
 * validation or I/O can fail.
 */
int rbt_mem_create(uint32_t page_size, struct rbt_mem **out_mem);
int rbt_mem_destroy(struct rbt_mem *memory);
int rbt_mem_storage(struct rbt_mem *memory, struct rbt_storage *out_storage);

int rbt_file_create(const char *path, uint32_t page_size, struct rbt_file **out_file);
int rbt_file_open(const char *path, struct rbt_file **out_file);
/* Close every tree borrowing the backend before closing it. */
int rbt_file_close(struct rbt_file *file);
int rbt_file_storage(struct rbt_file *file, struct rbt_storage *out_storage);

#ifdef __cplusplus
}
#endif

#endif
