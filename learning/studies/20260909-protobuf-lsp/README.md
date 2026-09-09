# Protobuf LSP: Value and Workflow Changes

> Source: [Protobuf now has full LSP support](https://buf.build/blog/protobuf-lsp)
>
> Read and verified on 2026-09-09 against Buf CLI v1.72.0.

## 1. Conclusion

The Buf LSP is valuable because it moves Protobuf correctness and code
intelligence into the editor's live, unsaved state while reusing the same
compiler and workspace model across editors. It is not merely better syntax
highlighting.

For Protobuf development, the practical shift is:

```text
Before
edit -> save -> run build/lint -> parse terminal error -> navigate -> edit

With LSP
edit -> in-memory parse/type/lint -> inline diagnostic/navigation/action
     -> save -> deterministic CLI gates -> generate -> review/CI
```

The second line is the desired workflow. LSP shortens the inner loop; it does
not replace `buf build`, `buf lint`, `buf breaking`, or reproducible code
generation.

Adopting the LSP does not change the `.proto` language, wire format, generated
APIs, runtime libraries, or deployment model. The minimum adoption is only
`buf lsp serve` plus an editor client; an existing `protoc` generation pipeline
can remain. The stronger, recommended adoption aligns editor, local hooks, and
CI on the same pinned Buf CLI so they parse and lint schemas consistently.

## 2. What LSP Is Worth

### 2.1 One semantic engine, many editors

Without a common protocol, every editor-specific Protobuf extension must
reimplement parsing, import resolution, symbol lookup, diagnostics, and
refactoring. With LSP, Buf implements those semantics once and editor clients
translate standard JSON-RPC messages into native UI.

This changes ecosystem cost from an editor-by-language matrix toward one
client per editor plus one server per language. More importantly, fixes in the
Buf semantic engine become available to VS Code, Neovim, and other LSP clients
without rewriting the Protobuf analyzer for each editor.

### 2.2 Feedback on the buffer, not only the filesystem

An LSP client sends `textDocument/didOpen` and `textDocument/didChange`.
The server analyzes the editor's current buffer even when it has not been
saved. The local probe kept a valid `service.proto` on disk, sent an invalid
version only over LSP, and received:

```text
multiple modifiers on message field type
line 11, characters 11..19
source: buf-lsp
```

The disk-based `buf build` and `buf lint` remained successful. This is the
essential workflow difference: a CLI gate observes a committed or saved
snapshot; LSP observes the developer's current thought.

### 2.3 Project semantics, not text matching

Protobuf intelligence requires more than tokenization:

- imports must resolve through Buf module and workspace rules;
- a symbol may be declared in another file or dependency;
- field and enum numbers must respect used and reserved ranges;
- options have declaration and value types;
- lint policy depends on `buf.yaml`;
- Protovalidate embeds CEL with its own syntax and semantics.

The v1.72.0 probe resolved `Customer` in `service.proto` to the exact declaration
range in `types.proto`. That is compiler-backed navigation, not a filename or
regular-expression lookup.

### 2.4 A persistent, incrementally invalidated project model

Buf's implementation uses the experimental `protocompile` AST, IR, and
incremental query executor. On a changed file it evicts cached file queries and
their dependents, rebuilds the relevant IR, indexes symbols, runs checks, and
publishes diagnostics.

There is an important nuance: v1.72.0 advertises full-document synchronization,
so the editor sends the whole changed `.proto` buffer rather than a character
patch. "Incremental" here primarily describes semantic query invalidation and
reuse inside the server, not incremental wire transfer or zero re-indexing.

## 3. Why Protobuf Benefits Disproportionately

Protobuf files are small, but their blast radius is large. A schema declaration
drives generated APIs in multiple languages, serialized compatibility, service
contracts, validation, and downstream consumers. A typo, wrong import, reused
field number, or unsafe rename can fail far from the edit that caused it.

LSP improves three high-cost activities:

| Activity | Without semantic editor support | With Buf LSP |
| --- | --- | --- |
| Authoring | Recall syntax, option names, imports, and numbers manually | Contextual completion, number suggestions, diagnostics, code actions |
| Understanding | Search text and follow import paths manually | Hover, definition/type-definition, references, document/workspace symbols |
| Repairing | Read terminal output, reopen file, locate the token | Precise range diagnostics, rename, organize imports, lint-ignore/deprecate actions |

The largest gain is causal locality: the editor points to the schema token
while the author still has the relevant design in working memory. This reduces
context switches and prevents avoidable defects from reaching review and CI.

## 4. Current Buf Capability Surface

The v1.72.0 `initialize` response advertised 17 capability groups:

| Area | Advertised support |
| --- | --- |
| Feedback | diagnostics through document sync, hover |
| Navigation | definition, type definition, references, document highlight |
| Authoring | completion, rename, formatting |
| Structure | document symbols, workspace symbols, folding ranges |
| Rendering | semantic tokens, document links |
| Actions | code actions, code lenses, execute command |

Release history adds useful detail:

| Version | Notable LSP change |
| --- | --- |
| v1.43.0 | Experimental LSP introduced as `buf beta lsp` |
| v1.59.0 | Stable `buf lsp serve`; references, basic completion, workspace symbols |
| v1.62.0 | Rename and prepare-rename |
| v1.63.0 | Field-number completion |
| v1.64.0 | Fully qualified completion, organize imports, links, folding, highlighting |
| v1.65.0 | Deprecate code action |
| v1.66.0 | Lint-ignore action and Protovalidate CEL hover |
| v1.68.0 | Edition 2024 compiler path and Buf config links/code lenses |
| v1.69.0 | Generate/update code lenses and config-path warnings |
| v1.70.0 | Completion for `buf.yaml`, `buf.gen.yaml`, and `buf.policy.yaml` |

This also qualifies the January article's roadmap. Import organization,
field/enum number suggestion, substantial configuration support, and CEL
support are now present. Automatic module import and complete custom-option
behavior should still be evaluated against the team's schemas rather than
assumed from the roadmap.

## 5. Recommended Development Workflow

### 5.1 Workstation setup

1. Pin one Buf CLI version for editors, local commands, and CI.
2. VS Code users install `bufbuild.vscode-buf` and set
   `buf.commandLine.path` when the repository provides a pinned binary.
3. Neovim and other LSP clients launch `buf lsp serve` with the repository root
   anchored by `buf.yaml` or `.git`.
4. Keep `buf.yaml`, `buf.gen.yaml`, and dependency locks in source control.

Pinning matters because the VS Code extension can download the latest CLI when
`buf` is absent. An editor silently using a newer analyzer than CI creates
different diagnostics and code-action behavior.

### 5.2 Inner loop

Use LSP continuously for:

- diagnostics on each in-memory edit;
- completion and documentation hover;
- definition, references, and symbol search;
- semantic highlighting;
- formatting and narrow code actions;
- safe workspace-aware rename, followed by review of the diff.

Do not run generation after every keystroke. Generate once the schema is
locally valid and stable enough to inspect downstream API changes.

### 5.3 Pre-commit and CI gates

Keep deterministic, headless checks:

```bash
buf format --diff --exit-code
buf lint
buf build
buf breaking --against '.git#branch=master'
buf generate
```

Run only the commands applicable to the repository, but do not remove CI gates
because the editor looks clean. CI still protects non-LSP editors, version
skew, generated-code drift, compatibility policy, and files not opened by the
developer.

## 6. What Changes for Developers

| Dimension | Change |
| --- | --- |
| Feedback latency | Errors move from save/command/CI time to edit time |
| Error quality | Diagnostics identify source ranges and can include actionable fixes |
| Navigation | Cross-file and dependency exploration becomes semantic |
| Refactoring | References and rename reduce manual search-and-replace risk |
| Cognitive load | Completion supplies syntax, symbols, options, and available numbers |
| Editor choice | The same server can serve multiple LSP-capable editors |
| Onboarding | New contributors need less repository-specific Protobuf knowledge up front |
| Review | Reviewers spend less time on mechanical schema errors and more on API design |

The strongest benefit is not raw typing speed. It is moving defect discovery
closer to defect creation while keeping the compiler's view of the project
available for navigation and repair.

## 7. Boundaries

- LSP is a developer-experience layer, not a compatibility or governance gate.
- The server and client negotiate capabilities; an advertised feature can
  still have edge cases.
- Buf v1.72.0 release notes contain LSP fixes, and unreleased notes still list
  definition/reference fixes for dependency and well-known-type files.
- Large-workspace memory efficiency is an article claim; this study did not
  benchmark memory or scaling.
- The article's "fully-featured" and "spec-compliant" labels were not subjected
  to a complete conformance suite here.
- The low-single-digit-millisecond local diagnostic observations are smoke-test
  results on a two-file workspace, not a production latency benchmark.

## 8. Reproduction Result

The demo uses a valid two-file Buf workspace and a dependency-free Node.js
JSON-RPC client.

Verified with Buf v1.72.0:

- `buf build`: pass;
- `buf lint`: pass;
- initial LSP diagnostics: zero;
- cross-file definition lookup: exact `Customer` declaration;
- unsaved duplicate modifier: one precise error diagnostic;
- unsaved repair: diagnostics returned to zero;
- advertised capability groups: 17.

See [demo/README.md](demo/README.md), [evidence/run.log](evidence/run.log), and
[exploration.md](exploration.md) for commands, raw output, and limitations.
