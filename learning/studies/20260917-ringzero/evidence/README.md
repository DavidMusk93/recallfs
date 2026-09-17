---
doc_id: recallfs-evidence-ringzero-study-v1
kind: reference
status: active
authority: evidence
applies_to:
  - learning/studies/20260917-ringzero
depends_on:
  - recallfs-study-ringzero-v1
  - recallfs-runbook-ringzero-hashing-benchmark-v1
supersedes: []
verified_by:
  - learning/studies/20260917-ringzero/evidence/raw/clippy.txt
  - learning/studies/20260917-ringzero/evidence/raw/cargo-test-final.txt
  - learning/studies/20260917-ringzero/evidence/raw/cargo-test-release.txt
  - learning/studies/20260917-ringzero/evidence/raw/hash-comparison-m5pro.csv
  - learning/studies/20260917-ringzero/evidence/raw/upstream-zig-test.txt
  - learning/studies/20260917-ringzero/evidence/raw/doc-validation.txt
  - learning/studies/20260917-ringzero/evidence/raw/ascii-graphs.txt
---

# Evidence Ledger

## Observed

| Evidence | Environment | Result | Scope |
| --- | --- | --- | --- |
| [`raw/upstream-revision.txt`](raw/upstream-revision.txt) | Git source snapshot | Revision, tree and no-license-file check recorded | Source identity only |
| [`raw/upstream-zig-test.txt`](raw/upstream-zig-test.txt) | macOS ARM64, Zig 0.16.0 | Upstream Maglev test fails at `slot < 4`; exit 1 | Upstream Zig unit test only |
| [`raw/cargo-test-initial-failure.txt`](raw/cargo-test-initial-failure.txt) | macOS ARM64, Rust 1.97.0 | Initial overstrong order-sensitivity assertion failed with exactly 8 changed slots | Preserved falsification |
| [`raw/cargo-test-final.txt`](raw/cargo-test-final.txt) | macOS ARM64, Rust 1.97.0 | 7/7 property tests pass | RecallFS hashing benchmark semantics |
| [`raw/cargo-test-release.txt`](raw/cargo-test-release.txt) | macOS ARM64, Rust 1.97.0 release | 7/7 property tests pass | Optimized-build semantics |
| [`raw/clippy.txt`](raw/clippy.txt) | Rust 1.97.0 | `clippy --all-targets -D warnings` passes | Static Rust checks |
| [`raw/hash-comparison-m5pro.csv`](raw/hash-comparison-m5pro.csv) | Apple M5 Pro, release + ThinLTO | Five selectors measured with 1M distribution keys and 7 x 500k lookup samples | Selector-only local comparison |
| [`raw/local-environment.txt`](raw/local-environment.txt) | Local host | OS, CPU and compiler versions captured | Reproduction metadata |
| [`raw/doc-validation.txt`](raw/doc-validation.txt) | Repository | Six document link sets and seven-node dependency DAG pass | Documentation integrity |
| [`raw/ascii-graphs.txt`](raw/ascii-graphs.txt) | Repository verifier | ASCII-only diagrams and rail alignment pass | Diagram integrity |

## Hashing Result

The exact raw result is the authority. Rounded values:

| Algorithm | Max/avg | Add churn | Remove-middle churn | Lookup ns | Build us | Main bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Modulo | 1.011 | 96.95% | 96.88% | 0.494 | 0.042 | 128 |
| Ring, 256 vnodes/backend | 1.096 | 2.87% | 3.28% | 8.408 | 65.834 | 131,072 |
| Rendezvous | 1.010 | 3.01% | 3.11% | 43.287 | 0.042 | 128 |
| Jump | 1.010 | 3.00% | 49.96% | 19.430 | 0.042 | 128 |
| Maglev, 4099 slots | 1.007 | 4.76% | 5.20% | 0.681 | 57.583 | 16,524 |

Interpretation limits:

- `lookup_ns` excludes the common key hash and includes loop/sink overhead;
- sub-microsecond build results are near timer and harness overhead and should
  not be ranked;
- Ring's balance depends on vnode count; Jump's middle-removal result reflects
  dense bucket renumbering;
- Maglev trades more churn than Ring/Rendezvous here for table lookup and exact
  slot-count balance;
- none of these numbers include BPF map lookup, packet parsing, checksum,
  redirect, NIC DMA, queueing, or backend work.

## Upstream Test Failure

RingZero's test creates backend IDs `[1, 2, 3, 4]` but asserts every generated
slot is `< 4`. A valid slot containing backend ID `4` therefore fails. If that
assertion were changed, the next `counts[slot]` operation would index beyond a
four-element array. The test name mentions determinism, but the test constructs
only one table and does not compare repeated or reordered builds.

This failure does not prove the builder algorithm is incorrect. It proves the
checked-in test does not currently validate the property it claims.

## Not Observed

- eBPF verifier acceptance on a Linux kernel;
- native versus generic XDP attachment;
- real NIC RX/TX, RSS queue scaling, NUMA placement, or redirect success;
- packet loss, duplication, reordering, p99/p999 latency, CPU, power, or
  sustained line rate;
- backend failover under active flows;
- return traffic from backend to client;
- concurrent control-plane writers or crash recovery;
- compatibility across kernel, driver, libbpf, and pinned-map schema versions.

The absence of these observations is a hard boundary: this study does not call
RingZero production-ready or claim a reproduced packet-rate improvement.
