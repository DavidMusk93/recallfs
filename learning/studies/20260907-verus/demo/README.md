# Verus Value Demo

This demo uses a storage-style `extent_end` function to separate five claims:

1. Ordinary Rust tests cover the examples selected by the author.
2. Verus rejects an arithmetic operation whose safety is not proven for every input.
3. Verus can accept a bad implementation when its specification is too weak.
4. A strong specification exposes an off-by-one implementation.
5. The corrected executable Rust code verifies, compiles, and runs.

## Quick Start: macOS arm64

The setup script downloads pinned Verus and Z3 releases into the repository's
ignored `.tmp/` directory, verifies their SHA-256 digests, and installs the
Rust toolchain required by that Verus release.

```bash
learning/studies/20260907-verus/demo/setup-macos-arm64.sh
learning/studies/20260907-verus/demo/run.sh
```

Pinned tools:

| Tool | Version | SHA-256 source |
| --- | --- | --- |
| Verus | `0.2026.09.06.8dea4a2` | GitHub release asset metadata |
| Rust | `1.98.0-aarch64-apple-darwin` | Required by the Verus release |
| Z3 | `4.16.0` | GitHub release asset metadata |

The Verus archive is about 406 MiB. `setup-macos-arm64.sh` changes only
ignored `.tmp/` files, except for installing the pinned Rust toolchain through
the existing `rustup` installation.

## Other Platforms or Existing Installations

Follow the official Verus installation guide, install Z3 4.16.0, then run:

```bash
VERUS_BIN=/absolute/path/to/verus \
VERUS_Z3_PATH=/absolute/path/to/z3 \
learning/studies/20260907-verus/demo/run.sh
```

## Expected Result

The expected failures are part of the demo:

```text
[1/5] Ordinary Rust tests exercise only selected inputs
test result: ok. 1 passed

[2/5] Verus rejects arithmetic without a proven bound
error: possible arithmetic underflow/overflow

[3/5] Verus accepts a wrong implementation against a weak specification
verification results:: 2 verified, 0 errors

[4/5] Verus rejects the off-by-one implementation against a strong specification
error: postcondition not satisfied

[5/5] Verus proves, compiles, and runs the corrected implementation
verification results:: 2 verified, 0 errors
verified executable exited successfully
```

Generated binaries and logs stay under `demo/build/`, which is gitignored.
