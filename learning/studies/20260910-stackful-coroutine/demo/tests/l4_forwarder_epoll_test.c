#define L4_IO_BUDGET_BYTES ((size_t)4096)
#define main l4_forwarder_epoll_program_main
#include "../examples/l4_forwarder_epoll.c"
#undef main

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
        .watch_count = (size_t)sockets[0] + 1,
    };
    CHECK(app.epoll_fd >= 0);
    app.watches = calloc(app.watch_count, sizeof(*app.watches));
    CHECK(app.watches != NULL);

    CHECK(watch_bind(&app, sockets[0], L4_FD_CLIENT, NULL) == 0);
    CHECK(watch_arm(&app, sockets[0], 0) == 0);
    CHECK(app.watches[sockets[0]].generation == 0);
    CHECK(watch_arm(&app, sockets[0], EPOLLIN) == 0);
    CHECK(app.watches[sockets[0]].generation == 1);

    watch_unbind(&app, sockets[0]);
    CHECK(app.watches[sockets[0]].generation == 2);
    CHECK(close(sockets[0]) == 0);
    CHECK(close(sockets[1]) == 0);
    CHECK(close(app.epoll_fd) == 0);
    free(app.watches);
    return 0;
}

int main(void)
{
    CHECK(test_bidirectional_budgets_are_independent() == 0);
    CHECK(test_half_close_after_buffer_drain() == 0);
    CHECK(test_unregistered_watch_noop_preserves_generation() == 0);
    puts("epoll L4 state-machine tests passed");
    return 0;
}
