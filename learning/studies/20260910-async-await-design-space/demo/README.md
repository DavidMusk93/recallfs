# Mini Async Demo

This C11 program makes the control machinery behind Rust `async` explicit.
It is a deterministic, single-threaded teaching executor, not a production
runtime.

## File Map

| Path | Role |
| --- | --- |
| `include/mini_async.h` | Minimal `Future`, `Context`, `Waker`, task, and executor contracts |
| `src/mini_async.c` | Fixed-capacity ready queue and poll/drop lifecycle |
| `include/async_examples.h` | Observable scenario interface |
| `src/async_examples.c` | Five explicit future state machines |
| `src/main.c` | Prints the observed traces |
| `tests/mini_async_test.c` | Exact C reconciliation anchors |
| `tests/rust_reference.rs` | Independent Rust oracle for three language-level anchors |

## Concept Map

| C demo | Rust |
| --- | --- |
| `ma_future.state` | hidden state-machine fields generated for `async fn` |
| `ma_future_vtable.poll` | `Future::poll` |
| `MA_POLL_PENDING` / `MA_POLL_READY` | `Poll::Pending` / `Poll::Ready` |
| `ma_context.waker` | `Context::waker()` |
| `ma_task_wake` | executor-specific `Waker::wake` behavior |
| `ma_executor.queue` | ready queue |
| `ma_future_drop` | dropping the suspended future and its fields |

The executor uses caller-owned storage and a fixed queue of 16 task pointers.
This keeps allocation and lifetime visible. Rust would enforce the relevant
ownership, pinning, and task bounds; C relies on the caller to preserve them.

## Build And Run

From this study directory:

```bash
cmake -S demo -B ../../../.tmp/async-await-design-space/build -G Ninja
cmake --build ../../../.tmp/async-await-design-space/build
ctest --test-dir ../../../.tmp/async-await-design-space/build \
  --output-on-failure
../../../.tmp/async-await-design-space/build/async_design_demo
```

Expected scenario output:

```text
lazy          trace=A  polls=1 inert=yes complete=yes cancelled=no handle-dropped=no cleanup=no coalesced=no
dynamic-await trace=AB polls=1 inert=yes complete=yes cancelled=no handle-dropped=no cleanup=no coalesced=no
wake          trace=AB polls=2 inert=yes complete=yes cancelled=no handle-dropped=no cleanup=no coalesced=yes
handle-drop   trace=AB polls=2 inert=yes complete=yes cancelled=no handle-dropped=yes cleanup=no coalesced=yes
cancel        trace=AD polls=1 inert=yes complete=yes cancelled=yes handle-dropped=no cleanup=yes coalesced=no
```

The `handle-drop` scenario proves only that releasing the non-owning join
handle does not request cancellation. The executor, task control block, and
future state all remain in the scenario's stack frame. Tokio's stronger
runtime-owned lifetime is an upstream API contract, not a property proved by
this C program.

## Rust Oracle

The Rust reference uses only `std`. It checks that future construction is
lazy, awaiting an immediately ready future does not suspend, and dropping a
pending future runs synchronous field destructors without resuming its body.

```bash
rustc --edition 2024 -D warnings demo/tests/rust_reference.rs \
  -o ../../../.tmp/async-await-design-space/rust_reference
../../../.tmp/async-await-design-space/rust_reference
```

Expected:

```text
rust lazy trace=A
rust dynamic-await trace=AB
rust cancel trace=AD
```

## What Is Deliberately Missing

- no OS reactor or real I/O;
- no threads or atomic synchronization;
- no allocation, stealing, priorities, or fairness policy;
- no Rust-equivalent enforcement of `Pin`, aliasing, lifetimes, or
  `Send + 'static`;
- no exception, panic, or `Result` propagation model;
- no async destructor.

The demo is useful because each omission is visible. Extending it into a real
runtime would require specifying and testing those contracts rather than
assuming they follow from the `poll` loop.
