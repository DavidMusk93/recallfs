#include "btree_backends.h"
#include "test_support.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

static void test_file_mode_and_lock(void) {
    char path[256];
    struct stat status;
    btree_file *first = NULL;
    btree_file *second = NULL;
    btree_storage storage;
    btree *tree = NULL;
    bool inserted;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(btree_file_create(path, 512u, &first), BTREE_OK);
    TEST_CHECK(stat(path, &status) == 0);
    TEST_CHECK((status.st_mode & (mode_t)0777) == (mode_t)(S_IRUSR | S_IWUSR));
    TEST_STATUS(btree_file_open(path, &second), BTREE_BUSY);
    TEST_CHECK(second == NULL);

    storage = btree_file_storage(first);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    TEST_STATUS(test_btree_put_u64(tree, 1u, 2u, &inserted), BTREE_OK);
    TEST_CHECK(inserted);
    btree_close(tree);
    TEST_STATUS(btree_file_close(first), BTREE_OK);

    TEST_STATUS(btree_file_open(path, &second), BTREE_OK);
    storage = btree_file_storage(second);
    TEST_STATUS(test_btree_open_u64(&storage, &tree), BTREE_OK);
    test_validate(tree);
    btree_close(tree);
    TEST_STATUS(btree_file_close(second), BTREE_OK);

    TEST_STATUS(btree_file_create(path, 512u, &first), BTREE_BUSY);
    TEST_CHECK(first == NULL);
    test_remove_database(path);
}

static void test_stale_wal_blocks_create(void) {
    char path[256];
    char wal_path[512];
    int length;
    int fd;
    btree_file *backend = NULL;

    test_temp_path(path, sizeof(path));
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_path));
    fd = open(wal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, "stale", 5u) == 5);
    TEST_CHECK(fsync(fd) == 0);
    TEST_CHECK(close(fd) == 0);

    TEST_STATUS(btree_file_create(path, 512u, &backend), BTREE_BUSY);
    TEST_CHECK(backend == NULL);
    TEST_CHECK(access(path, F_OK) != 0);
    TEST_CHECK(access(wal_path, F_OK) == 0);
    test_remove_database(path);
}

static void test_stale_temp_is_removed_on_open(void) {
    char path[256];
    char wal_temp_path[512];
    int length;
    int fd;
    btree_file *backend = NULL;
    btree_storage storage;
    btree *tree = NULL;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(btree_file_create(path, 512u, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);

    length = snprintf(wal_temp_path, sizeof(wal_temp_path), "%s.wal.tmp", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_temp_path));
    fd = open(wal_temp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, "unpublished", 11u) == 11);
    TEST_CHECK(close(fd) == 0);

    TEST_STATUS(btree_file_open(path, &backend), BTREE_OK);
    TEST_CHECK(access(wal_temp_path, F_OK) != 0);
    storage = btree_file_storage(backend);
    TEST_STATUS(test_btree_open_u64(&storage, &tree), BTREE_OK);
    test_validate(tree);
    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    test_remove_database(path);
}

static void test_final_component_symlink_is_rejected(void) {
    char target_directory[] = "/tmp/btree-target-directory-XXXXXX";
    char link_directory[] = "/tmp/btree-link-directory-XXXXXX";
    char target_path[PATH_MAX];
    char link_path[PATH_MAX];
    int length;
    btree_file *backend = NULL;

    TEST_CHECK(mkdtemp(target_directory) != NULL);
    TEST_CHECK(mkdtemp(link_directory) != NULL);
    length = snprintf(target_path, sizeof(target_path), "%s/tree.db", target_directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(target_path));
    length = snprintf(link_path, sizeof(link_path), "%s/tree.db", link_directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(link_path));

    TEST_STATUS(btree_file_create(target_path, 512u, &backend), BTREE_OK);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    backend = NULL;
    TEST_CHECK(symlink(target_path, link_path) == 0);

    TEST_STATUS(btree_file_open(link_path, &backend), BTREE_IO);
    TEST_CHECK(backend == NULL);

    TEST_CHECK(unlink(link_path) == 0);
    test_remove_database(target_path);
    TEST_CHECK(rmdir(link_directory) == 0);
    TEST_CHECK(rmdir(target_directory) == 0);
}

static void test_relative_path_survives_chdir(void) {
    char original_directory[PATH_MAX];
    char directory[] = "/tmp/btree-directory-XXXXXX";
    char absolute_path[PATH_MAX];
    int length;
    btree_file *backend = NULL;
    btree_storage storage;
    btree *tree = NULL;
    uint64_t value = 0u;
    bool inserted = false;

    TEST_CHECK(getcwd(original_directory, sizeof(original_directory)) != NULL);
    TEST_CHECK(mkdtemp(directory) != NULL);
    TEST_CHECK(chdir(directory) == 0);
    TEST_STATUS(btree_file_create("tree.db", 512u, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(test_btree_create_u64(&storage, &tree), BTREE_OK);
    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);

    TEST_STATUS(btree_file_open("tree.db", &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(test_btree_open_u64(&storage, &tree), BTREE_OK);
    TEST_CHECK(chdir("/") == 0);
    TEST_STATUS(test_btree_put_u64(tree, 17u, 19u, &inserted), BTREE_OK);
    TEST_CHECK(inserted);
    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);

    length = snprintf(absolute_path, sizeof(absolute_path), "%s/tree.db", directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(absolute_path));
    TEST_STATUS(btree_file_open(absolute_path, &backend), BTREE_OK);
    storage = btree_file_storage(backend);
    TEST_STATUS(test_btree_open_u64(&storage, &tree), BTREE_OK);
    TEST_STATUS(test_btree_get_u64(tree, 17u, &value), BTREE_OK);
    TEST_CHECK(value == 19u);
    btree_close(tree);
    TEST_STATUS(btree_file_close(backend), BTREE_OK);

    test_remove_database(absolute_path);
    TEST_CHECK(rmdir(directory) == 0);
    TEST_CHECK(chdir(original_directory) == 0);
}

static int hold_after_exec(const char *descriptor_text) {
    int descriptor = atoi(descriptor_text);
    char ready = 'R';

    TEST_CHECK(write(descriptor, &ready, 1u) == 1);
    TEST_CHECK(close(descriptor) == 0);
    sleep(2u);
    return 0;
}

static void test_descriptors_close_on_exec(const char *executable_path) {
    char path[256];
    char descriptor_text[32];
    int pipe_fds[2];
    int length;
    char ready = '\0';
    pid_t child;
    int child_status;
    btree_file *backend = NULL;
    btree_file *reopened = NULL;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(btree_file_create(path, 512u, &backend), BTREE_OK);
    TEST_CHECK(pipe(pipe_fds) == 0);
    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        (void)close(pipe_fds[0]);
        length = snprintf(descriptor_text, sizeof(descriptor_text), "%d", pipe_fds[1]);
        TEST_CHECK(length > 0);
        TEST_CHECK((size_t)length < sizeof(descriptor_text));
        execl(executable_path, executable_path, "--hold", descriptor_text, (char *)NULL);
        _exit(91);
    }
    TEST_CHECK(close(pipe_fds[1]) == 0);
    TEST_CHECK(read(pipe_fds[0], &ready, 1u) == 1);
    TEST_CHECK(ready == 'R');
    TEST_CHECK(close(pipe_fds[0]) == 0);

    TEST_STATUS(btree_file_close(backend), BTREE_OK);
    TEST_STATUS(btree_file_open(path, &reopened), BTREE_OK);
    TEST_STATUS(btree_file_close(reopened), BTREE_OK);
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 0);
    test_remove_database(path);
}

static void test_backend_arguments(void) {
    char path[256];
    btree_file *backend = NULL;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(btree_file_create(NULL, 512u, &backend), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(btree_file_create(path, 256u, &backend), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(btree_file_create(path, 513u, &backend), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(btree_file_open(NULL, &backend), BTREE_INVALID_ARGUMENT);
    TEST_STATUS(btree_file_close(NULL), BTREE_INVALID_ARGUMENT);
    test_remove_database(path);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--hold") == 0) {
        return hold_after_exec(argv[2]);
    }
    test_file_mode_and_lock();
    test_stale_wal_blocks_create();
    test_stale_temp_is_removed_on_open();
    test_final_component_symlink_is_rejected();
    test_relative_path_survives_chdir();
    test_descriptors_close_on_exec(argv[0]);
    test_backend_arguments();
    return 0;
}
