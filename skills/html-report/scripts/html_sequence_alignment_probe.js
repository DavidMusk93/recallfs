/*
 * Browser-evaluate payload for sequence-lane mechanism geometry.
 *
 * Run this source in the target report page. A passing result has
 * `ok: true`; every reported error is a hard gate.
 */
(() => {
  "use strict";

  const tolerancePx = 0.75;
  const errors = [];

  const visible = (element) => {
    if (!element) return false;
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

  const directChildren = (element, selector) =>
    [...element.children].filter((child) => child.matches(selector));

  const boardName = (board, index) =>
    board.id ? `#${board.id}` : `.sequence-board[${index}]`;

  const boards = [...document.querySelectorAll(".sequence-board")]
    .filter(visible)
    .map((board, boardIndex) => {
      const name = boardName(board, boardIndex);
      const boardErrors = [];
      const rawManifest = board.getAttribute("data-sequence-lanes") || "";
      const lanes = rawManifest
        .split(",")
        .map((lane) => lane.trim())
        .filter(Boolean);
      const head = board.querySelector(":scope > .sequence-head");
      const rows = [
        ...board.querySelectorAll(
          ":scope > .sequence-rows > .sequence-row"
        ),
      ].filter(visible);
      const headVisible = visible(head);
      const headCells = head
        ? directChildren(head, "[data-sequence-lane]")
        : [];
      const headByLane = new Map(
        headCells.map((cell) => [cell.dataset.sequenceLane, cell])
      );
      const kinds = headCells.map((cell) => cell.dataset.sequenceKind || "");
      const boardRect = board.getBoundingClientRect();
      const figure = board.closest(".sequence-figure");
      const figureRect = figure?.getBoundingClientRect();
      const caption = figure?.querySelector(":scope > figcaption");
      const customTracks = getComputedStyle(board)
        .getPropertyValue("--sequence-columns")
        .trim();

      if (!figure) {
        boardErrors.push("board must be inside .sequence-figure");
      } else {
        if (!visible(caption)) {
          boardErrors.push("sequence figure lacks a visible figcaption");
        }
        if (
          !closeEnough(boardRect.left, figureRect.left) ||
          !closeEnough(boardRect.right, figureRect.right)
        ) {
          boardErrors.push(
            `board does not fill figure: board=${boardRect.left}..${boardRect.right}, figure=${figureRect.left}..${figureRect.right}`
          );
        }
      }
      if (lanes.length < 4) {
        boardErrors.push(
          `lane manifest must contain time, at least two actors, and shared state; found ${lanes.length}`
        );
      }
      if (new Set(lanes).size !== lanes.length) {
        boardErrors.push("lane manifest contains duplicates");
      }
      if (!customTracks) {
        boardErrors.push("missing board-owned --sequence-columns");
      }
      if (!head) {
        boardErrors.push("missing direct .sequence-head");
      } else {
        const headLanes = headCells.map((cell) => cell.dataset.sequenceLane);
        if (headLanes.join(",") !== lanes.join(",")) {
          boardErrors.push(
            `header lanes ${headLanes.join(",")} do not match manifest ${lanes.join(",")}`
          );
        }
      }
      if (kinds.filter((kind) => kind === "actor").length < 2) {
        boardErrors.push("header must declare at least two actor lanes");
      }
      if (kinds.filter((kind) => kind === "shared-state").length !== 1) {
        boardErrors.push(
          "header must declare exactly one shared-state lane"
        );
      }
      if (!rows.length) {
        boardErrors.push("no visible sequence rows");
      }
      if (
        board.scrollWidth > board.clientWidth + tolerancePx ||
        boardRect.left < -tolerancePx ||
        boardRect.right > window.innerWidth + tolerancePx
      ) {
        boardErrors.push(
          `board overflows: client=${board.clientWidth}, scroll=${board.scrollWidth}, left=${boardRect.left}, right=${boardRect.right}`
        );
      }

      const rowResults = rows.map((row, rowIndex) => {
        const rowErrors = [];
        const rowRect = row.getBoundingClientRect();
        const ordinaryCells = directChildren(row, "[data-sequence-lane]");
        const spanCells = directChildren(row, "[data-sequence-span]");
        const rowLanes = ordinaryCells.map(
          (cell) => cell.dataset.sequenceLane
        );

        if (
          !closeEnough(rowRect.left, boardRect.left + board.clientLeft) ||
          !closeEnough(rowRect.right, boardRect.right - board.clientLeft)
        ) {
          rowErrors.push(
            `row width does not fill board: left=${rowRect.left}, right=${rowRect.right}`
          );
        }
        if (getComputedStyle(row).maxWidth !== "none") {
          rowErrors.push(
            `row max-width=${getComputedStyle(row).maxWidth}, expected none`
          );
        }
        if (spanCells.length > 1) {
          rowErrors.push("row has more than one declared span");
        }

        if (!spanCells.length) {
          if (rowLanes.join(",") !== lanes.join(",")) {
            rowErrors.push(
              `row lanes ${rowLanes.join(",")} do not match manifest ${lanes.join(",")}`
            );
          }
        } else {
          const timeLane = lanes[0];
          if (
            ordinaryCells.length !== 1 ||
            ordinaryCells[0].dataset.sequenceLane !== timeLane
          ) {
            rowErrors.push(
              "spanning row must retain only the time lane as an ordinary cell"
            );
          }
          const declaredSpan = spanCells[0].dataset.sequenceSpan
            .split(",")
            .map((lane) => lane.trim())
            .filter(Boolean);
          const expectedSpan = lanes.slice(1);
          if (declaredSpan.join(",") !== expectedSpan.join(",")) {
            rowErrors.push(
              `span ${declaredSpan.join(",")} must cover ${expectedSpan.join(",")}`
            );
          }
        }

        if (headVisible) {
          ordinaryCells.forEach((cell) => {
            const lane = cell.dataset.sequenceLane;
            const header = headByLane.get(lane);
            if (!header) return;
            const cellRect = cell.getBoundingClientRect();
            const headerRect = header.getBoundingClientRect();
            if (
              !closeEnough(cellRect.left, headerRect.left) ||
              !closeEnough(cellRect.right, headerRect.right)
            ) {
              rowErrors.push(
                `${lane} boundary differs from header: cell=${cellRect.left.toFixed(
                  2
                )}..${cellRect.right.toFixed(
                  2
                )}, header=${headerRect.left.toFixed(
                  2
                )}..${headerRect.right.toFixed(2)}`
              );
            }
          });
          spanCells.forEach((cell) => {
            const declaredSpan = cell.dataset.sequenceSpan
              .split(",")
              .map((lane) => lane.trim())
              .filter(Boolean);
            const first = headByLane.get(declaredSpan[0]);
            const last = headByLane.get(declaredSpan.at(-1));
            if (!first || !last) return;
            const cellRect = cell.getBoundingClientRect();
            const firstRect = first.getBoundingClientRect();
            const lastRect = last.getBoundingClientRect();
            if (
              !closeEnough(cellRect.left, firstRect.left) ||
              !closeEnough(cellRect.right, lastRect.right)
            ) {
              rowErrors.push(
                `outcome span boundary differs from declared lanes`
              );
            }
          });
        } else {
          ordinaryCells.slice(1).forEach((cell) => {
            const label = cell.querySelector(":scope > .mobile-lane");
            if (!visible(label)) {
              rowErrors.push(
                `${cell.dataset.sequenceLane} lacks a visible mobile lane label`
              );
            }
          });
        }

        [...ordinaryCells, ...spanCells].forEach((cell) => {
          if (cell.scrollWidth > cell.clientWidth + tolerancePx) {
            rowErrors.push(
              `${cell.dataset.sequenceLane || "outcome"} content overflows its cell`
            );
          }
        });

        rowErrors.forEach((error) =>
          boardErrors.push(`row ${rowIndex + 1}: ${error}`)
        );
        return {
          index: rowIndex + 1,
          lanes: rowLanes,
          left: rowRect.left,
          right: rowRect.right,
          width: rowRect.width,
          errors: rowErrors,
        };
      });

      boardErrors.forEach((error) => errors.push(`${name}: ${error}`));
      return {
        name,
        lanes,
        kinds,
        customTracks,
        headVisible,
        board: {
          left: boardRect.left,
          right: boardRect.right,
          width: boardRect.width,
          clientWidth: board.clientWidth,
          scrollWidth: board.scrollWidth,
        },
        rows: rowResults,
        errors: boardErrors,
      };
    });

  if (!boards.length) errors.push("no visible sequence boards");
  if (document.documentElement.scrollWidth > window.innerWidth + tolerancePx) {
    errors.push(
      `page horizontal overflow: ${document.documentElement.scrollWidth}px > ${window.innerWidth}px`
    );
  }

  return {
    ok: errors.length === 0,
    url: location.href,
    viewport: {
      width: window.innerWidth,
      height: window.innerHeight,
      pageScrollWidth: document.documentElement.scrollWidth,
    },
    boards,
    errors,
  };
})()
