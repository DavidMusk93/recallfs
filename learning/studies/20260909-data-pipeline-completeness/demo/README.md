# Completeness Semantics Demo

This C++20 demo tests the public semantics described in Datadog's data
completeness article. It does not reproduce Datadog's private implementation or
its scale.

## What It Models

| Component | Contract |
| --- | --- |
| Evidence state | Create and acknowledgment are idempotent and may arrive in either order |
| Evidence identity | Root bucket, payload ID, and segment ID remain stable |
| Sequential composition | Adjacent segments must conserve the same cohort |
| Branch composition | Delivery mass is separate from the least healthy required branch |
| Sampling | Create and acknowledgment reuse one propagated decision |
| Automation | Invalid, stale, undersampled, or topology-unknown signals fail closed |

## Build and Test

Portable build:

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/g++-15
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/completeness_demo
```

Sanitized build verified on the study machine:

```bash
SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
cmake -S . -B build \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_CXX_FLAGS="-isysroot $SDKROOT" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$SDKROOT/usr/lib" \
  -DENABLE_SANITIZERS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The explicit SDK library path is local-environment hygiene. This machine has a
`/usr/local/lib/libc++.1.dylib` whose install name is
`@rpath/libc++.1.dylib`; without the SDK path it shadows the macOS system stub
and produces an executable with no usable `LC_RPATH`.

## Test Matrix

The test executable covers:

1. the numeric assertion helper rejects `NaN`;
2. create then acknowledgment, including duplicates;
3. acknowledgment before create;
4. every create/ack sequence up to eight events;
5. root-bucket isolation;
6. the undefined empty-segment ratio;
7. valid sequential cohort composition;
8. rejection of adjacent cohort mismatch;
9. rejection of acknowledgments greater than creates;
10. delivery-mass versus required-branch semantics;
11. invalid branch counts;
12. consistent sampling across create and acknowledgment;
13. independent sampling as a false-loss counterexample;
14. completeness, health, freshness, sample-size, and topology automation gates.

## Observed Scenario Output

```text
conserved_chain.valid=true
conserved_chain.ratio=0.720000
mismatched_chain.valid=false
mismatched_chain.reason=adjacent segments do not conserve the same cohort
branches.delivery_mass=0.990000
branches.required_floor=0.000000
stable_sampling.creates=2087
stable_sampling.completeness=1.000000
independent_sampling.creates=1938
independent_sampling.completeness=0.128483
independent_sampling.early_acks=1861
```

The sampling scenario is deterministic. It is a counterexample, not a
statistical accuracy benchmark.
