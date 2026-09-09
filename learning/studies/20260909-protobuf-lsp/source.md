# Sources

## 1. Primary Source

| Field | Value |
| --- | --- |
| URL | `https://buf.build/blog/protobuf-lsp` |
| Title | Protobuf now has full LSP support |
| Author | Team Buf |
| Published | 2026-01-14 |
| Access Date | 2026-09-09 |
| Archive | `learning/sources/20260909-protobuf-lsp.md` |

The article was successfully opened and read in a browser. The archive
preserves the technical claims and setup instructions needed by this study.

## 2. Verification Sources

| Source | Revision or version | Use |
| --- | --- | --- |
| [Buf CLI](https://github.com/bufbuild/buf) | `v1.72.0`, commit `7d6f05675219fa077f776e9f05b7c7d1a9882e0c` | Capability declaration, incremental query path, release history |
| [Buf CLI changelog](https://github.com/bufbuild/buf/blob/v1.72.0/CHANGELOG.md) | v1.43.0 through v1.72.0 | Versioned LSP feature history |
| [Buf LSP command](https://buf.build/docs/reference/cli/buf/lsp/serve/) | Accessed 2026-09-09 | Supported invocation and flags |
| [VS Code Buf extension](https://github.com/bufbuild/vscode-buf) | commit `a8fa1136bf5ec763bcd4b8893330343becbf13c7` | Installation, binary selection, document selectors, exposed commands |
| [VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=bufbuild.vscode-buf) | Extension 0.8.x page accessed 2026-09-09 | User-visible features and automatic CLI installation |
| [LSP overview](https://microsoft.github.io/language-server-protocol/overviews/lsp/overview/) | Accessed 2026-09-09 | Protocol model, document synchronization, capability negotiation |

## 3. Source Inspection

The Buf source checkout was temporary and not committed. Relevant v1.72.0
implementation points were:

- `private/buf/buflsp/server.go`: initialization and server capabilities;
- `private/buf/buflsp/file.go`: buffer updates, query eviction, IR refresh,
  symbol indexing, checks, and diagnostics;
- `private/buf/buflsp/workspace.go`: workspace discovery, indexing, and
  lifecycle;
- `private/buf/buflsp/completion.go`: context-aware Protobuf completion,
  including field and enum numbers.

The VS Code extension source established:

- `.proto` and Buf configuration document selectors;
- minimum Buf v1.43.0 for the beta command and v1.59.0 for `buf lsp serve`;
- use of the configured or discovered Buf binary;
- automatic restart behavior and command-palette integration.

## 4. Executable Under Test

| Field | Value |
| --- | --- |
| Version | Buf v1.72.0 |
| Platform | macOS arm64 |
| Release asset | `buf-Darwin-arm64` |
| SHA-256 | `5176f23a6118b9978de1340c3e3301a4ed0d48e16a669510be44b4c355170d57` |
| Download URL | `https://github.com/bufbuild/buf/releases/download/v1.72.0/buf-Darwin-arm64` |

The downloaded binary and source checkouts remain under `.tmp/` and are
excluded from Git.

## 5. Evidence Limits

- The probe is a protocol smoke test, not an exhaustive LSP conformance suite.
- It uses two local `.proto` files, so it does not establish large-workspace
  latency or memory behavior.
- It verifies the v1.72.0 capability handshake and selected operations, not
  every advertised capability.
- The article's claim of being the first fully featured, spec-compliant
  Protobuf LSP remains attributed to Buf.
