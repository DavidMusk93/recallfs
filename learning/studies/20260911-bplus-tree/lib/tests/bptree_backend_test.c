#include "bptree_backends.h"
#include "test_support.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

static void test_file_mode_and_lock(void) {
    char path[256];
    struct stat status;
    bpt_file_backend *first = NULL;
    bpt_file_backend *second = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    bool inserted;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &first), BPT_OK);
    TEST_CHECK(stat(path, &status) == 0);
    TEST_CHECK((status.st_mode & (mode_t)0777) == (mode_t)(S_IRUSR | S_IWUSR));
    TEST_STATUS(bpt_file_backend_open(path, &second), BPT_BUSY);
    TEST_CHECK(second == NULL);

    storage = bpt_file_backend_storage(first);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_put(tree, 1u, 2u, &inserted), BPT_OK);
    TEST_CHECK(inserted);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(first), BPT_OK);

    TEST_STATUS(bpt_file_backend_open(path, &second), BPT_OK);
    storage = bpt_file_backend_storage(second);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    test_validate(tree);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(second), BPT_OK);

    TEST_STATUS(bpt_file_backend_create(path, 512u, &first), BPT_BUSY);
    TEST_CHECK(first == NULL);
    test_remove_database(path);
}

static void test_stale_wal_blocks_create(void) {
    char path[256];
    char wal_path[512];
    int length;
    int fd;
    bpt_file_backend *backend = NULL;

    test_temp_path(path, sizeof(path));
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    fd = open(wal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, "stale", 5u) == 5);
    TEST_CHECK(fsync(fd) == 0);
    TEST_CHECK(close(fd) == 0);

    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_BUSY);
    TEST_CHECK(backend == NULL);
    TEST_CHECK(access(path, F_OK) != 0);
    TEST_CHECK(access(wal_path, F_OK) == 0);
    test_remove_database(path);
}

static void test_backend_arguments(void) {
    char path[256];
    bpt_file_backend *backend = NULL;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(NULL, 512u, &backend), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_file_backend_create(path, 256u, &backend), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_file_backend_create(path, 513u, &backend), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_file_backend_open(NULL, &backend), BPT_INVALID_ARGUMENT);
    TEST_STATUS(bpt_file_backend_close(NULL), BPT_INVALID_ARGUMENT);
    test_remove_database(path);
}

int main(void) {
    test_file_mode_and_lock();
    test_stale_wal_blocks_create();
    test_backend_arguments();
    return 0;
}
