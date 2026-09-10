---
doc_id: recallfs-runbook-rco-demo-v1
kind: runbook
status: active
authority: design
applies_to:
  - learning/studies/20260910-stackful-coroutine/demo
depends_on:
  - recallfs-study-stackful-coroutine-v1
supersedes: []
verified_by:
  - CORO-RA-1
  - CORO-RA-2
  - CORO-RA-3
  - CORO-RA-4
  - CORO-RA-6
  - CORO-RA-7
---

# rco Demo

`rco` is a C11 stackful coroutine runtime for one Linux x86-64 thread. The
same project builds correctness tests, a context-switch benchmark, a coroutine
TCP layer-4 forwarder, and a non-coroutine epoll baseline.

## Contract

### Decision

Build and validate both forwarders from the same source tree on Linux x86-64.
Use FIL-C for executed C paths, Zig as the pinned LLVM frontend, native GCC for
target-machine performance, and the same E2E/benchmark inputs for both models.

### Scope

This runbook owns build, correctness, sanitizer, launch, and benchmark commands
for `learning/studies/20260910-stackful-coroutine/demo/`; the parent study owns
interpretation.

### Non-goals

These commands do not provision a service, tune a host, or establish real-NIC
capacity and tail-latency SLOs.

### Inputs And Outputs

| Input | Output |
| --- | --- |
| Linux x86-64 source tree and pinned compilers | runtime, tests, two forwarders |
| Two forwarder binaries and CPU/NUMA settings | aggregate, per-stream, and process-resource CSV |

### Interfaces And Ownership

CMake owns development builds; explicit GCC commands own performance binaries;
the A/B script owns child processes, ports, cleanup, and generated evidence.

### Invariants

- both forwarders receive identical CLI limits and traffic;
- every benchmark stream must make positive sender and receiver progress;
- build/test artifacts stay under the repository-local `.tmp` workspace;
- performance results come from native GCC binaries, never FIL-C.

### Failure Semantics

Any failed build, CTest, sanitizer, listener probe, stream check, or child
process aborts the run. PMU and frequency-control unavailability is recorded
without fabricating substitute counters.

### Worked Example

`RUNS=5 DURATION=3` executes `direct -> coroutine -> epoll` for each run,
producing 15 aggregate rows and 60 per-stream rows when `PARALLEL=4`.

### Reconciliation Anchors

See `CORO-RA-1` through `CORO-RA-7` in [`../README.md`](../README.md).

### Evidence And Unknowns

The latest observed outputs are under [`../evidence/raw/ab/`](../evidence/raw/ab/).
Real-NIC and long-duration behavior remain outside this runbook.

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
- epoll bidirectional budget, half-close, and generation state;
- overload rejection, transient accept errors, and high-`RLIMIT_NOFILE`
  startup memory;
- `/proc/<pid>/stat` parsing with spaces and `)` in `comm`;
- the same concurrent TCP forwarding, exact payload, half-close, graceful
  drain, and forced-shutdown suite against both forwarders.

## FIL-C

FIL-C 0.684 on d2 runs inside a pinned Debian bookworm container because the
d2 host glibc 2.28 is older than the compiler binary requirement. The compiler
distribution remains under `/root/recallfs/.tmp/fil-c`.

`rco_context_filc.c` replaces only the assembly boundary. Its switch function
aborts if called, so the FIL-C test cannot accidentally claim to validate
native stack switching.

```bash
docker run --rm --network none \
  -v /root/recallfs:/root/recallfs \
  -v /usr/bin/x86_64-linux-gnu-ld.bfd:/usr/bin/ld:ro \
  -v /lib/x86_64-linux-gnu/libbfd-2.31.1-system.so:\
/lib/x86_64-linux-gnu/libbfd-2.31.1-system.so:ro \
  -w /root/recallfs \
  debian@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171 \
  sh -ceu '
    src=learning/studies/20260910-stackful-coroutine/demo
    filcc=.tmp/fil-c/bin/filcc
    filrun=.tmp/fil-c/bin/filrun
    $filcc -DRCO_FILC -std=c11 -O2 -g \
      -Wall -Wextra -Werror -I "$src/include" -I "$src/src" \
      "$src/src/rco.c" "$src/tests/rco_context_filc.c" \
      "$src/tests/rco_filc_test.c" -o .tmp/rco-filc-test
    $filrun .tmp/rco-filc-test
    $filcc -std=c11 -O2 -g -Wall -Wextra -Werror \
      "$src/tests/l4_forwarder_epoll_test.c" -o .tmp/l4-epoll-filc-test
    $filrun .tmp/l4-epoll-filc-test
  '
```

The unusual linker mounts are only a compatibility adapter for the pinned
minimal container. They do not affect generated-code performance because
FIL-C results are never used as benchmark data.

## Zig LLVM Frontend

The pinned d2 cross-check compiler is Zig 0.16.0:

```text
archive: zig-x86_64-linux-0.16.0.tar.xz
sha256:  70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
driver:  .tmp/zig/dist/zig cc
LLVM:    Clang 21.1.0
```

`zig cc` builds both forwarders and the runtime tests at O0/O2/O3. The
ASan/UBSan E2E binaries also use `zig cc`. Performance binaries deliberately
use native GCC instead.

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

Use `.tmp/rco-build/rco_l4_forwarder_epoll` with the same arguments to run the
non-coroutine state-machine baseline.

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

gcc $flags \
  "$src/examples/l4_forwarder_epoll.c" \
  -o .tmp/rco-l4-forwarder-epoll
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
  .tmp/rco-l4-forwarder \
  .tmp/rco-l4-forwarder-epoll \
  .tmp/rco-l4-results
```

The L4 script requires `iperf3`, `jq`, `numactl`, and Python 3. It pins proxy,
server, and client to CPUs 0, 1, and 2 on NUMA node 0. Each run interleaves
direct, coroutine, and epoll-state-machine paths with a deterministic
three-position rotation recorded in `run-order.csv`. It rejects a run unless
every requested sender and receiver stream transfers data, all child processes
exit within bounded deadlines, and a normal proxy shutdown reports exactly one
summary with `forced_shutdown=false`. It writes per-stream results to
`stream-throughput.csv`, records process VM/CPU counters in
`process-resources.csv`, and records soft/hard `RLIMIT_NOFILE` in
`environment.txt`. Do not compare its loopback numbers with a real-NIC result.
