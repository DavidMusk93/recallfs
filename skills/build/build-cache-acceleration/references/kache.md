# Kache Qualification Guide

This reference is based on Kache `v0.19.0`. Check the installed version and
current upstream documentation before applying flags because the project
changes quickly.

The evidence-backed study is
[`learning/studies/20260910-kache-build-cache/README.md`](../../../../learning/studies/20260910-kache-build-cache/README.md).

## Capability Matrix

| Workload | Supported path | Important limits |
| --- | --- | --- |
| Rust libraries/build scripts | `RUSTC_WRAPPER=kache` | hidden inputs must be declared |
| Rust executables | supported on Linux/macOS | Windows disabled by default; runtime/linker identity gates apply |
| GCC/Clang C/C++ objects | PATH shims or `kache cc/c++` | conservative single-source compile only |
| clang-cl objects | explicit wrapper | debug objects remain local |
| C/C++ link | passthrough | not a link cache |
| CUDA/HIP | passthrough | multi-pass language model unsupported |
| PCH/modules | passthrough | dependency/output model unsupported |
| coverage/split DWARF | passthrough | path and side-output semantics unsupported |
| response files | passthrough | C/C++ response expansion unsupported in 0.19.0 |
| Remote | S3-compatible or filesystem | remote execution is not provided |

## Install and Pin

Prefer a release binary in CI:

```bash
kache --version
sha256sum /opt/kache/bin/kache
```

Pin both version and digest in the runner image or tool manifest. Building with
`cargo install kache` requires the release's declared minimum Rust version
(`1.95` for Kache 0.19.0).

## Rust Setup

One-job qualification:

```bash
export RUSTC_WRAPPER=/opt/kache/bin/kache
export KACHE_CACHE_DIR="$PWD/.tmp/kache-store"
export KACHE_RUNTIME_DIR="$PWD/.tmp/kache-runtime"
export KACHE_PROGRESS=verbose

cargo build --locked
rm -rf target
cargo build --locked
kache report --format json --since 15m --output kache-report.json
```

Use two separate worktrees or checkout paths to prove path portability. A
second command against an unchanged Cargo target only proves Cargo freshness,
not a compiler-cache hit.

For hidden inputs:

```toml
# crate-local kache.toml
extra_inputs = [
  ".sqlx/**/*.json",
  "migrations/**/*.sql",
]
```

For proc-macro environment inputs:

```toml
# .kache.toml
[cache]
key_env_vars = ["BOLTFFI_*"]
```

Do not include volatile job IDs, timestamps, or workspace paths unless they
really change compiler output; doing so destroys reuse.

## C/C++ Setup

PATH interception:

```bash
kache install-shims --force "$WORKSPACE_TMP/kache-shims"
export PATH="$WORKSPACE_TMP/kache-shims:$PATH"
```

Explicit prefix:

```bash
export CC="kache cc"
export CXX="kache c++"
export CC_KNOWN_WRAPPER_CUSTOM=kache
```

The `CC_KNOWN_WRAPPER_CUSTOM` setting is needed by Rust's `cc` crate for the
prefix form. It is unnecessary for compiler-name shims.

Prove interception from verbose output:

```bash
command -v cc
command -v c++
kache doctor --json
```

Then require a first-build `miss`, remove only the build output, rebuild, and
require a matching-key `local_hit`.

## Blade

For the public Blade revision tested by the companion study:

- compiler-name PATH shims are discovered when `cc_toolchain_config` does not
  pin `prefix`, `cc`, or `cxx`;
- the generated compile command includes `-H`;
- Kache 0.19.0 rejects `-H` unless explicitly allowlisted;
- adding the following project config allowed a normal business TU to hit
  across workspaces while preserving identical include-stack output:

```toml
[cc]
extra_allowlist_flags = ["-H"]
```

This is not a universal Blade recipe. Before using it:

1. run `blade ... --verbose`;
2. inspect the generated compiler command;
3. confirm the compiler path resolves through Kache;
4. inspect `kache report` passthrough reasons;
5. test local headers, generated headers, depfiles, include-stack checks, and a
   source/header change;
6. run the full target tests.

If Blade uses an explicit compiler path, configure that toolchain to point at a
Kache compiler-name shim. Do not replace the linker or archiver with Kache.

If an internal Blade uses Sailfish, Goma, or CAS, establish one compile-cache
owner. Compare:

```text
existing remote path
Kache local-only path
Kache remote path
uncached baseline
```

Do not run stacked production wrappers until their ordering, cache ownership,
and failure behavior are explicit.

## Local and Remote Layout

Use separate state scopes:

```bash
export KACHE_CACHE_DIR=/var/cache/kache/$NODE_NAME
export KACHE_RUNTIME_DIR="$WORKSPACE_TMP/kache-$BUILD_TAG"
```

`KACHE_CACHE_DIR` may persist and be shared by concurrent local jobs. Keep
`KACHE_RUNTIME_DIR` unique per executor/build because it contains sockets,
locks, event logs, and session state.

S3-compatible remote:

```bash
export KACHE_S3_BUCKET=my-build-cache
export KACHE_S3_REGION=us-east-1
# KACHE_S3_ENDPOINT for MinIO, R2, or Ceph
```

Filesystem remote:

```toml
[cache.remote]
type = "filesystem"
path = "/mnt/build-cache/kache"
```

Explicit ephemeral-runner lifecycle:

```bash
kache sync --pull
run_the_build
kache report --format json --output kache-report.json
kache sync --push
```

The daemon can restore and upload on demand, but explicit push is useful before
an ephemeral runner exits.

## Trust Boundary

Kache 0.19.0 automatically forces untrusted GitHub Actions and GitLab CI jobs
read-only. Jenkins is not included in that detection.

Set:

```bash
export KACHE_REMOTE_READONLY=1
```

for pull requests, unprotected branches, tags, replayed jobs, and any build
running untrusted code. Give write credentials only to protected-branch jobs.
Prefer workload identity or an instance role over long-lived access keys.

## Qualification Controls

Correctness:

```bash
KACHE_VERIFY=1 cargo build --locked
```

This recompiles Rust hits and compares outputs. It intentionally eliminates
the performance benefit and belongs only in qualification.

Stored-blob integrity:

```bash
KACHE_VERIFY_RESTORES=always cargo build --locked
```

Miss explanation:

```bash
kache why-miss crate_name
KACHE_LOG=trace cargo build -p crate_name 2>&1 \
  | grep "\\[key:crate_name\\]"
```

C/C++ passthrough inspection:

```bash
KACHE_LOG=kache=debug blade build //path:target --verbose
kache report --format json --output kache-report.json
jq '.bypass.reasons' kache-report.json
```

Useful report projections:

```bash
jq '{
  summary,
  storage,
  network,
  bypass: .bypass.reasons
}' kache-report.json
```

```bash
jq '[
  .all_events[]
  | {crate_name, result, elapsed_ms, compile_time_ms, cache_key}
]' kache-report.json
```

## Tuning

Consider only after baseline evidence:

| Setting | Use |
| --- | --- |
| `KACHE_MIN_STORE_COMPILE_MS` | avoid storing compiles cheaper than cache overhead |
| `KACHE_MAX_SIZE` | bound local registered blob bytes |
| `KACHE_PREFETCH_ENABLED=0` | avoid speculative work on already-warm persistent runners |
| `KACHE_BASE_DIR` | normalize one missing checkout/container root |
| `paths.base_dirs` | normalize several proven equivalent roots |
| `KACHE_KEY_SALT` | separate an unobserved toolchain/ABI boundary |
| `KACHE_EXPLAIN_MISS=1` | retain changed key groups |
| `KACHE_LOCAL_ONLY=1` | isolate local behavior from remote problems |
| `KACHE_DISABLED=1` | immediate rollback/passthrough |

Never broaden path normalization or flag allowlists speculatively. Both can
merge identities that should remain distinct.
