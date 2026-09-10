---
doc_id: recallfs-study-kache-build-cache-exploration-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-kache-build-cache
depends_on:
  - learning/studies/20260910-kache-build-cache/source.md
supersedes: []
verified_by:
  - learning/studies/20260910-kache-build-cache/demo/verify.sh
  - learning/studies/20260910-kache-build-cache/evidence/direct-cache-report.json
  - learning/studies/20260910-kache-build-cache/evidence/blade-without-allowlist-report.json
  - learning/studies/20260910-kache-build-cache/evidence/blade-with-allowlist-report.json
---

# Exploration Log

## 1. Source Acquisition

The IDE browser timed out while navigating to the supplied GitHub URL. The
public page and raw README remained available over HTTP. The repository was
then cloned and checked out at the `v0.19.0` tag:

```text
commit: 89b0c738115534156e759f157d0ce681b282d484
date:   2026-09-09T19:05:46+02:00
tag:    v0.19.0
```

The study read the architecture, cache-key, C/C++, CI, configuration,
deduplication, remote sync, compiler adapters, wrapper, store, and CI policy
implementations. It did not rely on the README alone.

The public Blade repository was cloned at:

```text
commit: b95bff3e4e35c3b53265c7522e42e1aedf84f3f8
date:   2026-08-13T22:17:42+08:00
```

Its toolchain selection, Ninja C/C++ rules, include-stack wrapper, and nominal
build-accelerator layer were inspected.

## 2. Mechanism Audit

### Rust

`compute_cache_key` includes compiler, target, crate identity, emit modes,
codegen options, source closure, extern contents, environment dependencies,
linker identity where relevant, and normalized paths. A dep-info pre-pass finds
source inputs unless an eligible input prediction can be reused.

### C/C++

`CcCompiler::cache_key` combines:

- key schema version;
- compiler name and version;
- resolved `-###` invocation;
- target architecture;
- modeled, probe-captured, raw-keyed, and user-allowlisted flags;
- dep-info shape;
- preprocessed output hash;
- include-shadowing digest.

`refuse_reasons` rejects unsupported shapes before key generation. This is the
main compatibility gate for Blade.

### Storage and remote

The wrapper checks local storage first, then asks the daemon for an exact
remote result. A miss joins a machine-wide flight, takes a memory-weighted
permit, obtains a per-key lock, rechecks local storage, compiles, and stores
content-addressed blobs.

Remote upload intent is persisted before asynchronous publication. Remote
failures do not prevent local compilation. The built-in untrusted-CI write
policy recognizes GitHub Actions and GitLab CI, but contains no Jenkins branch
or change-request detection.

## 3. Direct Wrapper Reproduction

The release binary was downloaded rather than built from source:

```bash
curl -fsSL \
  https://github.com/kunobi-ninja/kache/releases/download/v0.19.0/kache-aarch64-apple-darwin.tar.gz \
  -o .tmp/kache-study/kache.tar.gz
tar -xzf .tmp/kache-study/kache.tar.gz -C .tmp/kache-study/bin
```

The fixture then performed:

1. Rust build in `rust-a`;
2. identical Rust build in `rust-b`;
3. direct C++ compile to `answer-a.o`;
4. delete output;
5. identical C++ compile to `answer-b.o`.

Observed anchors:

| ID | Observation | Result |
| --- | --- | --- |
| `KC-RUST-01` | First Rust workspace is a miss | pass |
| `KC-RUST-02` | Second Rust workspace is a local hit with the same key | pass |
| `KC-CC-01` | First direct C++ object is a miss | pass |
| `KC-CC-02` | Second direct C++ object is a local hit with the same key | pass |
| `KC-DIRECT-03` | Report contains no error | pass |

The recorded report has 2 misses, 2 local hits, and 0 errors. The Rust hit
restored about 2.8 MiB via APFS reflink. The C++ object was copied because this
artifact path did not use the immutable hardlink route.

## 4. Blade Reproduction

### 4.1 Initial fixture failure

The first Blade attempt failed before compilation:

```text
Missing "hdrs" declaration. The public header files should be declared
explicitly, if no public header file, set "hdrs" to empty
```

The fixture was corrected to declare its header explicitly. This failure was
not attributed to Kache.

### 4.2 PATH interception without allowlist

`kache install-shims` created compiler-name symlinks, and the Blade-generated
Ninja command showed the shim path as `clang++`. Interception therefore worked.

However, every relevant object compile reported:

```text
unsupported|cc unsupported flag(s): -H - not yet
```

Observed anchors:

| ID | Observation | Result |
| --- | --- | --- |
| `KC-BLADE-01` | Blade resolves `clang++` to the Kache shim | pass |
| `KC-BLADE-02` | Default Blade object compiles are passthrough due to `-H` | pass |
| `KC-BLADE-03` | Two workspaces produce zero cacheable compile events | pass |

The report contains 44 passthrough events. Many are harmless compiler probes;
the decisive events are both `answer.cpp` calls being passthrough because of
`-H`.

### 4.3 Project-level `-H` allowlist

The fixture then added:

```toml
[cc]
extra_allowlist_flags = ["-H"]
```

`-H` prints the header inclusion tree to stderr and does not change object
code. Kache folds the spelling into the key and replays cached diagnostics.
The test includes a project-local header so an empty include stack cannot
produce a false pass.

Observed anchors:

| ID | Observation | Result |
| --- | --- | --- |
| `KC-BLADE-04` | First `answer.cpp` compile is a miss | pass |
| `KC-BLADE-05` | Second workspace gets a local hit with the same key | pass |
| `KC-BLADE-06` | Both Blade builds succeed | pass |
| `KC-BLADE-07` | Both `answer.cpp.incstk` files are byte-identical | pass |

The generated `scm.cc` missed in both workspaces with different keys. The
business TU still proved cross-workspace reuse.

The second `answer.cpp` wrapper event took longer than the first in this tiny
fixture. This rejects any performance conclusion from the smoke test while
preserving the compatibility conclusion.

## 5. Jenkins Analysis

The upstream shell-based CI recipe is mechanically usable in Jenkins, but its
trust policy is not. Source inspection found automatic forced read-only rules
only for `GITHUB_ACTIONS` and `GITLAB_CI`. A Jenkins pull-request build with
write credentials would therefore remain writable unless the pipeline sets
`KACHE_REMOTE_READONLY=1`.

Ephemeral Jenkins agents also lose local-store reuse. They need one of:

- a persistent, node-local `KACHE_CACHE_DIR`;
- a shared filesystem remote;
- an S3-compatible remote.

`KACHE_RUNTIME_DIR` must not be shared among executors because it contains the
daemon socket, locks, event logs, and session state.

## 6. What Was Not Tested

- No production Blade fork or actual Jenkins agent was available.
- Sailfish/Goma/CAS coexistence was not tested.
- No S3, MinIO, R2, or filesystem remote transfer was exercised.
- No Linux runner, GCC, clang-cl, Windows, or network-failure matrix was run.
- The tiny fixture is unsuitable for performance comparison.
- No claim is made about organization-specific credentials, cache retention,
  or compiler flags.

These are rollout gates, not assumptions to fill in.
