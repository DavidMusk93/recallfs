#ifndef BPTREE_BACKENDS_H
#define BPTREE_BACKENDS_H

#include "bptree.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bpt_memory_backend bpt_memory_backend;
typedef struct bpt_file_backend bpt_file_backend;

bpt_status bpt_memory_backend_create(uint32_t page_size, bpt_memory_backend **backend_out);
void bpt_memory_backend_destroy(bpt_memory_backend *backend);
bpt_storage bpt_memory_backend_storage(bpt_memory_backend *backend);

bpt_status bpt_file_backend_create(const char *path, uint32_t page_size,
                                   bpt_file_backend **backend_out);
bpt_status bpt_file_backend_open(const char *path, bpt_file_backend **backend_out);
bpt_status bpt_file_backend_close(bpt_file_backend *backend);
bpt_storage bpt_file_backend_storage(bpt_file_backend *backend);

#ifdef __cplusplus
}
#endif

#endif
