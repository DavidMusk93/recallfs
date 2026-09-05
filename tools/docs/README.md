# Documentation Tools

## `verify_markdown_c.rs`

Extracts every fenced `c` block from a Markdown file into one translation unit.
It emits `#line` directives so compiler diagnostics point back to the document.
An optional C harness can turn illustrative snippets into executable tests.

Build:

```bash
rustc --edition 2021 -D warnings tools/docs/verify_markdown_c.rs \
  -O -o .tmp/verify-markdown-c
```

Compile and run the SSD report examples with FIL-C:

```bash
.tmp/verify-markdown-c \
  --compiler .tmp/fil-c/bin/filcc \
  --runner .tmp/fil-c/bin/filrun \
  --harness tools/docs/tests/how_to_write_to_ssds_harness.c \
  docs/reports/how-to-write-to-ssds-database-engineering-notes.md
```

The generated translation unit and executable are written below
`.tmp/verify-markdown-c/` by default.
