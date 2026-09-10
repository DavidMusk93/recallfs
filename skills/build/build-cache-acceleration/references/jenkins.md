# Jenkins Build-Cache Rollout

Use this reference after the main skill has shown that compiler work is a
material bottleneck.

## State Model

| State | Scope | Rule |
| --- | --- | --- |
| Build output | one checkout/job | never share writable output trees across jobs |
| Kache runtime | one executor/build | unique socket, lock, event, and session directory |
| Local Kache store | one node | persistent directory; Kache coordinates concurrent access |
| Remote cache | trust domain | shared only across ABI-compatible toolchains and trusted writers |
| Credentials | shortest possible stage | prefer instance/workload identity |

Do not put `KACHE_RUNTIME_DIR` on a shared node-global path. Do not depend on an
ephemeral workspace for reusable local cache state.

## Trust Policy

Kache 0.19.0 does not automatically classify Jenkins jobs. Implement the
policy in the pipeline:

| Jenkins context | Remote access |
| --- | --- |
| Pull/change request (`CHANGE_ID` set) | read-only |
| Unprotected branch | read-only |
| Tag, replay, ad hoc parameterized build | read-only unless explicitly approved |
| Protected `master` push | read-write |

Remote read access is still a trust decision: cached objects execute in later
build and test steps. Isolate caches between organizations or threat domains.

## Declarative Pipeline Skeleton

This template assumes a pinned `kache` binary already exists at
`/opt/kache/bin/kache` and remote configuration comes from a controlled
`.ci/kache.toml`.

```groovy
pipeline {
  agent { label 'linux-build' }

  options {
    timestamps()
    disableConcurrentBuilds(abortPrevious: false)
  }

  environment {
    KACHE_BIN = '/opt/kache/bin/kache'
    KACHE_CONFIG = "${WORKSPACE}/.ci/kache.toml"
    KACHE_CACHE_DIR = '/var/cache/kache'
    KACHE_PROGRESS = 'verbose'
  }

  stages {
    stage('Build') {
      steps {
        sh '''
          set -eu

          tmp_root="${WORKSPACE_TMP:-${WORKSPACE}/.tmp}"
          export KACHE_RUNTIME_DIR="${tmp_root}/kache-${EXECUTOR_NUMBER}-${BUILD_NUMBER}"
          mkdir -p "$KACHE_RUNTIME_DIR"

          if [ -n "${CHANGE_ID:-}" ] || [ "${BRANCH_NAME:-}" != "master" ]; then
            export KACHE_REMOTE_READONLY=1
          else
            export KACHE_REMOTE_READONLY=0
          fi

          finish() {
            "$KACHE_BIN" report --format json \
              --output "$WORKSPACE/kache-report.json" || true
          }
          trap finish EXIT

          "$KACHE_BIN" --version
          "$KACHE_BIN" doctor --json > "$WORKSPACE/kache-doctor.json"
          "$KACHE_BIN" sync --pull --allow-partial

          export RUSTC_WRAPPER="$KACHE_BIN"

          "$KACHE_BIN" install-shims --force "${tmp_root}/kache-shims"
          export PATH="${tmp_root}/kache-shims:$PATH"

          ./ci/build-and-test.sh

          if [ "$KACHE_REMOTE_READONLY" = 0 ]; then
            "$KACHE_BIN" sync --push
          fi
        '''
      }
    }
  }

  post {
    always {
      archiveArtifacts(
        artifacts: 'kache-report.json,kache-doctor.json',
        allowEmptyArchive: true
      )
    }
  }
}
```

Do not use `disableConcurrentBuilds` as a substitute for correct cache/runtime
isolation. Retain or remove it based on the repository's build-output locking
model.

## Rust-Only Pilot

Start with Rust if the job uses Cargo:

```bash
export RUSTC_WRAPPER="$KACHE_BIN"
cargo build --locked
cargo test --locked
```

Do not add compiler-name shims during this phase. This keeps the experiment
small and prevents build-script C/C++ from changing at the same time.

After the Rust path is qualified, opt into build-script native compilation:

```bash
"$KACHE_BIN" install-shims --force "$WORKSPACE_TMP/kache-shims"
export PATH="$WORKSPACE_TMP/kache-shims:$PATH"
```

## Blade Pilot

Before the first canary:

1. capture `blade ... --verbose`;
2. identify the compiler executable and every outer wrapper;
3. determine whether Sailfish, Goma, CAS, ccache, or sccache is active;
4. select one cache owner for the experiment;
5. inspect whether compile commands use `-H`, response files, PCH, modules,
   coverage, CUDA, or custom flags.

For internal Blade variants, never set `DISABLE_CAS`, `BLADE_SAILFISH_SKIP`, or
similar controls based only on their names. Read the repository wrapper and
prove their effect on the target runner.

Run a narrow target first:

```bash
blade build //path/to/package:representative_target --verbose
kache report --format json --output kache-report.json
jq '.summary, .bypass.reasons' kache-report.json
```

If the report has no cacheable misses, there is no useful Kache benchmark yet.
Fix interception or eligibility first.

## Remote Failure Policy

Choose explicitly:

| Policy | Behavior | Suitable for |
| --- | --- | --- |
| strict | sync failure fails job | cache integrity/availability qualification |
| fail-open with alert | build proceeds, cache error is recorded | ordinary CI where cache is an optimization |
| local-only fallback | set `KACHE_LOCAL_ONLY=1` | remote incident isolation |
| full bypass | set `KACHE_DISABLED=1` | immediate rollback |

Do not hide all failures with `|| true`. If using `--allow-partial`, parse the
report and emit an observable warning or metric.

## Benchmark Matrix

For each candidate job, collect at least:

| Run | Node/store condition | Purpose |
| --- | --- | --- |
| A | cache disabled, fresh output | baseline |
| B | empty local/remote namespace | cold overhead |
| C | warm local store, fresh output | persistent-runner gain |
| D | empty local store, warm remote | ephemeral-runner gain |
| E | warm cache plus realistic source edit | developer/PR behavior |

Run A-E on the same runner class and repeat enough times to compare medians and
p95. Record queue time separately from execution time.

## Promotion Gates

Before increasing rollout:

- no correctness mismatch;
- no cache error or store failure;
- expected compiler calls are cacheable;
- passthrough reasons are understood;
- median wall-time improvement exceeds run-to-run noise;
- p95 does not regress beyond the agreed budget;
- remote traffic and storage remain within budget;
- PR jobs cannot write;
- a protected-branch publish completes before the ephemeral runner exits;
- disabling the wrapper restores the baseline path.

Archive `kache-report.json`, tool versions, runner labels, commit SHA, build
command, and benchmark summary with each canary.
