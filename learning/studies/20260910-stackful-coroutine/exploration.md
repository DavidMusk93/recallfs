---
doc_id: recallfs-study-stackful-coroutine-exploration-v1
kind: study
status: archived
authority: evidence
applies_to:
  - learning/studies/20260910-stackful-coroutine
depends_on:
  - recallfs-study-stackful-coroutine-v1
  - learning/studies/20260910-stackful-coroutine/source.md
supersedes: []
verified_by:
  - learning/studies/20260910-stackful-coroutine/evidence/raw/ab
---

# Exploration Log

## Contract

### Decision

Preserve failed approaches, discovered defects, environment constraints, and
superseded measurements as historical evidence. Current behavioral truth lives
in source/tests; current target behavior lives in the parent study.

### Scope

This log covers paper acquisition, d2 environment setup, runtime/L4 debugging,
review findings, and the progression to the final A/B evidence.

### Non-goals

It is not a runbook, current benchmark summary, or authority over the active
design.

### Inputs And Outputs

Investigation observations enter this log; durable fixes, tests, and raw
evidence leave it in
`learning/studies/20260910-stackful-coroutine/demo/`,
`learning/studies/20260910-stackful-coroutine/README.md`, and
`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/`.

### Interfaces And Ownership

Historical entries are append-only in meaning. Later evidence may supersede a
measurement but must not rewrite why an earlier run failed.

### Invariants

- failed runs are not presented as successful evidence;
- measurements identify the source revision and environment;
- FIL-C limitations and native results remain separate;
- superseded A/B values are explicitly superseded.

### Failure Semantics

An unexplained mismatch between this log, tracked source, and current raw
evidence is documentation drift and blocks a performance claim.

### Worked Example

The first A/B result was discarded after the epoll baseline was found to count
the same payload at both `recv` and `send` and to share one budget across both
directions. A focused test failed before the fix and passed afterward; only the
post-fix five-run data is current evidence.

### Reconciliation Anchors

The parent study defines `CORO-RA-1` through `CORO-RA-7`; this log records the
debugging path that produced those anchors.

### Evidence And Unknowns

Current raw evidence is
[`learning/studies/20260910-stackful-coroutine/evidence/raw/ab/`](evidence/raw/ab/).
Historical intermediate measurements are not retained as performance
conclusions.

## 1. 研究范围

用户给出的截图对应 `Stackful Coroutine Made Fast`。先核验作者页面，确认：

- 这是 ASPLOS'24 投稿稿件，不是录用论文；
- 作者称拒稿原因为核心 CACS 思路已在 libfringe 中实现；
- 主稿为 Submission #17，appendix 元数据却写 Submission #46。

由此把材料扩展为：

1. 主稿和 appendix；
2. coroutine 分类与语义；
3. C++ fibers 正反争论链；
4. cooperative task management；
5. mTCP 网络数据面背景；
6. compiler-supported context switching DOI。

8 个可公开获取的 PDF 已在 `ssh d2` 下载并校验。SWAPSTACK 论文的 author
copy 页面可访问，但 PDF endpoint 返回 HTTP 403，因此只记录 DOI，不使用
来源不明的替代文件。

## 2. 环境收敛

最初本机为 macOS ARM64。用户随后明确要求全部运行环境限定为 `ssh d2`，
因此停止本机执行假设并直接检查目标机：

| Item | Observed |
| --- | --- |
| Host | `n37-125-152` |
| Kernel | `5.15.198.bsk.1-amd64` |
| CPU | 2 sockets, 64 cores, Intel Xeon Platinum 8457C |
| NUMA | 2 nodes; CPUs 0-31 on node 0 |
| GCC | 8.3.0 |
| Zig | 0.16.0; `zig cc` reports Clang 21.1.0 |
| glibc | 2.28 |
| `perf` | 5.15.198 |

FIL-C 0.684 x86-64 archive SHA-256:

```text
eefb594bcbc1261a18dfa8b50041674635f53df2b5fe067915b5652adaed4e3f
```

The FIL-C compiler requires glibc 2.34 or newer, so it cannot execute directly
on the d2 host. The supported isolation path used a pinned Debian bookworm
container:

```text
debian@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171
```

The minimal image lacked `ld`. A slow package-install image build was aborted;
the successful adapter mounted d2's `x86_64-linux-gnu-ld.bfd` and matching
`libbfd` read-only into the newer userspace. FIL-C compilation and execution
then succeeded entirely on d2.

Zig 0.16.0 was downloaded from the official release URL after d2's direct
download stalled. The local proxy path retrieved the artifact, both local and
d2 SHA-256 checks matched:

```text
70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
```

All new LLVM-frontend validation used `zig cc`; native performance binaries
continued to use GCC with the production flags.

## 3. Proof-First

The public header and native behavior tests were written before the
implementation. The first d2 link failed with the expected undefined symbols,
including:

```text
undefined reference to `rco_yield'
undefined reference to `rco_wait_fd'
undefined reference to `rco_runtime_create'
undefined reference to `rco_spawn'
undefined reference to `rco_runtime_run'
```

This established that the later green run came from the new implementation.

## 4. Runtime Debugging

### 4.1 FIL-C unsupported syscall

The first FIL-C run stopped at:

```text
filc user error: unsupported syscall: 158
rco_check_shadow_stack
```

Syscall 158 is x86-64 `arch_prctl`. The CET shadow-stack status probe is a
native platform guard, not allocator or scheduler logic. The FIL-C build now
uses `RCO_FILC` to exclude only this unsupported probe. Native builds retain
the fail-fast check.

### 4.2 Cancelled waiter skipped cleanup

The first native shutdown test failed because the scheduler directly reaped
every cancelled READY task. That is valid only before first resume. An already
started task must resume from `rco_wait_fd()` with `-ECANCELED` so its C frames
can release resources.

The fix added a `started` state bit:

```text
cancel before first resume -> terminal without entering user code
cancel after suspension    -> READY -> resume cancellation point -> terminal
```

### 4.3 L4 concurrency fixture backlog

The first 32-client E2E lost exactly two 256 KiB flows. Proxy counters showed:

```text
connect_errors=2
io_errors=0
```

The Python `ThreadingTCPServer` fixture used its default listen backlog of 5.
Increasing the test backend backlog to 512 removed the failures. The proxy
code was not weakened to accommodate an overloaded fixture.

### 4.4 Static analyzer ownership proof

Clang initially could not prove that two sequential `connection_release()`
calls were safe after a failed spawn, even though a retained session reference
made the count nonzero. The flow was simplified so references are retained
only after a successful non-eager `rco_spawn()`. This removed both the warning
and unnecessary rollback operations.

### 4.5 Finalizer and hot scheduler path

Independent simplification review found that cancelling a pump before its
first resume could skip the pump body's manual `connection_release()`. The
runtime gained an exactly-once task finalizer that runs on the scheduler stack
for normal completion, started cancellation, unstarted cancellation, and
runtime teardown. A regression test makes an unstarted cancelled task close
its owned FD from the finalizer.

The same review found an unconditional `clock_gettime()` in every scheduler
turn. Returning immediately when the timer heap is empty reduced the measured
`rco_yield` median from approximately 61 ns to approximately 34 ns. Completed
task unlink changed from a linear scan to an intrusive O(1) unlink, and the FD
watch table changed from one dense allocation to lazy 256-entry chunks.

### 4.6 Review-driven scheduler fairness

The first benchmark evidence contained zero-byte sender streams despite a high
aggregate throughput. Three independent review lenses traced the same cause:
the scheduler only called `epoll_wait()` when the ready queue became empty, so
a yielding hot task could keep I/O waiters asleep indefinitely.

The scheduler now performs a nonblocking epoll poll after at most 64 READY
dispatches. A deterministic pipe test keeps one coroutine runnable while
another waits on an already-readable FD. A separate test covers a task waiting
for `READ|WRITE` when only `EPOLLOUT` arrives.

The same pass changed normal I/O registration to persistent `EPOLLONESHOT`
with `EPOLL_CTL_MOD` rearm, propagated unexpected acceptor wait failures to
process shutdown, and made the iperf harness reject any zero-progress stream.
All 20 proxy streams in the final five-run evidence made progress.

### 4.7 Non-coroutine A/B baseline

The A/B baseline was intentionally implemented as a separate state machine,
not as a coroutine runtime with yields removed. Its first proof was the
expected CMake failure while `l4_forwarder_epoll.c` was absent. After
implementation, the existing E2E suite ran unchanged against both binaries.

The first three-way benchmark attempt failed because an old port range was
still unavailable. The harness's LISTEN probe rejected the run before any
sample was recorded. A later pre-fix run used `BASE_PORT=47000`; it was
superseded by the fairness fix and is not final evidence.

The baseline made continuation state explicit: direction offsets and lengths,
source EOF, destination shutdown, upstream connect state, FD generation and
deadline heap membership. This is the state that the coroutine version keeps
in ordinary C frames and locals.

### 4.8 A/B fairness and resource parser

Review found two coupled fairness bugs in the baseline: `drive_direction()`
deducted bytes at both `recv()` and `send()`, and both directions shared one
1 MiB event budget. A real `socketpair` test made one client FD simultaneously
readable and writable with 4 KiB queued in each direction. It failed before
the fix, then proved both directions transfer 4 KiB with independent budgets.
FIL-C executed the same focused state-machine test.

The first process CPU sampler also read `/proc/<pid>/stat` with fixed `awk`
fields. Since field 2 `comm` may contain spaces and `)`, that can shift
`utime/stime`. The replacement strips through the final `) ` delimiter and
parses `stat` plus `status` in one `awk` process. Its fixture uses
`(worker ) with spaces)` and verifies exact fields 14 and 15.

## 5. Validation Progression

```text
source archive
      |
      v
proof-first link failure
      |
      v
FIL-C lifecycle tests
      |
      v
native scheduler and ABI tests
      |
      v
guard-page subprocess test
      |
      v
real TCP concurrent E2E
      |
      v
ASan + UBSan E2E
      |
      v
optimized binary inspection
      |
      v
CPU/NUMA-pinned benchmarks
```

The final validation used an isolated d2 copy of source files whose hashes
matched pushed `master` commit
`ae0364683fc45e99847872a9f1fe32e5de98c1f4`. Generated binaries and logs stayed
under `/root/recallfs/.tmp/rco-ab` until the complete evidence set passed
validation.

## 6. Performance Notes

The context benchmark first established a nonzero observable sink and
inspected the final binary. Five run-level medians produced:

```text
rco_yield:    35.203 35.223 35.131 35.158 35.122 ns
function:      1.637  1.638  1.637  1.635  1.649 ns
sched_yield: 228.384 228.039 227.283 228.312 228.287 ns
```

The L4 benchmark used independent pinned CPUs on NUMA node 0:

```text
CPU 0: selected L4 proxy
CPU 1: iperf3 server
CPU 2: iperf3 client
```

Five position-rotated runs with `BASE_PORT=52000` yielded medians of
74.747 Gbit/s direct, 24.883 Gbit/s coroutine, and 25.012 Gbit/s epoll state
machine. Every one of
the 40 proxied streams made progress. Coroutine/epoll paired ratios changed
direction across runs, so the result is parity rather than a throughput win.

The coroutine process had median `VmPeak`/`VmHWM` of 4,724/1,940 KiB; the
epoll process had 2,948/1,828 KiB. The virtual gap reflects independent
coroutine stacks, while demand paging kept the resident gap much smaller.

PMU events were attempted, but d2's KVM returned
`<not supported>` for cycles, instructions, branches, branch misses, and cache
misses. No substitute instruction count was presented as hardware evidence.
