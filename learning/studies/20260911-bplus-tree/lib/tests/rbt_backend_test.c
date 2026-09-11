#include "test_support.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

enum { TEST_PATH_CAPACITY = 1024 };

static void put_number(struct rbt *tree, uint64_t key_number, uint64_t value_number) {
    static const unsigned char PAYLOAD[] = {'v', '\0', 'a', 'l'};
    bool inserted = false;

    RBT_TEST_OK(rbt_test_put_u64_bytes(tree, key_number, value_number, PAYLOAD, sizeof(PAYLOAD),
                                       &inserted));
    RBT_TEST_CHECK(inserted);
}

static void expect_number(struct rbt *tree, uint64_t key_number, uint64_t expected) {
    struct rbt_value key_value = rbt_test_u64(key_number);
    struct rbt_record key = rbt_test_key(&key_value, 1u);
    struct rbt_row *row = NULL;
    const struct rbt_value *actual = NULL;

    RBT_TEST_OK(rbt_get(tree, &key, &row));
    RBT_TEST_OK(rbt_row_get_value(row, 0u, &actual));
    RBT_TEST_CHECK(actual->type == RBT_TYPE_U64);
    RBT_TEST_CHECK(!actual->is_null);
    RBT_TEST_CHECK(actual->as.u64 == expected);
    RBT_TEST_OK(rbt_row_destroy(row));
}

static void test_file_mode_and_exclusive_lock(void) {
    char path[256];
    struct stat status;
    struct rbt_file *first = NULL;
    struct rbt_file *second = (struct rbt_file *)(uintptr_t)1u;
    struct rbt_storage storage;
    struct rbt *tree = NULL;

    rbt_test_temp_path(path, sizeof(path));
    RBT_TEST_OK(rbt_file_create(path, 512u, &first));
    RBT_TEST_CHECK(stat(path, &status) == 0);
    RBT_TEST_CHECK((status.st_mode & (mode_t)0777) == (mode_t)(S_IRUSR | S_IWUSR));
    RBT_TEST_ERRNO(rbt_file_open(path, &second), EBUSY);
    RBT_TEST_CHECK(second == NULL);

    RBT_TEST_OK(rbt_file_storage(first, &storage));
    RBT_TEST_OK(rbt_test_create_u64_bytes(&storage, &tree));
    put_number(tree, 1u, 2u);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(first));

    RBT_TEST_OK(rbt_file_open(path, &second));
    RBT_TEST_OK(rbt_file_storage(second, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    expect_number(tree, 1u, 2u);
    (void)rbt_test_validate(tree);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(second));

    first = (struct rbt_file *)(uintptr_t)1u;
    RBT_TEST_ERRNO(rbt_file_create(path, 512u, &first), EBUSY);
    RBT_TEST_CHECK(first == NULL);
    rbt_test_remove_database(path);
}

static void test_stale_active_wal_blocks_create(void) {
    char path[256];
    char wal_path[512];
    int length;
    int descriptor;
    struct rbt_file *file = (struct rbt_file *)(uintptr_t)1u;

    rbt_test_temp_path(path, sizeof(path));
    length = snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(wal_path));
    descriptor = open(wal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    RBT_TEST_CHECK(descriptor >= 0);
    RBT_TEST_CHECK(write(descriptor, "stale", 5u) == 5);
    RBT_TEST_CHECK(fsync(descriptor) == 0);
    RBT_TEST_CHECK(close(descriptor) == 0);

    RBT_TEST_ERRNO(rbt_file_create(path, 512u, &file), EBUSY);
    RBT_TEST_CHECK(file == NULL);
    RBT_TEST_CHECK(access(path, F_OK) != 0);
    RBT_TEST_CHECK(access(wal_path, F_OK) == 0);
    rbt_test_remove_database(path);
}

static void test_stale_unpublished_temp_cleanup(void) {
    char path[256];
    char temporary_path[512];
    int length;
    int descriptor;
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt *tree = NULL;

    rbt_test_temp_path(path, sizeof(path));
    RBT_TEST_OK(rbt_file_create(path, 512u, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_test_create_u64_bytes(&storage, &tree));
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));

    length = snprintf(temporary_path, sizeof(temporary_path), "%s.wal.tmp", path);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(temporary_path));
    descriptor = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    RBT_TEST_CHECK(descriptor >= 0);
    RBT_TEST_CHECK(write(descriptor, "unpublished", 11u) == 11);
    RBT_TEST_CHECK(close(descriptor) == 0);

    RBT_TEST_OK(rbt_file_open(path, &file));
    RBT_TEST_CHECK(access(temporary_path, F_OK) != 0);
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    (void)rbt_test_validate(tree);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));
    rbt_test_remove_database(path);
}

static void test_final_component_symlink_rejected(void) {
    char target_directory[] = "/tmp/rbt-target-directory-XXXXXX";
    char link_directory[] = "/tmp/rbt-link-directory-XXXXXX";
    char target_path[TEST_PATH_CAPACITY];
    char link_path[TEST_PATH_CAPACITY];
    int length;
    struct rbt_file *file = NULL;

    RBT_TEST_CHECK(mkdtemp(target_directory) != NULL);
    RBT_TEST_CHECK(mkdtemp(link_directory) != NULL);
    length = snprintf(target_path, sizeof(target_path), "%s/tree.db", target_directory);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(target_path));
    length = snprintf(link_path, sizeof(link_path), "%s/tree.db", link_directory);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(link_path));

    RBT_TEST_OK(rbt_file_create(target_path, 512u, &file));
    RBT_TEST_OK(rbt_file_close(file));
    file = (struct rbt_file *)(uintptr_t)1u;
    RBT_TEST_CHECK(symlink(target_path, link_path) == 0);
    RBT_TEST_ERRNO(rbt_file_open(link_path, &file), EIO);
    RBT_TEST_CHECK(file == NULL);

    RBT_TEST_CHECK(unlink(link_path) == 0);
    rbt_test_remove_database(target_path);
    RBT_TEST_CHECK(rmdir(link_directory) == 0);
    RBT_TEST_CHECK(rmdir(target_directory) == 0);
}

static void test_relative_path_survives_chdir(void) {
    char original_directory[TEST_PATH_CAPACITY];
    char directory[] = "/tmp/rbt-directory-XXXXXX";
    char absolute_path[TEST_PATH_CAPACITY];
    int length;
    struct rbt_file *file = NULL;
    struct rbt_storage storage;
    struct rbt *tree = NULL;

    RBT_TEST_CHECK(getcwd(original_directory, sizeof(original_directory)) != NULL);
    RBT_TEST_CHECK(mkdtemp(directory) != NULL);
    RBT_TEST_CHECK(chdir(directory) == 0);
    RBT_TEST_OK(rbt_file_create("tree.db", 512u, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_test_create_u64_bytes(&storage, &tree));
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));

    RBT_TEST_OK(rbt_file_open("tree.db", &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    RBT_TEST_CHECK(chdir("/") == 0);
    put_number(tree, 17u, 19u);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));

    length = snprintf(absolute_path, sizeof(absolute_path), "%s/tree.db", directory);
    RBT_TEST_CHECK(length > 0);
    RBT_TEST_CHECK((size_t)length < sizeof(absolute_path));
    RBT_TEST_OK(rbt_file_open(absolute_path, &file));
    RBT_TEST_OK(rbt_file_storage(file, &storage));
    RBT_TEST_OK(rbt_open(&storage, &tree));
    expect_number(tree, 17u, 19u);
    RBT_TEST_OK(rbt_destroy(tree));
    RBT_TEST_OK(rbt_file_close(file));

    rbt_test_remove_database(absolute_path);
    RBT_TEST_CHECK(rmdir(directory) == 0);
    RBT_TEST_CHECK(chdir(original_directory) == 0);
}

static int hold_after_exec(const char *descriptor_text) {
    int descriptor = atoi(descriptor_text);
    char ready = 'R';

    RBT_TEST_CHECK(write(descriptor, &ready, 1u) == 1);
    RBT_TEST_CHECK(close(descriptor) == 0);
    sleep(2u);
    return 0;
}

static void test_descriptors_close_across_exec(const char *executable_path) {
    char path[256];
    char descriptor_text[32];
    int pipe_descriptors[2];
    int length;
    char ready = '\0';
    pid_t child;
    int child_status;
    struct rbt_file *file = NULL;
    struct rbt_file *reopened = NULL;

    rbt_test_temp_path(path, sizeof(path));
    RBT_TEST_OK(rbt_file_create(path, 512u, &file));
    RBT_TEST_CHECK(pipe(pipe_descriptors) == 0);
    child = fork();
    RBT_TEST_CHECK(child >= 0);
    if (child == 0) {
        (void)close(pipe_descriptors[0]);
        length = snprintf(descriptor_text, sizeof(descriptor_text), "%d", pipe_descriptors[1]);
        RBT_TEST_CHECK(length > 0);
        RBT_TEST_CHECK((size_t)length < sizeof(descriptor_text));
        execl(executable_path, executable_path, "--hold", descriptor_text, (char *)NULL);
        _exit(91);
    }
    RBT_TEST_CHECK(close(pipe_descriptors[1]) == 0);
    RBT_TEST_CHECK(read(pipe_descriptors[0], &ready, 1u) == 1);
    RBT_TEST_CHECK(ready == 'R');
    RBT_TEST_CHECK(close(pipe_descriptors[0]) == 0);

    RBT_TEST_OK(rbt_file_close(file));
    RBT_TEST_OK(rbt_file_open(path, &reopened));
    RBT_TEST_OK(rbt_file_close(reopened));
    RBT_TEST_CHECK(waitpid(child, &child_status, 0) == child);
    RBT_TEST_CHECK(WIFEXITED(child_status));
    RBT_TEST_CHECK(WEXITSTATUS(child_status) == 0);
    rbt_test_remove_database(path);
}

static void expect_memory_image(const struct rbt_storage *storage,
                                const unsigned char expected[512]) {
    unsigned char actual[512];
    uint64_t page_count = UINT64_MAX;

    memset(actual, 0, sizeof(actual));
    RBT_TEST_OK(storage->page_count(storage->context, &page_count));
    RBT_TEST_CHECK(page_count == 1u);
    RBT_TEST_OK(storage->read_page(storage->context, 0u, actual));
    RBT_TEST_CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
}

static void test_memory_invalid_write_sets_are_atomic(void) {
    struct rbt_mem *memory = NULL;
    struct rbt_storage storage;
    unsigned char original[512];
    unsigned char replacement[512];
    struct rbt_page_update updates[2];
    size_t index;

    for (index = 0u; index < sizeof(original); ++index) {
        original[index] = (unsigned char)(index ^ 0x5au);
        replacement[index] = (unsigned char)(index ^ 0xa5u);
    }
    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &storage));
    updates[0].page_id = 0u;
    updates[0].data = original;
    RBT_TEST_OK(storage.commit_pages(storage.context, updates, 1u));
    expect_memory_image(&storage, original);

    RBT_TEST_ERRNO(storage.commit_pages(storage.context, NULL, 1u), EINVAL);
    expect_memory_image(&storage, original);

    updates[0].page_id = 0u;
    updates[0].data = replacement;
    updates[1].page_id = 2u;
    updates[1].data = replacement;
    RBT_TEST_ERRNO(storage.commit_pages(storage.context, updates, 2u), EINVAL);
    expect_memory_image(&storage, original);

    updates[0].page_id = 1u;
    updates[0].data = replacement;
    updates[1].page_id = 1u;
    updates[1].data = original;
    RBT_TEST_ERRNO(storage.commit_pages(storage.context, updates, 2u), EINVAL);
    expect_memory_image(&storage, original);

    updates[0].page_id = 0u;
    updates[0].data = NULL;
    RBT_TEST_ERRNO(storage.commit_pages(storage.context, updates, 1u), EINVAL);
    expect_memory_image(&storage, original);

    updates[0].page_id = UINT64_MAX;
    updates[0].data = replacement;
    RBT_TEST_ERRNO(storage.commit_pages(storage.context, updates, 1u), ENOMEM);
    expect_memory_image(&storage, original);
    RBT_TEST_OK(rbt_mem_destroy(memory));
}

static void test_defensive_output_parameters(void) {
    char path[256];
    struct rbt_mem *memory = (struct rbt_mem *)(uintptr_t)1u;
    struct rbt_file *file = (struct rbt_file *)(uintptr_t)1u;
    struct rbt_storage storage;
    uint64_t page_count = UINT64_MAX;

    rbt_test_temp_path(path, sizeof(path));
    RBT_TEST_ERRNO(rbt_mem_create(513u, &memory), EINVAL);
    RBT_TEST_CHECK(memory == NULL);
    memset(&storage, 0xa5, sizeof(storage));
    RBT_TEST_ERRNO(rbt_mem_storage(NULL, &storage), EINVAL);
    RBT_TEST_CHECK(storage.context == NULL);
    RBT_TEST_CHECK(storage.page_size == 0u);
    RBT_TEST_CHECK(storage.read_page == NULL);
    RBT_TEST_CHECK(storage.page_count == NULL);
    RBT_TEST_CHECK(storage.commit_pages == NULL);

    RBT_TEST_ERRNO(rbt_file_create(NULL, 512u, &file), EINVAL);
    RBT_TEST_CHECK(file == NULL);
    file = (struct rbt_file *)(uintptr_t)1u;
    RBT_TEST_ERRNO(rbt_file_create(path, 513u, &file), EINVAL);
    RBT_TEST_CHECK(file == NULL);
    file = (struct rbt_file *)(uintptr_t)1u;
    RBT_TEST_ERRNO(rbt_file_open(NULL, &file), EINVAL);
    RBT_TEST_CHECK(file == NULL);
    memset(&storage, 0xa5, sizeof(storage));
    RBT_TEST_ERRNO(rbt_file_storage(NULL, &storage), EINVAL);
    RBT_TEST_CHECK(storage.context == NULL);
    RBT_TEST_CHECK(storage.page_size == 0u);
    RBT_TEST_CHECK(storage.read_page == NULL);
    RBT_TEST_CHECK(storage.page_count == NULL);
    RBT_TEST_CHECK(storage.commit_pages == NULL);

    RBT_TEST_OK(rbt_mem_create(512u, &memory));
    RBT_TEST_OK(rbt_mem_storage(memory, &storage));
    RBT_TEST_ERRNO(storage.page_count(NULL, &page_count), EINVAL);
    RBT_TEST_CHECK(page_count == 0u);
    RBT_TEST_OK(rbt_mem_destroy(memory));
    RBT_TEST_ERRNO(rbt_file_close(NULL), EINVAL);
    rbt_test_remove_database(path);
}

int main(int argument_count, char **arguments) {
    if (argument_count == 3 && strcmp(arguments[1], "--hold") == 0) {
        return hold_after_exec(arguments[2]);
    }
    test_file_mode_and_exclusive_lock();
    test_stale_active_wal_blocks_create();
    test_stale_unpublished_temp_cleanup();
    test_final_component_symlink_rejected();
    test_relative_path_survives_chdir();
    test_descriptors_close_across_exec(arguments[0]);
    test_memory_invalid_write_sets_are_atomic();
    test_defensive_output_parameters();
    return 0;
}
