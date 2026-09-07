# Exploration Log

## 1. Goal

Determine what Verus does, identify its real assurance boundary, and build a
small executable demo that distinguishes verification from ordinary Rust tests.

## 2. Research Steps

| Step | Action | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Read the Amazon Science article dated 2026-08-31 | Extracted the project positioning, Amazon claims, ecosystem examples, and stated trust boundary | `learning/sources/20260831-verus-amazon-science.md` |
| 2 | Read the Verus repository README and installation guide | Confirmed active-development status, Rust-subset support, release installation, and platform coverage | `source.md` |
| 3 | Read the official Verus tutorial | Confirmed `requires`/`ensures`, modular verification, mode semantics, Z3 use, and ghost-code erasure | `source.md` |
| 4 | Read the OOPSLA paper and project index | Confirmed linear ghost permissions and systems-oriented examples | `source.md` |
| 5 | Inspected a sparse checkout of the current Verus source | Matched demo syntax to upstream examples and identified the current Z3 dependency | Ignored `.tmp/verus-src/` |
| 6 | Downloaded the current macOS arm64 release | Verified the release archive against GitHub's SHA-256 | `source.md` |
| 7 | Installed the required Rust toolchain and Z3 | Reproduced the released verifier locally | `demo/setup-macos-arm64.sh` |
| 8 | Implemented and ran the five-stage demo | Observed both expected failures and successful proof/compile/run | `evidence/run.log` |

## 3. Demo Design

The demo uses storage extent arithmetic because it is small enough to audit but
still carries a real systems invariant:

```text
start + count must not overflow
start <= end
end <= capacity
end must mean exactly start + count
```

The evidence sequence deliberately includes a successful proof of a bad
implementation under a weak specification. Without that stage, the demo would
overstate Verus by implying that "verified" is an absolute property.

```text
+--------------------+
| ordinary Rust test |
+---------+----------+
          |
          v
+--------------------+
| unbounded contract |----> rejected: possible overflow
+---------+----------+
          |
          v
+--------------------+
| weak specification |----> accepted: wrong code meets weak claim
+---------+----------+
          |
          v
+--------------------+
| strong spec + bug  |----> rejected: postcondition failure
+---------+----------+
          |
          v
+--------------------+
| strong spec + fix  |----> verified, compiled, executed
+--------------------+
```

## 4. Observed Toolchain Details

| Observation | Result |
| --- | --- |
| Latest release queried on 2026-09-07 | `0.2026.09.06.8dea4a2` |
| Native Apple Silicon asset | Available |
| Release archive size | About 406 MiB |
| Required Rust toolchain | `1.98.0-aarch64-apple-darwin` |
| Z3 bundled in release archive | No |
| Z3 version selected by current Verus source | `4.16.0` |
| Corrected demo verification + compilation | 229 ms in the captured run |

The release installer documentation describes the Verus archive and Rust
toolchain setup. In this run, the verifier also required a separate Z3 binary;
the pinned setup script obtains the exact version named by the current upstream
`source/tools/get-z3.sh`.

## 5. Key Findings

- Verus extends Rust's safety story; it does not replace Rust's compiler.
- The proof target is the user-authored specification, not an implicit notion
  of correct business behavior.
- Rust ownership reduces the aliasing burden before SMT reasoning begins.
- Linear ghost state makes pointer and concurrent-resource proofs a first-class
  systems use case.
- Proof/spec code is erased, so assurance has proof-maintenance cost but no
  direct production runtime cost.
- The useful adoption boundary is a stable high-value core, not an arbitrary
  full application.
- AI can produce proof scaffolding without entering the trusted base when a
  verifier checks it, but AI-authored specifications still require independent
  human review.

## 6. Open Questions

- How stable are nontrivial proof suites across weekly Verus releases?
- What percentage of a real storage module must remain external or modeled?
- How much proof code is needed for a representative WAL recovery state machine?
- Which CI controls best prevent new `assume`, `admit`, or external bodies?
- Can a first production pilot hold verification latency below the team's
  interactive threshold while using deterministic solver settings?
