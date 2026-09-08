# Evidence Ledger

| Evidence | Claim supported | Boundary |
| --- | --- | --- |
| `upstream-fix.diff` | SQLite added post-lock salt validation and a forced WAL-restart regression test | Diff is the 2026-03-03 upstream fix, not every later 3.51.3 change |
| `source-digests.txt` | Identity of fetched primary pages and official amalgamation ZIPs | Raw web pages remain under `.tmp/` and are not committed |
| `fil-c-serialized-smoke.txt` | Final C source and SQLite 3.51.2 executed under FIL-C without a reported memory-safety/UB failure | Three small serialized attempts; not a race or performance proof |
| `local-environment.txt` | macOS hardware, compilers, source and binary digests | Native correctness stress only |
| `local-unsafe-3512.txt` | Vulnerable SQLite lost acknowledged writes in 3/3 runs | Timing-dependent; one observed outcome is structural corruption |
| `local-unsafe-3513.txt` | Adjacent fixed release completed 400 unsafe attempts with no observed loss | Finite negative observation |
| `local-serialized-3512.txt` | Vulnerable release completed 400 gated attempts with no observed loss | Process-local gate only |
| `local-rollback-3512.txt` | Vulnerable release completed 400 rollback-journal attempts with no observed loss | Does not measure WAL behavior |
| `target-environment.txt` | EPYC/Linux target identity, compiler, filesystem and artifact digests | No performance claim |
| `target-unsafe-3512.txt` | Vulnerable SQLite lost acknowledged writes in 3/3 target runs | Timing-dependent correctness stress |
| `target-unsafe-3513.txt` | Fixed release completed 400 target attempts and 68,200 writes with no observed loss | Finite negative observation |
| `target-serialized-3512.txt` | Vulnerable release completed 400 target gated attempts and 6,106 writes without observed loss | Requires all writers/checkpointers to honor one gate |
| `target-rollback-3512.txt` | Vulnerable release completed 400 target rollback attempts and 800 writes without observed loss | Reader/writer concurrency tradeoff not measured |

The strongest local oracle is in `local-unsafe-3512.txt`, run 3:

```text
committed=106
recovered=104
lost=2
integrity=ok
```

It directly disproves the proposition that a successful
`PRAGMA integrity_check` proves absence of WAL-reset write loss.
