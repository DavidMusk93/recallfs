#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define L4_ACCEPT_BUDGET 64
#define L4_DEFAULT_BUFFER_SIZE ((size_t)64 * 1024)
#define L4_DEFAULT_MAX_CONNECTIONS ((size_t)256)
#define L4_DEFAULT_CONNECT_TIMEOUT_MS 3000
#define L4_DEFAULT_GRACE_MS 30000
#define L4_EPOLL_BATCH 256
#define L4_IO_BUDGET_BYTES ((size_t)1024 * 1024)
#define L4_LISTENER_RETRY_MS 10

enum l4_fd_role {
    L4_FD_NONE = 0,
    L4_FD_LISTENER,
    L4_FD_SIGNAL,
    L4_FD_CLIENT,
    L4_FD_UPSTREAM,
};

enum l4_upstream_state {
    L4_UPSTREAM_CONNECTING = 0,
    L4_UPSTREAM_OPEN,
};

struct l4_options {
    const char *listen_host;
    const char *listen_port;
    const char *upstream_host;
    const char *upstream_port;
    size_t max_connections;
    size_t buffer_size;
    int connect_timeout_ms;
    int grace_ms;
    bool reuse_port;
    bool tcp_nodelay;
};

struct l4_stats {
    uint64_t accepted;
    uint64_t rejected;
    uint64_t connected;
    uint64_t connect_errors;
    uint64_t peer_resets;
    uint64_t io_errors;
    uint64_t half_closes;
    uint64_t bytes_client_to_upstream;
    uint64_t bytes_upstream_to_client;
    size_t active_connections;
    size_t peak_connections;
    bool forced_shutdown;
};

struct l4_app;
struct l4_connection;

struct l4_direction {
    unsigned char *data;
    size_t offset;
    size_t length;
    bool source_eof;
    bool destination_shutdown;
};

struct l4_connection {
    struct l4_app *app;
    struct l4_connection *previous;
    struct l4_connection *next;
    int client_fd;
    int upstream_fd;
    enum l4_upstream_state upstream_state;
    uint64_t connect_deadline_ns;
    size_t timer_index;
    unsigned next_direction;
    struct l4_direction directions[2];
    unsigned char buffers[];
};

struct l4_fd_watch {
    struct l4_connection *connection;
    uint32_t generation;
    uint32_t events;
    enum l4_fd_role role;
    bool registered;
    bool armed;
};

struct l4_app {
    struct sockaddr_storage upstream_address;
    socklen_t upstream_length;
    struct l4_options options;
    struct l4_stats stats;
    struct l4_fd_watch *watches;
    size_t watch_count;
    struct l4_connection **connect_heap;
    size_t connect_heap_count;
    struct l4_connection *connections;
    int epoll_fd;
    int listener_fd;
    int signal_fd;
    int fatal_error;
    uint64_t listener_retry_deadline_ns;
    uint64_t grace_deadline_ns;
    bool draining;
    bool stopping;
};

static int maybe_arm_listener(struct l4_app *app);
static void connection_close(struct l4_connection *connection);

static int negative_errno(void)
{
    return errno == 0 ? -EIO : -errno;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s --upstream-host ADDRESS --upstream-port PORT [options]\n"
            "\n"
            "Options:\n"
            "  --listen-host ADDRESS       Numeric bind address (default 127.0.0.1)\n"
            "  --listen-port PORT          Numeric listen port (default 9000)\n"
            "  --upstream-host ADDRESS     Required numeric backend address\n"
            "  --upstream-port PORT        Required numeric backend port\n"
            "  --max-connections N         Concurrent connection cap (default 256)\n"
            "  --buffer-size BYTES         Buffer per direction, 4K..1M (default 64K)\n"
            "  --connect-timeout-ms N      Backend connect deadline (default 3000)\n"
            "  --grace-ms N                SIGTERM drain deadline (default 30000)\n"
            "  --reuse-port                Enable SO_REUSEPORT\n"
            "  --tcp-nodelay               Enable TCP_NODELAY on data sockets\n"
            "  --help                      Show this text\n",
            program);
}

static int parse_size(const char *text,
                      size_t minimum,
                      size_t maximum,
                      size_t *out)
{
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) {
        return -EINVAL;
    }
    *out = (size_t)value;
    return 0;
}

static int parse_int(const char *text, int minimum, int maximum, int *out)
{
    size_t value = 0;
    int result = parse_size(text, (size_t)minimum, (size_t)maximum, &value);
    if (result == 0) {
        *out = (int)value;
    }
    return result;
}

static int parse_options(int argc, char **argv, struct l4_options *options)
{
    *options = (struct l4_options){
        .listen_host = "127.0.0.1",
        .listen_port = "9000",
        .max_connections = L4_DEFAULT_MAX_CONNECTIONS,
        .buffer_size = L4_DEFAULT_BUFFER_SIZE,
        .connect_timeout_ms = L4_DEFAULT_CONNECT_TIMEOUT_MS,
        .grace_ms = L4_DEFAULT_GRACE_MS,
    };

    for (int index = 1; index < argc; ++index) {
        const char *option = argv[index];
        if (strcmp(option, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 1;
        }
        if (strcmp(option, "--reuse-port") == 0) {
            options->reuse_port = true;
            continue;
        }
        if (strcmp(option, "--tcp-nodelay") == 0) {
            options->tcp_nodelay = true;
            continue;
        }
        if (index + 1 >= argc) {
            return -EINVAL;
        }
        const char *value = argv[++index];
        if (strcmp(option, "--listen-host") == 0) {
            options->listen_host = value;
        } else if (strcmp(option, "--listen-port") == 0) {
            options->listen_port = value;
        } else if (strcmp(option, "--upstream-host") == 0) {
            options->upstream_host = value;
        } else if (strcmp(option, "--upstream-port") == 0) {
            options->upstream_port = value;
        } else if (strcmp(option, "--max-connections") == 0) {
            if (parse_size(value, 1, 100000, &options->max_connections) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(option, "--buffer-size") == 0) {
            if (parse_size(value, 4096, 1024 * 1024,
                           &options->buffer_size) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(option, "--connect-timeout-ms") == 0) {
            if (parse_int(value, 1, 600000,
                          &options->connect_timeout_ms) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(option, "--grace-ms") == 0) {
            if (parse_int(value, 0, 600000, &options->grace_ms) != 0) {
                return -EINVAL;
            }
        } else {
            return -EINVAL;
        }
    }

    if (options->upstream_host == NULL || options->upstream_port == NULL ||
        options->buffer_size > SIZE_MAX / 2 ||
        options->max_connections >
            SIZE_MAX / (2 * options->buffer_size)) {
        return -EINVAL;
    }
    return 0;
}

static int set_socket_options(int fd, bool tcp_nodelay)
{
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) !=
        0) {
        return negative_errno();
    }
    if (tcp_nodelay &&
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) !=
            0) {
        return negative_errno();
    }
    return 0;
}

static int resolve_upstream(struct l4_app *app)
{
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV,
    };
    struct addrinfo *addresses = NULL;
    int result = getaddrinfo(app->options.upstream_host,
                             app->options.upstream_port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "upstream address: %s\n", gai_strerror(result));
        return -EINVAL;
    }

    int status = -EAFNOSUPPORT;
    for (const struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) {
        if (address->ai_addrlen > sizeof(app->upstream_address)) {
            continue;
        }
        memcpy(&app->upstream_address, address->ai_addr, address->ai_addrlen);
        app->upstream_length = (socklen_t)address->ai_addrlen;
        status = 0;
        break;
    }
    freeaddrinfo(addresses);
    return status;
}

static int create_listener(const struct l4_options *options)
{
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV,
    };
    struct addrinfo *addresses = NULL;
    int result = getaddrinfo(options->listen_host, options->listen_port,
                             &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "listen address: %s\n", gai_strerror(result));
        return -EINVAL;
    }

    int listener = -1;
    int last_error = -EADDRNOTAVAIL;
    for (const struct addrinfo *address = addresses; address != NULL;
         address = address->ai_next) {
        listener = socket(address->ai_family,
                          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          address->ai_protocol);
        if (listener < 0) {
            last_error = negative_errno();
            continue;
        }

        int enabled = 1;
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                       sizeof(enabled)) != 0 ||
            (options->reuse_port &&
             setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &enabled,
                        sizeof(enabled)) != 0)) {
            last_error = negative_errno();
            (void)close(listener);
            listener = -1;
            continue;
        }
        if (bind(listener, address->ai_addr, address->ai_addrlen) != 0 ||
            listen(listener, SOMAXCONN) != 0) {
            last_error = negative_errno();
            (void)close(listener);
            listener = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);
    return listener < 0 ? last_error : listener;
}

static int create_signal_fd(void)
{
    sigset_t mask;
    if (sigemptyset(&mask) != 0 ||
        sigaddset(&mask, SIGINT) != 0 ||
        sigaddset(&mask, SIGTERM) != 0 ||
        sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        return negative_errno();
    }
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    return fd < 0 ? negative_errno() : fd;
}

static size_t app_max_fds(size_t max_connections)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return 0;
    }
    size_t required =
        max_connections > (SIZE_MAX - 64) / 2
            ? SIZE_MAX
            : max_connections * 2 + 64;
    if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < required) {
        return 0;
    }
    size_t maximum = limit.rlim_cur == RLIM_INFINITY
                         ? (size_t)1024 * 1024
                         : (size_t)limit.rlim_cur;
    return maximum > INT32_MAX ? INT32_MAX : maximum;
}

static int monotonic_now_ns(uint64_t *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return negative_errno();
    }
    *out = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    return 0;
}

static uint64_t deadline_after_ms(uint64_t now, int milliseconds)
{
    uint64_t delta = (uint64_t)(unsigned)milliseconds * 1000000ULL;
    return now > UINT64_MAX - delta ? UINT64_MAX : now + delta;
}

static void app_fail(struct l4_app *app, int error)
{
    if (app->fatal_error == 0) {
        app->fatal_error = error == 0 ? -EIO : error;
    }
    app->stopping = true;
}

static void watch_advance_generation(struct l4_fd_watch *watch)
{
    watch->generation++;
    if (watch->generation == 0) {
        watch->generation = 1;
    }
}

static uint64_t watch_token(int fd, uint32_t generation)
{
    return ((uint64_t)generation << 32) | (uint32_t)fd;
}

static int watch_bind(struct l4_app *app,
                      int fd,
                      enum l4_fd_role role,
                      struct l4_connection *connection)
{
    if (fd < 0 || (size_t)fd >= app->watch_count) {
        return -EMFILE;
    }
    struct l4_fd_watch *watch = &app->watches[fd];
    if (watch->role != L4_FD_NONE || watch->registered) {
        return -EBUSY;
    }
    watch->role = role;
    watch->connection = connection;
    return 0;
}

static int watch_arm(struct l4_app *app, int fd, uint32_t events)
{
    if (fd < 0 || (size_t)fd >= app->watch_count) {
        return -EINVAL;
    }
    struct l4_fd_watch *watch = &app->watches[fd];
    if (watch->role == L4_FD_NONE) {
        return -EINVAL;
    }

    if (events == 0) {
        if (watch->registered &&
            epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL) != 0 &&
            errno != ENOENT && errno != EBADF) {
            return negative_errno();
        }
        watch->registered = false;
        watch->armed = false;
        watch->events = 0;
        watch_advance_generation(watch);
        return 0;
    }
    if (watch->armed && watch->events == events) {
        return 0;
    }

    watch_advance_generation(watch);
    struct epoll_event event = {
        .events = events | EPOLLONESHOT,
        .data.u64 = watch_token(fd, watch->generation),
    };
    int operation = watch->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(app->epoll_fd, operation, fd, &event) != 0) {
        return negative_errno();
    }
    watch->registered = true;
    watch->armed = true;
    watch->events = events;
    return 0;
}

static void watch_unbind(struct l4_app *app, int fd)
{
    if (fd < 0 || (size_t)fd >= app->watch_count) {
        return;
    }
    struct l4_fd_watch *watch = &app->watches[fd];
    if (watch->registered) {
        (void)epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    }
    watch->connection = NULL;
    watch->events = 0;
    watch->role = L4_FD_NONE;
    watch->registered = false;
    watch->armed = false;
    watch_advance_generation(watch);
}

static void close_watched_fd(struct l4_app *app, int *fd)
{
    if (*fd < 0) {
        return;
    }
    watch_unbind(app, *fd);
    (void)close(*fd);
    *fd = -1;
}

static void heap_swap(struct l4_app *app, size_t first, size_t second)
{
    struct l4_connection *temporary = app->connect_heap[first];
    app->connect_heap[first] = app->connect_heap[second];
    app->connect_heap[second] = temporary;
    app->connect_heap[first]->timer_index = first;
    app->connect_heap[second]->timer_index = second;
}

static bool heap_less(const struct l4_connection *left,
                      const struct l4_connection *right)
{
    return left->connect_deadline_ns < right->connect_deadline_ns;
}

static void heap_sift_up(struct l4_app *app, size_t index)
{
    while (index != 0) {
        size_t parent = (index - 1) / 2;
        if (!heap_less(app->connect_heap[index],
                       app->connect_heap[parent])) {
            break;
        }
        heap_swap(app, index, parent);
        index = parent;
    }
}

static void heap_sift_down(struct l4_app *app, size_t index)
{
    for (;;) {
        size_t left = index * 2 + 1;
        if (left >= app->connect_heap_count) {
            break;
        }
        size_t right = left + 1;
        size_t smallest =
            right < app->connect_heap_count &&
                    heap_less(app->connect_heap[right],
                              app->connect_heap[left])
                ? right
                : left;
        if (!heap_less(app->connect_heap[smallest],
                       app->connect_heap[index])) {
            break;
        }
        heap_swap(app, index, smallest);
        index = smallest;
    }
}

static void heap_insert(struct l4_app *app,
                        struct l4_connection *connection)
{
    size_t index = app->connect_heap_count++;
    app->connect_heap[index] = connection;
    connection->timer_index = index;
    heap_sift_up(app, index);
}

static void heap_remove(struct l4_app *app,
                        struct l4_connection *connection)
{
    if (connection->timer_index == SIZE_MAX) {
        return;
    }

    size_t index = connection->timer_index;
    size_t last = --app->connect_heap_count;
    connection->timer_index = SIZE_MAX;
    if (index == last) {
        app->connect_heap[last] = NULL;
        return;
    }

    app->connect_heap[index] = app->connect_heap[last];
    app->connect_heap[last] = NULL;
    app->connect_heap[index]->timer_index = index;
    if (index != 0 &&
        heap_less(app->connect_heap[index],
                  app->connect_heap[(index - 1) / 2])) {
        heap_sift_up(app, index);
    } else {
        heap_sift_down(app, index);
    }
}

static struct l4_connection *connection_create(struct l4_app *app,
                                                int client_fd)
{
    size_t buffer_bytes = app->options.buffer_size * 2;
    if (sizeof(struct l4_connection) > SIZE_MAX - buffer_bytes) {
        return NULL;
    }
    struct l4_connection *connection =
        malloc(sizeof(*connection) + buffer_bytes);
    if (connection == NULL) {
        return NULL;
    }
    memset(connection, 0, sizeof(*connection));
    connection->app = app;
    connection->client_fd = client_fd;
    connection->upstream_fd = -1;
    connection->timer_index = SIZE_MAX;
    connection->directions[0].data = connection->buffers;
    connection->directions[1].data =
        connection->buffers + app->options.buffer_size;
    return connection;
}

static void connection_link(struct l4_connection *connection)
{
    struct l4_app *app = connection->app;
    connection->next = app->connections;
    if (connection->next != NULL) {
        connection->next->previous = connection;
    }
    app->connections = connection;
    app->stats.active_connections++;
    if (app->stats.active_connections > app->stats.peak_connections) {
        app->stats.peak_connections = app->stats.active_connections;
    }
}

static void connection_close(struct l4_connection *connection)
{
    struct l4_app *app = connection->app;
    heap_remove(app, connection);
    close_watched_fd(app, &connection->client_fd);
    close_watched_fd(app, &connection->upstream_fd);

    if (connection->previous != NULL) {
        connection->previous->next = connection->next;
    } else {
        app->connections = connection->next;
    }
    if (connection->next != NULL) {
        connection->next->previous = connection->previous;
    }
    app->stats.active_connections--;

    if (app->draining && app->stats.active_connections == 0) {
        app->stopping = true;
    } else if (!app->stopping) {
        int result = maybe_arm_listener(app);
        if (result != 0) {
            app_fail(app, result);
        }
    }
    free(connection);
}

static int direction_source_fd(const struct l4_connection *connection,
                               unsigned direction)
{
    return direction == 0 ? connection->client_fd : connection->upstream_fd;
}

static int direction_destination_fd(const struct l4_connection *connection,
                                    unsigned direction)
{
    return direction == 0 ? connection->upstream_fd : connection->client_fd;
}

static bool direction_source_is_pending(
    const struct l4_connection *connection,
    unsigned direction)
{
    return connection->upstream_state != L4_UPSTREAM_OPEN &&
           direction_source_fd(connection, direction) ==
               connection->upstream_fd;
}

static int direction_finish(struct l4_connection *connection,
                            unsigned direction)
{
    struct l4_direction *state = &connection->directions[direction];
    if (!state->source_eof || state->length != 0 ||
        state->destination_shutdown) {
        return 0;
    }
    if (direction_destination_fd(connection, direction) ==
            connection->upstream_fd &&
        connection->upstream_state != L4_UPSTREAM_OPEN) {
        return 0;
    }

    int destination = direction_destination_fd(connection, direction);
    if (shutdown(destination, SHUT_WR) != 0 &&
        errno != ENOTCONN && errno != EPIPE) {
        return negative_errno();
    }
    state->destination_shutdown = true;
    connection->app->stats.half_closes++;
    return 0;
}

static int drive_direction(struct l4_connection *connection,
                           unsigned direction,
                           size_t *budget,
                           bool read_ready,
                           bool write_ready)
{
    struct l4_app *app = connection->app;
    struct l4_direction *state = &connection->directions[direction];

    for (;;) {
        if (state->length != 0) {
            if (direction_destination_fd(connection, direction) ==
                    connection->upstream_fd &&
                connection->upstream_state != L4_UPSTREAM_OPEN) {
                return 0;
            }
            if (!write_ready || *budget == 0) {
                return 0;
            }

            size_t count = state->length < *budget ? state->length : *budget;
            ssize_t written =
                send(direction_destination_fd(connection, direction),
                     state->data + state->offset, count, MSG_NOSIGNAL);
            if (written > 0) {
                size_t bytes = (size_t)written;
                state->offset += bytes;
                state->length -= bytes;
                *budget -= bytes;
                if (direction == 0) {
                    app->stats.bytes_client_to_upstream += (uint64_t)bytes;
                } else {
                    app->stats.bytes_upstream_to_client += (uint64_t)bytes;
                }
                if (state->length == 0) {
                    state->offset = 0;
                    int result = direction_finish(connection, direction);
                    if (result != 0) {
                        return result;
                    }
                }
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 0;
            }
            return written == 0 ? -EPIPE : negative_errno();
        }

        int result = direction_finish(connection, direction);
        if (result != 0 || state->source_eof) {
            return result;
        }
        if (direction_source_is_pending(connection, direction) ||
            !read_ready || *budget == 0) {
            return 0;
        }

        size_t count =
            app->options.buffer_size < *budget
                ? app->options.buffer_size
                : *budget;
        ssize_t received =
            recv(direction_source_fd(connection, direction),
                 state->data, count, 0);
        if (received > 0) {
            size_t bytes = (size_t)received;
            state->offset = 0;
            state->length = bytes;
            *budget -= bytes;
            write_ready = true;
            continue;
        }
        if (received == 0) {
            state->source_eof = true;
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return negative_errno();
    }
}

static bool connection_done(const struct l4_connection *connection)
{
    return connection->directions[0].destination_shutdown &&
           connection->directions[1].destination_shutdown;
}

static void record_io_error(struct l4_app *app, int error)
{
    if (error == -ECONNRESET || error == -EPIPE || error == -ENOTCONN) {
        app->stats.peer_resets++;
    } else {
        app->stats.io_errors++;
    }
}

static void connection_interest(const struct l4_connection *connection,
                                int fd,
                                uint32_t *events)
{
    uint32_t result = 0;
    for (unsigned direction = 0; direction < 2; ++direction) {
        const struct l4_direction *state =
            &connection->directions[direction];
        int source = direction_source_fd(connection, direction);
        int destination =
            direction_destination_fd(connection, direction);
        if (source == fd && !state->source_eof && state->length == 0 &&
            !(source == connection->upstream_fd &&
              connection->upstream_state != L4_UPSTREAM_OPEN)) {
            result |= EPOLLIN;
        }
        if (destination == fd && state->length != 0 &&
            !(destination == connection->upstream_fd &&
              connection->upstream_state != L4_UPSTREAM_OPEN)) {
            result |= EPOLLOUT;
        }
    }
    if (fd == connection->upstream_fd &&
        connection->upstream_state == L4_UPSTREAM_CONNECTING) {
        result |= EPOLLOUT;
    }
    if (result != 0) {
        result |= EPOLLRDHUP;
    }
    *events = result;
}

static int connection_sync(struct l4_connection *connection)
{
    uint32_t client_events = 0;
    uint32_t upstream_events = 0;
    connection_interest(connection, connection->client_fd, &client_events);
    connection_interest(connection, connection->upstream_fd,
                        &upstream_events);
    int result =
        watch_arm(connection->app, connection->client_fd, client_events);
    if (result != 0) {
        return result;
    }
    return watch_arm(connection->app, connection->upstream_fd,
                     upstream_events);
}

static int finish_connect(struct l4_connection *connection)
{
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (getsockopt(connection->upstream_fd, SOL_SOCKET, SO_ERROR,
                   &socket_error, &length) != 0) {
        return negative_errno();
    }
    if (socket_error != 0) {
        return -socket_error;
    }
    heap_remove(connection->app, connection);
    connection->upstream_state = L4_UPSTREAM_OPEN;
    connection->app->stats.connected++;
    return 0;
}

static void handle_connection_event(struct l4_connection *connection,
                                    int event_fd,
                                    uint32_t events)
{
    struct l4_app *app = connection->app;
    if (event_fd == connection->upstream_fd &&
        connection->upstream_state == L4_UPSTREAM_CONNECTING) {
        int result = finish_connect(connection);
        if (result != 0) {
            app->stats.connect_errors++;
            connection_close(connection);
            return;
        }
    }

    size_t budget = L4_IO_BUDGET_BYTES;
    uint32_t read_events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    uint32_t write_events = EPOLLOUT | EPOLLHUP | EPOLLERR;
    for (unsigned turn = 0; turn < 2; ++turn) {
        unsigned direction = (connection->next_direction + turn) % 2;
        bool read_ready =
            direction_source_fd(connection, direction) == event_fd &&
            (events & read_events) != 0;
        bool write_ready =
            direction_destination_fd(connection, direction) == event_fd &&
            (events & write_events) != 0;
        int result = drive_direction(connection, direction, &budget,
                                     read_ready, write_ready);
        if (result != 0) {
            record_io_error(app, result);
            connection_close(connection);
            return;
        }
    }
    connection->next_direction ^= 1U;

    if (connection_done(connection)) {
        connection_close(connection);
        return;
    }
    int result = connection_sync(connection);
    if (result != 0) {
        connection_close(connection);
        app_fail(app, result);
    }
}

static int connection_start(struct l4_connection *connection,
                            bool *infrastructure_error)
{
    struct l4_app *app = connection->app;
    int family = ((struct sockaddr *)&app->upstream_address)->sa_family;
    int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return negative_errno();
    }
    connection->upstream_fd = fd;

    int result = set_socket_options(fd, app->options.tcp_nodelay);
    if (result != 0) {
        return result;
    }
    if (connect(fd, (struct sockaddr *)&app->upstream_address,
                app->upstream_length) == 0) {
        connection->upstream_state = L4_UPSTREAM_OPEN;
        app->stats.connected++;
    } else if (errno == EINPROGRESS) {
        uint64_t now = 0;
        result = monotonic_now_ns(&now);
        if (result != 0) {
            *infrastructure_error = true;
            return result;
        }
        connection->upstream_state = L4_UPSTREAM_CONNECTING;
        connection->connect_deadline_ns =
            deadline_after_ms(now, app->options.connect_timeout_ms);
        heap_insert(app, connection);
    } else {
        return negative_errno();
    }

    result = watch_bind(app, connection->client_fd, L4_FD_CLIENT,
                        connection);
    if (result == 0) {
        result = watch_bind(app, connection->upstream_fd, L4_FD_UPSTREAM,
                            connection);
    }
    if (result == 0) {
        result = connection_sync(connection);
    }
    if (result != 0) {
        *infrastructure_error = true;
    }
    return result;
}

static int maybe_arm_listener(struct l4_app *app)
{
    if (app->stopping || app->draining || app->listener_fd < 0 ||
        app->stats.active_connections >= app->options.max_connections ||
        app->listener_retry_deadline_ns != 0) {
        return 0;
    }
    return watch_arm(app, app->listener_fd, EPOLLIN);
}

static void handle_listener_event(struct l4_app *app)
{
    size_t accepted_this_event = 0;
    while (!app->stopping && !app->draining &&
           accepted_this_event < L4_ACCEPT_BUDGET &&
           app->stats.active_connections < app->options.max_connections) {
        int client = accept4(app->listener_fd, NULL, NULL,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EMFILE || errno == ENFILE) {
                uint64_t now = 0;
                int result = monotonic_now_ns(&now);
                if (result != 0) {
                    app_fail(app, result);
                    return;
                }
                app->stats.rejected++;
                app->listener_retry_deadline_ns =
                    deadline_after_ms(now, L4_LISTENER_RETRY_MS);
                return;
            }
            app_fail(app, negative_errno());
            return;
        }
        accepted_this_event++;
        app->stats.accepted++;

        if (set_socket_options(client, app->options.tcp_nodelay) != 0) {
            app->stats.rejected++;
            (void)close(client);
            continue;
        }
        struct l4_connection *connection =
            connection_create(app, client);
        if (connection == NULL) {
            app->stats.rejected++;
            (void)close(client);
            continue;
        }
        connection_link(connection);

        bool infrastructure_error = false;
        int result =
            connection_start(connection, &infrastructure_error);
        if (result != 0) {
            if (infrastructure_error) {
                connection_close(connection);
                app_fail(app, result);
                return;
            }
            app->stats.connect_errors++;
            connection_close(connection);
        }
    }

    int result = maybe_arm_listener(app);
    if (result != 0) {
        app_fail(app, result);
    }
}

static int read_signal(int fd)
{
    struct signalfd_siginfo signal_info;
    for (;;) {
        ssize_t count = read(fd, &signal_info, sizeof(signal_info));
        if (count == (ssize_t)sizeof(signal_info)) {
            return (int)signal_info.ssi_signo;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        return count < 0 ? negative_errno() : -EIO;
    }
}

static void start_draining(struct l4_app *app)
{
    app->draining = true;
    close_watched_fd(app, &app->listener_fd);
    if (app->stats.active_connections == 0) {
        app->stopping = true;
        return;
    }

    uint64_t now = 0;
    int result = monotonic_now_ns(&now);
    if (result != 0) {
        app_fail(app, result);
        return;
    }
    app->grace_deadline_ns =
        deadline_after_ms(now, app->options.grace_ms);
}

static void handle_signal_event(struct l4_app *app)
{
    int received_signal = read_signal(app->signal_fd);
    if (received_signal < 0) {
        app_fail(app, received_signal);
        return;
    }
    if (received_signal != 0) {
        if (app->draining) {
            app->stats.forced_shutdown = true;
            app->stopping = true;
            return;
        }
        start_draining(app);
    }
    if (!app->stopping) {
        int result = watch_arm(app, app->signal_fd, EPOLLIN);
        if (result != 0) {
            app_fail(app, result);
        }
    }
}

static void process_deadlines(struct l4_app *app)
{
    if (app->connect_heap_count == 0 &&
        app->listener_retry_deadline_ns == 0 &&
        (!app->draining || app->stats.active_connections == 0)) {
        return;
    }

    uint64_t now = 0;
    int result = monotonic_now_ns(&now);
    if (result != 0) {
        app_fail(app, result);
        return;
    }

    while (app->connect_heap_count != 0) {
        struct l4_connection *connection = app->connect_heap[0];
        if (connection->connect_deadline_ns > now) {
            break;
        }
        heap_remove(app, connection);
        app->stats.connect_errors++;
        connection_close(connection);
    }

    if (app->listener_retry_deadline_ns != 0 &&
        app->listener_retry_deadline_ns <= now) {
        app->listener_retry_deadline_ns = 0;
        result = maybe_arm_listener(app);
        if (result != 0) {
            app_fail(app, result);
            return;
        }
    }

    if (app->draining && app->stats.active_connections != 0 &&
        app->grace_deadline_ns <= now) {
        app->stats.forced_shutdown = true;
        app->stopping = true;
    }
}

static int next_timeout_ms(struct l4_app *app)
{
    uint64_t deadline = UINT64_MAX;
    if (app->connect_heap_count != 0) {
        deadline = app->connect_heap[0]->connect_deadline_ns;
    }
    if (app->listener_retry_deadline_ns != 0 &&
        app->listener_retry_deadline_ns < deadline) {
        deadline = app->listener_retry_deadline_ns;
    }
    if (app->draining && app->stats.active_connections != 0 &&
        app->grace_deadline_ns < deadline) {
        deadline = app->grace_deadline_ns;
    }
    if (deadline == UINT64_MAX) {
        return -1;
    }

    uint64_t now = 0;
    int result = monotonic_now_ns(&now);
    if (result != 0) {
        app_fail(app, result);
        return 0;
    }
    if (deadline <= now) {
        return 0;
    }
    uint64_t remaining = deadline - now;
    uint64_t milliseconds = (remaining + 999999ULL) / 1000000ULL;
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static void dispatch_event(struct l4_app *app,
                           const struct epoll_event *event)
{
    int fd = (int)(uint32_t)event->data.u64;
    uint32_t generation = (uint32_t)(event->data.u64 >> 32);
    if (fd < 0 || (size_t)fd >= app->watch_count) {
        return;
    }
    struct l4_fd_watch *watch = &app->watches[fd];
    if (!watch->registered || !watch->armed ||
        watch->generation != generation) {
        return;
    }

    enum l4_fd_role role = watch->role;
    struct l4_connection *connection = watch->connection;
    watch->armed = false;
    if (role == L4_FD_LISTENER) {
        handle_listener_event(app);
    } else if (role == L4_FD_SIGNAL) {
        handle_signal_event(app);
    } else if ((role == L4_FD_CLIENT || role == L4_FD_UPSTREAM) &&
               connection != NULL) {
        handle_connection_event(connection, fd, event->events);
    }
}

static int run_event_loop(struct l4_app *app)
{
    struct epoll_event events[L4_EPOLL_BATCH];
    while (!app->stopping) {
        process_deadlines(app);
        if (app->stopping) {
            break;
        }

        int timeout = next_timeout_ms(app);
        int count =
            epoll_wait(app->epoll_fd, events, L4_EPOLL_BATCH, timeout);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            app_fail(app, negative_errno());
            break;
        }
        for (int index = 0; index < count && !app->stopping; ++index) {
            dispatch_event(app, &events[index]);
        }
    }
    return app->fatal_error;
}

static void print_summary(const struct l4_app *app)
{
    printf("accepted=%" PRIu64 " rejected=%" PRIu64
           " connected=%" PRIu64 " connect_errors=%" PRIu64
           " peer_resets=%" PRIu64 " io_errors=%" PRIu64
           " half_closes=%" PRIu64
           " bytes_client_to_upstream=%" PRIu64
           " bytes_upstream_to_client=%" PRIu64
           " peak_connections=%zu forced_shutdown=%s\n",
           app->stats.accepted, app->stats.rejected, app->stats.connected,
           app->stats.connect_errors, app->stats.peer_resets,
           app->stats.io_errors, app->stats.half_closes,
           app->stats.bytes_client_to_upstream,
           app->stats.bytes_upstream_to_client,
           app->stats.peak_connections,
           app->stats.forced_shutdown ? "true" : "false");
}

static void close_all_connections(struct l4_app *app)
{
    app->stopping = true;
    struct l4_connection *connection = app->connections;
    while (connection != NULL) {
        struct l4_connection *next = connection->next;
        connection_close(connection);
        connection = next;
    }
}

int main(int argc, char **argv)
{
    struct l4_app app = {
        .epoll_fd = -1,
        .listener_fd = -1,
        .signal_fd = -1,
    };
    int result = parse_options(argc, argv, &app.options);
    if (result == 1) {
        return EXIT_SUCCESS;
    }
    if (result != 0) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    result = resolve_upstream(&app);
    if (result != 0) {
        return EXIT_FAILURE;
    }
    app.listener_fd = create_listener(&app.options);
    if (app.listener_fd < 0) {
        fprintf(stderr, "listener setup failed: %s\n",
                strerror(-app.listener_fd));
        return EXIT_FAILURE;
    }
    app.signal_fd = create_signal_fd();
    if (app.signal_fd < 0) {
        fprintf(stderr, "signalfd setup failed: %s\n", strerror(-app.signal_fd));
        (void)close(app.listener_fd);
        return EXIT_FAILURE;
    }

    app.watch_count = app_max_fds(app.options.max_connections);
    if (app.watch_count == 0 ||
        app.watch_count > SIZE_MAX / sizeof(*app.watches)) {
        fputs("RLIMIT_NOFILE is too small for max-connections\n", stderr);
        (void)close(app.signal_fd);
        (void)close(app.listener_fd);
        return EXIT_FAILURE;
    }
    app.watches = calloc(app.watch_count, sizeof(*app.watches));
    app.connect_heap =
        calloc(app.options.max_connections, sizeof(*app.connect_heap));
    if (app.watches == NULL || app.connect_heap == NULL) {
        fputs("forwarder state allocation failed\n", stderr);
        free(app.connect_heap);
        free(app.watches);
        (void)close(app.signal_fd);
        (void)close(app.listener_fd);
        return EXIT_FAILURE;
    }

    app.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (app.epoll_fd < 0) {
        fprintf(stderr, "epoll setup failed: %s\n", strerror(errno));
        free(app.connect_heap);
        free(app.watches);
        (void)close(app.signal_fd);
        (void)close(app.listener_fd);
        return EXIT_FAILURE;
    }
    result = watch_bind(&app, app.listener_fd, L4_FD_LISTENER, NULL);
    if (result == 0) {
        result = watch_bind(&app, app.signal_fd, L4_FD_SIGNAL, NULL);
    }
    if (result == 0) {
        result = maybe_arm_listener(&app);
    }
    if (result == 0) {
        result = watch_arm(&app, app.signal_fd, EPOLLIN);
    }
    if (result != 0) {
        fprintf(stderr, "event registration failed: %s\n", strerror(-result));
        close_watched_fd(&app, &app.signal_fd);
        close_watched_fd(&app, &app.listener_fd);
        (void)close(app.epoll_fd);
        free(app.connect_heap);
        free(app.watches);
        return EXIT_FAILURE;
    }

    printf("listening=%s:%s upstream=%s:%s max_connections=%zu"
           " buffer_size=%zu\n",
           app.options.listen_host, app.options.listen_port,
           app.options.upstream_host, app.options.upstream_port,
           app.options.max_connections, app.options.buffer_size);
    fflush(stdout);

    result = run_event_loop(&app);
    close_all_connections(&app);
    close_watched_fd(&app, &app.listener_fd);
    close_watched_fd(&app, &app.signal_fd);
    (void)close(app.epoll_fd);
    print_summary(&app);
    free(app.connect_heap);
    free(app.watches);

    if (result != 0 || app.fatal_error != 0) {
        fprintf(stderr, "forwarder failed: app=%d\n", app.fatal_error);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
