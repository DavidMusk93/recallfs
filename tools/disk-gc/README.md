# disk-gc

Reclaim disk space from **stale build caches**, **stale git worktrees** and
**stale tmp entries**. Std-only Rust (no crates), so it builds on a host whose
disk is already full.

```text
 candidates                    assess (parallel)            decide            apply
 --root   -> worktrees,  ---> size + newest max(mtime, ---> fresh?  keep ---> rescue git state
             build dirs        ctime), mounts, nested       guard?  keep      rm -rf
 --cache / --cache-children   worktrees, repo hosts        in use? keep      git worktree prune
 --tmp    -> children                                       git ok? else keep log + df before/after
```

## Rules

| Kind | How it is found | Deleted when |
| --- | --- | --- |
| `worktree` | `.git` file -> `<common>/worktrees/<name>` whose `gitdir` points back | unused >= `--worktree-days` (30) and `git` can read it |
| `orphan-worktree` | admin dir missing, moved, or owned by another path | same age **and** `--orphans` |
| `build-cache` | `CACHEDIR.TAG`, out-of-source `CMakeCache.txt`, blade `build64_*` within 2 parents of `BLADE_ROOT` (covers `pgo/build64_*`), `.ccache`, kache dir with `index.db` / `blobs`, cargo `target` next to `Cargo.toml`, `--cache`, `--cache-children`. Also searched **inside** a linked worktree, including one that is itself kept | unused >= `--cache-days` (7) and >= `--min-size` |
| `stale-tmp` | children of `--tmp` (skips dot entries, `systemd-private-*`) | unused >= `--tmp-days` (30) and >= `--min-size` |

"Unused" = newest `max(mtime, ctime)` over the whole tree (plus worktree
`HEAD`/`index`/`logs/HEAD`). atime is ignored: indexers bump it. Git runs with
`GIT_OPTIONAL_LOCKS=0`, so scanning does not refresh the index.

Always kept (fail closed): protected paths (`--protect`, state dir, cwd, own
exe), paths with < 2 components, trees with scan errors, trees containing a
mount point, trees hosting a repo whose linked worktrees live outside, paths
used by a live process (`/proc/*/{cwd,exe,root,fd}`), worktrees `git` cannot read.

## Rescue before removing a worktree

Under `<state-dir>/run-<UTC>/rescue/<path>/`:

- `meta.tsv`: path, HEAD, branch
- `status.txt`: `git status --porcelain`
- `tracked.patch`: `git diff --binary HEAD` (only when tracked files changed)
- detached HEAD -> `refs/disk-gc/<UTC>/<name>` in the main repo keeps commits reachable

Untracked files are listed, not copied. Branches stay in the main repo.

## Build and test

```bash
cd tools/disk-gc
cargo build --release          # -> ../../.tmp/disk-gc/target/release/disk-gc
DISK_GC_TEST_DIR=$PWD/../../.tmp/disk-gc/test cargo test
```

## Usage on d2 (n37-125-152)

Always `plan` first, read `plan.tsv`, then `apply` with the same flags.

Host:

```bash
BIN=/root/Documents/recallfs/.tmp/disk-gc/target/release/disk-gc
$BIN plan \
  --root /root --root /data00/home/sunmingqiang/Documents --root /workspace \
  --cache-children /root/.cache --cache /root/deps/build \
  --tmp /tmp --tmp /var/tmp \
  --state-dir /root/Documents/recallfs/.tmp/disk-gc/state
```

The `tide-dev-agent-dev-container-*` container writes into its overlay upper
dir on `/`; its worktrees use container paths, so run the binary inside it
(same glibc 2.28) from a bind-mounted directory:

```bash
docker exec <container> /workspace/tide-dev-agent/.tmp/disk-gc plan \
  --root /workspace --root /root --tmp /tmp \
  --state-dir /workspace/tide-dev-agent/.tmp/disk-gc-state
```
