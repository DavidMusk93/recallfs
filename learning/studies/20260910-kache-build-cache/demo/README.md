# Kache Compatibility Demo

This fixture verifies behavior, not production performance.

## Requirements

- `kache` 0.19.0
- Rust 1.85 or newer for the fixture
- Clang or GCC
- `jq`
- optional: public `blade-build` checkout for the Blade anchors

## Run

Direct Rust and C++ checks:

```bash
KACHE_BIN=/absolute/path/to/kache ./verify.sh
```

Include the public Blade compatibility checks:

```bash
KACHE_BIN=/absolute/path/to/kache \
BLADE_BIN=/absolute/path/to/blade \
./verify.sh
```

Generated files stay under the repository's ignored
`.tmp/kache-build-cache-demo/` directory.

## Anchors

| ID | Assertion |
| --- | --- |
| `KC-RUST-01` | First Rust workspace build misses |
| `KC-RUST-02` | Identical second Rust workspace build locally hits |
| `KC-CC-01` | First direct C++ object compile misses |
| `KC-CC-02` | Repeated direct C++ object compile locally hits |
| `KC-DIRECT-03` | Direct report has no errors |
| `KC-BLADE-01` | Blade resolves the compiler through Kache PATH shims |
| `KC-BLADE-02` | Blade's default `-H` compile passes through |
| `KC-BLADE-03` | Default Blade run has no cacheable object compile |
| `KC-BLADE-04` | Allowlisted first Blade business TU misses |
| `KC-BLADE-05` | Allowlisted second workspace business TU hits |
| `KC-BLADE-06` | Both allowlisted Blade builds succeed |
| `KC-BLADE-07` | Cached and uncached include-stack files are identical |

The script writes full machine-readable reports to its temporary work
directory. The study's `evidence/` directory contains compact reports from the
recorded run.
