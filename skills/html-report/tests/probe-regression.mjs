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

const examplePath = path.join(
  repoRoot,
  "skills/html-report/examples/flowkit-migration.html",
);
const probePath = path.join(
  repoRoot,
  "skills/html-report/scripts/html_table_alignment_probe.js",
);
const [exampleHtml, probeSource] = await Promise.all([
  fs.readFile(examplePath, "utf8"),
  fs.readFile(probePath, "utf8"),
]);

const browser = await chromium.launch({ channel: "chrome", headless: true });
const failures = [];
const expectedKinds = "text,measure,identifier";

const runProbe = (page) => page.evaluate((source) => eval(source), probeSource);

const expectKinds = (name, actual, shouldMatch) => {
  if ((actual === expectedKinds) !== shouldMatch) {
    failures.push({ name, expectedKinds, actual, shouldMatch });
  }
};

const expectProbe = async (name, viewport, mutate, expectedOk, errorText) => {
  const page = await browser.newPage({ viewport });
  await page.setContent(exampleHtml);
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
  for (const viewport of [
    { width: 1440, height: 960 },
    { width: 390, height: 844 },
  ]) {
    await expectProbe(
      `golden-${viewport.width}`,
      viewport,
      null,
      true,
      null,
    );
  }

  const manifestPage = await browser.newPage({
    viewport: { width: 1440, height: 960 },
  });
  await manifestPage.setContent(exampleHtml);
  const declaredKinds = await manifestPage
    .locator("table")
    .first()
    .getAttribute("data-column-kinds");
  expectKinds("semantic-kind-manifest", declaredKinds, true);
  await manifestPage
    .locator("table")
    .first()
    .evaluate((table) =>
      table.setAttribute("data-column-kinds", "text,measure,measure")
    );
  const wrongKinds = await manifestPage
    .locator("table")
    .first()
    .getAttribute("data-column-kinds");
  expectKinds("semantic-kind-negative", wrongKinds, false);
  await manifestPage.close();

  await expectProbe(
    "nested-block-alignment",
    { width: 1440, height: 960 },
    () => {
      const cell = document.querySelector("tbody tr td:nth-child(2)");
      cell.innerHTML = '<div style="text-align:left;width:100%">17</div>';
    },
    false,
    "descendant text-align=left",
  );

  await expectProbe(
    "opacity-hidden-table",
    { width: 1440, height: 960 },
    () => {
      document.querySelector("table").style.opacity = "0";
    },
    false,
    "no visible tables",
  );

  await expectProbe(
    "scale-zero-table",
    { width: 1440, height: 960 },
    () => {
      document.querySelector("table").style.transform = "scale(0)";
    },
    false,
    "no visible tables",
  );

  await expectProbe(
    "empty-column-kind",
    { width: 1440, height: 960 },
    () => {
      document
        .querySelector("table")
        .setAttribute("data-column-kinds", "text,,identifier");
    },
    false,
    "empty kind",
  );

  await expectProbe(
    "second-header-row",
    { width: 1440, height: 960 },
    () => {
      const head = document.querySelector("thead");
      head.append(head.rows[0].cloneNode(true));
    },
    false,
    "exactly one visible thead row",
  );

  await expectProbe(
    "column-span",
    { width: 1440, height: 960 },
    () => {
      document.querySelector("tbody td").colSpan = 2;
    },
    false,
    "uses colspan=2",
  );

  await expectProbe(
    "hidden-overflow",
    { width: 390, height: 844 },
    () => {
      document.querySelector(".table-wrap").style.overflowX = "hidden";
    },
    false,
    "horizontal overflow is not contained",
  );

  await expectProbe(
    "clipping-ancestor",
    { width: 390, height: 844 },
    () => {
      const table = document.querySelector("table");
      const clipper = document.createElement("div");
      clipper.style.overflowX = "hidden";
      table.before(clipper);
      clipper.append(table);
    },
    false,
    "clipped before reaching .table-wrap",
  );

  await expectProbe(
    "clipped-cell-content",
    { width: 1440, height: 960 },
    () => {
      const cell = document.querySelector("tbody tr td:nth-child(3)");
      cell.textContent = "identifier_".repeat(80);
      cell.style.whiteSpace = "nowrap";
      cell.style.overflowX = "hidden";
    },
    false,
    "clips horizontally overflowing content",
  );
} finally {
  await browser.close();
}

if (failures.length) {
  console.error(JSON.stringify({ ok: false, failures }, null, 2));
  process.exit(1);
}

console.log("HTML table probe regression passed: 3 positive, 10 negative gates");
