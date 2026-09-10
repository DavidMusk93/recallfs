# Evidence

Validation date: 2026-09-10.

## Result Matrix

| Gate | Toolchain | Result | Raw evidence |
| --- | --- | --- | --- |
| Paper identity | `pdfinfo`, SHA-256 | 28 pages; digest matches source index | [`paper.txt`](raw/paper.txt) |
| Native build/test | Apple Clang 21.0.0, CMake 4.0.3 | five semantics anchors and overflow cleanup pass | [`native.txt`](raw/native.txt) |
| C safety/UB | FIL-C 0.684 | six checks pass | [`filc.txt`](raw/filc.txt) |
| LLVM optimized C | Zig 0.16.0, Clang 21.1.0, `-O2` | six checks pass | [`zig.txt`](raw/zig.txt) |
| Sanitizers | Apple Clang ASan + UBSan | six checks pass | [`sanitizers.txt`](raw/sanitizers.txt) |
| Rust oracle | rustc 1.97.0, LLVM 22.1.6 | exact traces `A`, `AB`, `AD` | [`rust-reference.txt`](raw/rust-reference.txt) |

FIL-C validates the executed C paths for memory safety and undefined behavior.
It is not a proof of the model and is not used as performance evidence.

## Reconciliation Results

| Anchor | C | Rust oracle | Status |
| --- | --- | --- | --- |
| `AA-LAZY-1` | trace `A`, one poll, constructor inert | trace `A`, constructor inert | pass |
| `AA-SUSPEND-1` | trace `AB`, one outer poll | trace `AB`, one outer poll | pass |
| `AA-WAKE-1` | trace `AB`, two polls, two wakes coalesced | not modeled | pass in C |
| `AA-DETACH-1` | trace `AB`, detached task completes | documented Tokio contract | pass in C |
| `AA-CANCEL-1` | trace `AD`, one poll, drop cleanup | trace `AD` | pass |
| Spawn overflow | rejected future is dropped exactly once | not applicable | pass in C |

The Rust oracle deliberately covers only behavior available from `std`.
Tokio detached-task behavior is grounded in the upstream `spawn` and
`JoinHandle` documentation recorded in [`../source.md`](../source.md).

## Commands

Native:

```bash
cmake -S learning/studies/20260910-async-await-design-space/demo \
  -B .tmp/async-await-design-space/evidence-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build .tmp/async-await-design-space/evidence-native
ctest --test-dir .tmp/async-await-design-space/evidence-native \
  --output-on-failure
```

FIL-C:

```bash
src=learning/studies/20260910-async-await-design-space/demo
.tmp/fil-c/bin/filcc -std=c11 -O2 -g -Wall -Wextra -Werror \
  -I "$src/include" \
  "$src/src/mini_async.c" "$src/src/async_examples.c" \
  "$src/tests/mini_async_test.c" \
  -o .tmp/async-await-design-space/evidence-filc-test
.tmp/fil-c/bin/filrun \
  .tmp/async-await-design-space/evidence-filc-test
```

Zig LLVM frontend:

```bash
src=learning/studies/20260910-async-await-design-space/demo
zig cc -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror \
  -I "$src/include" \
  "$src/src/mini_async.c" "$src/src/async_examples.c" \
  "$src/tests/mini_async_test.c" \
  -o .tmp/async-await-design-space/evidence-zig-test
.tmp/async-await-design-space/evidence-zig-test
```

Sanitizers:

```bash
src=learning/studies/20260910-async-await-design-space/demo
cc -std=c11 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
  -I "$src/include" \
  "$src/src/mini_async.c" "$src/src/async_examples.c" \
  "$src/tests/mini_async_test.c" \
  -o .tmp/async-await-design-space/evidence-sanitized-test
.tmp/async-await-design-space/evidence-sanitized-test
```

Rust oracle:

```bash
rustc --edition 2024 -D warnings \
  learning/studies/20260910-async-await-design-space/demo/tests/rust_reference.rs \
  -o .tmp/async-await-design-space/evidence-rust-reference
.tmp/async-await-design-space/evidence-rust-reference
```

## Negative Evidence

The proof-first CMake configure failed before implementation because the
declared source files were absent. After implementation, the first Zig `-O2`
build failed because `assert`-only checks were compiled out under `NDEBUG`.
The tests were changed to explicit result checks and all toolchains then
passed.

## Evidence Boundary

These checks establish that the local C model:

- executes the five documented traces;
- does not rely on debug-only assertions for validation;
- has no FIL-C, ASan or UBSan finding on the exercised paths;
- agrees with a Rust `std` oracle on three core language behaviors.

They do not establish:

- equivalence to the paper's full Redex semantics;
- correctness under concurrent or cross-thread wakeups;
- production executor fairness, scalability or I/O behavior;
- the behavior of Rust/Tokio versions other than the documented contracts.
