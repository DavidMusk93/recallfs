# Sources and Evidence

## 1. Primary Sources

| Source | Access Date | Used For |
| --- | --- | --- |
| `https://www.amazon.science/blog/developing-provably-correct-rust-code-with-verus` | 2026-09-07 | Article claims, Amazon usage, ecosystem examples, trust-boundary warning |
| `https://github.com/verus-lang/verus` | 2026-09-07 | Project status, scope, Rust-subset limitation, documentation map |
| `https://verus-lang.github.io/verus/guide/overview.html` | 2026-09-07 | Verification model, Rust ownership integration, Z3, stated non-goals |
| `https://verus-lang.github.io/verus/guide/getting_started_cmd_line.html` | 2026-09-07 | Command-line verification and compilation workflow |
| `https://verus-lang.github.io/verus/guide/requires_ensures.html` | 2026-09-07 | Preconditions, postconditions, modular verification, ghost-code erasure |
| `https://verus-lang.github.io/verus/guide/modes.html` | 2026-09-07 | `spec`, `proof`, and `exec` modes |
| `https://arxiv.org/html/2303.05491v2` | 2026-09-07 | Linear ghost types, pointer/concurrency reasoning, mode-system rationale |
| `https://verus-lang.github.io/verus/publications-and-projects/` | 2026-09-07 | Published systems and project examples |
| `https://github.com/verus-lang/verus/releases/tag/release/0.2026.09.06.8dea4a2` | 2026-09-07 | Verified release version and platform asset |

## 2. Reproducibility Pins

| Component | Pinned Value | Evidence |
| --- | --- | --- |
| Verus release | `0.2026.09.06.8dea4a2` | `version.json` and `verus --version` |
| Verus commit | `8dea4a2196ebf99449fe2f141a2fb30acae3f17c` | Release `version.json` |
| Verus macOS arm64 archive SHA-256 | `23be2e8113b50bc91729377d8ac032f658cc5187e389a319d8458f9c37b07b4a` | GitHub release asset metadata and local `shasum` |
| Required Rust toolchain | `1.98.0-aarch64-apple-darwin` | Verus launcher and `version.json` |
| Z3 | `4.16.0` | Verus repository `source/tools/get-z3.sh` |
| Z3 macOS arm64 archive SHA-256 | `41828fa07d5cb77bfaee326e8e6dac074f26329c09c633f9e66012bb917cf8ae` | GitHub release asset metadata and local `shasum` |

## 3. Locally Inspected Upstream Material

A shallow sparse checkout of `verus-lang/verus` was kept under ignored
`.tmp/verus-src/` while preparing this report. Relevant files included:

| Upstream File | Relevance |
| --- | --- |
| `README.md` | Active-development status and supported scope |
| `INSTALL.md` | Binary-release installation and platform support |
| `examples/guide/requires_ensures.rs` | Canonical contract and compilation example |
| `examples/guide/overflow.rs` | Canonical bounded-integer overflow example |
| `examples/guide/modes.rs` | Executable/specification/proof mode examples |
| `source/tools/get-z3.sh` | Z3 4.16.0 dependency and platform artifact name |

## 4. Local Verification Evidence

| Evidence | Path |
| --- | --- |
| Full five-stage run | `learning/studies/20260907-verus/evidence/run.log` |
| Reproduction script | `learning/studies/20260907-verus/demo/run.sh` |
| Pinned macOS arm64 setup | `learning/studies/20260907-verus/demo/setup-macos-arm64.sh` |

The final corrected program produced:

```text
verification results:: 2 verified, 0 errors
total-time: 229 ms
verified executable exited successfully
```

The timing is one local observation, not a general benchmark.
