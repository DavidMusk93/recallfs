# Exploration Log

## 1. Research Questions

1. What value does LSP add beyond syntax highlighting and command wrappers?
2. Which parts of Protobuf development move into the editor?
3. Which repository and CI gates must remain?
4. Which capabilities exist in the current implementation rather than only in
   the January 2026 announcement?

## 2. Source Path

The article was opened in a browser on 2026-09-09. Its text, links, install
instructions, compiler architecture claims, diagnostic example, and roadmap
were extracted.

The current Buf release was then checked rather than assuming the article's
publication-time state:

```text
Buf CLI:       v1.72.0
Buf commit:    7d6f05675219fa077f776e9f05b7c7d1a9882e0c
VS Code ext:   a8fa1136bf5ec763bcd4b8893330343becbf13c7
```

The Buf changelog shows that LSP first appeared experimentally in v1.43.0,
moved to `buf lsp serve` in v1.59.0, and gained multiple authoring,
navigation, code-action, CEL, and configuration features through v1.70.0.

## 3. Implementation Reading

The v1.72.0 `Initialize` handler advertises:

```text
textDocumentSync
codeActionProvider
completionProvider
definitionProvider
typeDefinitionProvider
documentFormattingProvider
documentHighlightProvider
hoverProvider
referencesProvider
renameProvider
semanticTokensProvider
workspaceSymbolProvider
documentSymbolProvider
foldingRangeProvider
documentLinkProvider
codeLensProvider
executeCommandProvider
```

The server requests `TextDocumentSyncKindFull`. On every `.proto`
`didChange`, it receives the current whole buffer and then:

```text
Update
  -> cancel stale checks
  -> refresh IR
     -> update in-memory opener
     -> evict changed File query keys and dependents
     -> run incremental IR queries
  -> rebuild symbol indexes
  -> run compiler and lint checks
  -> publish diagnostics
```

This establishes a more precise interpretation of "incremental compilation":
semantic computations are query-driven and invalidated by dependency, while
wire synchronization is whole-document and symbol indexing is currently
rebuilt for affected files.

## 4. Reproduction Design

The demo keeps a valid two-file workspace on disk:

```text
types.proto
  -> declares acme.v1.Customer

service.proto
  -> imports types.proto
  -> refers to Customer
```

The Node.js probe implements only LSP's `Content-Length` framing and JSON-RPC
request/notification dispatch. It has no package dependencies.

The sequence is:

```text
initialize
  -> inspect server capabilities
didOpen(valid files)
  -> expect zero diagnostics
definition(Customer)
  -> expect types.proto declaration range
didChange(unsaved duplicate modifier)
  -> expect precise error diagnostic
didChange(unsaved repair)
  -> expect diagnostics cleared
```

The invalid text is never written to disk. Successful `buf build` and
`buf lint` therefore demonstrate the difference between filesystem gates and
in-memory editor feedback.

## 5. Commands

The release binary was downloaded to the ignored `.tmp/` workspace and checked
against the published SHA-256:

```bash
curl -fL \
  -o .tmp/buf-lsp-study/buf \
  https://github.com/bufbuild/buf/releases/download/v1.72.0/buf-Darwin-arm64
chmod +x .tmp/buf-lsp-study/buf
printf '%s  %s\n' \
  5176f23a6118b9978de1340c3e3301a4ed0d48e16a669510be44b4c355170d57 \
  .tmp/buf-lsp-study/buf | shasum -a 256 -c -
```

The reproducible checks were:

```bash
BUF="$PWD/.tmp/buf-lsp-study/buf"
DEMO="$PWD/learning/studies/20260909-protobuf-lsp/demo"

(cd "$DEMO/workspace" && "$BUF" build)
(cd "$DEMO/workspace" && "$BUF" lint)
node "$DEMO/src/lsp_probe.mjs" "$BUF"
```

## 6. Observed Result

The final run reported:

- Buf server version `1.72.0`;
- all 17 capability groups listed above;
- zero diagnostics for the valid buffer;
- definition of `Customer` at `types.proto` line 4, characters 8 through 16;
- one error on the second `repeated` token in the unsaved invalid buffer;
- zero diagnostics after the unsaved repair.

Across four local runs, the invalid-edit and repair notifications arrived in
approximately `0.7` to `2.3 ms`. This is only a smoke-test observation on a
warm, two-file workspace and must not be used as a latency or scalability
claim.

## 7. Failed Attempt

The first probe attempted a graceful `shutdown` request and waited for its
response after collecting the evidence. The minimal client remained alive, so
the run was interrupted after its complete result had printed.

The final probe closes standard input and uses a short forced-termination
fallback after printing the result. This concerns test-harness teardown only;
the capability, definition, and diagnostic exchanges had already completed.
No product shutdown defect is inferred from this experiment.

## 8. Interpretation

The test supports the central workflow claim: the server understands unsaved
Protobuf source with workspace context and returns editor-consumable semantic
results. It does not support removing CLI or CI checks. Those gates operate on
a reproducible repository snapshot and cover compatibility and generation
concerns outside the interactive probe.
