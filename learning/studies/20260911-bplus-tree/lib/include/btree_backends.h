#ifndef BTREE_BACKENDS_H
#define BTREE_BACKENDS_H

#include "btree.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct btree_mem btree_mem;
typedef struct btree_file btree_file;

/* A backend must outlive every btree using its returned storage value. */
btree_status btree_mem_create(uint32_t page_size, btree_mem **backend_out);
void btree_mem_destroy(btree_mem *backend);
btree_storage btree_mem_storage(btree_mem *backend);

btree_status btree_file_create(const char *path, uint32_t page_size, btree_file **backend_out);
btree_status btree_file_open(const char *path, btree_file **backend_out);
/* Close all trees borrowing this backend before calling close. */
btree_status btree_file_close(btree_file *backend);
btree_storage btree_file_storage(btree_file *backend);

#ifdef __cplusplus
}
#endif

#endif
