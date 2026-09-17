---
doc_id: recallfs-exploration-ringzero-study-v2
kind: study
status: archived
authority: evidence
applies_to:
  - learning/studies/20260917-ringzero
depends_on:
  - recallfs-study-ringzero-v2
  - recallfs-source-ringzero-study-v1
  - recallfs-evidence-ringzero-study-v2
supersedes:
  - recallfs-exploration-ringzero-study-v1
verified_by:
  - recallfs-evidence-ringzero-study-v2
---

# Exploration Log

## 1. Intake And Source Freeze

The task started from <https://github.com/immanuwell/ringzero> plus three
questions: kernel-bypass cost, eBPF value under a zero-drop requirement, and
Maglev trade-offs.

A complete git clone pinned revision
`9a56125f02fcbce55448482d442d99729573ab06`. The tree contains 18 files and no
license file, so no upstream source was copied into the tracked study. The
source page, papers and revision identity were archived under `learning/`.

The first framing error was to treat the request as a generic explainer. The
user corrected the workflow to [`skills/learn.md`](../../../skills/learn.md):
the result must preserve source, exploration, a reusable artifact and evidence
matched to each claim.

## 2. Architecture Read

The code separated into:

```text
Zig control plane
  attach + pinned maps + health checks + Maglev rebuild
                         |
                         v
NIC RX -> native XDP -> VIP -> flow hash -> Maglev -> backend -> rewrite -> redirect
```

RingZero is not DPDK-style userspace kernel bypass. Its packet program runs
inside the Linux kernel at the XDP driver hook. A redirected packet bypasses
later `sk_buff`, routing, netfilter and socket work on the load-balancer host
while the kernel still owns the NIC, driver and execution boundary.

## 3. Source Claims Versus Code

The upstream README labels the project a demo and says its veth setup cannot
establish tens-of-millions packet rates. The source review found further
boundaries:

1. Maglev membership comes from `BPF_MAP_TYPE_HASH` iteration without a
   canonical sort.
2. Backend hash identity is a locally allocated integer, not a stable endpoint
   identity.
3. A rebuild publishes 4099 individual map updates, not an atomic generation.
4. There is no connection tracking to protect active-flow affinity.
5. The data plane rewrites destination IP/MAC but does not define the return
   path.
6. IPv4 `frag_off` is not checked before interpreting L4 bytes.
7. `auto` attach can fall back to generic XDP.
8. The load generator reports successful sends without a receive sequence
   oracle, while the polling sink only reports sampled counts.

These became study findings rather than upstream patches because the
repository has no license granting modification or redistribution rights.

## 4. Upstream Test Failure

Direct execution on d2:

```bash
zig test src/maglev.zig
```

Result: 0 passed, 1 failed. The test creates backend IDs `[1,2,3,4]` but
requires every generated ID to be `< 4`; a valid ID 4 therefore fails. If that
assertion were simply relaxed, the next `counts[slot]` access would index a
four-element array with 4. The test name claims determinism but constructs only
one table.

This is evidence of a defective test, not evidence that the population loop is
incorrect.

## 5. Artifact Correction: Rust To C

The first artifact was a Rust selector benchmark executed on macOS. It
established useful properties, including exactly eight changed Maglev slots
for one reversed four-backend fixture, but it did not exercise the requested
Linux/XDP practice environment.

The user provided `ssh d2` and explicitly required a C artifact. The Rust
implementation was therefore superseded by:

- a C11 opaque selector API for Modulo, Ring, Rendezvous, Jump and Maglev;
- CMake out-of-source builds;
- a comparison benchmark with a result checksum and build fingerprint;
- a UDP sender/receiver with wire-format checks and an independent sequence
  bitmap;
- d2 automation for FIL-C, Zig/Clang, GCC and RingZero netns validation.

The old Rust/macOS logs remain as historical exploration, not current
authority.

## 6. Test-First C Migration

The first FIL-C run intentionally linked tests before the implementation and
failed with unresolved `rz_*` symbols. That preserved a real red state rather
than backfilling tests after the code.

The next negative result exposed a CLI bug: `--order-offset 0` was treated as
if zero were invalid. The parser was corrected to allow zero while retaining
the post-parse range check.

The final FIL-C 0.684 run passed:

- six hashing suites;
- the realistic benchmark code path;
- UDP encoding/decoding and bitmap accounting;
- a one-packet UDP send path.

Checked allocation arithmetic, canonical backend ownership, duplicate
rejection and explicit network byte order were added before the final run.
FIL-C timings were excluded from performance results.

## 7. d2 Toolchain And Build Findings

d2 is a Debian 10 KVM guest running kernel `5.15.198.bsk.1-amd64`, with 64
Xeon Platinum 8457C CPUs across two NUMA nodes. The selected benchmark CPU was
16 on NUMA node 0.

Practical compatibility failures mattered:

1. A pinned Zig 0.16.0 archive was installed and its SHA-256 recorded. The
   upstream BPF target spelling failed; `-target bpfel-freestanding` built both
   objects.
2. Debian's libbpf development metadata reported 4.19 while
   `libbpf.so.1.0.1` was present at runtime. Pinned v1.0.1 headers plus an
   explicit library path were required.
3. `bpftool` loaded and verified `xdp_lb_prog`; the JIT image was 1694 bytes.
4. The old `iproute2` parser warned about a zero-sized BTF `DATASEC` for the
   small `xdp_pass` object, but native veth attachment still succeeded.
5. The upstream Python probe used `bytes.hex(":")`, unsupported by Python 3.7.
   A temporary compatibility transformation allowed checksum/rewrite
   validation.

All compatibility changes lived in d2 temporary workspaces. They were not
applied to the unlicensed upstream checkout in this repository.

## 8. Selector Benchmark

Two optimized native builds were measured:

- Zig 0.16.0/Clang 21.1.0 with ThinLTO;
- GCC 8.3.0 with LTO.

Both used `-O3 -march=native -mtune=native -DNDEBUG`, CPU 16, five rotated
algorithm orders and identical datasets. Both produced exactly the same
balance, churn and checksum values.

Representative Zig medians:

| Algorithm | Lookup ns | Build us | Add churn | Remove-middle |
| --- | ---: | ---: | ---: | ---: |
| Modulo | 3.298 | 0.514 | 96.95% | 96.88% |
| Ring | 98.283 | 1138.465 | 2.87% | 3.28% |
| Rendezvous | 117.616 | 0.536 | 3.01% | 3.11% |
| Jump | 37.015 | 0.534 | 3.00% | 49.96% |
| Maglev | 3.294 | 66.704 | 4.76% | 5.20% |

The Zig 4x lookup workload took `4.205 s` versus `1.082 s` for 1x; GCC took
`4.194 s` versus `1.065 s`. Both final binaries retained the measured loop.
Hardware PMU events were unavailable in the KVM guest, so no cycles, cache,
TLB or branch claim was made.

## 9. Native-XDP Practice

The RingZero object passed the kernel verifier and attached to a veth in native
driver mode. A first two-backend rewrite probe failed because it assumed the
flow would select backend 1; the same config selected backend 2. The check was
corrected to validate the configured result rather than hard-code one backend.
This failure is consistent with, but does not independently prove, the
non-canonical membership finding.

A controlled one-backend E2E then sent 10,000 paced UDP packets through
RingZero:

```text
sender:   expected=10000 sent=10000
receiver: expected=10000 unique=10000 missing=0 duplicates=0 invalid=0
XDP:      packets=10000 dropped=0
cleanup:  pass
```

This closes the sequence ledger for a veth workload with requested 50 us
inter-packet pacing. The sender did not measure elapsed time, so no achieved
packet rate is claimed. The run proves packet rewrite/redirect correctness in
that envelope.

The upstream high-rate script produced the opposite lesson:

```text
sender submitted: 2407680
XDP observed:      1909482
sink reports:        12266
```

Because the sender and receiver populations do not reconcile and no sequence
oracle exists, the run is negative evidence for the script's ability to
qualify zero drop. It is not a measured RingZero loss rate: loss attribution
across source sockets, namespace veth queues, XDP redirect and polling sinks is
absent.

## 10. Remaining Target Work

d2 supplied real Linux build, verifier, JIT and native-veth evidence, but its
`eth0` is a one-queue `virtio_net` interface. The remaining qualification
requires:

- physical native-XDP NICs and exact driver/firmware identity;
- separate generator, DUT and receiver hosts;
- RSS queues, IRQ/CPU affinity and NUMA-local memory;
- 64/128/512/1500-byte packets, flow distributions and microbursts;
- an offered-load sweep with receiver sequence ledger and every-layer counters;
- CPU, p99/p999 latency and power measurements;
- backend failover during active flows and a complete return path;
- concurrent publication, crash/restart, schema migration and long soak.

The practical result is narrower and stronger than the original desktop
study: C selector behavior is reproduced under FIL-C and two native compilers,
and RingZero's bounded native-XDP path is observed. Physical-NIC throughput and
high-load zero drop remain explicitly unproven.
