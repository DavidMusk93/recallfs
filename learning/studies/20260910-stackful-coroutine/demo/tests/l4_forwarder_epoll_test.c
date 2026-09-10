#define L4_IO_BUDGET_BYTES ((size_t)4096)
#define accept4 l4_test_accept4
#define main l4_forwarder_epoll_program_main
#include "../examples/l4_forwarder_epoll.c"
#undef main
#undef accept4

#define L4_TEST_ACCEPT_RESULTS 16

static int test_accept_results[L4_TEST_ACCEPT_RESULTS];
static size_t test_accept_result_count;
static size_t test_accept_result_index;

int l4_test_accept4(int fd,
                    struct sockaddr *address,
                    socklen_t *address_length,
                    int flags)
{
    (void)fd;
    (void)address;
    (void)address_length;
    (void)flags;
    if (test_accept_result_index >= test_accept_result_count) {
        errno = EAGAIN;
        return -1;
    }

    int result = test_accept_results[test_accept_result_index++];
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int create_socket_pair(int sockets[2])
{
    return socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                      sockets);
}

static int initialize_watch_table(struct l4_app *app, size_t max_fds)
{
    app->max_fds = max_fds;
    app->watch_chunk_count =
        max_fds / L4_FD_WATCH_CHUNK_SIZE +
        (max_fds % L4_FD_WATCH_CHUNK_SIZE != 0);
    app->watch_chunks =
        calloc(app->watch_chunk_count, sizeof(*app->watch_chunks));
    return app->watch_chunks == NULL ? -ENOMEM : 0;
}

static void set_accept_results(const int *results, size_t count)
{
    if (count > L4_TEST_ACCEPT_RESULTS) {
        abort();
    }
    memcpy(test_accept_results, results, count * sizeof(*results));
    test_accept_result_count = count;
    test_accept_result_index = 0;
}

static int write_exact(int fd, const unsigned char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, data + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int read_exact(int fd, unsigned char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t received = read(fd, data + offset, length - offset);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int test_bidirectional_budgets_are_independent(void)
{
    int clients[2] = {-1, -1};
    int upstreams[2] = {-1, -1};
    CHECK(create_socket_pair(clients) == 0);
    CHECK(create_socket_pair(upstreams) == 0);

    struct l4_app app = {
        .options.buffer_size = L4_IO_BUDGET_BYTES,
    };
    size_t allocation_size =
        sizeof(struct l4_connection) + 2 * app.options.buffer_size;
    struct l4_connection *connection = calloc(1, allocation_size);
    CHECK(connection != NULL);
    connection->app = &app;
    connection->client_fd = clients[0];
    connection->upstream_fd = upstreams[0];
    connection->upstream_state = L4_UPSTREAM_OPEN;
    connection->directions[0].data = connection->buffers;
    connection->directions[1].data =
        connection->buffers + app.options.buffer_size;

    unsigned char client_payload[L4_IO_BUDGET_BYTES];
    unsigned char upstream_payload[L4_IO_BUDGET_BYTES];
    unsigned char observed[L4_IO_BUDGET_BYTES];
    memset(client_payload, 0x5a, sizeof(client_payload));
    memset(upstream_payload, 0xa5, sizeof(upstream_payload));

    CHECK(write_exact(clients[1], client_payload, sizeof(client_payload)) == 0);
    memcpy(connection->directions[1].data, upstream_payload,
           sizeof(upstream_payload));
    connection->directions[1].length = sizeof(upstream_payload);

    CHECK(drive_ready_directions(connection, clients[0],
                                 EPOLLIN | EPOLLOUT) == 0);
    CHECK(app.stats.bytes_client_to_upstream == sizeof(client_payload));
    CHECK(app.stats.bytes_upstream_to_client == sizeof(upstream_payload));
    CHECK(connection->directions[0].length == 0);
    CHECK(connection->directions[1].length == 0);

    CHECK(read_exact(upstreams[1], observed, sizeof(observed)) == 0);
    CHECK(memcmp(observed, client_payload, sizeof(observed)) == 0);
    CHECK(read_exact(clients[1], observed, sizeof(observed)) == 0);
    CHECK(memcmp(observed, upstream_payload, sizeof(observed)) == 0);

    free(connection);
    CHECK(close(clients[0]) == 0);
    CHECK(close(clients[1]) == 0);
    CHECK(close(upstreams[0]) == 0);
    CHECK(close(upstreams[1]) == 0);
    return 0;
}

static int test_half_close_after_buffer_drain(void)
{
    int clients[2] = {-1, -1};
    int upstreams[2] = {-1, -1};
    CHECK(create_socket_pair(clients) == 0);
    CHECK(create_socket_pair(upstreams) == 0);

    struct l4_app app = {0};
    struct l4_connection connection = {
        .app = &app,
        .client_fd = clients[0],
        .upstream_fd = upstreams[0],
        .upstream_state = L4_UPSTREAM_OPEN,
    };
    connection.directions[0].source_eof = true;

    CHECK(direction_finish(&connection, 0) == 0);
    CHECK(connection.directions[0].destination_shutdown);
    CHECK(app.stats.half_closes == 1);

    unsigned char byte = 0;
    CHECK(read(upstreams[1], &byte, sizeof(byte)) == 0);

    CHECK(close(clients[0]) == 0);
    CHECK(close(clients[1]) == 0);
    CHECK(close(upstreams[0]) == 0);
    CHECK(close(upstreams[1]) == 0);
    return 0;
}

static int test_unregistered_watch_noop_preserves_generation(void)
{
    int sockets[2] = {-1, -1};
    CHECK(create_socket_pair(sockets) == 0);

    struct l4_app app = {
        .epoll_fd = epoll_create1(EPOLL_CLOEXEC),
    };
    CHECK(app.epoll_fd >= 0);
    CHECK(initialize_watch_table(&app, (size_t)sockets[0] + 1) == 0);

    CHECK(watch_bind(&app, sockets[0], L4_FD_CLIENT, NULL) == 0);
    CHECK(watch_arm(&app, sockets[0], 0) == 0);
    struct l4_fd_watch *watch = watch_get(&app, sockets[0], false);
    CHECK(watch != NULL);
    CHECK(watch->generation == 0);
    CHECK(watch_arm(&app, sockets[0], EPOLLIN) == 0);
    CHECK(watch->generation == 1);
    struct epoll_event stale_event = {
        .events = EPOLLIN,
        .data.u64 = watch_token(sockets[0], watch->generation),
    };

    watch_advance_generation(watch);
    CHECK(watch->generation == 2);
    dispatch_event(&app, &stale_event);
    CHECK(watch->armed);
    CHECK(watch->generation == 2);

    watch_unbind(&app, sockets[0]);
    CHECK(watch->generation == 3);
    CHECK(close(sockets[0]) == 0);
    CHECK(close(sockets[1]) == 0);
    CHECK(close(app.epoll_fd) == 0);
    watch_table_destroy(&app);
    return 0;
}

static int test_watch_table_allocates_chunks_lazily(void)
{
    struct l4_app app = {0};
    CHECK(initialize_watch_table(&app, 1024 * 1024) == 0);
    CHECK(app.watch_chunk_count == 4096);
    for (size_t index = 0; index < app.watch_chunk_count; ++index) {
        CHECK(app.watch_chunks[index] == NULL);
    }

    CHECK(watch_get(&app, 17, true) != NULL);
    CHECK(app.watch_chunks[0] != NULL);
    for (size_t index = 1; index < app.watch_chunk_count; ++index) {
        CHECK(app.watch_chunks[index] == NULL);
    }

    watch_table_destroy(&app);
    return 0;
}

static int test_transient_accept_errors_preserve_established_sessions(void)
{
    int listener[2] = {-1, -1};
    CHECK(create_socket_pair(listener) == 0);

    struct l4_app app = {
        .options.max_connections = 2,
        .stats.active_connections = 1,
        .epoll_fd = epoll_create1(EPOLL_CLOEXEC),
        .listener_fd = listener[0],
    };
    CHECK(app.epoll_fd >= 0);
    CHECK(initialize_watch_table(&app, (size_t)listener[0] + 1) == 0);
    CHECK(watch_bind(&app, app.listener_fd, L4_FD_LISTENER, NULL) == 0);
    CHECK(watch_arm(&app, app.listener_fd, EPOLLIN) == 0);
    struct l4_fd_watch *listener_watch =
        watch_get(&app, app.listener_fd, false);
    CHECK(listener_watch != NULL);
    listener_watch->armed = false;

    const int results[] = {
        -ECONNABORTED,
        -ENETDOWN,
        -EPROTO,
        -ENOPROTOOPT,
        -EHOSTDOWN,
        -ENONET,
        -EHOSTUNREACH,
        -EOPNOTSUPP,
        -ENETUNREACH,
        -EAGAIN,
    };
    set_accept_results(results, sizeof(results) / sizeof(results[0]));
    handle_listener_event(&app);

    CHECK(test_accept_result_index ==
          sizeof(results) / sizeof(results[0]));
    CHECK(app.stats.rejected == 9);
    CHECK(app.stats.active_connections == 1);
    CHECK(app.fatal_error == 0);
    CHECK(!app.stopping);
    CHECK(listener_watch->armed);

    close_watched_fd(&app, &app.listener_fd);
    CHECK(close(listener[1]) == 0);
    CHECK(close(app.epoll_fd) == 0);
    watch_table_destroy(&app);
    return 0;
}

static int test_resource_accept_error_uses_bounded_retry(void)
{
    struct l4_app app = {
        .options.max_connections = 2,
        .stats.active_connections = 1,
        .listener_fd = 123,
    };
    const int errors[] = {EMFILE, ENFILE, ENOBUFS, ENOMEM};
    for (size_t index = 0;
         index < sizeof(errors) / sizeof(errors[0]);
         ++index) {
        int results[] = {-errors[index]};
        set_accept_results(results, sizeof(results) / sizeof(results[0]));
        app.listener_retry_deadline_ns = 0;
        handle_listener_event(&app);

        CHECK(test_accept_result_index == 1);
        CHECK(app.stats.rejected == index + 1);
        CHECK(app.stats.active_connections == 1);
        CHECK(app.listener_retry_deadline_ns != 0);
        CHECK(app.fatal_error == 0);
        CHECK(!app.stopping);
    }
    return 0;
}

static int test_fatal_accept_error_stops_forwarder(void)
{
    struct l4_app app = {
        .options.max_connections = 2,
        .stats.active_connections = 1,
        .listener_fd = 123,
    };
    const int results[] = {-EBADF};
    set_accept_results(results, sizeof(results) / sizeof(results[0]));
    handle_listener_event(&app);

    CHECK(test_accept_result_index == 1);
    CHECK(app.stats.rejected == 0);
    CHECK(app.stats.active_connections == 1);
    CHECK(app.fatal_error == -EBADF);
    CHECK(app.stopping);
    return 0;
}

static int test_capacity_rejects_accepted_socket_promptly(void)
{
    int listener[2] = {-1, -1};
    int excess[2] = {-1, -1};
    CHECK(create_socket_pair(listener) == 0);
    CHECK(create_socket_pair(excess) == 0);

    struct l4_app app = {
        .options.max_connections = 1,
        .stats.active_connections = 1,
        .epoll_fd = epoll_create1(EPOLL_CLOEXEC),
        .listener_fd = listener[0],
    };
    CHECK(app.epoll_fd >= 0);
    CHECK(initialize_watch_table(&app, (size_t)listener[0] + 1) == 0);
    CHECK(watch_bind(&app, app.listener_fd, L4_FD_LISTENER, NULL) == 0);
    CHECK(watch_arm(&app, app.listener_fd, EPOLLIN) == 0);
    struct l4_fd_watch *listener_watch =
        watch_get(&app, app.listener_fd, false);
    CHECK(listener_watch != NULL);
    listener_watch->armed = false;

    const int results[] = {excess[0]};
    set_accept_results(results, sizeof(results) / sizeof(results[0]));
    handle_listener_event(&app);

    CHECK(test_accept_result_index == 1);
    CHECK(app.stats.accepted == 1);
    CHECK(app.stats.rejected == 1);
    CHECK(app.stats.active_connections == 1);
    CHECK(app.listener_retry_deadline_ns != 0);
    CHECK(app.fatal_error == 0);
    CHECK(!app.stopping);
    errno = 0;
    CHECK(close(excess[0]) == -1);
    CHECK(errno == EBADF);

    close_watched_fd(&app, &app.listener_fd);
    CHECK(close(listener[1]) == 0);
    CHECK(close(excess[1]) == 0);
    CHECK(close(app.epoll_fd) == 0);
    watch_table_destroy(&app);
    return 0;
}

int main(void)
{
    CHECK(test_bidirectional_budgets_are_independent() == 0);
    CHECK(test_half_close_after_buffer_drain() == 0);
    CHECK(test_unregistered_watch_noop_preserves_generation() == 0);
    CHECK(test_watch_table_allocates_chunks_lazily() == 0);
    CHECK(test_transient_accept_errors_preserve_established_sessions() == 0);
    CHECK(test_resource_accept_error_uses_bounded_retry() == 0);
    CHECK(test_fatal_accept_error_stops_forwarder() == 0);
    CHECK(test_capacity_rejects_accepted_socket_promptly() == 0);
    puts("epoll L4 state-machine tests passed");
    return 0;
}
