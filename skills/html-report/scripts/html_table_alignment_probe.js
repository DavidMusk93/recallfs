/*
 * Browser-evaluate payload for self-contained technical HTML table gates.
 *
 * Run this source in the target report page after setting the viewport and
 * interaction state. A passing result has `ok: true`; any error is a hard gate.
 */
(() => {
  "use strict";

  const tolerancePx = 0.75;
  const maxErrorsPerColumn = 12;
  const allowedKinds = new Set(["text", "measure", "identifier"]);
  const errors = [];

  const visible = (element) => {
    for (let node = element; node; node = node.parentElement) {
      const style = getComputedStyle(node);
      if (
        node.hidden ||
        style.display === "none" ||
        style.visibility === "hidden" ||
        Number.parseFloat(style.opacity) === 0
      ) {
        return false;
      }
    }
    return [...element.getClientRects()].some(
      (rect) => rect.width > tolerancePx && rect.height > tolerancePx
    );
  };

  const closeEnough = (left, right) =>
    Math.abs(left - right) <= tolerancePx;

  const textFragments = (cell) => {
    const walker = document.createTreeWalker(
      cell,
      NodeFilter.SHOW_TEXT,
      {
        acceptNode: (node) =>
          node.textContent.trim()
            ? NodeFilter.FILTER_ACCEPT
            : NodeFilter.FILTER_REJECT,
      }
    );
    const rects = [];
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      const range = document.createRange();
      range.selectNodeContents(node);
      rects.push(
        ...[...range.getClientRects()].filter(
          (rect) => rect.width > 0 && rect.height > 0
        )
      );
    }
    return rects;
  };

  const contentAnchor = (rects, side) => {
    if (!rects.length) return null;
    return side === "right"
      ? Math.max(...rects.map((rect) => rect.right))
      : Math.min(...rects.map((rect) => rect.left));
  };

  const textParents = (cell) => {
    const walker = document.createTreeWalker(
      cell,
      NodeFilter.SHOW_TEXT,
      {
        acceptNode: (node) =>
          node.textContent.trim()
            ? NodeFilter.FILTER_ACCEPT
            : NodeFilter.FILTER_REJECT,
      }
    );
    const parents = new Set();
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      if (node.parentElement && node.parentElement !== cell) {
        parents.add(node.parentElement);
      }
    }
    return [...parents];
  };

  const tableName = (table, index) => {
    if (table.id) return `#${table.id}`;
    const stableClass = [...table.classList].find(
      (name) => name !== "table" && !name.startsWith("is-")
    );
    return stableClass ? `.${stableClass}` : `table[${index}]`;
  };

  const scrollBoundaryFor = (table, wrapper) => {
    for (
      let node = table.parentElement;
      node && node !== wrapper;
      node = node.parentElement
    ) {
      const style = getComputedStyle(node);
      if (
        node.scrollWidth > node.clientWidth + tolerancePx &&
        ["hidden", "clip"].includes(style.overflowX)
      ) {
        return { clippedBy: node, scrollHost: null };
      }
    }
    if (!wrapper) return { clippedBy: null, scrollHost: null };
    const wrapperStyle = getComputedStyle(wrapper);
    const scrollHost =
      wrapper.scrollWidth > wrapper.clientWidth + tolerancePx &&
      ["auto", "scroll"].includes(wrapperStyle.overflowX)
        ? wrapper
        : null;
    return { clippedBy: null, scrollHost };
  };

  const tables = [...document.querySelectorAll("table")]
    .filter(visible)
    .map((table, tableIndex) => {
      const name = tableName(table, tableIndex);
      const tableErrors = [];
      const headerRows = table.tHead
        ? [...table.tHead.rows].filter(visible)
        : [];
      const headerRow = headerRows.length === 1 ? headerRows[0] : null;
      const bodyRows = table.tBodies.length
        ? [...table.tBodies].flatMap((body) => [...body.rows]).filter(visible)
        : [];
      const headers = headerRow ? [...headerRow.cells] : [];
      const rawKinds = table.getAttribute("data-column-kinds");
      const kinds =
        rawKinds === null ? [] : rawKinds.split(",").map((kind) => kind.trim());

      if (headerRows.length !== 1) {
        tableErrors.push(
          `expected exactly one visible thead row, found ${headerRows.length}`
        );
      }
      if (!bodyRows.length) tableErrors.push("no visible tbody rows");
      if (rawKinds === null || rawKinds.trim() === "") {
        tableErrors.push("missing data-column-kinds");
      } else if (kinds.some((kind) => kind === "")) {
        tableErrors.push("data-column-kinds contains an empty kind");
      } else if (kinds.length !== headers.length) {
        tableErrors.push(
          `data-column-kinds has ${kinds.length} entries, expected ${headers.length}`
        );
      }
      kinds.forEach((kind, index) => {
        if (!allowedKinds.has(kind)) {
          tableErrors.push(`column ${index + 1} has unsupported kind ${kind}`);
        }
      });

      const columns = [];
      if (
        headerRow &&
        bodyRows.length &&
        kinds.length === headers.length &&
        kinds.every((kind) => allowedKinds.has(kind))
      ) {
        const colCount = table.querySelectorAll("colgroup > col").length;
        if (colCount !== headers.length) {
          tableErrors.push(
            `colgroup has ${colCount} tracks, expected ${headers.length}`
          );
        }
        if (getComputedStyle(table).tableLayout !== "fixed") {
          tableErrors.push("table-layout must be fixed");
        }

        [headerRow, ...bodyRows].forEach((row, rowIndex) => {
          const role = rowIndex === 0 ? "header" : `visible row ${rowIndex}`;
          if (row.cells.length !== headers.length) {
            tableErrors.push(
              `${role} has ${row.cells.length} cells, expected ${headers.length}`
            );
          }
          [...row.cells].forEach((cell, cellIndex) => {
            if (cell.colSpan !== 1 || cell.rowSpan !== 1) {
              tableErrors.push(
                `${role} cell ${cellIndex + 1} uses colspan=${cell.colSpan}, rowspan=${cell.rowSpan}`
              );
            }
          });
        });

        headers.forEach((header, columnIndex) => {
          const kind = kinds[columnIndex];
          const expectedAlign = kind === "measure" ? "right" : "left";
          const headerRect = header.getBoundingClientRect();
          const headerAnchor = contentAnchor(
            textFragments(header),
            expectedAlign
          );
          const cells = bodyRows
            .map((row) => row.cells[columnIndex])
            .filter(Boolean);
          const columnErrors = [];
          let suppressedErrors = 0;
          const addColumnError = (message) => {
            if (columnErrors.length < maxErrorsPerColumn) {
              columnErrors.push(message);
            } else {
              suppressedErrors += 1;
            }
          };

          [header, ...cells].forEach((cell, cellIndex) => {
            const role = cellIndex === 0 ? "header" : `row ${cellIndex}`;
            const style = getComputedStyle(cell);
            if (style.textAlign !== expectedAlign) {
              addColumnError(
                `${role} text-align=${style.textAlign}, expected ${expectedAlign}`
              );
            }
            textParents(cell).forEach((element) => {
              const descendantAlign = getComputedStyle(element).textAlign;
              if (descendantAlign !== expectedAlign) {
                addColumnError(
                  `${role} descendant text-align=${descendantAlign}, expected ${expectedAlign}`
                );
              }
            });
            if (
              !closeEnough(cell.offsetLeft, header.offsetLeft) ||
              !closeEnough(cell.offsetWidth, header.offsetWidth)
            ) {
              addColumnError(
                `${role} track differs from header: left=${cell.offsetLeft}, width=${cell.offsetWidth}`
              );
            }
            if (kind === "measure") {
              if (!style.fontVariantNumeric.includes("tabular-nums")) {
                addColumnError(`${role} lacks tabular-nums`);
              }
              if (style.whiteSpace !== "nowrap") {
                addColumnError(
                  `${role} white-space=${style.whiteSpace}, expected nowrap`
                );
              }
            }

            const fragments = textFragments(cell);
            const anchor = contentAnchor(fragments, expectedAlign);
            if (
              anchor !== null &&
              headerAnchor !== null &&
              !closeEnough(anchor, headerAnchor)
            ) {
              addColumnError(
                `${role} content ${expectedAlign} anchor differs by ${Math.abs(
                  anchor - headerAnchor
                ).toFixed(2)}px`
              );
            }

            const cellRect = cell.getBoundingClientRect();
            const contentLeft =
              cellRect.left + (Number.parseFloat(style.paddingLeft) || 0);
            const contentRight =
              cellRect.right - (Number.parseFloat(style.paddingRight) || 0);
            if (
              fragments.some(
                (rect) =>
                  rect.left < contentLeft - tolerancePx ||
                  rect.right > contentRight + tolerancePx
              )
            ) {
              addColumnError(`${role} text escapes the cell content box`);
            }
            if (
              cell.scrollWidth > cell.clientWidth + tolerancePx &&
              ["hidden", "clip"].includes(style.overflowX)
            ) {
              addColumnError(`${role} clips horizontally overflowing content`);
            }
          });
          if (suppressedErrors > 0) {
            columnErrors.push(
              `${suppressedErrors} additional errors omitted for this column`
            );
          }

          columnErrors.forEach((error) =>
            tableErrors.push(`column ${columnIndex + 1} (${kind}): ${error}`)
          );
          columns.push({
            index: columnIndex + 1,
            kind,
            expectedAlign,
            left: headerRect.left,
            right: headerRect.right,
            width: headerRect.width,
            visibleCells: cells.length,
            errors: columnErrors,
          });
        });
      }

      const wrapper = table.closest(".table-wrap");
      const tableOverflowsWrapper =
        wrapper && table.scrollWidth > wrapper.clientWidth + tolerancePx;
      const { clippedBy, scrollHost } = scrollBoundaryFor(table, wrapper);
      if (!wrapper) {
        tableErrors.push("missing .table-wrap scroll container");
      }
      if (clippedBy) {
        tableErrors.push("table overflow is clipped before reaching .table-wrap");
      }
      if (tableOverflowsWrapper && !scrollHost) {
        tableErrors.push("horizontal overflow is not contained by a scroll host");
      }

      tableErrors.forEach((error) => errors.push(`${name}: ${error}`));
      return {
        name,
        kinds,
        visibleRows: bodyRows.length,
        columns,
        wrapper: wrapper
          ? {
              clientWidth: wrapper.clientWidth,
              scrollWidth: wrapper.scrollWidth,
              overflowX: getComputedStyle(wrapper).overflowX,
            }
          : null,
        errors: tableErrors,
      };
    });

  if (document.documentElement.scrollWidth > window.innerWidth + tolerancePx) {
    errors.push(
      `page horizontal overflow: ${document.documentElement.scrollWidth}px > ${window.innerWidth}px`
    );
  }
  if (!tables.length) errors.push("no visible tables");

  return {
    ok: errors.length === 0,
    url: location.href,
    viewport: {
      width: window.innerWidth,
      height: window.innerHeight,
      pageScrollWidth: document.documentElement.scrollWidth,
    },
    state: {
      pressed: [...document.querySelectorAll('[aria-pressed="true"]')].map(
        (element) => element.textContent.trim()
      ),
      selectedTabs: [
        ...document.querySelectorAll('[role="tab"][aria-selected="true"]'),
      ].map((element) => element.textContent.trim()),
    },
    tables,
    errors,
  };
})()
