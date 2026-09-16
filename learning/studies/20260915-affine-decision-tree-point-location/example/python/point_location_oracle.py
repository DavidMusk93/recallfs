from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any


DOMAIN = (0.0, 0.0, 12.0, 8.0)
RESULT_SCHEMA = "odt-example-result/v1"
SUMMARY_SCHEMA = "odt-example-summary/v1"
RESULT_KEYS = {
    "schema",
    "query_id",
    "class",
    "x_hex",
    "y_hex",
    "scalar",
    "batch",
    "restored",
}
SEMANTIC_KEYS = {"kind", "status", "region_id"}


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Site:
    ordinal: int
    region_id: int
    x: float
    y: float
    exact_x: Fraction
    exact_y: Fraction


@dataclass(frozen=True)
class Query:
    query_id: int
    query_class: str
    x_hex: str
    y_hex: str
    x: float
    y: float


@dataclass(frozen=True)
class Expected:
    kind: str
    status: str
    region_id: int

    def as_dict(self) -> dict[str, str | int]:
        return {
            "kind": self.kind,
            "status": self.status,
            "region_id": self.region_id,
        }


def _parse_binary64(text: str, context: str) -> float:
    try:
        value = float.fromhex(text)
    except ValueError as error:
        raise ProtocolError(f"{context}: invalid binary64 value {text!r}") from error
    if value.hex() != text:
        raise ProtocolError(f"{context}: non-canonical binary64 value {text!r}")
    return value


def read_sites(path: Path) -> list[Site]:
    with path.open(newline="", encoding="ascii") as stream:
        reader = csv.DictReader(stream)
        expected_header = ["ordinal", "region_id", "x_hex", "y_hex"]
        if reader.fieldnames != expected_header:
            raise ProtocolError(f"{path}: invalid site CSV header")
        sites: list[Site] = []
        region_ids: set[int] = set()
        coordinates: set[tuple[float, float]] = set()
        for ordinal, row in enumerate(reader):
            if int(row["ordinal"]) != ordinal:
                raise ProtocolError(f"{path}: non-contiguous site ordinal at row {ordinal + 2}")
            region_id = int(row["region_id"])
            x = _parse_binary64(row["x_hex"], f"{path}:{ordinal + 2}:x")
            y = _parse_binary64(row["y_hex"], f"{path}:{ordinal + 2}:y")
            if not math.isfinite(x) or not math.isfinite(y):
                raise ProtocolError(f"{path}:{ordinal + 2}: non-finite site")
            if not (DOMAIN[0] <= x <= DOMAIN[2] and DOMAIN[1] <= y <= DOMAIN[3]):
                raise ProtocolError(f"{path}:{ordinal + 2}: site outside domain")
            if region_id in region_ids:
                raise ProtocolError(f"{path}:{ordinal + 2}: duplicate region ID")
            if (x, y) in coordinates:
                raise ProtocolError(f"{path}:{ordinal + 2}: duplicate site")
            region_ids.add(region_id)
            coordinates.add((x, y))
            sites.append(
                Site(
                    ordinal,
                    region_id,
                    x,
                    y,
                    Fraction.from_float(x),
                    Fraction.from_float(y),
                )
            )
    if len(sites) != 1000:
        raise ProtocolError(f"{path}: expected 1000 sites, found {len(sites)}")
    return sites


def read_queries(path: Path) -> list[Query]:
    with path.open(newline="", encoding="ascii") as stream:
        reader = csv.DictReader(stream)
        expected_header = [
            "query_id",
            "class",
            "x_hex",
            "y_hex",
            "site_a",
            "site_b",
            "expected_kind",
            "expected_status",
            "expected_region_id",
        ]
        if reader.fieldnames != expected_header:
            raise ProtocolError(f"{path}: invalid query CSV header")
        queries: list[Query] = []
        for query_id, row in enumerate(reader):
            if int(row["query_id"]) != query_id:
                raise ProtocolError(f"{path}: non-contiguous query ID at row {query_id + 2}")
            query_class = row["class"]
            if not query_class or not query_class.replace("_", "").isalnum():
                raise ProtocolError(f"{path}:{query_id + 2}: invalid query class")
            queries.append(
                Query(
                    query_id,
                    query_class,
                    row["x_hex"],
                    row["y_hex"],
                    _parse_binary64(row["x_hex"], f"{path}:{query_id + 2}:x"),
                    _parse_binary64(row["y_hex"], f"{path}:{query_id + 2}:y"),
                )
            )
    if not queries:
        raise ProtocolError(f"{path}: no queries")
    return queries


def exact_expected(query: Query, sites: list[Site]) -> Expected:
    if not math.isfinite(query.x) or not math.isfinite(query.y):
        return Expected("error", "invalid data", 0)
    if not (
        DOMAIN[0] <= query.x <= DOMAIN[2]
        and DOMAIN[1] <= query.y <= DOMAIN[3]
    ):
        return Expected("outside", "ok", 0)

    exact_x = Fraction.from_float(query.x)
    exact_y = Fraction.from_float(query.y)
    winner = min(
        sites,
        key=lambda site: (
            (exact_x - site.exact_x) ** 2 + (exact_y - site.exact_y) ** 2,
            site.ordinal,
        ),
    )
    return Expected("region", "ok", winner.region_id)


def _validate_semantic(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict) or not SEMANTIC_KEYS <= value.keys():
        raise ProtocolError(f"{context}: invalid result object")
    if value["kind"] not in {"region", "outside", "error"}:
        raise ProtocolError(f"{context}: invalid result kind")
    if not isinstance(value["status"], str) or type(value["region_id"]) is not int:
        raise ProtocolError(f"{context}: invalid result field type")
    return value


def read_results(path: Path, queries: list[Query]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open(encoding="ascii") as stream:
        for line_number, line in enumerate(stream, start=1):
            try:
                row = json.loads(line)
            except json.JSONDecodeError as error:
                raise ProtocolError(f"{path}:{line_number}: invalid JSON") from error
            if not isinstance(row, dict) or set(row) != RESULT_KEYS:
                raise ProtocolError(f"{path}:{line_number}: invalid result schema")
            if row["schema"] != RESULT_SCHEMA:
                raise ProtocolError(f"{path}:{line_number}: unsupported result schema")
            rows.append(row)
    if len(rows) != len(queries):
        raise ProtocolError(
            f"{path}: result count {len(rows)} does not match query count {len(queries)}"
        )
    for query, row in zip(queries, rows, strict=True):
        if (
            row["query_id"] != query.query_id
            or row["class"] != query.query_class
            or row["x_hex"] != query.x_hex
            or row["y_hex"] != query.y_hex
        ):
            raise ProtocolError(f"{path}: query identity mismatch at {query.query_id}")
        for path_name in ("scalar", "batch", "restored"):
            _validate_semantic(row[path_name], f"{path}:{query.query_id}:{path_name}")
    return rows


def read_summary(path: Path, query_count: int) -> dict[str, Any]:
    try:
        summary = json.loads(path.read_text(encoding="ascii"))
    except json.JSONDecodeError as error:
        raise ProtocolError(f"{path}: invalid summary JSON") from error
    if not isinstance(summary, dict) or summary.get("schema") != SUMMARY_SCHEMA:
        raise ProtocolError(f"{path}: unsupported summary schema")
    inputs = summary.get("inputs")
    mismatches = summary.get("mismatches")
    if not isinstance(inputs, dict) or not isinstance(mismatches, dict):
        raise ProtocolError(f"{path}: invalid summary object")
    if inputs.get("site_count") != 1000:
        raise ProtocolError(f"{path}: summary site count mismatch")
    if inputs.get("query_count") != query_count:
        raise ProtocolError(f"{path}: summary query count mismatch")
    if type(mismatches.get("total")) is not int:
        raise ProtocolError(f"{path}: invalid mismatch summary")
    return summary


def compare(
    sites: list[Site],
    queries: list[Query],
    rows: list[dict[str, Any]],
    summary: dict[str, Any],
) -> tuple[dict[str, int], int, list[str]]:
    totals = {"region": 0, "outside": 0, "error": 0}
    mismatch_count = 0
    mismatches: list[str] = []
    for query, row in zip(queries, rows, strict=True):
        expected = exact_expected(query, sites)
        totals[expected.kind] += 1
        expected_fields = expected.as_dict()
        for path_name in ("scalar", "batch", "restored"):
            observed = {
                key: row[path_name][key]
                for key in ("kind", "status", "region_id")
            }
            if observed != expected_fields:
                mismatch_count += 1
                if len(mismatches) < 20:
                    mismatches.append(
                        f"query={query.query_id} class={query.query_class} "
                        f"path={path_name} expected={expected_fields} observed={observed}"
                    )
    if summary["mismatches"]["total"] != 0:
        mismatch_count += summary["mismatches"]["total"]
        mismatches.append(
            f"C summary reports {summary['mismatches']['total']} internal mismatches"
        )
    return totals, mismatch_count, mismatches


def print_report(
    sites_path: Path,
    queries_path: Path,
    results_path: Path,
    summary_path: Path,
    sites: list[Site],
    queries: list[Query],
    rows: list[dict[str, Any]],
    totals: dict[str, int],
    mismatch_count: int,
    mismatches: list[str],
) -> None:
    print("Oblique Decision Tree exact differential oracle")
    print(f"sites=1000 queries={len(queries)}")
    print(
        "expected totals: "
        f"region={totals['region']} outside={totals['outside']} error={totals['error']}"
    )
    print(
        f"checked paths: scalar={len(rows)} batch={len(rows)} restored={len(rows)}"
    )
    print(f"mismatches={mismatch_count}")
    print("representative rows:")
    seen: set[str] = set()
    for query, row in zip(queries, rows, strict=True):
        if query.query_class in seen:
            continue
        seen.add(query.query_class)
        expected = exact_expected(query, sites)
        scalar = row["scalar"]
        print(
            f"  id={query.query_id} class={query.query_class} "
            f"point=({query.x_hex},{query.y_hex}) "
            f"expected={expected.kind}:{expected.region_id} "
            f"scalar={scalar['kind']}:{scalar['region_id']}"
        )
    print("artifacts:")
    print(f"  sites={sites_path}")
    print(f"  queries={queries_path}")
    print(f"  results={results_path}")
    print(f"  summary={summary_path}")
    for mismatch in mismatches:
        print(f"mismatch: {mismatch}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check ODT example outputs with exact nearest-site arithmetic"
    )
    parser.add_argument("--sites", type=Path, required=True)
    parser.add_argument("--queries", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()

    try:
        sites = read_sites(args.sites)
        queries = read_queries(args.queries)
        rows = read_results(args.results, queries)
        summary = read_summary(args.summary, len(queries))
        totals, mismatch_count, mismatches = compare(sites, queries, rows, summary)
        print_report(
            args.sites,
            args.queries,
            args.results,
            args.summary,
            sites,
            queries,
            rows,
            totals,
            mismatch_count,
            mismatches,
        )
        return 1 if mismatch_count else 0
    except (OSError, ProtocolError, UnicodeError, ValueError, TypeError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
