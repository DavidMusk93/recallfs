---
doc_id: recallfs-runbook-oblique-decision-tree-library-v1
kind: runbook
status: active
authority: implementation
applies_to:
  - learning/studies/20260915-affine-decision-tree-point-location/lib
depends_on:
  - recallfs-study-affine-decision-tree-point-location-v1
supersedes: []
verified_by:
  - learning/studies/20260915-affine-decision-tree-point-location/verify.sh
  - learning/studies/20260915-affine-decision-tree-point-location/evidence/manifest.json
---

# Oblique Decision Tree C Library

`odt` compiles caller-provided two-dimensional sites into an immutable
Oblique Decision Tree for Voronoi point location. The library owns
construction, validation, compact runtime storage, querying, encoding, and
loading. The application owns generation publication and reclamation.

## Build And Install

```bash
cmake -S learning/studies/20260915-affine-decision-tree-point-location/lib \
  -B .tmp/odt-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DODT_BUILD_TESTS=ON
cmake --build .tmp/odt-build
ctest --test-dir .tmp/odt-build --output-on-failure
cmake --install .tmp/odt-build --prefix .tmp/odt-install
```

Consume the installed package with CMake:

```cmake
find_package(odt 1.0 CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE odt::odt)
```

The installed public surface is `include/odt.h`, `lib/libodt.a`, and CMake
package metadata. Both C11 and C++17 consumers are qualification-gated.

## Lifecycle

```text
caller sites + domain + limits
              |
              v
       odt_build or odt_load
              |
              v
     unpublished generation
              |
        application publish
              |
              v
       concurrent readers
              |
      application quiescence
              |
              v
    odt_generation_destroy
```

The generation is immutable after a successful build or load. `odt_query`,
`odt_query_batch`, and metadata accessors allocate nothing and may run
concurrently against the same live generation.

The library deliberately does not expose a publication primitive. An
application may atomically exchange generation pointers, but it must use an
epoch, hazard-pointer, reference-count, or grace-period scheme before
destroying the retired generation. Concurrent destroy and query-after-destroy
are invalid.

## Minimal C Consumer

```c
#include <odt.h>

odt_query_result locate(const odt_site *sites, size_t site_count, odt_point point) {
    const odt_domain domain = {0.0, 0.0, 12.0, 8.0};
    odt_generation *generation = NULL;
    odt_query_result result = {ODT_RESULT_ERROR, ODT_INTERNAL_ERROR, 0};
    odt_status status;

    status = odt_build(&domain, sites, site_count, NULL, NULL, NULL, NULL,
                       &generation);
    if (status != ODT_OK) {
        result.status = status;
        return result;
    }

    status = odt_query(generation, &point, &result, NULL);
    odt_generation_destroy(generation);
    if (status != ODT_OK) {
        result.kind = ODT_RESULT_ERROR;
        result.status = status;
        result.region_id = 0;
    }
    return result;
}
```

Inspect `result.kind` before reading `region_id`: `ODT_RESULT_REGION` carries
the region, `ODT_RESULT_OUTSIDE` is a successful outside classification, and
`ODT_RESULT_ERROR` carries the failure in `result.status`.

Use `odt_limits_init` before overriding resource ceilings. Use
`odt_allocator_init` before replacing both allocator callbacks. Versioned
configuration structs must retain the initializer-provided `struct_size` and
`struct_version`.

## Input And Result Contract

- The domain must be a finite positive closed rectangle.
- At least one finite, distinct site must lie inside the domain.
- Every `region_id` must be unique.
- Rectangle boundary points are inside; points beyond it return
  `ODT_RESULT_OUTSIDE`.
- Exact nearest-site ties are resolved by input ordinal, not by region ID.
- A non-finite scalar point returns `ODT_INVALID_DATA` without changing the
  caller's output.
- An accepted batch writes every result slot exactly once. A non-finite point
  becomes a per-point `ODT_RESULT_ERROR`; it does not abort other points.
- Invalid batch envelopes fail before any output mutation.

Construction is deterministic for identical binary64 inputs and limits.
Builder work, fragment count, depth, node count, peak tracked temporary bytes,
and duration are returned in `odt_build_stats`.

## Persistence And Recovery

`odt_encode` and `odt_load` use caller-owned callbacks. The version-1 wire
format is little-endian, sectioned, CRC32C-protected, and independent of C
structure layout. A load validates limits, section ranges, graph invariants,
and numeric metadata before returning an unpublished generation.

`odt_save_file_atomic` supports local POSIX filesystems:

```text
encode -> temp file -> fsync(file) -> rename -> fsync(parent)
```

A failure before rename preserves the previous destination. A parent-directory
sync failure after rename returns `ODT_COMMIT_UNKNOWN`: the new file may be
visible without known crash durability. Recovery is to reopen with
`odt_load_file`, validate the complete generation, and only then decide whether
to retry publication.

Network filesystems are outside the version-1 atomic-file contract.

## Compatibility

The public API and ABI are versioned independently from the persistence
format. Compatibility is preserved within one major API release. Minor
releases may append fields to initialized versioned structs and append enum
values, but do not reorder or reinterpret existing fields.

## Qualification

Run the maintained fail-closed workflow from the repository root:

```bash
learning/studies/20260915-affine-decision-tree-point-location/verify.sh
```

The workflow runs FIL-C before native builds, then Debug/Release tests,
sanitizers, static analysis, exact differential checks, fault and corruption
tests, concurrency, installed consumers, native benchmark checks, negative
probes, and evidence-manifest verification.

The benchmark evidence is environment-specific. It is not a DuckDB benchmark
and does not claim to reproduce the article's reported speedup.
