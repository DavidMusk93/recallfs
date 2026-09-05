# C Tooling

Repository C code is compiled and tested with [FIL-C](https://fil-c.org/).
FIL-C currently supports Linux only, so macOS uses a repository-local
Linux/ARM64 Lima VM.

The local runtime is intentionally kept out of Git:

```text
.tmp/fil-c/
|-- bin/
|   |-- filcc
|   `-- filrun
|-- dist/
|-- downloads/
|-- lima/
`-- lima-home/
```

- `filcc`: starts the local VM when needed and invokes FIL-C's `clang`.
- `filrun`: starts the same VM and runs a FIL-C-produced executable.
- `dist`: the verified FIL-C binary distribution.
- `lima` and `lima-home`: the host VM runtime and instance state.

Build the bridge after the runtime has been installed:

```bash
mkdir -p .tmp/fil-c/bin
rustc --edition 2021 -D warnings tools/c/filc_bridge.rs \
  -O -o .tmp/fil-c/bin/filcc
cp .tmp/fil-c/bin/filcc .tmp/fil-c/bin/filrun
```

Verify the compiler:

```bash
.tmp/fil-c/bin/filcc --version
```

The bridge deliberately fails if FIL-C or Lima is absent. It never falls back
to the host compiler.
