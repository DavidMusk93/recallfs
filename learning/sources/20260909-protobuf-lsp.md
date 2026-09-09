# Protobuf LSP Article Archive

| Field | Value |
| --- | --- |
| URL | `https://buf.build/blog/protobuf-lsp` |
| Title | Protobuf now has full LSP support |
| Author | Team Buf |
| Published | 2026-01-14 |
| Access Date | 2026-09-09 |
| Topic | Protobuf, Language Server Protocol, developer workflow |

## 1. Archived Summary

Buf announces what it describes as the first fully featured,
specification-compliant LSP server for Protobuf. The server is distributed in
the Buf CLI and can be used by any editor with an LSP client. The official
VS Code extension manages this automatically; other editors start
`buf lsp serve`.

The article identifies these developer-facing language services:

- go to definition;
- code completion;
- find references;
- semantic syntax highlighting;
- diagnostics.

The architectural basis is Buf's `protocompile` compiler frontend. For the LSP,
Buf introduced a query-driven frontend intended to support incremental
compilation and more precise diagnostics. It also introduced a new AST and
intermediate representation instead of using `FileDescriptorProto` as the
primary source representation.

The article's diagnostic example reports a duplicated `repeated` modifier at
the second token and points back to the first token:

```text
error: encountered more than one type modifier
  --> testdata/parser/type/repeated.proto:23:14
   |
23 |     repeated repeated M x4 = 4;
   |     -------- ^^^^^^^^ help: consider removing this
   |     |
   |     first one is here
```

## 2. Installation Notes

For VS Code, install the official `bufbuild.vscode-buf` extension. It uses an
installed Buf CLI or downloads one into extension storage when none is found.

The article gives this Neovim configuration:

```lua
vim.lsp.config('buf-lsp', {
    cmd = { 'buf', 'lsp', 'serve' },
    filetypes = { 'proto' },
    root_markers = { 'buf.yaml', '.git' },
})
```

Other LSP-capable editors can launch:

```bash
buf lsp serve
```

The server uses standard input and output unless `--pipe` selects a Unix
socket.

## 3. Article Roadmap

At publication, Buf listed these planned improvements:

- add imports as an automatic fix;
- integrate more tightly with `buf.yaml`, including automatic module imports;
- complete and find references for custom options;
- suggest field and enum numbers;
- add dedicated Protovalidate support, including CEL syntax highlighting.

Later Buf releases delivered several of these items, including organize-imports
actions, field and enum number completion, configuration-file support, and
Protovalidate CEL hover and semantic-token support. The study records the
versioned evidence rather than treating every roadmap item as complete.

## 4. Source Claims Versus Verified Facts

The phrases "first fully-featured" and "spec-compliant" are vendor claims from
the article. This archive does not attempt a complete LSP conformance test.

The accompanying study independently verifies the v1.72.0 capability
handshake, cross-file definition lookup, diagnostics for an unsaved invalid
buffer, and diagnostic clearing after an unsaved repair.

## 5. Archive Boundary

The source page was fully readable in a browser. This archive preserves its
metadata, technical mechanism, setup instructions, diagnostic example, and
roadmap without copying the complete article.
