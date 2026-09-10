# rco Demo

`rco` is a C11 stackful coroutine runtime for one Linux x86-64 thread. The
same project builds correctness tests, a context-switch benchmark, and a TCP
layer-4 forwarder.

## Platform Contract

| Item | Contract |
| --- | --- |
| OS | Linux |
| Architecture | x86-64, System V AMD64 ABI |
| Scheduler | One OS thread, cooperative, FIFO |
| I/O | Level-triggered `epoll` with `EPOLLONESHOT` rearm |
| Stack | Fixed-size private `mmap`, one guard page on each side |
| Cancellation | Cooperative at yield, wait, and sleep points |
| Unsupported | Thread migration, preemption, dynamic stacks, CET shadow stack |

The runtime returns negative `errno` values. It does not virtualize `errno`,
TLS, signal masks, locale, or blocking library calls. An FD registered with
`rco_wait_fd()` must be closed with `rco_close_fd()` while the runtime is
active.

## Build And Test

Run on `ssh d2`:

```bash
cmake -S learning/studies/20260910-stackful-coroutine/demo \
  -B .tmp/rco-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=gcc
cmake --build .tmp/rco-build -j
ctest --test-dir .tmp/rco-build --output-on-failure
```

The CTest targets cover:

- nested stack suspension and FIFO scheduling;
- cancellation before first resume and while blocked;
- timer timeout and nonblocking pipe wakeup;
- callee-saved GPR and floating-point control preservation;
- stack cache reuse and guard-page overflow;
- concurrent TCP forwarding, exact payloads, half-close, graceful drain, and
  forced shutdown.

## FIL-C

FIL-C 0.684 on d2 runs inside a pinned Debian bookworm container because the
d2 host glibc 2.28 is older than the compiler binary requirement. The compiler
distribution remains under `.tmp/fil-c/`.

`rco_context_filc.c` replaces only the assembly boundary. Its switch function
aborts if called, so the FIL-C test cannot accidentally claim to validate
native stack switching.

```bash
docker run --rm --network none \
  -v /root/recallfs:/root/recallfs \
  -v /usr/bin/x86_64-linux-gnu-ld.bfd:/usr/bin/ld:ro \
  -v /lib/x86_64-linux-gnu/libbfd-2.31.1-system.so:\
/lib/x86_64-linux-gnu/libbfd-2.31.1-system.so:ro \
  -w /root/recallfs debian:bookworm-slim sh -ceu '
    src=learning/studies/20260910-stackful-coroutine/demo
    .tmp/fil-c/bin/filcc -DRCO_FILC -std=c11 -O2 -g \
      -Wall -Wextra -Werror -I "$src/include" -I "$src/src" \
      "$src/src/rco.c" "$src/tests/rco_context_filc.c" \
      "$src/tests/rco_filc_test.c" -o .tmp/rco-filc-test
    .tmp/fil-c/bin/filrun .tmp/rco-filc-test
  '
```

The unusual linker mounts are only a compatibility adapter for the pinned
minimal container. They do not affect generated-code performance because
FIL-C results are never used as benchmark data.

## Library Example

```c
#include "rco.h"

static int worker(void *argument)
{
    int fd = *(int *)argument;
    unsigned ready = 0;
    int result = rco_wait_fd(fd, RCO_EVENT_READ, 1000, &ready);
    if (result != 0) {
        return result;
    }
    return (ready & RCO_EVENT_READ) != 0 ? 0 : -1;
}

int main(void)
{
    struct rco_runtime *runtime = NULL;
    if (rco_runtime_create(NULL, &runtime) != 0) {
        return 1;
    }

    int fd = /* nonblocking descriptor */;
    if (rco_spawn(runtime, 0, worker, &fd, NULL) != 0) {
        return 1;
    }
    int result = rco_runtime_run(runtime);
    (void)rco_runtime_destroy(runtime);
    return result == 0 ? 0 : 1;
}
```

## L4 Forwarder

The forwarder accepts numeric IPv4 or IPv6 addresses only. Avoiding runtime DNS
prevents one blocking resolver call from stalling the cooperative scheduler.
It binds `127.0.0.1` by default. A wildcard listener must be selected
explicitly with `--listen-host 0.0.0.0` or `--listen-host ::`.

```bash
.tmp/rco-build/rco_l4_forwarder \
  --listen-host 0.0.0.0 \
  --listen-port 9000 \
  --upstream-host 127.0.0.1 \
  --upstream-port 8080 \
  --max-connections 256 \
  --buffer-size 65536 \
  --connect-timeout-ms 3000 \
  --grace-ms 30000
```

Operational controls:

| Option | Meaning |
| --- | --- |
| `--reuse-port` | Allow multiple independently pinned proxy processes |
| `--tcp-nodelay` | Prefer small-message latency over TCP coalescing |
| `--max-connections` | Hard connection and allocation bound |
| `--buffer-size` | Per-direction backpressure buffer |
| `--connect-timeout-ms` | Deadline for nonblocking upstream connect |
| `--grace-ms` | Drain deadline after the first termination signal |

The default 256 connections require roughly 576 available descriptors,
including runtime overhead. The process refuses startup if `RLIMIT_NOFILE`
cannot satisfy the configured connection bound.

## Benchmarks

Build the native benchmark binaries with the production flags:

```bash
src=learning/studies/20260910-stackful-coroutine/demo
flags='-std=c11 -O3 -march=native -mtune=native -DNDEBUG -flto
  -fno-ipa-icf -Wall -Wextra -Werror -fno-omit-frame-pointer
  -fcf-protection=branch'

gcc $flags -I "$src/include" -I "$src/src" \
  "$src/src/rco.c" "$src/src/rco_context_x86_64.S" \
  "$src/bench/rco_bench.c" -o .tmp/rco-bench

gcc $flags -I "$src/include" -I "$src/src" \
  "$src/src/rco.c" "$src/src/rco_context_x86_64.S" \
  "$src/examples/l4_forwarder.c" -o .tmp/rco-l4-forwarder
```

Context benchmark:

```bash
numactl --physcpubind=0 --membind=0 \
  .tmp/rco-bench 1000000 11
```

L4 direct-versus-proxy benchmark:

```bash
RUNS=5 DURATION=3 \
  "$src/bench/l4_bench.sh" \
  .tmp/rco-l4-forwarder .tmp/rco-l4-results
```

The L4 script requires `iperf3`, `jq`, `numactl`, and Python 3. It pins proxy,
server, and client to CPUs 0, 1, and 2 on NUMA node 0. It rejects a run unless
every requested sender and receiver stream transfers data, and writes
per-stream results to `stream-throughput.csv`. Do not compare its loopback
numbers with a real-NIC result.
