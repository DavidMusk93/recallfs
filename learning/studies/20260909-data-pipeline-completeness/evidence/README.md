# Evidence Ledger

| Evidence | What it establishes | Boundary |
| --- | --- | --- |
| `red-test.txt` | Tests existed before behavior and failed on missing model symbols | Proves test-first order, not semantic correctness |
| `test-output.txt` | All 14 tests passed under ASan and UBSan | Covers the implemented model on one local platform |
| `scenario-output.txt` | Concrete valid and invalid composition and sampling results | Deterministic demonstration, not production statistics |
| `local-environment.txt` | Compiler, OS, CPU, commands, and toolchain issue | Enables local reproduction |
| `source-digests.txt` | SHA-256 identities for source archive and demo inputs | Does not authenticate upstream web content |

## Evidence Strength

The strongest results are constructive:

- exhaustive short create/ack sequences converge solely from observed evidence;
- a conserved sequential chain produces a 72 percent end-to-end ratio;
- a non-conserved chain is rejected instead of producing a false scalar;
- 99 percent delivery mass can coexist with a zero-percent required branch;
- changing the sampling seed between create and acknowledgment manufactures an
  approximately 13 percent result for a pipeline that actually delivered every
  payload.

Finite tests do not prove Datadog's unpublished implementation. They establish
the contracts that an implementation of the public model must satisfy.
