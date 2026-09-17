---
doc_id: recallfs-evidence-ringzero-study-v2
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260917-ringzero
depends_on:
  - recallfs-study-ringzero-v2
  - recallfs-runbook-ringzero-hashing-benchmark-v2
supersedes:
  - recallfs-evidence-ringzero-study-v1
verified_by:
  - learning/studies/20260917-ringzero/evidence/raw/d2-c/filc.txt
  - learning/studies/20260917-ringzero/evidence/raw/d2-c/benchmark-summary.csv
  - learning/studies/20260917-ringzero/evidence/raw/d2-c/evidence-manifest.sha256
  - learning/studies/20260917-ringzero/evidence/raw/d2-ringzero-build/verifier.txt
  - learning/studies/20260917-ringzero/evidence/raw/d2-ringzero/input-manifest.txt
  - learning/studies/20260917-ringzero/evidence/raw/d2-ringzero/sender.txt
  - learning/studies/20260917-ringzero/evidence/raw/d2-ringzero/receiver.txt
  - learning/studies/20260917-ringzero/evidence/raw/d2-ringzero/stats-after.txt
  - learning/studies/20260917-ringzero/evidence/raw/d2-ringzero/cleanup.txt
---

# Evidence Ledger

## Evidence Classes

| Class | Meaning |
| --- | --- |
| Source fact | Applies only to RingZero revision `9a56125f02fcbce55448482d442d99729573ab06` |
| Safety/correctness | Executed C paths under FIL-C or native CTest |
| Selector performance | C selector/checksum-loop benchmark on d2; excludes packet path |
| Packet-path observation | RingZero attached to native XDP on d2 veth |
| Negative evidence | A failed assertion, incomplete counter reconciliation, or unavailable measurement |

## Source And C Correctness

| Evidence | Environment | Result | Scope |
| --- | --- | --- | --- |
| [`raw/upstream-revision.txt`](raw/upstream-revision.txt) | Git source snapshot | Revision, tree and missing license recorded | Source identity |
| [`raw/c-migration/filc-red-link.txt`](raw/c-migration/filc-red-link.txt) | FIL-C 0.684 | Tests fail to link before implementation | Test-first red state |
| [`raw/c-migration/filc-cli-red.txt`](raw/c-migration/filc-cli-red.txt) | FIL-C 0.684 | `--order-offset 0` exposed a CLI validation defect | Preserved falsification |
| [`raw/c-migration/filc-final.txt`](raw/c-migration/filc-final.txt) | FIL-C 0.684 | Hashing suites and UDP oracle pass after fixes | Migrated C paths |
| [`raw/d2-c/filc.txt`](raw/d2-c/filc.txt) | d2, FIL-C 0.684 | 6 hashing suites, benchmark path, UDP self-test and send path pass | Executed C safety/correctness paths |
| [`raw/d2-c/zig-tests.txt`](raw/d2-c/zig-tests.txt) | d2, Zig 0.16.0/Clang 21.1.0 | CTest 7/7 pass | Optimized native semantics |
| [`raw/d2-c/gcc-tests.txt`](raw/d2-c/gcc-tests.txt) | d2, GCC 8.3.0 | CTest 7/7 pass | Independent native compiler |

FIL-C is a correctness and memory-safety gate, not a performance baseline.
The d2 native builds use `-O3 -march=native -mtune=native -DNDEBUG`; the Zig
build enables ThinLTO and the GCC build enables LTO. Exact commands are in
[`raw/d2-c/zig-compile-commands.json`](raw/d2-c/zig-compile-commands.json) and
[`raw/d2-c/gcc-compile-commands.json`](raw/d2-c/gcc-compile-commands.json).

## Selector Benchmark

Environment:

- d2 KVM guest, Linux `5.15.198.bsk.1-amd64`;
- Intel Xeon Platinum 8457C, CPU 16 pinned to NUMA node 0;
- five rotated algorithm orders per compiler;
- 1,000,000 distribution keys and 1,000,000 timed lookup keys;
- exact source, tool and binary digests preserved under `raw/d2-c/`.

The authority is
[`raw/d2-c/benchmark-summary.csv`](raw/d2-c/benchmark-summary.csv). Rounded
median-of-medians:

| Compiler | Algorithm | Max/avg | Add churn | Remove-middle | Lookup ns | Build us | Main bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Zig Clang 21.1 | Modulo | 1.011 | 96.95% | 96.88% | 3.298 | 0.514 | 128 |
| Zig Clang 21.1 | Ring | 1.096 | 2.87% | 3.28% | 98.283 | 1138.465 | 131,072 |
| Zig Clang 21.1 | Rendezvous | 1.010 | 3.01% | 3.11% | 117.616 | 0.536 | 128 |
| Zig Clang 21.1 | Jump | 1.010 | 3.00% | 49.96% | 37.015 | 0.534 | 128 |
| Zig Clang 21.1 | Maglev | 1.007 | 4.76% | 5.20% | 3.294 | 66.704 | 16,396 |
| GCC 8.3 | Modulo | 1.011 | 96.95% | 96.88% | 3.289 | 0.491 | 128 |
| GCC 8.3 | Ring | 1.096 | 2.87% | 3.28% | 92.804 | 1124.337 | 131,072 |
| GCC 8.3 | Rendezvous | 1.010 | 3.01% | 3.11% | 125.079 | 0.559 | 128 |
| GCC 8.3 | Jump | 1.010 | 3.00% | 49.96% | 35.259 | 0.525 | 128 |
| GCC 8.3 | Maglev | 1.007 | 4.76% | 5.20% | 3.290 | 62.485 | 16,396 |

Both compilers produced identical balance, churn and checksum values. Separate
1x/4x anti-dead-code-elimination probes and final-binary disassembly are
preserved for Zig and GCC. Hardware PMU events were unavailable in the KVM
guest, so the evidence is limited to pinned topology, wall/process time,
two-compiler agreement, disassembly, digests and work scaling.

These measurements exclude BPF map lookup, parsing, checksum rewrite,
redirect, NIC DMA, queueing and backend work. Sub-microsecond build results
must not be ranked as precise algorithm costs.

## RingZero Build And Verifier

| Evidence | Result | Boundary |
| --- | --- | --- |
| [`raw/d2-ringzero-build/environment.txt`](raw/d2-ringzero-build/environment.txt) | Debian 10, kernel 5.15, Zig 0.16.0, one-queue `virtio_net` | Target identity |
| [`raw/d2-ringzero-build/bpf-build.txt`](raw/d2-ringzero-build/bpf-build.txt) | `xdp_lb.o` and `xdp_pass.o` built with `-target bpfel-freestanding` | BPF compilation |
| [`raw/d2-ringzero-build/verifier.txt`](raw/d2-ringzero-build/verifier.txt) | `xdp_lb_prog` loaded; 2896 B translated, 1694 B JIT | Kernel verifier/JIT |
| [`raw/d2-ringzero-build/control-ldd.txt`](raw/d2-ringzero-build/control-ldd.txt) | Control binary linked to the selected runtime libraries | Dependency identity |
| [`raw/d2-ringzero-build/upstream-maglev-test.txt`](raw/d2-ringzero-build/upstream-maglev-test.txt) | Direct upstream test exits 1 at `slot < 4` | Upstream test defect |

The upstream BPF target spelling did not work with this Zig release; the
successful target was `bpfel-freestanding`. Debian's development headers and
default linker library were older than the installed `libbpf.so.1.0.1`, so the
build used pinned libbpf v1.0.1 headers and an explicit runtime library path.
These are compatibility findings, not tracked patches to the unlicensed
upstream source.

## Native-XDP Packet Path

The controlled probe used one backend and independent sequence accounting:

| Evidence | Exact observation |
| --- | --- |
| [`raw/d2-ringzero/attach.txt`](raw/d2-ringzero/attach.txt) | `xdp_lb_prog` attached in native driver mode |
| [`raw/d2-ringzero/sender.txt`](raw/d2-ringzero/sender.txt) | expected=10,000, sent=10,000 |
| [`raw/d2-ringzero/receiver.txt`](raw/d2-ringzero/receiver.txt) | unique=10,000, missing=0, duplicates=0, invalid=0 |
| [`raw/d2-ringzero/stats-after.txt`](raw/d2-ringzero/stats-after.txt) | XDP packets=10,000, dropped=0 |
| [`raw/d2-ringzero/cleanup.txt`](raw/d2-ringzero/cleanup.txt) | namespaces, links and pins cleaned |

This proves the bounded one-backend native-XDP/veth path with requested 50 us
inter-packet pacing. The sender did not measure elapsed time, so this ledger
does not assert achieved pps or a high-load zero-drop envelope. The run-local
input manifest binds the upstream revision, RingZero/BPF objects, sequence
binary and validation script to the packet evidence.

The upstream high-rate demo is negative evidence. Its sender reported
2,407,680 sends in two seconds (`1,203,734 pps`), the XDP counter observed
1,909,482 packets, and polling UDP sinks reported only 12,266 packets in total.
The sender has no sequence ledger and the sink counts cannot be reconciled to
the offered population, so
[`raw/d2-ringzero-history/high-rate-demo-excerpt.txt`](raw/d2-ringzero-history/high-rate-demo-excerpt.txt)
cannot support a zero-drop claim. The file preserves the decisive counters
from the earlier transient run; the final controlled-run directory is kept
separate so reruns cannot overwrite this negative evidence.

An earlier fixed-backend assertion also failed because the same two-backend
configuration selected backend 2 instead of the assumed backend 1. This
observation is consistent with the audited non-canonical BPF hash-map iteration
path, but one observation alone does not prove its cause. The exact rewrite
excerpt is preserved in
[`raw/d2-ringzero-history/fixed-backend-expectation-failure-excerpt.txt`](raw/d2-ringzero-history/fixed-backend-expectation-failure-excerpt.txt).

## Superseded Historical Evidence

The root `raw/cargo-*`, `raw/clippy.txt`,
`raw/hash-comparison-m5pro.csv`, `raw/local-environment.txt` and
`raw/upstream-zig-test.txt` files preserve the initial Rust/macOS exploration.
They are historical evidence only. The maintained artifact and current result
authority are the C implementation and d2 evidence above.

## Not Observed

- physical-NIC XDP, RSS/multi-queue scaling, NIC DMA counters or NUMA locality;
- a sustainable offered-load curve, microbursts, p99/p999 latency, CPU or power;
- high-rate or long-soak zero loss with an independent sequence ledger;
- backend failover while active TCP/UDP flows remain live;
- a complete backend-to-client return path;
- concurrent control-plane writers, atomic generation publication or crash
  recovery;
- upgrade and pinned-map schema compatibility across kernel/driver versions.

These gaps are hard boundaries. The evidence supports a mechanism study and a
bounded veth correctness result, not a production-readiness or line-rate claim.
