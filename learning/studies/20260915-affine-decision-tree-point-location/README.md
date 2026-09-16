---
doc_id: recallfs-study-affine-decision-tree-point-location-v1
kind: study
status: active
authority: design
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location/lib
  - learning/studies/20260915-affine-decision-tree-point-location/example
depends_on:
  - recallfs-agent-ready-docs-v1
  - recallfs-source-affine-decision-tree-point-location-v1
  - recallfs-study-affine-decision-tree-point-location-exploration-v1
supersedes: []
verified_by:
  - ODT-RA-1
  - ODT-RA-2
  - ODT-RA-3
  - ODT-RA-4
  - ODT-RA-5
  - ODT-RA-6
  - ODT-RA-7
  - ODT-RA-8
---

# Production Oblique Decision Tree Point Location

## Decision

本 study 的交付物是可复用的 C11 library，不是一次性 demo。调用方提供矩形
domain、二维 sites 和 region IDs；library 构造被矩形裁剪的 Voronoi cells，
编译出 immutable Oblique Decision Tree generation，并接受 scalar 或 batch
point queries，返回 region、outside 或错误结论。

核心方法是转化和化归：

```text
spatial nearest-site problem
              |
              v
piecewise-constant region function
              |
              v
exact half-space predicates
              |
              v
mutable geometry and BSP builder
              |
       validate and compile
              |
              v
compact immutable runtime program
              |
              v
compare, branch, return conclusion
```

查询快的来源是把重复几何工作前移到构建阶段。运行时成本取决于树深，而不是每次
遍历全部 sites 或 polygons。这个结论不自动外推到 DuckDB、其他数据分布或其他
硬件。

## Delivered Surface

| Path | Responsibility |
| --- | --- |
| [`lib/include/odt.h`](lib/include/odt.h) | versioned public C API and ownership contract |
| [`lib/src/`](lib/src/) | exact predicates, Voronoi construction, BSP compile, query, persistence |
| [`lib/README.md`](lib/README.md) | installation and production integration runbook |
| [`example/data/`](example/data/) | canonical 1,000-site and 3,102-query corpus |
| [`example/src/odt_example.c`](example/src/odt_example.c) | human and machine correctness workflow |
| [`example/src/benchmark.c`](example/src/benchmark.c) | checked native tree-versus-exact-brute benchmark |
| [`example/python/`](example/python/) | deterministic generator and independent `Fraction` oracle |
| [`verify.sh`](verify.sh) | FIL-C-first qualification and evidence promotion |
| [`evidence/`](evidence/) | revision-, command-, digest-, and environment-bound evidence |

The historical hand-authored evaluator remains under [`demo/`](demo/) as
exploration material. It is not the production library and does not define the
current API.

## Semantics

For sites $s_i$ and $s_j$ and query point $p$, nearest-site comparison is:

$$
\lVert p-s_i\rVert^2\le\lVert p-s_j\rVert^2
$$

Expanding and cancelling $p^Tp$ gives an affine predicate:

$$
2(s_i-s_j)^Tp\ge\lVert s_i\rVert^2-\lVert s_j\rVert^2
$$

Each runtime node therefore needs only one site-pair bisector, two child
references, and a certified floating filter. Uncertain comparisons fall back
to allocation-free exact dyadic arithmetic over the original binary64 values.
There is no heuristic uncertainty band.

The result contract is:

- points on the closed rectangle boundary are inside;
- points outside return `ODT_RESULT_OUTSIDE`;
- non-finite scalar inputs return `ODT_INVALID_DATA`;
- exact equal-distance ties choose the lower input ordinal;
- region ID values do not affect tie-breaking;
- scalar, batch, persisted/restored, C exact baseline, and Python `Fraction`
  oracle must agree.

## Build And Publish

```text
caller-owned sites
        |
        v
validate domain, IDs, limits, numeric environment
        |
        v
construct exact-certified clipped Voronoi cells
        |
        v
choose deterministic balanced cuts and split fragments
        |
        v
prove leaf paths and compile packed runtime generation
        |
        v
return unpublished generation + build statistics
        |
        v
application publishes one complete pointer
```

Construction is fail-closed. Invalid data, exhausted limits, allocation
failure, unsupported floating environment, or failed path proof returns no
generation. The builder reports node, leaf, fragment, work, depth, peak
temporary memory, and duration metrics.

The split objective minimizes:

```text
(largest child region count,
 new fragment count,
 total child fragments,
 defining-site ordinal pair)
```

Both children must be nonempty and the defining sites must land on opposite
sides. These conditions make progress deterministic while controlling tree
depth and polygon fragmentation.

## Query And Concurrency

```text
published immutable generation
        |
        +--> scalar reader --> affine path --> conclusion
        |
        +--> batch reader  --> affine paths --> conclusions
        |
        +--> metadata reader
```

Queries allocate no memory and mutate no generation state. Concurrent readers
may share a live generation without library locks. Batch envelope validation
is transactional: an invalid count, stride, range, or overlap fails before any
result changes; after acceptance every slot is written once.

Publication and reclamation remain application responsibilities:

```text
readers use generation A
        |
builder creates and validates generation B
        |
application atomically publishes B
        |
new readers use B; existing readers may still use A
        |
application proves A is quiescent
        |
destroy A
```

An atomic pointer swap alone is insufficient. The application needs an epoch,
hazard-pointer, reference-count, or grace-period protocol before destroying
the old generation.

## Persistence And Recovery

The version-1 wire format is little-endian and independent of C structure
layout. It stores original site bits and tree topology, then derives runtime
filters again during load. CRC32C, section bounds, resource limits, numeric
requirements, and graph invariants are checked before publication.

```text
live generation
        |
encode -> temp file -> fsync file -> rename -> fsync directory
        |
        v
versioned snapshot
        |
load -> validate all sections and invariants
        |
        v
unpublished restored generation
```

Pre-rename failures preserve the old destination. A failure after rename but
before parent-directory sync returns `ODT_COMMIT_UNKNOWN`; recovery reopens and
validates the destination before deciding whether to retry. The atomic-file
contract covers local POSIX filesystems, not network filesystems.

## Generated Corpus

The maintained generator uses PCG32 seed `20260916` and writes canonical
binary64 hexadecimal CSV:

| Input class | Count |
| --- | ---: |
| non-uniform sites | 1,000 |
| seeded random queries | 2,048 |
| site-coordinate queries | 1,000 |
| exact bisector queries | 32 |
| rectangle-boundary queries | 8 |
| outside queries | 8 |
| non-finite queries | 6 |
| all queries | 3,102 |

The Python 3.13.12 oracle uses `Fraction.from_float`; it neither reads nor
executes the C tree. The C benchmark uses a second independent method: all
finite in-domain binary64 coordinates are converted to one exact power-of-two
integer scale, then squared distances are compared with fixed-limb unsigned
arithmetic.

## Build And Verify

Run the complete qualification workflow:

```bash
learning/studies/20260915-affine-decision-tree-point-location/verify.sh
```

The maintained order is:

```text
FIL-C correctness
  -> fixed Zig Debug and Release
  -> Zig ASan/UBSan
  -> static analysis and formatting
  -> generated protocol and Python exact oracle
  -> corruption, fault, and concurrency gates
  -> clean install and C/C++ consumers
  -> named-target access attempt
  -> native benchmark and disassembly
  -> controlled negative probes
  -> digest manifest promotion and verification
```

The native benchmark uses at least:

```text
-O3 -march=native -mtune=native -DNDEBUG
```

It interleaves repeated scalar, batch, restored-scalar, and exact brute-force
samples after warmup. Every output contributes to an observable checksum, and
every timed pass is compared with the loop-adjusted validated checksum. The
steady-state gate requires scalar and batch median throughput to exceed exact
brute force by at least `2x`, and the lower bound of each paired log-ratio 95%
confidence interval to exceed `1.0x`.

Build wall time, load time, peak temporary memory, break-even query counts, and
total-cost speedup for one canonical 3,102-query corpus are reported
separately. This prevents steady-state throughput from hiding construction
cost. Runs without supported CPU/NUMA binding are explicitly marked local,
unpinned, and non-target-qualifying.

See [`evidence/README.md`](evidence/README.md) for observed values and explicit
limitations.

## Operational Boundaries

- Updates rebuild a complete generation; there is no in-place site mutation.
- The application must retain allocator context until generation destruction.
- Encodes and file saves are not concurrent operations on the same generation
  in version 1.
- Limits bound sites, fragments, nodes, depth, build work, build memory,
  encoded bytes, and load allocation.
- FIL-C validates executed paths and is not a proof or performance baseline.
- Sanitizer, static-analysis, differential, concurrency, corruption, package,
  and benchmark evidence answer different questions and are not substitutes.
- The named target may deny credentials or PMU access; that remains a recorded
  limitation rather than being replaced with VM or local results.
- This study does not implement a DuckDB extension and makes no claim to
  reproduce the article's `59x` result.

## Reconciliation Anchors

| Anchor | Contract | Verification |
| --- | --- | --- |
| `ODT-RA-1` | Invalid domains, duplicate IDs/sites, non-finite sites, and exhausted limits publish nothing | API, geometry, builder, and fault tests |
| `ODT-RA-2` | Exact binary64 nearest-site ties use input ordinal | C exact tests and Python `Fraction` oracle |
| `ODT-RA-3` | Canonical tree stays within depth, comparison, fragment, memory, and exact-fallback limits | optimized architecture gate |
| `ODT-RA-4` | Scalar, batch, restored, and independent oracle conclusions agree | 3,102-query differential protocol |
| `ODT-RA-5` | Invalid batch envelopes are transactional; accepted batches write every slot | batch tests and FIL-C |
| `ODT-RA-6` | Corruption and every maintained allocation/I/O failpoint reject without partial publication | corruption and fault tests |
| `ODT-RA-7` | Concurrent readers preserve the single-thread checksum; package consumers use installed artifacts only | concurrency, TSan capability, C/C++ consumer gates |
| `ODT-RA-8` | Benchmark variants preserve one classification checksum and survive DCE/semantic negative probes | native benchmark, disassembly, and evidence manifest |

## Evidence Boundary

The evidence establishes the behavior of this revision and the environments
recorded in [`evidence/manifest.json`](evidence/manifest.json). It does not
establish target-machine performance when target access is denied, deployment
under an application's reclamation protocol, crash durability on unsupported
filesystems, or end-to-end database performance.
