---
doc_id: recallfs-exploration-ringzero-study-v1
kind: study
status: archived
authority: evidence
applies_to:
  - learning/studies/20260917-ringzero
depends_on:
  - recallfs-study-ringzero-v1
  - recallfs-source-ringzero-study-v1
supersedes: []
verified_by:
  - recallfs-evidence-ringzero-study-v1
---

# Exploration Log

## 1. Intake And Source Freeze

The task started from <https://github.com/immanuwell/ringzero> plus three
questions: kernel-bypass cost, eBPF value under a zero-drop requirement, and
Maglev trade-offs.

The first browser fetch reached GitHub but exposed only a sign-in shell. A
complete git clone succeeded, so the investigation pinned revision
`9a56125f02fcbce55448482d442d99729573ab06` under
`.tmp/data/ringzero-upstream/`. The source tree has 18 files and no license
file. No upstream source was copied into the tracked study.

The first framing error was to treat the request as a generic visual explainer.
The user corrected the workflow to `skills/learn.md`. The output was therefore
reframed as a durable study plus a bounded benchmark, with source archives,
exploration history, reconciliation anchors, and explicit target-machine gaps.

## 2. Architecture Read

The code separated cleanly into:

```text
Zig control plane
  attach + pinned maps + health checks + Maglev rebuild
                         |
                         v
NIC RX -> native XDP -> VIP -> flow hash -> Maglev -> backend -> rewrite -> redirect
```

The important terminology correction was immediate: RingZero is not a DPDK-like
userspace kernel bypass. Its fast path runs inside the Linux kernel at the XDP
driver hook. It bypasses later kernel-stack work such as `sk_buff`, routing,
netfilter and sockets for redirected packets.

## 3. Source Claims Versus Code

The README correctly labels the project a demo and warns that veth does not
establish tens-of-millions packet rates. The source review found additional
boundaries:

1. Maglev membership is collected by iterating `BPF_MAP_TYPE_HASH`, without a
   canonical sort.
2. The hash identity is a locally allocated integer backend ID, not the stable
   endpoint identity.
3. A rebuilt table is published with 4099 individual map updates, not an
   atomic generation swap.
4. There is no connection-tracking table, although the original Maglev system
   relies on connection tracking as primary affinity protection.
5. The data plane rewrites destination IP and MAC but contains no return-path
   implementation. Calling that alone DSR does not define how a backend reply
   recovers the client-visible VIP source.
6. `frag_off` is not checked. A non-initial IPv4 fragment can be interpreted as
   though its payload started with a TCP or UDP header.
7. Generic XDP is an automatic fallback, so an attach success does not prove
   the native driver path used by performance claims.
8. The packet generator ignores `ENOBUFS` and reports submitted sends; backend
   sinks do not validate sequence numbers. It cannot prove zero loss.

These became study findings rather than code changes because the requested
subject is the upstream project, which grants no modification license.

## 4. Upstream Test

Command:

```bash
zig test src/maglev.zig
```

Observed result: 0 passed, 1 failed. The test feeds IDs `[1,2,3,4]` but checks
`slot < 4`, so valid backend ID 4 fails. The next count indexing operation
would also be out of range. The test does not execute its claimed determinism
property.

This is a test defect, not evidence that the core population loop is wrong.

## 5. Artifact Selection

An XDP demo would duplicate RingZero and could not run on the local macOS host.
A production load-balancer library was not requested. The unresolved,
cross-project question was Maglev's actual trade-off against common selectors,
so `benchmark/` was selected.

The benchmark uses Rust because it is a maintained, reusable, memory-safe
artifact and does not introduce new C requiring a Linux/FIL-C adapter. It
compares the same stable key stream across:

- Modulo;
- Ring with 256 virtual nodes per backend;
- Rendezvous;
- Jump;
- Maglev with 4099 slots.

## 6. Failed Assumption

The first negative test asserted that reversing four backend IDs would change
more than half of the Maglev table. It failed:

```text
expected material order sensitivity, saw 8 changed slots
```

The interpretation was corrected rather than weakening evidence:

- input order does affect the table, so unsorted map iteration is not a valid
  distributed consistency contract;
- the measured effect for this exact fixture is 8 of 4099 slots, not a
  catastrophic rebuild;
- even a small difference matters to the affected active flows when there is
  no connection tracking.

The final anchor checks the exact value 8 and separately verifies that a
canonical builder produces identical tables.

## 7. Final Local Run

```bash
cargo test \
  --manifest-path learning/studies/20260917-ringzero/benchmark/Cargo.toml

cargo run --release \
  --manifest-path learning/studies/20260917-ringzero/benchmark/Cargo.toml \
  --bin compare -- \
  --keys 1000000 \
  --lookup-keys 500000 \
  --rounds 7 \
  --backends 32 \
  --table-size 4099 \
  --vnodes 256
```

Results:

- property tests: 7/7 pass;
- Maglev sampled max/average: `1.006976`;
- Maglev add/remove-middle churn: `4.7569%` / `5.2030%`;
- Ring add/remove-middle churn: `2.8685%` / `3.2751%`;
- Rendezvous add/remove-middle churn: `3.0103%` / `3.1076%`;
- Jump add/remove-middle churn: `3.0044%` / `49.9622%`;
- selector-only lookup median on Apple M5 Pro:
  Maglev `0.681 ns`, Ring `8.408 ns`, Jump `19.430 ns`,
  Rendezvous `43.287 ns`.

The lookup numbers are not an XDP performance claim. They only demonstrate the
control-plane/precomputed-table trade: Maglev spends more work and memory at
membership change time so the packet selector becomes one modulo plus one
contiguous table access.

## 8. Remaining Target Work

The study could not execute XDP on macOS. A valid zero-drop and packet-rate
qualification still requires:

- Linux target kernel and exact revision;
- native-XDP-capable NIC and driver;
- separate generator and receiver hosts;
- sequence-number oracle at the receiver;
- hardware, driver, XDP action, redirect, egress and receiver counters;
- queue/RSS/NUMA/IRQ placement;
- packet sizes, flow cardinalities, burst model and offered-load sweep;
- backend add/down/up while long-lived flows are active;
- control-plane race and crash injection;
- soak evidence and a declared loss confidence bound.
