---
name: "build-cache-acceleration"
description: "Diagnoses and optimizes compiler caching for Rust and C/C++ builds. Invoke for Kache, sccache, ccache, Blade, Cargo, Jenkins CI, cache misses, or build-cache rollout."
---

# Build Cache Acceleration

Optimize build time from measured evidence. Do not equate installing a cache
with accelerating a build.

## Required Outcome

Produce:

1. a phase-level baseline;
2. proof that intended compiler invocations reach the cache wrapper;
3. cold, local-hit, and remote-hit measurements where applicable;
4. correctness and trust-boundary evidence;
5. a go, no-go, or bounded-canary decision;
6. exact rollback steps.

For Kache-specific support and commands, read
[`references/kache.md`](references/kache.md). For Jenkins rollout, also read
[`references/jenkins.md`](references/jenkins.md).

## Workflow

```text
discover build path
        |
        v
measure phase baseline
        |
        v
identify current cache / remote execution owner
        |
        v
prove compiler interception
        |
        v
qualify cache correctness
        |
        v
measure cold / local / remote
        |
        v
inspect misses and overhead
        |
        v
canary with rollback
        |
        v
publish evidence and decision
```

### 1. Discover Before Changing

Record:

- repository revision and dirty state;
- build command, target, profile, and feature set;
- compiler, linker, SDK/sysroot, target triple, and relevant environment;
- runner OS, CPU, memory, filesystem, and network locality;
- whether the runner and workspace are persistent;
- existing ccache, sccache, Kache, Goma, remote execution, CAS, or build-system
  cache layers;
- where time is spent: checkout/dependencies, graph/configure, codegen,
  compile, link, test, and package.

Use verbose build output or the generated Ninja file to inspect the real
compiler command. Environment variables alone are not interception evidence.

If an existing remote execution or compiler cache is active, benchmark it as
the baseline. Do not stack wrappers without a documented ownership model.

### 2. Classify the Bottleneck

Compiler caching is a fit when repeated builds spend material wall time
recompiling unchanged translation units or crates.

It is not the primary fix when time is dominated by:

- dependency download;
- build-graph analysis or configure;
- source generation;
- a single large link;
- test execution;
- packaging or upload;
- serialized global locks;
- saturated disk, network, memory, or executor capacity.

Fix the dominant phase first. A high cache hit count cannot compensate for a
non-compile bottleneck.

### 3. Select an Integration Boundary

| Workload | Preferred boundary |
| --- | --- |
| Cargo/Rust | `RUSTC_WRAPPER` |
| Make/CMake/autotools | compiler launcher, `CC`/`CXX`, or compiler-name shims |
| Blade/Ninja | generated compiler command or toolchain config |
| Existing sccache | retain as baseline; migrate or use an explicitly supported fallback |
| Existing Goma/CAS/remote execution | choose one owner after comparative measurement |

The wrapper must see the real compile invocation, source inputs, dependency
metadata, and output paths. Absolute compiler paths, nested wrappers, response
files, PCH/modules, coverage, CUDA/HIP, and custom flags can change eligibility.

### 4. Prove Interception

Use an isolated cache and runtime directory. Run one clean build with verbose
compiler commands and a machine-readable cache report.

Require all of:

- compiler command resolves to the intended wrapper;
- report contains cacheable `miss` events on the first build;
- unsupported invocations have explicit passthrough reasons;
- build and tests pass.

If the report contains only passthroughs, stop. Diagnose flags and wrapper
ordering before measuring performance.

### 5. Qualify Correctness

Use the same revision and toolchain for:

1. uncached clean build;
2. cold cache build;
3. fresh output tree with warm local cache;
4. fresh workspace or runner with warm remote cache;
5. one representative source/header/config change.

Compare test results and required artifacts. For reproducible artifacts,
compare digests after removing known nondeterministic metadata. For Kache Rust
rollouts, use `KACHE_VERIFY=1` during qualification to recompile hits and
compare outputs; do not leave it enabled for normal performance runs.

Never add a C/C++ flag to an allowlist merely to improve hit rate. Establish
whether it changes object bytes, output shape, diagnostics consumed by the
build system, dependency discovery, host identity, or side files. Record the
proof and add one flag at a time.

### 6. Benchmark Fairly

Measure these phases separately:

| Phase | Store | Build output |
| --- | --- | --- |
| Baseline | cache disabled | fresh |
| Cold | empty | fresh |
| Local hit | populated locally | fresh |
| Remote hit | empty locally, populated remote | fresh |
| Source change | populated | one realistic edit |

Keep revision, toolchain, target, profile, features, runner class, CPU limits,
parallelism, and remote region fixed. Warm up once, collect repeated samples,
and report median and p95 wall time.

Record:

- end-to-end wall time;
- compile-weighted hit rate;
- cacheable versus passthrough coverage;
- key, lookup, restore, and store time;
- remote latency, bytes, and failures;
- duplicate outputs;
- cache size and GC behavior;
- correctness result.

Do not claim a speedup from a smoke test, one sample, or count-based hit rate.

### 7. Diagnose Misses

Classify misses before changing configuration:

- expected source/header/dependency change;
- compiler, linker, SDK, target, feature, or profile drift;
- checkout path leakage;
- volatile environment value;
- undeclared hidden input;
- unsupported invocation or flag;
- missing local persistence or remote publication;
- trust policy forcing read-only;
- remote timeout, credential, or namespace mismatch;
- over-specific key producing duplicate output bytes.

Prefer the cache's structured report and explanation command over log-string
guessing. Preserve the report as a CI artifact.

### 8. Roll Out Safely

Use a bounded canary:

- one representative job and target;
- pinned cache binary and checksum;
- isolated namespace;
- untrusted jobs read-only;
- trusted protected branch as the only writer;
- explicit size/retention limit;
- metrics and alerting;
- one-variable rollback.

Rollback must be possible by removing the wrapper environment/PATH entry or
setting the cache's disable switch. It must not require deleting the source
workspace.

## Decision Rules

Choose **go** only when:

- correctness gates pass;
- wrapper interception and cacheable coverage are proven;
- representative wall time improves beyond measured noise;
- remote and storage costs are acceptable;
- trust boundaries and rollback are operational.

Choose **bounded canary** when correctness is proven but production hit rate,
remote behavior, or workload coverage is not yet known.

Choose **no-go** when the build is dominated by uncached phases, wrappers do
not see the compile, unsafe inputs cannot be keyed, or hit overhead offsets the
saved compile time.

## Report Format

Return a concise evidence ledger:

| Field | Content |
| --- | --- |
| Baseline | revision, command, environment, phase times |
| Interception | actual compiler command and cache event proof |
| Eligibility | cached and passthrough counts with reasons |
| Correctness | tests, artifact comparison, invalidation result |
| Performance | median/p95 for baseline, cold, local, remote |
| Cost | storage growth, remote bytes/latency, operational burden |
| Risk | hidden inputs, poisoning, credentials, concurrency |
| Decision | go, no-go, or bounded canary |
| Rollback | exact switch and verification |

Label upstream claims, local observations, and production measurements
separately.
