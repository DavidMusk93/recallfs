# Exploration Log

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
| Clang | 11.0.1 |
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

The final validation was rerun from a fresh clone of pushed `master` at commit
`1a824544d6a487d32dec3949b0e90deb1dd8b602`, not from the rsync working tree
used during development.

## 6. Performance Notes

The context benchmark first established a nonzero observable sink and
inspected the final binary. Five run-level medians produced:

```text
rco_yield:    33.952 33.777 34.029 33.854 33.999 ns
function:      1.639  1.634  1.642  1.635  1.638 ns
sched_yield: 229.096 231.198 228.380 227.298 227.552 ns
```

The L4 benchmark used independent pinned CPUs on NUMA node 0:

```text
CPU 0: coroutine L4 proxy
CPU 1: iperf3 server
CPU 2: iperf3 client
```

Five direct and five proxied samples yielded medians of 75.328 and
26.484 Gbit/s. PMU events were attempted, but d2's KVM returned
`<not supported>` for cycles, instructions, branches, branch misses, and cache
misses. No substitute instruction count was presented as hardware evidence.
