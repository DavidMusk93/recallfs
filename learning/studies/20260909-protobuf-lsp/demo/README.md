# Protobuf LSP Probe

This demo drives `buf lsp serve` directly over standard input/output. It
verifies capability negotiation, cross-file definition lookup, diagnostics for
an invalid unsaved buffer, and diagnostic clearing after repair.

## Requirements

- Node.js 20 or newer;
- Buf CLI 1.59.0 or newer on `PATH`, or an explicit path argument.

The study result was produced with Node.js v24.3.0 and Buf v1.72.0.

## Run

With `buf` on `PATH`:

```bash
npm run probe
```

With an explicit binary:

```bash
node src/lsp_probe.mjs /absolute/path/to/buf
```

Run the filesystem gates separately:

```bash
cd workspace
buf build
buf lint
```

## What the Probe Proves

The committed `workspace/acme/v1/service.proto` is valid. The probe opens it,
asks for the definition of `Customer`, then sends this invalid replacement only
through `textDocument/didChange`:

```proto
repeated repeated Customer customer = 1;
```

The invalid text is not written to disk. A successful result contains:

- the server name and version;
- advertised capability names;
- a definition location in `types.proto`;
- an error range over the duplicate modifier;
- zero diagnostics after the valid in-memory text is restored.

The measured notification times are diagnostic smoke-test observations, not
benchmarks.

## Layout

```text
demo/
  package.json
  src/
    lsp_probe.mjs
  workspace/
    buf.yaml
    acme/v1/
      service.proto
      types.proto
```

No third-party Node.js package is required.
