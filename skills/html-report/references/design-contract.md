# Technical HTML Design Contract

This reference defines the visual and structural contract for standalone
technical reports. It is not a fixed theme. Keep the contract stable while
letting the subject determine emphasis, density, and section shapes.

## 1. Information Before Decoration

A report is successful when the reader can scan from conclusion to evidence
without decoding the layout.

Use visual weight in this order:

1. report subject;
2. conclusion or current state;
3. primary metrics;
4. evidence and mechanism;
5. boundaries and next actions;
6. provenance.

Do not give decorative content more weight than evidence.

## 2. Page Geometry

Recommended fallback dimensions:

```css
:root {
  color-scheme: light;
  font-family:
    -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC",
    "Hiragino Sans GB", "Microsoft YaHei", system-ui, sans-serif;
  font-optical-sizing: auto;
  letter-spacing: 0;
}

* {
  box-sizing: border-box;
}

html {
  max-width: 100%;
  overflow-x: hidden;
}

body {
  margin: 0;
  min-width: 0;
  background: var(--page);
  color: var(--ink);
  font-size: 16px;
  line-height: 1.58;
  letter-spacing: 0;
}

main {
  width: min(1120px, calc(100% - 32px));
  margin-inline: auto;
  padding: 44px 0 72px;
}

p,
li {
  max-width: 72ch;
}

@media (max-width: 700px) {
  main {
    width: min(100% - 24px, 1120px);
    padding-top: 28px;
  }
}
```

Use stable responsive constraints. Do not scale font size continuously with
viewport width. Select one desktop size and a smaller mobile size in a media
query.

Wide elements can use the full `main` width. Running prose should not.

## 3. Color System

Use a neutral page plus semantic accents. A suitable fallback:

```css
:root {
  --page: #f5f5f7;
  --surface: #ffffff;
  --ink: #1d1d1f;
  --muted: #606067;
  --line: #d2d2d7;

  --info: #075ca8;
  --info-soft: #eaf3fb;
  --success: #08783e;
  --success-soft: #e8f7ee;
  --warning: #8a4b08;
  --warning-soft: #fff5df;
  --danger: #a13232;
  --danger-soft: #fbeaea;
}
```

Rules:

- Use semantic accents only where they carry meaning.
- Keep body `<strong>` at `color: inherit`.
- Do not use color alone; pair it with status text or a familiar symbol.
- A callout uses a subtle full-surface tint. Do not attach a decorative stripe
  to one edge.
- Avoid a single-hue interface. The page should contain neutral surfaces and
  distinct semantic colors.
- Do not use gradients, decorative blobs, fake glass, or atmospheric texture.

Text contrast is local. A muted color that works on the page may fail inside a
tinted callout. Verify each text/background pair at rendered size.

## 4. Typography

Technical reports need a compact hierarchy:

```css
h1 {
  max-width: 900px;
  margin: 8px 0 12px;
  font-size: 40px;
  line-height: 1.12;
  letter-spacing: 0;
}

h2 {
  margin: 0 0 14px;
  font-size: 23px;
  line-height: 1.25;
  letter-spacing: 0;
}

h3 {
  margin: 0 0 8px;
  font-size: 18px;
  line-height: 1.3;
  letter-spacing: 0;
}

@media (max-width: 700px) {
  h1 { font-size: 32px; }
  h2 { font-size: 21px; }
}
```

Do not use hero-scale type inside metric cards, tables, or panels. Establish
hierarchy with weight, size, spacing, and contrast together.

Long identifiers use a monospace stack and may wrap in prose:

```css
code {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 0.92em;
}

p code,
dd code,
td code {
  overflow-wrap: anywhere;
  word-break: break-word;
}
```

## 5. Header Anatomy

A technical header should be unframed:

```html
<header>
  <p class="eyebrow">Domain / Report type</p>
  <h1>Literal subject</h1>
  <p class="lede">One paragraph that frames the result and scope.</p>
  <aside class="conclusion">The decision-ready conclusion.</aside>
  <div class="metrics" aria-label="Key metrics">...</div>
</header>
```

Do not wrap the header in a card. Use a bottom divider to move into detail.

Metrics are individual repeated items, so cards are appropriate:

```css
.metrics {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 12px;
  margin-top: 28px;
}

.metric {
  min-width: 0;
  padding: 16px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: var(--surface);
}

.metric strong {
  display: block;
  font-size: 28px;
  line-height: 1.1;
  font-variant-numeric: tabular-nums;
}

@media (max-width: 700px) {
  .metrics {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
```

Use metrics only for quantities that help the reader judge the conclusion.

## 6. Sections and Metadata

Sections are full-width bands inside the page container, not floating cards:

```css
section {
  min-width: 0;
  padding: 30px 0;
  border-bottom: 1px solid var(--line);
}
```

Use `<dl>` for runtime/source identity:

```html
<dl class="facts">
  <dt>Revision</dt>
  <dd><code>abc123</code></dd>
  <dt>Window</dt>
  <dd>2026-09-08 10:00-10:15 UTC</dd>
</dl>
```

```css
.facts {
  display: grid;
  grid-template-columns: 190px minmax(0, 1fr);
  gap: 8px 18px;
}

.facts dt { color: var(--muted); }
.facts dd { min-width: 0; margin: 0; overflow-wrap: anywhere; }

@media (max-width: 700px) {
  .facts { grid-template-columns: 1fr; gap: 2px; }
  .facts dd { margin-bottom: 10px; }
}
```

## 7. Tables

Every table has:

- a wrapper with internal horizontal scrolling;
- `data-column-kinds`;
- one `<col>` per column;
- `table-layout: fixed`;
- a visible body row or a separate empty state;
- no `rowspan` or `colspan` that obscures column semantics.

Baseline CSS:

```css
.table-wrap {
  max-width: 100%;
  overflow-x: auto;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: var(--surface);
}

table {
  width: 100%;
  min-width: 720px;
  border-collapse: collapse;
  table-layout: fixed;
}

th,
td {
  padding: 12px 14px;
  border-bottom: 1px solid var(--line);
  vertical-align: top;
  text-align: left;
}

th {
  background: #ececf0;
  color: #353539;
  font-size: 13px;
  font-weight: 700;
}

th.measure,
td.measure {
  text-align: right;
  white-space: nowrap;
  font-variant-numeric: tabular-nums;
}
```

Column rules:

| Kind | Content examples | Required style |
| --- | --- | --- |
| `text` | explanation, state, decision | `text-align: left` |
| `measure` | count, bytes, rate, duration | right, tabular, nowrap |
| `identifier` | revision, PID, IPv6, enum | `text-align: left` |

Never classify an identifier as a measure merely because it is numeric.
One column has one semantic kind across all visible rows. Do not mix narrative
text and measures in one column and compensate with per-cell alignment classes.
Split the table or restructure the data instead.

The DOM probe validates the declared column geometry; it cannot infer whether
an author chose the correct semantic kind. Browser acceptance therefore pairs
the probe with an explicit review of each header and representative cell
against the table above.

## 8. ASCII Graphs

Use:

```css
.ascii-graph {
  width: max-content;
  max-width: 100%;
  margin: 18px auto 0;
  overflow-x: auto;
  padding: 16px;
  border: 1px solid #c7c7cc;
  border-radius: 6px;
  background: #ffffff;
  color: #1d1d1f;
  font: 13px/1.45 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  white-space: pre;
}
```

The graph source uses printable ASCII characters and no tabs. Keep labels
inside box boundaries. Complex RecallFS architecture graphs must also pass the
repository ASCII graph verifier.

## 9. Code Blocks

Use this markup:

```html
<pre class="code-block" data-language="rust"><code><span class="code-line"><span class="tok-keyword">fn</span> main() {</span><!--
--><span class="code-line">    println!(<span class="tok-string">"ok"</span>);</span><!--
--><span class="code-line">}</span></code></pre>
```

Requirements:

- `data-language` matches the content;
- every logical line uses `.code-line`;
- line numbers come from CSS counters and are not selectable;
- no whitespace text node appears between adjacent `.code-line` spans;
- `.code-line` uses `white-space: pre`;
- `code` uses `min-width: max-content`;
- long lines scroll inside the block;
- unsupported languages use `data-language="text"` without fake token colors;
- non-text code distinguishes at least keyword, type, string, number, comment,
  and function tokens when those categories appear.

Baseline:

```css
.code-block {
  max-width: 100%;
  overflow-x: auto;
  overflow-y: hidden;
  counter-reset: line;
  border-radius: 8px;
}

.code-block code {
  display: block;
  min-width: max-content;
}

.code-line {
  display: block;
  white-space: pre;
  counter-increment: line;
}

.code-line::before {
  content: counter(line);
  display: inline-block;
  width: 3ch;
  margin-right: 1.25rem;
  color: #8e8e93;
  text-align: right;
  user-select: none;
}
```

Copying a code block must not include line numbers or the language label.

## 10. Callouts, Status, and Risks

Use full tints:

```css
.callout {
  padding: 16px 18px;
  border: 1px solid var(--line);
  border-radius: 6px;
}

.callout.success {
  background: var(--success-soft);
  color: #064d29;
}

.callout.warning {
  background: var(--warning-soft);
  color: #663806;
}
```

A status chip uses a complete border/fill shape. Chips in one row share radius,
padding, and border weight.

Risk wording must distinguish:

- observed failure;
- inferred mechanism;
- proposal;
- unverified hypothesis;
- explicit residual risk.

Visual weight must not collapse those evidence levels.

## 11. Accessibility

Required baseline:

```css
@media (prefers-reduced-motion: reduce) {
  *,
  *::before,
  *::after {
    scroll-behavior: auto !important;
    animation-duration: 0.01ms !important;
    animation-iteration-count: 1 !important;
    transition-duration: 0.01ms !important;
  }
}

@media (prefers-reduced-transparency: reduce) {
  .surface {
    background: var(--surface);
    backdrop-filter: none;
  }
}

@media (prefers-contrast: more) {
  :root { --line: #707075; }
  .metric,
  .table-wrap,
  .callout { border-width: 2px; }
}
```

Also require:

- logical heading order;
- visible keyboard focus for controls;
- descriptive link text;
- text or symbols in addition to color;
- `aria-label` only where native text is insufficient;
- no hidden text that duplicates visible metadata.

## 12. Interaction

Static reports are the default. When interaction is justified:

- use buttons for commands and toggles;
- use tabs only for alternate views of the same region;
- keep default content accessible without animation;
- preserve current focus and keyboard navigation;
- support reduced motion;
- ensure every visible table state passes the table probe.

Do not add a framework runtime. Keep inline JavaScript small, explicit, and
independent from report content.

## 13. Browser Acceptance

Source review cannot prove visual correctness. Browser validation must inspect:

- full-page composition;
- first viewport hierarchy;
- desktop and 390px layout;
- focused table/code/diagram regions;
- page-level and component-level overflow;
- table text anchors and numeric alignment;
- interaction states;
- console and network failures;
- referenced assets.

Use the bundled browser acceptance reference and probe as the executable
contract.
