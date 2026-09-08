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

Build the bridge before invoking the guest; it has no external crate
dependencies:

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

## Fresh Checkout Bootstrap

The versions below are the repository-tested macOS/ARM64 setup. Keep downloads,
the compiler, VM state, and generated binaries under `.tmp/fil-c/`.

| Component | Version | SHA-256 |
| --- | --- | --- |
| FIL-C Linux AArch64 | 0.684 | `564813b819a6e73879bdd993e2176b38ccbd5c5219e5adcbe1589e874c860666` |
| Lima Darwin ARM64 | 2.2.0 | `bbdef91774885a0d05f7b048c4eb89ae2bcf3a0c252ae7ca7934e63df76d93c3` |

Download and verify:

```bash
mkdir -p .tmp/fil-c/{bin,dist,downloads,lima,lima-home}

curl -fL -o .tmp/fil-c/downloads/filc-0.684-linux-aarch64.tar.xz \
  https://github.com/pizlonator/fil-c/releases/download/v0.684/filc-0.684-linux-aarch64.tar.xz
curl -fL -o .tmp/fil-c/downloads/lima-2.2.0-Darwin-arm64.tar.gz \
  https://github.com/lima-vm/lima/releases/download/v2.2.0/lima-2.2.0-Darwin-arm64.tar.gz

printf '%s  %s\n' \
  564813b819a6e73879bdd993e2176b38ccbd5c5219e5adcbe1589e874c860666 \
  .tmp/fil-c/downloads/filc-0.684-linux-aarch64.tar.xz \
  bbdef91774885a0d05f7b048c4eb89ae2bcf3a0c252ae7ca7934e63df76d93c3 \
  .tmp/fil-c/downloads/lima-2.2.0-Darwin-arm64.tar.gz |
  shasum -a 256 -c -
```

Extract the tools:

```bash
tar -xJf .tmp/fil-c/downloads/filc-0.684-linux-aarch64.tar.xz \
  -C .tmp/fil-c/dist --strip-components=1
tar -xzf .tmp/fil-c/downloads/lima-2.2.0-Darwin-arm64.tar.gz \
  -C .tmp/fil-c/lima
```

Create the Linux guest from Lima's pinned Ubuntu template:

```bash
LIMA_HOME="$PWD/.tmp/fil-c/lima-home" \
  .tmp/fil-c/lima/bin/limactl create --tty=false \
  --name recallfs-filc --vm-type vz --cpus 2 --memory 2 --disk 10 \
  --containerd none --mount-writable \
  .tmp/fil-c/lima/share/lima/templates/ubuntu-25.10.yaml

LIMA_HOME="$PWD/.tmp/fil-c/lima-home" \
  .tmp/fil-c/lima/bin/limactl start --tty=false recallfs-filc
```

If the repository path makes Lima's Unix socket exceed `UNIX_PATH_MAX`, replace
the empty `.tmp/fil-c/lima-home` directory with a symlink to a short private
directory under `/tmp`, then repeat `limactl create`.

Install the guest linker and patch the FIL-C distribution inside Linux:

```bash
.tmp/fil-c/bin/filrun sudo apt-get update
.tmp/fil-c/bin/filrun sudo apt-get install -y binutils patchelf
.tmp/fil-c/bin/filrun sh -c \
  "cd '$PWD/.tmp/fil-c/dist' && ./setup.sh"
```

The last block requires the bridge binaries built earlier in this document.
After setup, rerun `.tmp/fil-c/bin/filcc --version` and confirm that the output
contains `Fil-C`; a host Clang version is a failed setup.
