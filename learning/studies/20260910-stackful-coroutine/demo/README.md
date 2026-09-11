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
  - CORO-RA-8
  - CORO-RA-9
  - CORO-RA-10
  - CORO-RA-11
  - CORO-RA-12
---

# rco Demo

`rco` is a C11 stackful coroutine runtime for Linux x86-64. The project also
builds coroutine-local state, deferred preemption, a multicore work-stealing
pool, correctness tests, benchmarks, a coroutine TCP layer-4 forwarder, and a
non-coroutine epoll baseline.

## Contract

### Decision

Build and validate both forwarders from the same source tree on Linux x86-64.
Use FIL-C for executed C paths, native GCC for the default L4 benchmark, and
Zig as the pinned LLVM frontend and native compiler for all three
context-switch variants.

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
| Linux x86-64 source tree and pinned compilers | default runtime, two experimental CACS runtimes, tests, pool, four forwarders |
| Worker count, task spec, affinity, shutdown mode | exact-once completion, pre-start stealing, failure and wake statistics |
| Four forwarder binaries and CPU/NUMA settings | five-mode aggregate, per-stream, process-resource, and rotation CSV |

### Interfaces And Ownership

CMake owns development builds; explicit GCC commands own default performance
binaries; the benchmark scripts own child processes and ports. The
`bench/cacs_scale_evidence.sh` driver owns the bounded CACS work/clean roots,
validation, manifest generation, and promotion to the declared raw root.

### Invariants

- all four forwarders receive identical CLI limits and traffic;
- every benchmark stream must make positive sender and receiver progress;
- build/test artifacts stay under the repository-local `.tmp` workspace;
- default L4 performance uses native GCC; CACS performance uses native
  Zig/Clang; FIL-C timing is never benchmark evidence.
- SysV/CACS/CACS+PN comparisons use one Zig version, identical flags, context
  size, benchmark source and run-order policy.
- a pool worker owns its runtime and every started coroutine for their full
  lifetime; migration statistics remain zero;
- job entry failures are observable in pool statistics and do not stop
  unrelated jobs.

### Failure Semantics

Any failed build, CTest, sanitizer, listener probe, stream check, or child
process aborts the run. PMU and frequency-control unavailability is recorded
without fabricating substitute counters.

### Worked Example

The default `RUNS=5 DURATION=3` A/B executes direct, coroutine, and epoll
positions, producing 15 aggregate rows and 60 per-stream rows when
`PARALLEL=4`. The CACS evidence run rotates direct plus the four SysV, CACS,
CACS+PN, and epoll forwarder modes through five positions, producing 25
aggregate rows, 100 per-stream rows, 20 forwarder-resource rows, and 25
run-order rows.

### Reconciliation Anchors

See `CORO-RA-1` through `CORO-RA-12` in [`../README.md`](../README.md).

### Evidence And Unknowns

The latest observed outputs are under
[`../evidence/raw/ab/`](../evidence/raw/ab/) and
[`../evidence/raw/cacs-scale/`](../evidence/raw/cacs-scale/). Real-NIC and
long-duration behavior remain outside this runbook.

## Platform Contract

| Item | Contract |
| --- | --- |
| OS | Linux |
| Architecture | x86-64, System V AMD64 ABI |
| Runtime scheduler | One owner OS thread, FIFO, optional deferred preemption |
| Pool scheduler | One runtime per worker; only unstarted jobs are stealable |
| I/O | Level-triggered `epoll` with `EPOLLONESHOT` rearm |
| Stack | Fixed-size private `mmap`, one guard page on each side |
| Cancellation | Cooperative at yield, wait, and sleep points |
| Local state | `errno` by default; sparse TLS; opt-in signal mask and locale |
| Unsupported | Arbitrary-PC preemption, started-task migration, dynamic stacks, CET shadow stack |

The runtime returns negative errno-style values. It does not hook blocking
library calls. An FD registered with `rco_wait_fd()` must be closed with
`rco_close_fd()` while the runtime is active.

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
- coroutine-local errno, sparse TLS/destructors, signal mask, and locale;
- deferred preemption request/safe-point behavior and signal/timer restoration;
- concurrent pool submission, pre-start stealing, affinity, cancellation,
  shutdown, timed join recovery, failure statistics, and zero migration;
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
      "$src/src/rco.c" "$src/src/rco_pool.c" \
      "$src/tests/rco_context_filc.c" \
      "$src/tests/rco_filc_test.c" -o .tmp/rco-filc-test
    $filrun .tmp/rco-filc-test
    $filcc -DRCO_FILC -std=c11 -O2 -g \
      -Wall -Wextra -Werror -I "$src/include" -I "$src/src" \
      "$src/src/rco.c" "$src/tests/rco_context_filc.c" \
      "$src/bench/rco_high_concurrency_bench.c" \
      -o .tmp/rco-high-concurrency-filc
    set +e
    $filrun .tmp/rco-high-concurrency-filc \
      --tasks 0 --stack-bytes 32768 \
      --touch-bytes 4096 --yields-per-task 1 \
      >.tmp/rco-high-concurrency-filc.out \
      2>.tmp/rco-high-concurrency-filc.err
    status=$?
    set -e
    test "$status" -eq 2
    grep -q "tasks must be" .tmp/rco-high-concurrency-filc.err
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

`zig cc` builds all three runtime variants and four forwarders at O0/O2/O3.
The ASan/UBSan E2E binaries also use `zig cc`. Default L4 performance binaries
use native GCC; CACS comparisons use the same native Zig/Clang build for every
variant so compiler choice is not a confound.

## Experimental CACS Variants

The installed `rco` target remains the complete SysV backend. CACS targets are
opt-in and require Zig/Clang 21:

```bash
zig="$PWD/.tmp/zig/dist/zig;cc"
cmake -S learning/studies/20260910-stackful-coroutine/demo \
  -B .tmp/rco-cacs-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$zig" \
  -DCMAKE_ASM_COMPILER="$zig" \
  -DCMAKE_C_FLAGS_RELEASE='-O3 -march=native -mtune=native -DNDEBUG -flto' \
  -DCMAKE_EXE_LINKER_FLAGS_RELEASE=-flto \
  -DRCO_BUILD_CACS=ON
cmake --build .tmp/rco-cacs-build -j
ctest --test-dir .tmp/rco-cacs-build --output-on-failure
```

Generated runtime/benchmark targets:

| Mode | Library | Yield benchmark | Pool benchmark | L4 E2E binary |
| --- | --- | --- | --- | --- |
| SysV | `rco` | `rco_bench` | `rco_pool_bench_sysv` | `rco_l4_forwarder` |
| CACS | `rco_cacs` | `rco_bench_cacs` | `rco_pool_bench_cacs` | `rco_l4_forwarder_cacs` |
| CACS + PN | `rco_cacs_preserve_none` | `rco_bench_cacs_preserve_none` | `rco_pool_bench_cacs_preserve_none` | `rco_l4_forwarder_cacs_preserve_none` |

The CACS libraries are experimental and are not installed. The preserve-none
compile definition is public because every caller must use the same unstable
calling convention. A GCC build with `RCO_BUILD_CACS=ON` fails during configure
instead of silently ignoring the attribute.

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

## Coroutine-Local State

Zero `local_state_flags` enables `RCO_LOCAL_STATE_ERRNO`. Use
`RCO_LOCAL_STATE_NONE` only for the explicit legacy low-overhead path.
`RCO_LOCAL_STATE_SIGNAL_MASK` and `RCO_LOCAL_STATE_LOCALE` are opt-in.
Including [`rco_local.h`](include/rco_local.h) requires `_GNU_SOURCE`.

TLS keys belong to one runtime. Values are task-local and allocated in sparse
32-slot chunks. Deleting a key clears values without running destructors. Task
exit runs up to `PTHREAD_DESTRUCTOR_ITERATIONS` destructor passes before the
user finalizer. TLS access remains valid inside TLS destructors, but not inside
the user finalizer.

When preemption is enabled, `config.preempt_signal` is reserved. A task may
query or change its local signal mask with `rco_sigmask()`, but blocking that
signal via `SIG_BLOCK` or `SIG_SETMASK` returns `-EINVAL`.

## Deferred Preemption

Set `preempt_quantum_ns` and a realtime `preempt_signal` in `rco_config`.
The runtime creates one `CLOCK_THREAD_CPUTIME_ID` timer on the owner thread and
uses a guarded alternate signal stack. The signal handler only records a
pending request. Actual switching occurs at `rco_preempt_point()` or when the
outermost `rco_preempt_enable()` observes a pending request.

`rco_preempt_disable()` nests. Cancellation and runtime stop take precedence
over a pending preemption. This is not arbitrary-PC hard preemption: code that
does not call a suspension API or safe point is not forcibly switched.

## Multicore Pool

[`rco_pool.h`](include/rco_pool.h) creates one owner-only `rco_runtime` per
worker. Only queued, unstarted jobs may be stolen. Once claimed, a job keeps
the same worker, TID, stack, epoll instance, and timer owner until finalization.

```c
struct rco_pool_config config = {
    .worker_count = 4,
    .max_jobs = 4096,
    .dispatch_batch = 8,
    .first_cpu = 0,
    .pin_workers = true,
};
struct rco_pool *pool = NULL;

if (rco_pool_create(&config, &pool) != 0 ||
    rco_pool_start(pool) != 0) {
    return 1;
}
```

`RCO_AFFINITY_ANY` balances submissions, `PREFER` permits pre-start stealing,
and `REQUIRE` pins a job before it starts. `rco_submit_local()` pins a child to
the current worker. Blocking pool wait/join calls from a pool worker return
`-EDEADLK`.

A successful submit transfers finalizer ownership to the pool. The finalizer
runs exactly once, including queued cancellation and shutdown. A nonzero job
entry result does not stop unrelated jobs: it increments `stats.failed`, and
the first result is retained in `stats.first_job_error`. `wait_idle()` and
`join()` report pool/runtime lifecycle errors.

Use `RCO_SHUTDOWN_DRAIN` to complete accepted work or `RCO_SHUTDOWN_CANCEL` to
cancel it. A failed `rco_pool_start()` internally joins created workers and
leaves the pool destroyable.

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

High-concurrency CPU/memory matrix:

```bash
numactl --physcpubind=0 --membind=0 \
  python3 "$src/bench/rco_high_concurrency_bench.py" \
    --sysv .tmp/rco-cacs-build/rco_high_concurrency_bench_sysv \
    --cacs .tmp/rco-cacs-build/rco_high_concurrency_bench_cacs \
    --cacs-preserve-none \
      .tmp/rco-cacs-build/rco_high_concurrency_bench_cacs_preserve_none \
    --output-dir .tmp/rco-cacs-results \
    --runs 5
```

The default matrix produces 150 validated samples: task-count scaling through
16,384 tasks, touched-stack scaling through 256 KiB/task, and yield-frequency
scaling through 1,024 yields/task. The process stops at a residency barrier so
the parent can sample `/proc/<pid>/status`, `smaps_rollup`, and setup faults
before timed execution. `SIGINT` and `SIGTERM` are converted into the harness
error path so an interrupted parent resumes and terminates the complete active
process group.

Multicore pool matrix:

```bash
.tmp/rco-cacs-build/rco_pool_bench_cacs \
  --workers 4 \
  --jobs 1024 \
  --iterations 65536 \
  --preempt-quantum-ns 1000000 \
  --pin-first-cpu 0
```

The evidence driver runs SysV/CACS/CACS+PN with 1/2/4 workers, preemption
disabled/enabled, five position-rotated runs per cell, and three backend stress
loops of 500 iterations. A sample is accepted only with the deterministic
checksum, exact-once entry/finalizer counts, all workers used, pre-start steals
for multiworker cases, zero migrations, zero job failures, and empty queues.
The final gate requires median speedup of at least `1.50x` for two workers and
`2.50x` for four workers.

L4 direct-versus-proxy benchmark:

```bash
RUNS=5 DURATION=3 \
  "$src/bench/l4_bench.sh" \
  .tmp/rco-l4-forwarder \
  .tmp/rco-l4-forwarder-epoll \
  .tmp/rco-l4-results
```

With `RCO_BUILD_CACS=ON`, the same harness accepts two additional binaries and
runs a five-position rotation:

```bash
RUNS=5 DURATION=3 \
  "$src/bench/l4_bench.sh" \
  .tmp/rco-cacs-build/rco_l4_forwarder \
  .tmp/rco-cacs-build/rco_l4_forwarder_epoll \
  .tmp/rco-l4-cacs-results \
  .tmp/rco-cacs-build/rco_l4_forwarder_cacs \
  .tmp/rco-cacs-build/rco_l4_forwarder_cacs_preserve_none
```

The complete CACS correctness, codegen, scale, and L4 evidence run has one
no-argument invocation from the d2 checkout:

```bash
ssh d2 'cd /root/recallfs && exec bash learning/studies/20260910-stackful-coroutine/demo/bench/cacs_scale_evidence.sh'
```

The driver runs only on Linux x86-64. It uses only
`.tmp/zig/dist/zig` and `.tmp/fil-c`, derives the checked-out source commit,
and writes builds plus candidate evidence below `.tmp`. It enables
`CMAKE_EXPORT_COMPILE_COMMANDS`, records complete compile and link commands for
O0, O2, O3, sanitizer, and GCC SysV builds, and validates compiler, backend,
binary-hash, schema, count, scaling, and workload oracles before creating
`validation.txt` and `SHA256SUMS`. Only a fully validated candidate is promoted to
`learning/studies/20260910-stackful-coroutine/evidence/raw/cacs-scale`.

The current L4 example remains one runtime on one thread. A multicore CACS L4
forwarder should create one `SO_REUSEPORT` listener per pool worker and keep
each accepted connection on that worker for its full lifetime.

The L4 script requires `iperf3`, `jq`, `numactl`, and Python 3. It pins proxy,
server, and client to CPUs 0, 1, and 2 on NUMA node 0. Each run interleaves
the configured direct/forwarder modes with a deterministic position rotation
recorded in `run-order.csv`. It rejects a run unless
every requested sender and receiver stream transfers data, all child processes
exit within bounded deadlines, and a normal proxy shutdown reports exactly one
summary with `forced_shutdown=false`. Before measurement, each forwarder must
report the expected compile-time identity through `--backend-identity`; the
harness rejects duplicate identities and digests, executes private copies, and
rechecks both source and copy around every sample. Aggregate and resource rows
carry the verified identity and SHA-256. Summary output labels ratio-of-medians
separately and uses the median of run-paired ratios for the historical
`*_over_*` keys. It writes per-stream results to
`stream-throughput.csv`, records process VM/CPU counters in
`process-resources.csv`, and records soft/hard `RLIMIT_NOFILE` in
`environment.txt`. Do not compare its loopback numbers with a real-NIC result.
