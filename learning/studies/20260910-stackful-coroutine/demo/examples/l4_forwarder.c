#define _GNU_SOURCE

#include "rco.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define L4_ACCEPT_BUDGET 64
#define L4_DEFAULT_BUFFER_SIZE ((size_t)64 * 1024)
#define L4_DEFAULT_MAX_CONNECTIONS ((size_t)256)
#define L4_DEFAULT_CONNECT_TIMEOUT_MS 3000
#define L4_DEFAULT_GRACE_MS 30000
#define L4_IO_BUDGET_BYTES ((size_t)1024 * 1024)

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

struct l4_pump {
    struct l4_connection *connection;
    int source_fd;
    int destination_fd;
    bool client_to_upstream;
    unsigned char *buffer;
};

struct l4_connection {
    struct l4_app *app;
    int client_fd;
    int upstream_fd;
    size_t references;
    bool aborted;
    struct l4_pump pumps[2];
    unsigned char buffers[];
};

struct l4_app {
    struct rco_runtime *runtime;
    struct sockaddr_storage upstream_address;
    socklen_t upstream_length;
    struct l4_options options;
    struct l4_stats stats;
    int listener_fd;
    int signal_fd;
    int fatal_error;
    bool draining;
};

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
            "  --listen-host ADDRESS       Numeric bind address (default 0.0.0.0)\n"
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
        .listen_host = "0.0.0.0",
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

static void app_fail(struct l4_app *app, int error)
{
    if (app->fatal_error == 0) {
        app->fatal_error = error == 0 ? -EIO : error;
    }
    (void)rco_runtime_stop(app->runtime);
}

static void connection_abort(struct l4_connection *connection)
{
    if (connection->aborted) {
        return;
    }
    connection->aborted = true;
    if (connection->client_fd >= 0) {
        (void)shutdown(connection->client_fd, SHUT_RDWR);
    }
    if (connection->upstream_fd >= 0) {
        (void)shutdown(connection->upstream_fd, SHUT_RDWR);
    }
}

static void connection_retain(struct l4_connection *connection)
{
    connection->references++;
}

static void connection_release(struct l4_connection *connection)
{
    if (--connection->references != 0) {
        return;
    }

    struct l4_app *app = connection->app;
    if (connection->client_fd >= 0) {
        (void)rco_close_fd(connection->client_fd);
        connection->client_fd = -1;
    }
    if (connection->upstream_fd >= 0) {
        (void)rco_close_fd(connection->upstream_fd);
        connection->upstream_fd = -1;
    }
    app->stats.active_connections--;
    free(connection);

    if (app->draining && app->stats.active_connections == 0) {
        (void)rco_runtime_stop(app->runtime);
    }
}

static void session_finalizer(void *argument)
{
    connection_release(argument);
}

static void pump_finalizer(void *argument)
{
    struct l4_pump *pump = argument;
    connection_release(pump->connection);
}

static int wait_for_event(int fd, unsigned events, int timeout_ms)
{
    unsigned ready = 0;
    int result = rco_wait_fd(fd, events, timeout_ms, &ready);
    if (result != 0) {
        return result;
    }
    return (ready & events) == 0 ? -EIO : 0;
}

static int connect_upstream(struct l4_connection *connection)
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
        return 0;
    }
    if (errno != EINPROGRESS) {
        return negative_errno();
    }

    result =
        wait_for_event(fd, RCO_EVENT_WRITE, app->options.connect_timeout_ms);
    if (result != 0) {
        return result;
    }

    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
        return negative_errno();
    }
    return socket_error == 0 ? 0 : -socket_error;
}

static int send_all(int fd,
                    const unsigned char *buffer,
                    size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t written =
            send(fd, buffer + offset, length - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && errno == EAGAIN) {
            int result = wait_for_event(fd, RCO_EVENT_WRITE, -1);
            if (result != 0) {
                return result;
            }
            continue;
        }
        if (written == 0) {
            return -EPIPE;
        }
        return negative_errno();
    }
    return 0;
}

static int pump_entry(void *argument)
{
    struct l4_pump *pump = argument;
    struct l4_connection *connection = pump->connection;
    struct l4_app *app = connection->app;
    size_t budget = 0;
    int result = 0;

    while (!connection->aborted) {
        ssize_t received =
            recv(pump->source_fd, pump->buffer, app->options.buffer_size, 0);
        if (received > 0) {
            result = send_all(pump->destination_fd, pump->buffer,
                              (size_t)received);
            if (result != 0) {
                break;
            }
            if (pump->client_to_upstream) {
                app->stats.bytes_client_to_upstream += (uint64_t)received;
            } else {
                app->stats.bytes_upstream_to_client += (uint64_t)received;
            }
            budget += (size_t)received;
            if (budget >= L4_IO_BUDGET_BYTES) {
                budget = 0;
                result = rco_yield();
                if (result != 0) {
                    break;
                }
            }
            continue;
        }
        if (received == 0) {
            if (shutdown(pump->destination_fd, SHUT_WR) != 0 &&
                errno != ENOTCONN && errno != EPIPE) {
                result = negative_errno();
            } else {
                app->stats.half_closes++;
            }
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            result = wait_for_event(pump->source_fd, RCO_EVENT_READ, -1);
            if (result != 0) {
                break;
            }
            continue;
        }
        result = negative_errno();
        break;
    }

    if (result != 0 && result != -ECANCELED && result != -EBADF &&
        !connection->aborted) {
        if (result == -ECONNRESET || result == -EPIPE ||
            result == -ENOTCONN) {
            app->stats.peer_resets++;
        } else {
            app->stats.io_errors++;
        }
    }
    if (result != 0) {
        connection_abort(connection);
    }
    return result;
}

static int session_entry(void *argument)
{
    struct l4_connection *connection = argument;
    struct l4_app *app = connection->app;

    int result = connect_upstream(connection);
    if (result != 0) {
        app->stats.connect_errors++;
        connection_abort(connection);
        return result;
    }
    app->stats.connected++;

    connection->pumps[0] = (struct l4_pump){
        .connection = connection,
        .source_fd = connection->client_fd,
        .destination_fd = connection->upstream_fd,
        .client_to_upstream = true,
        .buffer = connection->buffers,
    };
    connection->pumps[1] = (struct l4_pump){
        .connection = connection,
        .source_fd = connection->upstream_fd,
        .destination_fd = connection->client_fd,
        .client_to_upstream = false,
        .buffer = connection->buffers + app->options.buffer_size,
    };

    const struct rco_task_spec first_pump = {
        .entry = pump_entry,
        .argument = &connection->pumps[0],
        .finalizer = pump_finalizer,
    };
    result = rco_spawn_task(app->runtime, &first_pump, NULL);
    if (result != 0) {
        connection_abort(connection);
        return result;
    }
    connection_retain(connection);

    const struct rco_task_spec second_pump = {
        .entry = pump_entry,
        .argument = &connection->pumps[1],
        .finalizer = pump_finalizer,
    };
    result = rco_spawn_task(app->runtime, &second_pump, NULL);
    if (result != 0) {
        connection_abort(connection);
        return result;
    }
    connection_retain(connection);

    return 0;
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
    connection->references = 1;
    return connection;
}

static int accept_entry(void *argument)
{
    struct l4_app *app = argument;
    while (!app->draining) {
        size_t accepted_this_turn = 0;
        while (accepted_this_turn < L4_ACCEPT_BUDGET && !app->draining) {
            int client = accept4(app->listener_fd, NULL, NULL,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN) {
                    break;
                }
                if (app->draining && errno == EBADF) {
                    return 0;
                }
                if (errno == EMFILE || errno == ENFILE) {
                    app->stats.rejected++;
                    (void)rco_sleep_ms(10);
                    break;
                }
                app_fail(app, negative_errno());
                return app->fatal_error;
            }
            accepted_this_turn++;
            app->stats.accepted++;

            if (app->stats.active_connections >=
                app->options.max_connections) {
                app->stats.rejected++;
                (void)close(client);
                continue;
            }
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
            app->stats.active_connections++;
            if (app->stats.active_connections > app->stats.peak_connections) {
                app->stats.peak_connections =
                    app->stats.active_connections;
            }
            const struct rco_task_spec session = {
                .entry = session_entry,
                .argument = connection,
                .finalizer = session_finalizer,
            };
            int result = rco_spawn_task(app->runtime, &session, NULL);
            if (result != 0) {
                app->stats.rejected++;
                connection_release(connection);
            }
        }

        if (app->draining) {
            break;
        }
        if (accepted_this_turn == L4_ACCEPT_BUDGET) {
            int result = rco_yield();
            if (result != 0) {
                return result;
            }
            continue;
        }
        int result = wait_for_event(app->listener_fd, RCO_EVENT_READ, -1);
        if (result != 0) {
            return app->draining || result == -ECANCELED || result == -EBADF
                       ? 0
                       : result;
        }
    }
    return 0;
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
        if (count < 0 && errno == EAGAIN) {
            return 0;
        }
        return count < 0 ? negative_errno() : -EIO;
    }
}

static int signal_entry(void *argument)
{
    struct l4_app *app = argument;
    int result = wait_for_event(app->signal_fd, RCO_EVENT_READ, -1);
    if (result != 0) {
        app_fail(app, result);
        return result;
    }
    int received_signal = read_signal(app->signal_fd);
    if (received_signal <= 0) {
        app_fail(app, received_signal == 0 ? -EIO : received_signal);
        return received_signal;
    }

    app->draining = true;
    if (app->listener_fd >= 0) {
        (void)rco_close_fd(app->listener_fd);
        app->listener_fd = -1;
    }
    if (app->stats.active_connections == 0) {
        return rco_runtime_stop(app->runtime);
    }

    result = wait_for_event(app->signal_fd, RCO_EVENT_READ,
                            app->options.grace_ms);
    if (result == 0) {
        (void)read_signal(app->signal_fd);
        app->stats.forced_shutdown = true;
    } else if (result == -ETIMEDOUT) {
        app->stats.forced_shutdown = true;
    } else if (result == -ECANCELED) {
        return 0;
    } else {
        app_fail(app, result);
        return result;
    }
    return rco_runtime_stop(app->runtime);
}

static size_t runtime_max_fds(size_t max_connections)
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

int main(int argc, char **argv)
{
    struct l4_app app = {
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

    size_t max_fds = runtime_max_fds(app.options.max_connections);
    if (max_fds == 0 ||
        app.options.max_connections > (SIZE_MAX - 8) / 2) {
        fputs("RLIMIT_NOFILE is too small for max-connections\n", stderr);
        (void)close(app.signal_fd);
        (void)close(app.listener_fd);
        return EXIT_FAILURE;
    }
    struct rco_config config = {
        .default_stack_size = RCO_STACK_SIZE_DEFAULT,
        .max_coroutines = app.options.max_connections * 2 + 8,
        .max_fds = max_fds,
        .stack_cache_bytes = 64 * 1024 * 1024,
    };
    result = rco_runtime_create(&config, &app.runtime);
    if (result != 0) {
        fprintf(stderr, "runtime setup failed: %s\n", strerror(-result));
        (void)close(app.signal_fd);
        (void)close(app.listener_fd);
        return EXIT_FAILURE;
    }
    if (rco_spawn(app.runtime, 0, accept_entry, &app, NULL) != 0 ||
        rco_spawn(app.runtime, 0, signal_entry, &app, NULL) != 0) {
        fputs("failed to create service coroutines\n", stderr);
        (void)rco_runtime_destroy(app.runtime);
        (void)close(app.signal_fd);
        (void)close(app.listener_fd);
        return EXIT_FAILURE;
    }

    printf("listening=%s:%s upstream=%s:%s max_connections=%zu"
           " buffer_size=%zu\n",
           app.options.listen_host, app.options.listen_port,
           app.options.upstream_host, app.options.upstream_port,
           app.options.max_connections, app.options.buffer_size);
    fflush(stdout);

    result = rco_runtime_run(app.runtime);
    if (app.listener_fd >= 0) {
        (void)close(app.listener_fd);
    }
    (void)close(app.signal_fd);
    print_summary(&app);

    struct rco_stats runtime_stats;
    if (rco_runtime_get_stats(app.runtime, &runtime_stats) == 0) {
        printf("coroutines_spawned=%" PRIu64
               " context_switches=%" PRIu64
               " stacks_mapped=%" PRIu64
               " stacks_reused=%" PRIu64 "\n",
               runtime_stats.spawned, runtime_stats.context_switches,
               runtime_stats.stacks_mapped, runtime_stats.stacks_reused);
    }
    int destroy_result = rco_runtime_destroy(app.runtime);
    if (result != 0 || app.fatal_error != 0 || destroy_result != 0) {
        fprintf(stderr, "forwarder failed: runtime=%d app=%d destroy=%d\n",
                result, app.fatal_error, destroy_result);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
