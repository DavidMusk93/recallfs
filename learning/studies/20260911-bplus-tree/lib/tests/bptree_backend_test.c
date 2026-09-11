#include "bptree_backends.h"
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

static void test_stale_temp_is_removed_on_open(void) {
    char path[256];
    char wal_temp_path[512];
    int length;
    int fd;
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    length = snprintf(wal_temp_path, sizeof(wal_temp_path), "%s.wal.tmp", path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(wal_temp_path));
    fd = open(wal_temp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(write(fd, "unpublished", 11u) == 11);
    TEST_CHECK(close(fd) == 0);

    TEST_STATUS(bpt_file_backend_open(path, &backend), BPT_OK);
    TEST_CHECK(access(wal_temp_path, F_OK) != 0);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    test_validate(tree);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    test_remove_database(path);
}

static void test_relative_path_survives_chdir(void) {
    char original_directory[PATH_MAX];
    char directory[] = "/tmp/bptree-directory-XXXXXX";
    char absolute_path[PATH_MAX];
    int length;
    bpt_file_backend *backend = NULL;
    bpt_storage storage;
    bpt_tree *tree = NULL;
    uint64_t value = 0u;
    bool inserted = false;

    TEST_CHECK(getcwd(original_directory, sizeof(original_directory)) != NULL);
    TEST_CHECK(mkdtemp(directory) != NULL);
    TEST_CHECK(chdir(directory) == 0);
    TEST_STATUS(bpt_file_backend_create("tree.db", 512u, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_create(&storage, &tree), BPT_OK);
    TEST_CHECK(chdir("/") == 0);
    TEST_STATUS(bpt_tree_put(tree, 17u, 19u, &inserted), BPT_OK);
    TEST_CHECK(inserted);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

    length = snprintf(absolute_path, sizeof(absolute_path), "%s/tree.db", directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(absolute_path));
    TEST_STATUS(bpt_file_backend_open(absolute_path, &backend), BPT_OK);
    storage = bpt_file_backend_storage(backend);
    TEST_STATUS(bpt_tree_open(&storage, &tree), BPT_OK);
    TEST_STATUS(bpt_tree_get(tree, 17u, &value), BPT_OK);
    TEST_CHECK(value == 19u);
    bpt_tree_close(tree);
    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);

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
    bpt_file_backend *backend = NULL;
    bpt_file_backend *reopened = NULL;

    test_temp_path(path, sizeof(path));
    TEST_STATUS(bpt_file_backend_create(path, 512u, &backend), BPT_OK);
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

    TEST_STATUS(bpt_file_backend_close(backend), BPT_OK);
    TEST_STATUS(bpt_file_backend_open(path, &reopened), BPT_OK);
    TEST_STATUS(bpt_file_backend_close(reopened), BPT_OK);
    TEST_CHECK(waitpid(child, &child_status, 0) == child);
    TEST_CHECK(WIFEXITED(child_status));
    TEST_CHECK(WEXITSTATUS(child_status) == 0);
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

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--hold") == 0) {
        return hold_after_exec(argv[2]);
    }
    test_file_mode_and_lock();
    test_stale_wal_blocks_create();
    test_stale_temp_is_removed_on_open();
    test_relative_path_survives_chdir();
    test_descriptors_close_on_exec(argv[0]);
    test_backend_arguments();
    return 0;
}
