#include "bptree_backends.h"
#include "test_support.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int child_main(const char *path, const char *crash_point, uint64_t key, uint64_t value) {
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    bool inserted = false;

    TEST_CHECK(setenv("BPTREE_TEST_CRASH_POINT", crash_point, 1) == 0);
    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    (void)bpt_tree_put(tree, key, value, &inserted);
    return 90;
}

static int recovery_child_main(const char *path, const char *crash_point) {
    bpt_file_backend *backend = NULL;

    TEST_CHECK(setenv("BPTREE_TEST_CRASH_POINT", crash_point, 1) == 0);
    (void)bpt_file_backend_open(path, &backend);
    return 90;
}

static void run_crash_case(const char *crash_point, bool expect_committed) {
    char path[256];
    char wal_path[512];
    int length;
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    pid_t child;
    int child_status;
    uint64_t value = 0u;
    bool inserted = false;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_put(tree, 1u, 10u, &inserted), BPT_OK);
    TEST_CHECK(inserted);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(child_main(path, crash_point, 2u, 20u));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_get(tree, 1u, &value), BPT_OK);
    TEST_CHECK(value == 10u);
    if (expect_committed) {
        TEST_STATUS(bpt_tree_get(tree, 2u, &value), BPT_OK);
        TEST_CHECK(value == 20u);
    } else {
        TEST_STATUS(bpt_tree_get(tree, 2u, &value), BPT_NOT_FOUND);
    }
    test_validate(tree);

    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    TEST_CHECK(access(wal_path, F_OK) != 0);

    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

static void run_recovery_crash_case(const char *recovery_crash_point) {
    char path[256];
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    pid_t child;
    int child_status;
    uint64_t value = 0u;
    bool inserted = false;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_put(tree, 1u, 10u, &inserted), BPT_OK);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(child_main(path, "wal_synced", 2u, 20u));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(recovery_child_main(path, recovery_crash_point));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_get(tree, 1u, &value), BPT_OK);
    TEST_CHECK(value == 10u);
    TEST_STATUS(bpt_tree_get(tree, 2u, &value), BPT_OK);
    TEST_CHECK(value == 20u);
    test_validate(tree);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

static void test_every_torn_wal_prefix(void) {
    char path[256];
    char wal_path[512];
    int length;
    int fd;
    struct stat status;
    unsigned char *wal;
    size_t prefix;
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    pid_t child;
    int child_status;
    uint64_t value = 0u;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        _exit(child_main(path, "wal_synced", 3u, 30u));
    }
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 86);

    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    fd = open(wal_path, O_RDONLY);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(fstat(fd, &status) == 0);
    TEST_CHECK(status.st_size > 1);
    TEST_CHECK((uint64_t)status.st_size <= (uint64_t)SIZE_MAX);
    wal = malloc((size_t)status.st_size);
    TEST_CHECK(wal != NULL);
    TEST_CHECK(read(fd, wal, (size_t)status.st_size) == status.st_size);
    TEST_CHECK(close(fd) == 0);

    for (prefix = 1u; prefix < (size_t)status.st_size; ++prefix) {
        fd = open(wal_path, O_WRONLY | O_TRUNC);
        TEST_CHECK(fd >= 0);
        TEST_CHECK(write(fd, wal, prefix) == (ssize_t)prefix);
        TEST_CHECK(close(fd) == 0);
        TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_CORRUPT);
        TEST_CHECK(backend == NULL);
    }

    wal[(size_t)status.st_size - 1u] ^= UINT8_C(0x80);
    fd = open(wal_path, O_WRONLY | O_TRUNC);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, wal, (size_t)status.st_size) == status.st_size);
    TEST_CHECK(close(fd) == 0);
    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_CORRUPT);
    TEST_CHECK(backend == NULL);
    wal[(size_t)status.st_size - 1u] ^= UINT8_C(0x80);

    fd = open(wal_path, O_WRONLY | O_TRUNC);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, wal, (size_t)status.st_size) == status.st_size);
    TEST_CHECK(close(fd) == 0);
    free(wal);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_get(tree, 3u, &value), BPT_OK);
    TEST_CHECK(value == 30u);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

int main(void) {
    run_crash_case("wal_created", false);
    run_crash_case("wal_synced", true);
    run_crash_case("first_page_written", true);
    run_crash_case("data_synced", true);
    run_recovery_crash_case("recovery_first_page_written");
    run_recovery_crash_case("recovery_data_synced");
    test_every_torn_wal_prefix();
    return 0;
}
