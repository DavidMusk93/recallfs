# HTML Browser Acceptance

Use this workflow for every new or materially changed HTML report. Validation
targets the actual HTTP response and rendered DOM, not only source code or a
local screenshot.

## 1. Acceptance Matrix

| Surface | Required evidence |
| --- | --- |
| Every report | Desktop screenshot, 390px screenshot, page geometry, console |
| Report with tables | Table probe for every visible table state |
| Report with sequence lanes | Sequence probe plus focused desktop/mobile screenshots |
| Report with filters/tabs/details affecting layout | Every reachable visible state |
| Report with wide tables | Mobile left edge and right edge screenshots |
| Report with bars or proportions | Actual pixel geometry versus declared value |
| Report with canvas/3D | Nonblank pixel check and framing at both viewports |

If a report has no tables, skip the table probe. Do not run a table-required
probe and reinterpret its failure.

## 2. Serve the Artifact

Serve the directory containing the final artifact. Use an available static
server and a free port. Example:

```bash
ruby -run -e httpd <report-directory> -p 8765
```

Use a real `http://` URL because browser sandboxes commonly reject `file://`.
The bytes served must be the bytes intended for delivery.

Record:

- server command;
- URL;
- artifact digest when evidence integrity matters;
- whether the page depends on any external request.

Stop the server after acceptance unless the user asks to keep it running.

## 3. Enumerate Visible States

List:

- default state;
- every tab;
- every filter that changes rows or columns;
- expanded/collapsed state that changes comparison geometry;
- theme or mode toggle when present;
- table scroll positions needed to inspect both mobile edges.

Hidden rows must be validated while visible. Zero-sized hidden elements are not
evidence.

## 4. Desktop Viewport

Use a viewport near 1440px wide.

Check:

- the subject and conclusion are clear in the first viewport;
- the next section is visible or hinted below the fold;
- prose measure is readable;
- metrics form a stable grid;
- tables, code, and diagrams align to the page grid;
- no text overlaps or clips;
- no section is styled as an unnecessary floating card;
- `document.documentElement.scrollWidth <= window.innerWidth`;
- console and network logs contain no report-caused errors.

Capture:

- one full-page screenshot;
- focused screenshots for each critical table, code block, or diagram.

## 5. Mobile Viewport

Use exactly 390px unless the task defines another target.

Repeat the full state matrix. Check:

- no page-level horizontal overflow;
- metric grids collapse predictably;
- long words and identifiers wrap without covering neighbors;
- code and ASCII graphs scroll internally;
- sequence lanes stack by numbered event with visible lane labels;
- tables scroll inside `.table-wrap`;
- buttons and tabs retain usable hit targets;
- headings do not overflow;
- the title does not consume the entire viewport.

For a wide table, capture its left edge and then its right edge after scrolling
the table wrapper. A first-viewport screenshot does not validate a table that
never entered view.

## 6. Page Geometry Probe

Run:

```js
JSON.stringify({
  viewportWidth: window.innerWidth,
  pageScrollWidth: document.documentElement.scrollWidth,
  pageOverflow:
    document.documentElement.scrollWidth > window.innerWidth + 0.75,
  bodyWidth: document.body.getBoundingClientRect().width,
  headings: [...document.querySelectorAll("h1,h2,h3")].map((heading) => ({
    text: heading.textContent.trim(),
    left: heading.getBoundingClientRect().left,
    right: heading.getBoundingClientRect().right,
    viewportRight: window.innerWidth,
  })),
  scrollContainers: [...document.querySelectorAll(
    ".table-wrap,.code-block,.ascii-graph,.sequence-board,.ownership-flow"
  )].map((element) => ({
    className: element.className,
    clientWidth: element.clientWidth,
    scrollWidth: element.scrollWidth,
    overflowX: getComputedStyle(element).overflowX,
  })),
})
```

Failure conditions:

- page overflow is true;
- a heading starts before 0 or ends beyond the viewport;
- wide content has `scrollWidth > clientWidth` without
  `overflow-x: auto|scroll`;
- a supposedly fixed-format region changes size when labels or values update.

## 6.1. Sequence-Lane Probe

For every visible sequence-lane mechanism, execute the complete source of:

```text
../scripts/html_sequence_alignment_probe.js
```

A pass is:

```json
{
  "ok": true,
  "errors": []
}
```

The probe validates:

- an ordered `data-sequence-lanes` manifest;
- at least two actor lanes and exactly one shared-state lane;
- one board-owned `--sequence-columns` definition;
- ordinary row lane order and outcome-span declarations;
- full-width rows unaffected by global prose `max-width`;
- exact desktop header/row lane boundaries within `0.75px`;
- visible mobile lane labels when the desktop header is hidden;
- component and page overflow.

Also inspect the focused screenshots. The probe cannot determine whether the
event decomposition is causally correct, whether a state lane is explained in
plain language, or whether the first invalid invariant is the right one.

For desktop, require every ordinary row's left/right lane boundaries to match
the corresponding header lane. For mobile, require one vertical event stack
per time step; do not accept a scaled-down desktop matrix.

When changing the sequence contract or probe, run:

```bash
PLAYWRIGHT_MODULE="file://$PWD/.tmp/tools/playwright/node_modules/playwright/index.mjs" \
  node skills/html-report/tests/sequence-probe-regression.mjs
```

The regression runner requires two valid viewport cases to pass and six
malformed structures to fail closed: inherited row width, an undersized board,
divergent header tracks, missing shared-state semantics, an incorrect outcome
span, and hidden mobile lane labels.

## 7. Table Probe

Probe source:

```text
../scripts/html_table_alignment_probe.js
```

Execute the entire file in the target page with browser evaluation. A pass is:

```json
{
  "ok": true,
  "errors": []
}
```

The probe verifies:

- legal, count-matched `data-column-kinds`;
- one `colgroup > col` per column;
- `table-layout: fixed`;
- equal cell counts for visible rows;
- consistent cell tracks;
- left anchors for `text` and `identifier`;
- right anchors, tabular numerals, and nowrap for `measure`;
- internal table scrolling;
- no page-level horizontal overflow.

Do not replace the probe with `th/td` box equality. Cells in one HTML table
already share tracks; content anchor alignment is the harder contract.

The probe verifies declared geometry, not author intent. Independently inspect
the header and representative values to confirm each declared kind is
semantically correct. A PID or revision remains an `identifier` even when it
contains only digits.

When changing the bundled probe or golden example, run:

```bash
npm install --prefix .tmp/tools/playwright --no-save playwright
PLAYWRIGHT_MODULE="file://$PWD/.tmp/tools/playwright/node_modules/playwright/index.mjs" \
  node skills/html-report/tests/probe-regression.mjs
```

The regression runner validates the golden report at 1440px and 390px, then
requires malformed kinds, nested alignment overrides, visually hidden tables,
multiple header rows, spans, clipped wrappers, and clipped cell content to
fail.

## 8. Proportion Geometry

For progress bars, stacked bars, and distribution tracks, compare the rendered
fill to the declared ratio:

```js
const track = document.querySelector("[data-ratio-track]");
const fill = track.querySelector("[data-ratio-fill]");
const declared = Number(fill.dataset.ratio);
const actual =
  fill.getBoundingClientRect().width /
  track.getBoundingClientRect().width;
({
  declared,
  actual,
  errorPercentagePoints: Math.abs(declared - actual) * 100,
});
```

The error must be at most `0.5` percentage points. Nonzero values must not
render as zero-width fills. Capture computed color and numeric labels because
color cannot be the only encoding.

For cumulative intervals drawn on separate rows, verify each later left edge
equals the prior right edge within `0.5px`.

## 9. Code Block Probe

For each `.code-block`:

```js
[...document.querySelectorAll(".code-block")].map((block) => {
  const lines = [...block.querySelectorAll(".code-line")];
  const code = block.querySelector("code");
  return {
    language: block.dataset.language,
    lines: lines.length,
    whiteSpace: lines[0] && getComputedStyle(lines[0]).whiteSpace,
    codeMinWidth: code && getComputedStyle(code).minWidth,
    clientWidth: block.clientWidth,
    scrollWidth: block.scrollWidth,
    overflowX: getComputedStyle(block).overflowX,
    whitespaceTextNodes: [...(code?.childNodes || [])].filter(
      (node) => node.nodeType === Node.TEXT_NODE && node.textContent.trim() === ""
    ).length,
  };
})
```

Require:

- nonempty real language;
- continuous `.code-line` elements;
- `white-space: pre`;
- `min-width: max-content`;
- internal scrolling for long lines;
- no bare whitespace text nodes between line spans;
- line numbers excluded from copied text.

## 10. Interaction Checks

For each control:

- inspect the page before action;
- interact through the browser;
- wait for the smallest reliable condition;
- capture a fresh snapshot;
- rerun page and table probes;
- verify keyboard focus and labels;
- verify reduced-motion behavior when motion exists.

Element references can expire after DOM changes. Take a fresh snapshot before
each interaction.

## 11. Evidence Layout

Store temporary evidence under:

```text
.tmp/reports/<report-name>/
  desktop-full.png
  desktop-<table>.png
  mobile-full.png
  mobile-<table>-left.png
  mobile-<table>-right.png
  desktop-sequence.png
  mobile-sequence.png
  desktop-sequence-probe.json
  mobile-sequence-probe.json
  desktop-table-probe.json
  mobile-table-probe.json
  geometry.json
  console.txt
```

Do not commit these files by default.

## 12. Failure Handling

A report fails acceptance when:

- the page has horizontal overflow;
- visible text overlaps or clips;
- a critical table never enters a screenshot;
- a table probe reports any error;
- a sequence-lane row does not fill the board or its lane boundaries differ
  from the header;
- a shared-state lane is presented as though it were another actor;
- a mobile sequence hides the desktop header without visible lane labels;
- a numeric column is not right-aligned and tabular;
- an identifier is treated as a measure;
- interaction states were omitted;
- a progress visualization has incorrect geometry;
- console or network failures affect content;
- only one viewport was tested.

Fix and rerun. If the built-in browser lacks a required viewport or DOM
capability, use an available Playwright/Chromium path against the same URL.
Record that split. Source inspection alone is not a fallback for rendered
acceptance.

## 13. Completion Report

Report:

| Field | Required value |
| --- | --- |
| Artifact | Absolute or repository-relative path |
| URL | Exact tested URL |
| Viewports | Desktop width and 390px |
| States | All states tested |
| Table probes | Pass/fail per state |
| Sequence probes | Pass/fail per sequence mechanism |
| Page overflow | Width values |
| Screenshots | Evidence paths |
| Console/network | Errors or `none` |
| Gaps | Anything not verified |
