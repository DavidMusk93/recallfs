---
doc_id: recallfs-evidence-bplus-tree-v1
kind: reference
status: draft
authority: evidence
applies_to:
  - learning/studies/20260911-bplus-tree/lib
depends_on:
  - recallfs-study-bplus-tree-v1
supersedes: []
verified_by:
  - BPT-RA-1
  - BPT-RA-2
  - BPT-RA-3
  - BPT-RA-4
  - BPT-RA-5
  - BPT-RA-6
---

# Evidence Ledger

This ledger starts as the verification contract. It must not claim an anchor
passed until the corresponding raw command output has been captured.

| Gate | Required evidence |
| --- | --- |
| Native correctness | CMake build plus complete CTest output |
| Memory safety | FIL-C execution of deterministic suites |
| Undefined behavior | ASan/UBSan build and complete CTest output |
| Persistence | repeated close/reopen against the independent model |
| Crash recovery | subprocess exits at every durable WAL boundary |
| Corruption | page, file-header, child-reference, sibling, and WAL rejection |
| Public consumption | external C11 translation unit links only through installed headers |

The implementation is not release-ready while this document remains
`status: draft`.
