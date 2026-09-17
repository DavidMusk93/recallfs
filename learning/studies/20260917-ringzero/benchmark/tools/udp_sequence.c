#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    PACKET_SIZE = 64,
    MAX_PACE_MICROSECONDS = 1000000,
    MAX_TIMEOUT_MILLISECONDS = 3600000,
};

static const uint64_t packet_magic = UINT64_C(0x52494e475a45524f);
static const uint64_t checksum_salt = UINT64_C(0xa5a55a5af00dcafe);

typedef struct {
    uint64_t magic;
    uint64_t sequence;
    uint64_t checksum;
    uint8_t padding[PACKET_SIZE - 3 * sizeof(uint64_t)];
} wire_packet;

_Static_assert(sizeof(wire_packet) == PACKET_SIZE,
               "wire packet must remain exactly 64 bytes");

static bool host_is_little_endian(void) {
    const uint16_t marker = 1;
    uint8_t first_byte = 0;
    memcpy(&first_byte, &marker, sizeof(first_byte));
    return first_byte == 1;
}

static uint64_t host_to_network_u64(uint64_t value) {
    if (!host_is_little_endian()) {
        return value;
    }
    uint32_t high = (uint32_t)(value >> 32);
    uint32_t low = (uint32_t)value;
    return ((uint64_t)htonl(low) << 32) | htonl(high);
}

static uint64_t network_to_host_u64(uint64_t value) {
    if (!host_is_little_endian()) {
        return value;
    }
    uint32_t high = (uint32_t)(value >> 32);
    uint32_t low = (uint32_t)value;
    return ((uint64_t)ntohl(low) << 32) | ntohl(high);
}

static wire_packet make_packet(uint64_t sequence) {
    wire_packet packet = {0};
    packet.magic = host_to_network_u64(packet_magic);
    packet.sequence = host_to_network_u64(sequence);
    packet.checksum = host_to_network_u64(packet_magic ^ sequence ^ checksum_salt);
    for (size_t index = 0; index < sizeof(packet.padding); ++index) {
        packet.padding[index] = (uint8_t)(sequence + index);
    }
    return packet;
}

static bool decode_packet(const wire_packet *packet, uint64_t *sequence_out) {
    uint64_t magic = network_to_host_u64(packet->magic);
    uint64_t sequence = network_to_host_u64(packet->sequence);
    uint64_t checksum = network_to_host_u64(packet->checksum);
    if (magic != packet_magic ||
        checksum != (packet_magic ^ sequence ^ checksum_salt)) {
        return false;
    }
    for (size_t index = 0; index < sizeof(packet->padding); ++index) {
        if (packet->padding[index] != (uint8_t)(sequence + index)) {
            return false;
        }
    }
    *sequence_out = sequence;
    return true;
}

static uint64_t parse_u64(const char *label, const char *text, bool allow_zero) {
    char *end = NULL;
    errno = 0;
    if (text[0] == '-') {
        fprintf(stderr, "invalid %s: %s\n", label, text);
        exit(2);
    }
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || (!allow_zero && value == 0)) {
        fprintf(stderr, "invalid %s: %s\n", label, text);
        exit(2);
    }
    return (uint64_t)value;
}

static uint16_t parse_port(const char *text) {
    uint64_t value = parse_u64("port", text, false);
    if (value > UINT16_MAX) {
        fprintf(stderr, "port out of range: %s\n", text);
        exit(2);
    }
    return (uint16_t)value;
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static struct sockaddr_in parse_address(const char *ip, uint16_t port) {
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", ip);
        exit(2);
    }
    return address;
}

static void sleep_microseconds(uint64_t microseconds) {
    struct timespec delay = {
        .tv_sec = (time_t)(microseconds / UINT64_C(1000000)),
        .tv_nsec = (long)((microseconds % UINT64_C(1000000)) * UINT64_C(1000)),
    };
    while (nanosleep(&delay, &delay) != 0) {
        if (errno != EINTR) {
            perror("nanosleep");
            exit(2);
        }
    }
}

static int run_sender(const char *ip, uint16_t port, uint64_t count,
                      uint64_t pace_microseconds) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return 2;
    }
    struct sockaddr_in destination = parse_address(ip, port);
    uint64_t sent = 0;
    for (uint64_t sequence = 0; sequence < count; ++sequence) {
        wire_packet packet = make_packet(sequence);
        ssize_t result;
        do {
            result = sendto(socket_fd, &packet, sizeof(packet), 0,
                            (const struct sockaddr *)&destination, sizeof(destination));
        } while (result < 0 && errno == EINTR);
        if (result != (ssize_t)sizeof(packet)) {
            perror("sendto");
            close(socket_fd);
            return 1;
        }
        ++sent;
        if (pace_microseconds != 0) {
            sleep_microseconds(pace_microseconds);
        }
    }
    close(socket_fd);
    printf("mode=send expected=%" PRIu64 " sent=%" PRIu64 "\n", count, sent);
    return sent == count ? 0 : 1;
}

static bool bit_is_set(const uint8_t *bitmap, uint64_t index) {
    return (bitmap[index / 8] & (uint8_t)(1U << (index % 8))) != 0;
}

static void set_bit(uint8_t *bitmap, uint64_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

static int run_receiver(const char *ip, uint16_t port, uint64_t expected,
                        uint64_t timeout_milliseconds) {
    if (expected > (SIZE_MAX - 7) / 8) {
        fputs("expected packet count is too large\n", stderr);
        return 2;
    }
    size_t bitmap_size = (size_t)((expected + 7) / 8);
    uint8_t *bitmap = calloc(bitmap_size, 1);
    if (bitmap == NULL) {
        fputs("bitmap allocation failed\n", stderr);
        return 2;
    }

    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        free(bitmap);
        return 2;
    }
    int receive_buffer = 4 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                   sizeof(receive_buffer)) != 0) {
        perror("setsockopt(SO_RCVBUF)");
        close(socket_fd);
        free(bitmap);
        return 2;
    }
    struct sockaddr_in bind_address = parse_address(ip, port);
    if (bind(socket_fd, (const struct sockaddr *)&bind_address, sizeof(bind_address)) !=
        0) {
        perror("bind");
        close(socket_fd);
        free(bitmap);
        return 2;
    }

    uint64_t unique = 0;
    uint64_t duplicates = 0;
    uint64_t invalid = 0;
    uint64_t deadline = monotonic_milliseconds() + timeout_milliseconds;
    for (;;) {
        uint64_t now = monotonic_milliseconds();
        if (now >= deadline) {
            break;
        }
        uint64_t remaining = deadline - now;
        int poll_timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {
            .fd = socket_fd,
            .events = POLLIN,
        };
        int ready = poll(&descriptor, 1, poll_timeout);
        if (ready == 0) {
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            close(socket_fd);
            free(bitmap);
            return 2;
        }

        uint8_t buffer[PACKET_SIZE + 1];
        wire_packet packet;
        ssize_t received = recvfrom(socket_fd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            perror("recvfrom");
            close(socket_fd);
            free(bitmap);
            return 2;
        }
        if (received != (ssize_t)sizeof(packet)) {
            ++invalid;
            continue;
        }
        memcpy(&packet, buffer, sizeof(packet));
        uint64_t sequence = 0;
        if (!decode_packet(&packet, &sequence) || sequence >= expected) {
            ++invalid;
            continue;
        }
        if (bit_is_set(bitmap, sequence)) {
            ++duplicates;
        } else {
            set_bit(bitmap, sequence);
            ++unique;
        }
    }

    close(socket_fd);
    free(bitmap);
    uint64_t missing = expected - unique;
    printf("mode=receive expected=%" PRIu64 " unique=%" PRIu64 " missing=%" PRIu64
           " duplicates=%" PRIu64 " invalid=%" PRIu64 "\n",
           expected, unique, missing, duplicates, invalid);
    return missing == 0 && duplicates == 0 && invalid == 0 ? 0 : 1;
}

static int run_self_test(void) {
    static const uint8_t expected_magic[] = {
        0x52, 0x49, 0x4e, 0x47, 0x5a, 0x45, 0x52, 0x4f,
    };
    uint8_t bitmap[16] = {0};
    for (uint64_t sequence = 0; sequence < 128; ++sequence) {
        wire_packet packet = make_packet(sequence);
        if (memcmp(&packet.magic, expected_magic, sizeof(expected_magic)) != 0) {
            return 1;
        }
        uint64_t decoded = UINT64_MAX;
        if (!decode_packet(&packet, &decoded) || decoded != sequence ||
            bit_is_set(bitmap, sequence)) {
            return 1;
        }
        set_bit(bitmap, sequence);
        if (!bit_is_set(bitmap, sequence)) {
            return 1;
        }
    }
    wire_packet corrupted = make_packet(7);
    corrupted.padding[3] ^= 1;
    uint64_t decoded = 0;
    if (decode_packet(&corrupted, &decoded)) {
        return 1;
    }
    puts("PASS: UDP sequence encoding and bitmap accounting");
    return 0;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "usage:\n"
            "  %s self-test\n"
            "  %s send <dst-ip> <port> <count> <pace-us>\n"
            "  %s receive <bind-ip> <port> <count> <timeout-ms>\n",
            program, program, program);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        return run_self_test();
    }
    if (argc == 6 && strcmp(argv[1], "send") == 0) {
        uint64_t pace = parse_u64("pace-us", argv[5], true);
        if (pace > MAX_PACE_MICROSECONDS) {
            fputs("pace-us exceeds 1000000\n", stderr);
            return 2;
        }
        return run_sender(argv[2], parse_port(argv[3]),
                          parse_u64("count", argv[4], false), pace);
    }
    if (argc == 6 && strcmp(argv[1], "receive") == 0) {
        uint64_t timeout = parse_u64("timeout-ms", argv[5], false);
        if (timeout > MAX_TIMEOUT_MILLISECONDS) {
            fputs("timeout-ms exceeds 3600000\n", stderr);
            return 2;
        }
        return run_receiver(argv[2], parse_port(argv[3]),
                            parse_u64("count", argv[4], false), timeout);
    }
    print_usage(argv[0]);
    return 2;
}
