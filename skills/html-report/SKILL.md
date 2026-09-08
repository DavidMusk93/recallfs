---
name: "html-report"
description: "Builds self-contained, evidence-first technical HTML reports and validates them in a real browser. Invoke for HTML reports, visual analyses, dashboards, or polished explainers."
---

# HTML Report

Build a technical report that is clear before it is decorative. The report must
help a reader answer three questions quickly:

1. What happened or what was learned?
2. What evidence supports the conclusion?
3. What action, risk, or boundary remains?

This skill was distilled from Flowkit's `apple-design` defaults, HTML rendering
contract, browser acceptance workflow, executable table probe, and production
report examples. It adapts those rules to RecallFS instead of copying Flowkit's
directory and service assumptions.

## 1. Trigger

Invoke this skill when the user asks for:

- an HTML report, visual report, technical explainer, dashboard, or analysis;
- a polished browser-readable version of engineering evidence;
- a self-contained artifact that must be shared or reviewed visually;
- an existing technical HTML page to be redesigned or validated.

Do not invoke it for a normal Markdown answer, a product application, or a
landing page. Use the application's own design system for product UI.

## 2. Required Companion

Load `apple-design` when it is available. Apply its restraint, typography,
material, feedback, and accessibility principles. For technical reports, this
skill and `references/design-contract.md` override conflicting `apple-design`
guidance, including viewport-scaled type, nonzero letter spacing, decorative
translucency, gradient masks, and motion.

Keep this skill's technical-report rules authoritative for:

- evidence density;
- semantic HTML;
- table column contracts;
- code and ASCII graph rendering;
- browser geometry acceptance.

Do not add motion merely because `apple-design` discusses motion. Static
technical reports normally need no animation.

An artifact-specific workflow may impose stricter content or metadata rules.
Those explicit deltas override this generic report contract; unrelated generic
HTML guidance is not duplicated into the specialized workflow.

## 3. Output Location

Honor a user-specified path first. Otherwise select by artifact ownership:

| Artifact | Default location |
| --- | --- |
| Repository-wide report | `docs/reports/html/<topic>.html` |
| Teaching explainer | `docs/explainers/<topic>.html` |
| Learning study | `learning/studies/<study>/report.html` |
| Project-owned report | `projects/<project>/docs/<topic>.html` |
| Temporary prototype or browser evidence | `.tmp/reports/` |

Commit intentional reports and skill examples. Do not commit screenshots,
browser probe output, temporary servers, or generated scratch files.

## 4. Workflow

```text
+-----------------------------+
| define reader and decision  |
+-------------+---------------+
              |
              v
+-----------------------------+
| bind claims to evidence     |
| source, window, revision    |
+-------------+---------------+
              |
              v
+-----------------------------+
| choose semantic page shape  |
| summary -> detail -> action |
+-------------+---------------+
              |
              v
+-----------------------------+
| compose one HTML5 file      |
| inline CSS and small JS     |
+-------------+---------------+
              |
              v
+-----------------------------+
| inspect source invariants   |
+-------------+---------------+
              |
              v
+-----------------------------+
| serve the actual URL        |
| desktop + 390px browser     |
+-------------+---------------+
              |
              v
+-----------------------------+
| run DOM and table probes    |
+-------------+---------------+
              |
        no    |    yes
       +------+------+
       |             |
       v             v
+-------------+  +------------------+
| fix repeat  |  | retain evidence  |
+-------------+  +------------------+
```

### Step 1: Define the reading contract

Before writing HTML, state internally:

- primary reader;
- decision or understanding the page must enable;
- one-sentence conclusion;
- evidence set and missing evidence;
- required wide content such as tables, code, or diagrams;
- whether any interaction is genuinely needed.

If the conclusion is not yet defensible, do not hide uncertainty behind visual
polish.

### Step 2: Design the information architecture

Use this default order, adapting it to the subject:

1. literal subject title;
2. concise lede;
3. conclusion or status callout;
4. 3-5 meaningful metrics when available;
5. runtime/source identity;
6. mechanism or causal flow;
7. evidence tables and code;
8. risks, limits, next actions;
9. visible source/composition footer.

The first viewport must establish the subject and conclusion and leave a hint of
the next section. Do not build a marketing hero or put the title inside a card.

### Step 3: Compose semantic, self-contained HTML

Produce one complete HTML5 file:

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <link rel="icon" href="data:,">
  <title>Literal report subject</title>
  <style>/* all report CSS */</style>
</head>
<body>
  <main>
    <header>...</header>
    <section id="evidence">...</section>
    <footer>...</footer>
  </main>
</body>
</html>
```

Use `<header>`, `<main>`, `<section>`, `<article>`, `<dl>`, `<table>`,
`<details>`, and `<footer>` according to their meaning. Avoid `<div>` soup.

Keep all semantic content as real text. Do not hide facts in CSS pseudo-elements,
SVG geometry, `data-*` mirrors, or JSON blobs.

### Step 4: Apply the visual system

Read [`references/design-contract.md`](references/design-contract.md) before
composing CSS.

The default report language is quiet and work-focused:

- system font stack;
- `letter-spacing: 0`;
- 14-16px body type with readable leading;
- fixed heading sizes selected with media queries, not viewport-scaled type;
- centered 820-1120px content container;
- prose held near 65-80 characters;
- light neutral page plus white surfaces;
- restrained blue, green, amber, and red semantic accents;
- borders and full-surface tints instead of decorative gradients;
- cards only for metrics, repeated records, and real callouts;
- radius at or below 8px unless the repository defines otherwise.

Never use gradient orbs, bokeh, decorative cards, oversized empty space, or a
single-hue palette.

### Step 5: Render mechanisms and evidence

Use strictly aligned ASCII graphs when the graph is the technical source of
truth:

```html
<pre class="ascii-graph">+-----------+
| source    |
+-----+-----+
      |
      v
+-----------+
| result    |
+-----------+</pre>
```

ASCII graph content uses printable ASCII only. Keep the graph white with dark
text in every color scheme. Center the block while preserving left-aligned
content.

Use inline SVG only when spatial geometry or quantitative comparison is more
legible than text. Prose remains complete without the SVG.

Render code with a true language label, static token classes when practical,
line-number spans, preserved logical lines, and internal horizontal scrolling.
Read the code-block contract in the design reference.

### Step 6: Build tables from column semantics

Every visible table declares a stable column model:

```html
<div class="table-wrap">
  <table data-column-kinds="text,measure,identifier">
    <colgroup>
      <col style="width:48%">
      <col style="width:20%">
      <col style="width:32%">
    </colgroup>
    ...
  </table>
</div>
```

Allowed kinds:

| Kind | Meaning | Alignment |
| --- | --- | --- |
| `text` | Narrative, state, explanation | Left |
| `measure` | Comparable quantity, ratio, duration, bytes | Right |
| `identifier` | PID, revision, address, log ID, enum | Left |

`measure` columns require right-aligned headers and cells, tabular numerals, and
no wrapping. Tables use `table-layout: fixed`. Wide tables scroll inside their
own wrapper and never widen the page.

### Step 7: Add interaction only when it reduces work

Use native HTML first:

- `<details>` for secondary content in repeated records;
- anchor links for navigation;
- a small static table of contents for long reports.

Small inline JavaScript is allowed for reliable active-section navigation or a
content filter. Do not include a framework runtime. Every interactive state that
changes visible content enters the browser test matrix.

### Step 8: Run the source audit

Before opening a browser, verify:

- one complete HTML file;
- no unresolved placeholders;
- no external layout/style/runtime dependency;
- visible metadata and provenance;
- stable, unique ASCII IDs;
- meaningful headings in order;
- all images and SVGs have accessible text alternatives;
- all tables have `data-column-kinds`, `colgroup`, and fixed layout;
- reduced-motion, reduced-transparency, and increased-contrast fallbacks;
- no semantic fact exists only in color.

### Step 9: Run browser acceptance

Read [`references/browser-acceptance.md`](references/browser-acceptance.md) and
serve the artifact over HTTP. Validate the actual URL, not only the source file.

At minimum:

- inspect a desktop viewport near 1440px;
- inspect a 390px mobile viewport;
- inspect every content-changing state;
- capture full-page and focused screenshots;
- assert no page-level horizontal overflow;
- run the bundled table probe on every visible table state;
- inspect console errors and failed asset requests;
- verify long words, code, diagrams, and tables stay within their own scroll
  containers.

When changing this skill's probe or golden example, run
`tests/probe-regression.mjs`. It verifies the example at both required
viewports and proves malformed tables fail closed.

If the host browser cannot set viewport width, use an available Playwright or
Chromium harness for the missing viewport while retaining the same URL and DOM
checks. Record the tool boundary; do not call a desktop-only check "mobile".

### Step 10: Finish

Save temporary acceptance evidence under `.tmp/reports/`. Report:

- artifact path and URL;
- viewports and states tested;
- probe result;
- screenshots;
- console/network failures;
- any unsupported or unverified state.

Stop the temporary server before finishing unless the user asked to keep it
running.

## 5. Completion Checklist

- [ ] The first viewport states the subject and conclusion.
- [ ] Claims are bound to real evidence and scope.
- [ ] The page is one self-contained HTML5 file.
- [ ] Semantic HTML carries all meaning.
- [ ] Typography, spacing, and palette are restrained and readable.
- [ ] Tables declare and pass their column contracts.
- [ ] Code and diagrams scroll internally without page overflow.
- [ ] Accessibility preference fallbacks exist.
- [ ] Desktop and 390px browser states were visually inspected.
- [ ] DOM geometry and console/network checks passed.
- [ ] Temporary evidence stayed under `.tmp/`.
- [ ] The temporary server was stopped or intentionally handed over.

## 6. Source Provenance

Migrated and adapted on 2026-09-08 from Flowkit revision
`76b7f3f5e77948f47e8390522c61cbb7c9b10d57`:

| Source | Role | Source SHA-256 |
| --- | --- | --- |
| `.trae/skills/apple-design/SKILL.md` | Restraint, typography, materials, accessibility | `11840b24a11d7f94f39c6aaab074750ae4e4de4ef54ee4b1dd97e16ebd485e61` |
| `.trae/skills/ce-plan/references/html-rendering.md` | Self-contained semantic HTML contract | `7b6fa44961597c75e1befb631c535f24e1f52c00e8827a0c5dc0dd52c9e75690` |
| `docs/workflow/html-browser-acceptance.md` | Browser and table acceptance | `8abd94d90c5cf41d45cf045f96e28fb562c3bcf1099d60407f77fe496ca0ed16` |
| `docs/script/html_table_alignment_probe.js` | Executable table geometry gate | `20677271c203011dad30d786769b7d78b8f06efb4b5c3289afc6dcd34f31c74a` |
| `docs/report/html/20260905-job24172-expand-remediation.html` | Production report anatomy | visual inspection |

Flowkit-specific report service, systemd, cluster identity, and directory rules
were intentionally not copied. RecallFS paths and ownership rules govern here.
The table probe was strengthened after migration; the source digest records the
upstream baseline, not byte identity with the reviewed RecallFS version.
