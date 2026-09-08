import fs from "node:fs/promises";
import path from "node:path";
import { pathToFileURL } from "node:url";

const repoRoot = path.resolve(import.meta.dirname, "../../..");
const defaultPlaywright = path.join(
  repoRoot,
  ".tmp/tools/playwright/node_modules/playwright/index.mjs",
);
const playwrightSpecifier =
  process.env.PLAYWRIGHT_MODULE ||
  (await fs
    .access(defaultPlaywright)
    .then(() => pathToFileURL(defaultPlaywright).href)
    .catch(() => "playwright"));
const { chromium } = await import(playwrightSpecifier);

const probePath = path.join(
  repoRoot,
  "skills/html-report/scripts/html_sequence_alignment_probe.js",
);
const probeSource = await fs.readFile(probePath, "utf8");

const goldenHtml = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    * { box-sizing: border-box; }
    html, body { margin: 0; max-width: 100%; overflow-x: hidden; }
    main { width: min(1080px, calc(100% - 24px)); margin: 20px auto; }
    .sequence-figure { width: 100%; margin: 0; }
    li { max-width: 72ch; }
    .sequence-board {
      --sequence-columns: 72px minmax(0, 1fr) minmax(0, 1fr) minmax(0, 1.2fr);
      width: 100%;
      overflow: hidden;
      border: 1px solid #ccc;
    }
    .sequence-head, .sequence-row {
      display: grid;
      grid-template-columns: var(--sequence-columns);
    }
    .sequence-head > *, .sequence-row > * {
      min-width: 0;
      padding: 12px;
      border-left: 1px solid #ccc;
    }
    .sequence-head > :first-child, .sequence-row > :first-child {
      border-left: 0;
    }
    .sequence-rows { margin: 0; padding: 0; list-style: none; }
    .sequence-row { width: 100%; max-width: none; border-top: 1px solid #ccc; }
    .sequence-outcome { grid-column: 2 / 5; }
    .mobile-lane { display: none; }
    @media (max-width: 700px) {
      .sequence-head { display: none; }
      .sequence-row { grid-template-columns: 48px minmax(0, 1fr); }
      .sequence-time { grid-row: 1 / 4; }
      .sequence-cell, .sequence-outcome { grid-column: 2; }
      .mobile-lane { display: block; }
    }
  </style>
</head>
<body>
  <main>
    <figure class="sequence-figure">
      <figcaption>Time moves from top to bottom.</figcaption>
      <div
        class="sequence-board"
        data-sequence-lanes="time,actor-a,actor-b,shared"
      >
        <div class="sequence-head">
          <span data-sequence-lane="time" data-sequence-kind="time">Step</span>
          <span data-sequence-lane="actor-a" data-sequence-kind="actor">A</span>
          <span data-sequence-lane="actor-b" data-sequence-kind="actor">B</span>
          <span data-sequence-lane="shared" data-sequence-kind="shared-state">
            Shared state, not an actor
          </span>
        </div>
        <ol class="sequence-rows">
          <li class="sequence-row">
            <div class="sequence-time" data-sequence-lane="time">1</div>
            <div class="sequence-cell" data-sequence-lane="actor-a">
              <span class="mobile-lane">Actor A</span>
              Read old state
            </div>
            <div class="sequence-cell" data-sequence-lane="actor-b">
              <span class="mobile-lane">Actor B</span>
              Publish new state
            </div>
            <div class="sequence-cell" data-sequence-lane="shared">
              <span class="mobile-lane">Shared state</span>
              Generation changed
            </div>
          </li>
          <li class="sequence-row">
            <div class="sequence-time" data-sequence-lane="time">2</div>
            <div
              class="sequence-outcome"
              data-sequence-span="actor-a,actor-b,shared"
            >
              Delayed consequence
            </div>
          </li>
        </ol>
      </div>
    </figure>
  </main>
</body>
</html>`;

const browser = await chromium.launch({ channel: "chrome", headless: true });
const failures = [];
const runProbe = (page) => page.evaluate((source) => eval(source), probeSource);

const expectProbe = async (name, viewport, mutate, expectedOk, errorText) => {
  const page = await browser.newPage({ viewport });
  await page.setContent(goldenHtml);
  if (mutate) await page.evaluate(mutate);
  const result = await runProbe(page);
  const matchedError =
    errorText === null || result.errors.some((error) => error.includes(errorText));
  if (result.ok !== expectedOk || !matchedError) {
    failures.push({ name, expectedOk, errorText, result });
  }
  await page.close();
};

try {
  await expectProbe(
    "golden-desktop",
    { width: 1440, height: 960 },
    null,
    true,
    null,
  );
  await expectProbe(
    "golden-mobile",
    { width: 390, height: 844 },
    null,
    true,
    null,
  );
  await expectProbe(
    "inherited-row-max-width",
    { width: 1440, height: 960 },
    () => {
      document.querySelector(".sequence-row").style.maxWidth = "70%";
    },
    false,
    "row width does not fill board",
  );
  await expectProbe(
    "undersized-board",
    { width: 1440, height: 960 },
    () => {
      document.querySelector(".sequence-board").style.width = "70%";
    },
    false,
    "board does not fill figure",
  );
  await expectProbe(
    "header-track-drift",
    { width: 1440, height: 960 },
    () => {
      document.querySelector(".sequence-head").style.gridTemplateColumns =
        "60px 1fr 1fr 2fr";
    },
    false,
    "boundary differs from header",
  );
  await expectProbe(
    "missing-shared-state-kind",
    { width: 1440, height: 960 },
    () => {
      document
        .querySelector('[data-sequence-kind="shared-state"]')
        .setAttribute("data-sequence-kind", "actor");
    },
    false,
    "exactly one shared-state lane",
  );
  await expectProbe(
    "bad-outcome-span",
    { width: 1440, height: 960 },
    () => {
      document
        .querySelector("[data-sequence-span]")
        .setAttribute("data-sequence-span", "actor-b,shared");
    },
    false,
    "must cover",
  );
  await expectProbe(
    "hidden-mobile-lane-labels",
    { width: 390, height: 844 },
    () => {
      document.querySelectorAll(".mobile-lane").forEach((label) => {
        label.style.display = "none";
      });
    },
    false,
    "lacks a visible mobile lane label",
  );
} finally {
  await browser.close();
}

if (failures.length) {
  console.error(JSON.stringify({ ok: false, failures }, null, 2));
  process.exit(1);
}

console.log("HTML sequence probe regression passed: 2 positive, 6 negative gates");
