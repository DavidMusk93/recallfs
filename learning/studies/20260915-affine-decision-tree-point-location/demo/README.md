# C11 2D Point Location Demo

This directory contains a generic 2D affine decision-tree evaluator and one
hand-derived Voronoi example.

## Layout

```text
demo/
  verify.sh
  source-manifest.sha256
  CMakeLists.txt
  include/
    odt.h
    point_location_example.h
  src/
    odt.c
    point_location_example.c
    main.c
  tests/
    odt_test.c
  python/
    .python-version
    pyproject.toml
    point_location_oracle.py
    tests/test_cli.py
    tests/test_differential.py
```

## Data Contract

Each node evaluates:

```text
weights[0] * x + weights[1] * y >= threshold
```

`above` is selected when true and `below` otherwise. Positive references are
one-based node indices. Negative references are one-based leaf indices with
the sign inverted. Reference zero is invalid.

The evaluator contract is this direct IEEE 754 binary64 computation and
comparison, not exact real arithmetic. A rounded finite score equal to the
threshold selects `above`; a non-finite score returns `ODT_NUMERIC_RANGE`
without changing the result.

The tree borrows immutable arrays from its caller. Validate once before
publishing the tree; `odt_locate_2d` performs no allocation.

## Complete Verification

From the repository root, run:

```bash
learning/studies/20260915-affine-decision-tree-point-location/demo/verify.sh
```

This validates the tracked local-source manifest, clears its dedicated
`.tmp/affine-decision-tree/verify` directory, and then runs FIL-C, fixed Zig
0.16.0 release binaries, Zig ASan/UBSan, Python 3.13.12 tests through uv,
CMake/CTest, formatting, and the document dependency check. The Python tests
receive the Zig CLI built during the same invocation.

## Native Build

```bash
cmake -S learning/studies/20260915-affine-decision-tree-point-location/demo \
  -B .tmp/affine-decision-tree/native -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build .tmp/affine-decision-tree/native
ctest --test-dir .tmp/affine-decision-tree/native --output-on-failure
.tmp/affine-decision-tree/native/point_location
```

Machine mode accepts whitespace-separated `x y` pairs and emits
`region comparisons`:

```bash
printf '2 2\n5 5\n-1 4\n' |
  .tmp/affine-decision-tree/native/point_location --machine
```

CLI regression tests cover malformed machine input and unsupported arguments.
Injected output and flush failures remain an untested boundary: there is no
deterministic failure mechanism shared by macOS stdio and FIL-C. Machine mode
still checks `printf` and `fflush` so detected failures return nonzero.

## Python Oracle

The Python code uses no third-party runtime dependency. pyenv pins the
interpreter and uv creates the isolated environment and lockfile.

```bash
cd learning/studies/20260915-affine-decision-tree-point-location/demo/python
pyenv install -s 3.13.12
PYENV_VERSION=3.13.12 uv sync --python "$(pyenv prefix 3.13.12)/bin/python3"
uv run python point_location_oracle.py --count 8 --seed 20260915
POINT_LOCATION_BIN="$(git rev-parse --show-toplevel)/.tmp/affine-decision-tree/native/point_location" \
  uv run python -m unittest discover -s tests -v
```

The oracle checks the rectangle and then computes all three squared Euclidean
distances exactly from the input binary64 values. It does not parse or execute
the C tree. Cases whose nearest two exact squared distances differ by at most
eight ULPs of the larger distance are marked uncertain: outside that band the
C region must equal the exact nearest site, while inside it the C result must
still be a defined site region. Deterministic `math.nextafter` grids cover all
pairwise bisector midpoints, the three-site Voronoi vertex, and the known
boundary-sensitive point `(5.414124727934967, 0.01525163239020344)`.

## Scope

This is a bounded learning artifact. It has no mutable builder, persistence,
thread-safety protocol, vectorized execution, or performance claim.
