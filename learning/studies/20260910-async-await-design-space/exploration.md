---
doc_id: recallfs-study-async-await-design-space-exploration-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-async-await-design-space
depends_on:
  - learning/studies/20260910-async-await-design-space/source.md
  - learning/studies/20260910-async-await-design-space/demo/README.md
supersedes: []
verified_by:
  - learning/studies/20260910-async-await-design-space/evidence/README.md
---

# Exploration Log

## 1. Source Resolution

The title and authors in the supplied image matched the Cognitive Engineering
Lab publication page. That page identifies:

```text
Title:   A Design Space Exploration of Async/Await
Authors: Gavin Gray, Shriram Krishnamurthi, Will Crichton
Venue:   OOPSLA 2026
arXiv:   2608.20677
```

The PDF was downloaded from arXiv rather than from the social-media image or a
mirror. Its SHA-256 and metadata are recorded in [`source.md`](source.md).

The paper's Zenodo record identifies artifact `v1.3`, which contains Redex
models, concrete examples and a differential fuzzer for seven runtimes. The
artifact source archive is about 7 MB, while each runnable container image is
about 2.2 GB. A slow source-archive transfer was stopped; no partial archive is
treated as evidence.

## 2. Claim Extraction

The paper's claims were separated into taxonomy, runtime classification and
validation:

| Class | Claim |
| --- | --- |
| Taxonomy | Straight-line asynchrony differs along nine observable dimensions |
| Cross-language | No two of seven tested runtime configurations agree on all three introductory programs |
| Rust language | Calling an async function creates a lazy future |
| Rust execution | `.await` has dynamic suspension |
| Rust runtime | Tokio and Smol differ in reference/lifetime behavior |
| Cancellation | Rust cancellation is unaware and uses top-down drop |
| Formalization | Language variants are modeled over a common async calculus |
| Validation | Generated real outputs are checked against model-allowed outputs |

The paper does not benchmark these alternatives or establish that any one
combination is universally preferable.

## 3. Demo Boundary

Three implementation choices were considered:

1. reproduce the full Redex artifact;
2. write a C simulator for all nine dimensions and seven runtimes;
3. write a small C executor exposing the Rust slice.

The third was selected. The first duplicates the official artifact and has a
large environment cost. The second risks creating an unvalidated alternative
semantics. The Rust slice answers the user's question while keeping every
state transition inspectable.

The C model uses:

```text
Future = state pointer + poll/drop vtable
Task   = Future + lifecycle flags + executor ownership
Waker  = function pointer + task pointer
Queue  = fixed-capacity ring of ready tasks
```

It intentionally has no threads and no actual I/O. An event is signalled
directly by the test, which invokes the stored waker.

## 4. Proof-First Sequence

Five tests were written before the implementation:

| Anchor | Required observation |
| --- | --- |
| Lazy | no body output at construction; `A` after first poll |
| Dynamic await | ready child and parent finish as `AB` in one poll |
| Wake | first poll is pending; two wakes produce one queued poll |
| Handle drop | releasing a join handle does not request cancellation |
| Cancel | abort stops polling and synchronous drop produces `AD` |

The first configure failed because `src/mini_async.c` and
`src/async_examples.c` did not yet exist. This was the expected pre-
implementation failure. After implementation, CTest passed all five anchors.

A later simplification review added a sixth lifecycle check: when spawn fails
because the ready queue is full, the rejected future is dropped exactly once.
A code review added a seventh: a task that wakes itself and returns `Ready`
must not falsely exhaust the exact poll budget. The five paper-facing
semantics anchors remain unchanged.

The detach claim was also narrowed after review. The C scenario keeps the task
control block and future state in caller-owned storage, so it demonstrates
handle-drop behavior but not Tokio's stronger runtime-owned cross-scope
lifetime.

## 5. Optimized-Test Failure

The initial test used C `assert`. Apple Clang Debug and FIL-C executed the
checks, but Zig `-O2` treated them as disabled and reported each observation as
an unused variable:

```text
error: unused variable 'observation' [-Werror,-Wunused-variable]
```

This exposed a test-design error rather than a runtime defect. Replacing
`assert` with an explicit `CHECK` macro made result validation independent of
`NDEBUG`. The optimized Zig, FIL-C, native CTest and sanitizer runs then all
passed.

## 6. Rust Cross-Check

A dependency-free Rust program was added as an independent oracle for the
language-level observations:

```text
rust lazy trace=A
rust dynamic-await trace=AB
rust cancel trace=AD
```

This confirms:

- an async function body does not run at construction;
- awaiting `ready(())` completes in the same outer poll;
- dropping a pending future runs a field destructor without resuming the
  async body.

It does not cover Tokio's detached-task semantics. That behavior is taken from
Tokio's `spawn` and `JoinHandle` contracts, not inferred from the C model.

## 7. Cancellation Interpretation

The first draft phrase “Rust cancellation calls Drop” was too imprecise.
Cancellation is better described as:

1. stop scheduling or polling the incomplete future;
2. eventually destroy its suspended state;
3. run synchronous destructors for initialized fields.

This distinction matters because dropping a Tokio `JoinHandle` does not drop
the task future; it detaches the task. `JoinHandle::abort`, runtime shutdown,
losing an in-task `select!` branch and dropping an unspawned future are
different ownership paths even though all eventually rely on dropping an
incomplete future to discard its continuation.

## 8. Remaining Limits

- The C executor has no concurrent wake race.
- `ma_future.state` stability is a caller obligation, not a checked `Pin`
  contract.
- The demo has no failure propagation channel.
- The demo shows one Tokio-like strong runtime ownership policy, not Smol.
- The official Redex model and differential fuzzer were inspected through the
  paper, metadata and source repository, but not executed locally.
