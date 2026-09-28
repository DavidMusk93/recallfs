#!/usr/bin/env python3
"""Build and benchmark the selected TPC-H SF10 queries on DuckDB and GenDB."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import time
from datetime import date, datetime, timezone
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path
from typing import Any

import duckdb


QUERY_IDS = ("Q1", "Q3", "Q6", "Q9", "Q18")
BEST_ITERATIONS = {"Q1": "iter_3", "Q3": "iter_1", "Q6": "iter_0", "Q9": "iter_2", "Q18": "iter_1"}

QUERIES = {
    "Q1": """
        SELECT
            l_returnflag,
            l_linestatus,
            SUM(l_quantity) AS sum_qty,
            SUM(l_extendedprice) AS sum_base_price,
            SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
            SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
            AVG(l_quantity) AS avg_qty,
            AVG(l_extendedprice) AS avg_price,
            AVG(l_discount) AS avg_disc,
            COUNT(*) AS count_order
        FROM lineitem
        WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
        GROUP BY l_returnflag, l_linestatus
        ORDER BY l_returnflag, l_linestatus
    """,
    "Q3": """
        SELECT
            l_orderkey,
            SUM(l_extendedprice * (1 - l_discount)) AS revenue,
            o_orderdate,
            o_shippriority
        FROM customer, orders, lineitem
        WHERE
            c_mktsegment = 'BUILDING'
            AND c_custkey = o_custkey
            AND l_orderkey = o_orderkey
            AND o_orderdate < DATE '1995-03-15'
            AND l_shipdate > DATE '1995-03-15'
        GROUP BY l_orderkey, o_orderdate, o_shippriority
        ORDER BY revenue DESC, o_orderdate
        LIMIT 10
    """,
    "Q6": """
        SELECT
            SUM(l_extendedprice * l_discount) AS revenue
        FROM lineitem
        WHERE
            l_shipdate >= DATE '1994-01-01'
            AND l_shipdate < DATE '1994-01-01' + INTERVAL '1' YEAR
            AND l_discount BETWEEN 0.06 - 0.01 AND 0.06 + 0.01
            AND l_quantity < 24
    """,
    "Q9": """
        SELECT
            nation, o_year,
            SUM(amount) AS sum_profit
        FROM (
            SELECT
                n_name AS nation,
                EXTRACT(YEAR FROM o_orderdate) AS o_year,
                l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
            FROM part, supplier, lineitem, partsupp, orders, nation
            WHERE
                s_suppkey = l_suppkey
                AND ps_suppkey = l_suppkey
                AND ps_partkey = l_partkey
                AND p_partkey = l_partkey
                AND o_orderkey = l_orderkey
                AND s_nationkey = n_nationkey
                AND p_name LIKE '%green%'
        ) AS profit
        GROUP BY nation, o_year
        ORDER BY nation, o_year DESC
    """,
    "Q18": """
        SELECT
            c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice,
            SUM(l_quantity) AS sum_qty
        FROM customer, orders, lineitem
        WHERE
            o_orderkey IN (
                SELECT l_orderkey
                FROM lineitem
                GROUP BY l_orderkey
                HAVING SUM(l_quantity) > 300
            )
            AND c_custkey = o_custkey
            AND o_orderkey = l_orderkey
        GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
        ORDER BY o_totalprice DESC, o_orderdate
        LIMIT 100
    """,
}

EXPECTED_HEADERS = {
    "Q1": (
        "l_returnflag",
        "l_linestatus",
        "sum_qty",
        "sum_base_price",
        "sum_disc_price",
        "sum_charge",
        "avg_qty",
        "avg_price",
        "avg_disc",
        "count_order",
    ),
    "Q3": ("l_orderkey", "revenue", "o_orderdate", "o_shippriority"),
    "Q6": ("revenue",),
    "Q9": ("nation", "o_year", "sum_profit"),
    "Q18": (
        "c_name",
        "c_custkey",
        "o_orderkey",
        "o_orderdate",
        "o_totalprice",
        "sum_qty",
    ),
}

# Decimal places emitted by the generated C++ programs. Non-listed fields are
# compared as exact strings, so keys, dates, grouping labels, and counts remain
# strict correctness checks.
DECIMAL_PLACES = {
    "Q1": {
        "sum_qty": 2,
        "sum_base_price": 2,
        "sum_disc_price": 4,
        "sum_charge": 6,
        "avg_qty": 2,
        "avg_price": 2,
        "avg_disc": 2,
    },
    "Q3": {"revenue": 2},
    "Q6": {"revenue": 2},
    "Q9": {"sum_profit": 2},
    "Q18": {"o_totalprice": 2, "sum_qty": 2},
}

EXPECTED_ROW_COUNTS = {
    "nation": 25,
    "region": 5,
    "supplier": 100_000,
    "part": 2_000_000,
    "partsupp": 8_000_000,
    "customer": 1_500_000,
    "orders": 15_000_000,
    "lineitem": 59_986_052,
}

TABLE_LOAD_ORDER = (
    "nation",
    "region",
    "supplier",
    "part",
    "partsupp",
    "customer",
    "orders",
    "lineitem",
)

TIMING_RE = re.compile(r"^\[TIMING\]\s+([^:]+):\s+([0-9.]+)\s+ms$", re.MULTILINE)


def strip_foreign_keys(schema_sql: str) -> str:
    """Match the paper harness: retain primary keys while removing foreign keys."""
    result = re.sub(r"--[^\n]*", "", schema_sql)
    result = re.sub(
        r",?\s*FOREIGN KEY\s*\([^)]+\)\s*REFERENCES\s+\w+\([^)]+\)",
        "",
        result,
        flags=re.IGNORECASE,
    )
    result = re.sub(
        r"\s*REFERENCES\s+\w+\([^)]+\)",
        "",
        result,
        flags=re.IGNORECASE,
    )
    return re.sub(r",\s*\)", "\n)", result)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def setup_duckdb(args: argparse.Namespace) -> None:
    data_dir = args.data_dir.resolve()
    database = args.database.resolve()
    schema_path = args.schema.resolve()
    evidence = args.evidence.resolve()

    if database.exists():
        if not args.force:
            raise SystemExit(f"database already exists: {database}; pass --force to rebuild")
        database.unlink()
    database.parent.mkdir(parents=True, exist_ok=True)

    started = time.perf_counter()
    connection = duckdb.connect(str(database))
    connection.execute(f"SET threads = {args.threads}")

    schema_sql = strip_foreign_keys(schema_path.read_text(encoding="utf-8"))
    for statement in schema_sql.split(";"):
        statement = statement.strip()
        if statement.upper().startswith("CREATE"):
            connection.execute(statement)

    row_counts: dict[str, int] = {}
    load_times_ms: dict[str, float] = {}
    for table in TABLE_LOAD_ORDER:
        source = data_dir / f"{table}.tbl"
        if not source.is_file():
            raise FileNotFoundError(source)
        load_start = time.perf_counter()
        copy_result = connection.execute(
            f"COPY {table} FROM '{source}' (DELIMITER '|')"
        ).fetchone()
        load_times_ms[table] = (time.perf_counter() - load_start) * 1000.0
        if copy_result is None:
            raise RuntimeError(f"{table}: COPY did not return an imported row count")
        count = int(copy_result[0])
        row_counts[table] = count
        if count != EXPECTED_ROW_COUNTS[table]:
            raise RuntimeError(
                f"{table}: expected {EXPECTED_ROW_COUNTS[table]} rows, got {count}"
            )
        print(f"loaded {table}: {count:,} rows in {load_times_ms[table]:.1f} ms")

    connection.execute("CHECKPOINT")
    duckdb_version = connection.execute("PRAGMA version").fetchone()
    connection.close()

    atomic_json(
        evidence,
        {
            "created_at": datetime.now(timezone.utc).isoformat(),
            "data_dir": str(data_dir),
            "database": str(database),
            "database_bytes": database.stat().st_size,
            "database_sha256": sha256_file(database),
            "duckdb_python_version": duckdb.__version__,
            "duckdb_engine_version": list(duckdb_version),
            "threads": args.threads,
            "row_counts": row_counts,
            "load_times_ms": load_times_ms,
            "total_setup_ms": (time.perf_counter() - started) * 1000.0,
        },
    )


def csv_cell(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, (date, datetime)):
        return value.isoformat()
    return str(value)


def write_rows(path: Path, headers: tuple[str, ...], rows: list[tuple[Any, ...]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(headers)
        writer.writerows([[csv_cell(value) for value in row] for row in rows])


def quantize_decimal(value: str, places: int) -> str:
    quantum = Decimal(1).scaleb(-places)
    parsed = Decimal(value)
    return format(parsed.quantize(quantum, rounding=ROUND_HALF_UP), f".{places}f")


def canonical_csv(path: Path, query_id: str) -> tuple[str, int, list[list[str]]]:
    with path.open(encoding="utf-8", newline="") as source:
        rows = list(csv.reader(source))
    if not rows:
        raise RuntimeError(f"empty CSV: {path}")

    headers = tuple(cell.strip().lower() for cell in rows[0])
    if headers != EXPECTED_HEADERS[query_id]:
        raise RuntimeError(
            f"{query_id}: header mismatch: expected {EXPECTED_HEADERS[query_id]}, got {headers}"
        )

    places = DECIMAL_PLACES[query_id]
    canonical_rows: list[list[str]] = []
    for row_number, row in enumerate(rows[1:], start=2):
        if len(row) != len(headers):
            raise RuntimeError(
                f"{query_id}: row {row_number} has {len(row)} cells, expected {len(headers)}"
            )
        normalized: list[str] = []
        for header, cell in zip(headers, row):
            cell = cell.strip()
            if header in places:
                normalized.append(quantize_decimal(cell, places[header]))
            else:
                normalized.append(cell)
        canonical_rows.append(normalized)

    payload = json.dumps(
        {"headers": headers, "rows": canonical_rows},
        ensure_ascii=True,
        separators=(",", ":"),
    ).encode()
    return hashlib.sha256(payload).hexdigest(), len(canonical_rows), canonical_rows


def compare_canonical_rows(
    query_id: str,
    expected: list[list[str]],
    actual: list[list[str]],
) -> tuple[bool, list[dict[str, Any]]]:
    if len(expected) != len(actual):
        return False, []

    headers = EXPECTED_HEADERS[query_id]
    places = DECIMAL_PLACES[query_id]
    mismatches: list[dict[str, Any]] = []
    for row_number, (expected_row, actual_row) in enumerate(
        zip(expected, actual), start=1
    ):
        for column, (expected_cell, actual_cell) in enumerate(
            zip(expected_row, actual_row)
        ):
            header = headers[column]
            if header in places:
                tolerance = Decimal("0.01")
                matches = (
                    abs(Decimal(expected_cell) - Decimal(actual_cell)) <= tolerance
                )
            else:
                matches = expected_cell == actual_cell
            if not matches:
                mismatches.append(
                    {
                        "row": row_number,
                        "column": header,
                        "expected": expected_cell,
                        "actual": actual_cell,
                    }
                )
            if len(mismatches) == 3:
                return False, mismatches
    return not mismatches, mismatches


def exact_mismatch_samples(
    query_id: str,
    expected: list[list[str]],
    actual: list[list[str]],
    *,
    comparison: str,
) -> list[dict[str, Any]]:
    if len(expected) != len(actual):
        return [
            {
                "comparison": comparison,
                "row": None,
                "column": "__row_count__",
                "expected": len(expected),
                "actual": len(actual),
            }
        ]

    headers = EXPECTED_HEADERS[query_id]
    mismatches: list[dict[str, Any]] = []
    for row_number, (expected_row, actual_row) in enumerate(
        zip(expected, actual), start=1
    ):
        for column, (expected_cell, actual_cell) in enumerate(
            zip(expected_row, actual_row)
        ):
            if expected_cell == actual_cell:
                continue
            mismatches.append(
                {
                    "comparison": comparison,
                    "row": row_number,
                    "column": headers[column],
                    "expected": expected_cell,
                    "actual": actual_cell,
                }
            )
            if len(mismatches) == 3:
                return mismatches
    return mismatches


def validate_results(
    query_id: str,
    duckdb_path: Path,
    gendb_path: Path,
    reference_path: Path | None = None,
    allow_numeric_tolerance: bool = False,
) -> dict[str, Any]:
    duck_hash, duck_rows, duck_values = canonical_csv(duckdb_path, query_id)
    gendb_hash, gendb_rows, gendb_values = canonical_csv(gendb_path, query_id)
    equivalent, mismatches = compare_canonical_rows(
        query_id, duck_values, gendb_values
    )
    exact_hash = duck_hash == gendb_hash
    exact_mismatches = exact_mismatch_samples(
        query_id,
        duck_values,
        gendb_values,
        comparison="duckdb_vs_gendb",
    )
    result: dict[str, Any] = {
        "status": "pass" if exact_hash or (allow_numeric_tolerance and equivalent) else "fail",
        "comparison": (
            "exact_hash"
            if exact_hash
            else "bounded_numeric"
            if equivalent
            else "mismatch"
        ),
        "bounded_numeric_equivalent": equivalent,
        "rows": {"duckdb": duck_rows, "gendb": gendb_rows},
        "canonical_sha256": {"duckdb": duck_hash, "gendb": gendb_hash},
    }
    if not exact_hash:
        result["mismatch_samples"] = exact_mismatches
        result["bounded_mismatch_samples"] = mismatches
    if result["status"] != "pass":
        if duck_rows != gendb_rows:
            result["error"] = "row count mismatch"
        else:
            result["error"] = "canonical cell mismatch"
    if reference_path is not None:
        reference_hash, reference_rows, reference_values = canonical_csv(
            reference_path, query_id
        )
        result["rows"]["author_reference"] = reference_rows
        result["canonical_sha256"]["author_reference"] = reference_hash
        reference_duckdb, reference_duckdb_mismatches = compare_canonical_rows(
            query_id, reference_values, duck_values
        )
        reference_gendb, reference_gendb_mismatches = compare_canonical_rows(
            query_id, reference_values, gendb_values
        )
        reference_exact = reference_hash == duck_hash == gendb_hash
        reference_equivalent = reference_duckdb and reference_gendb
        reference_exact_mismatches = (
            exact_mismatch_samples(
                query_id,
                reference_values,
                duck_values,
                comparison="author_vs_duckdb",
            )
            + exact_mismatch_samples(
                query_id,
                reference_values,
                gendb_values,
                comparison="author_vs_gendb",
            )
        )[:3]
        result["reference_comparison"] = (
            "exact_hash"
            if reference_exact
            else "bounded_numeric"
            if reference_equivalent
            else "mismatch"
        )
        result["bounded_numeric_equivalent_to_author"] = reference_equivalent
        if not reference_exact:
            result["reference_mismatch_samples"] = reference_exact_mismatches
            result["bounded_reference_mismatch_samples"] = (
                reference_duckdb_mismatches + reference_gendb_mismatches
            )[:3]
        if not reference_exact and not (
            allow_numeric_tolerance and reference_equivalent
        ):
            result["status"] = "fail"
            result["error"] = "author reference hash mismatch"
    return result


def correctness_is_bounded_numeric_only(result: dict[str, Any]) -> bool:
    comparisons = {
        result["comparison"],
        result.get("reference_comparison", "exact_hash"),
    }
    return "mismatch" not in comparisons and "bounded_numeric" in comparisons


def summarize_correctness(
    samples: list[dict[str, Any]],
    *,
    require_exact: bool,
) -> dict[str, Any]:
    exact_samples = sum(
        sample["result"]["comparison"] == "exact_hash"
        and sample["result"].get("reference_comparison", "exact_hash")
        == "exact_hash"
        for sample in samples
    )
    bounded_samples = sum(
        correctness_is_bounded_numeric_only(sample["result"])
        for sample in samples
    )
    failed_samples = len(samples) - exact_samples - bounded_samples
    strict_status = "pass" if exact_samples == len(samples) else "fail"
    tolerant_status = "pass" if failed_samples == 0 else "fail"
    return {
        "status": strict_status if require_exact else tolerant_status,
        "strict_status": strict_status,
        "tolerant_status": tolerant_status,
        "total_samples": len(samples),
        "exact_samples": exact_samples,
        "bounded_numeric_only_samples": bounded_samples,
        "failed_samples": failed_samples,
        "samples": samples,
    }


def parse_gendb_timings(stdout: str) -> dict[str, float]:
    timings = {name.strip(): float(value) for name, value in TIMING_RE.findall(stdout)}
    missing = {"total", "output"} - timings.keys()
    if missing:
        raise RuntimeError(
            f"GenDB output is missing timing phases {sorted(missing)}:\n{stdout}"
        )
    return timings


def run_gendb(
    executable: Path,
    gendb_dir: Path,
    result_dir: Path,
    result_path: Path,
    timeout_seconds: float,
) -> dict[str, Any]:
    result_path.unlink(missing_ok=True)
    started = time.perf_counter()
    process = subprocess.run(
        [str(executable), str(gendb_dir), str(result_dir)],
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout_seconds,
    )
    wall_ms = (time.perf_counter() - started) * 1000.0
    if process.returncode != 0:
        raise RuntimeError(
            f"{executable.name} exited {process.returncode}\n"
            f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}"
        )
    if not result_path.is_file() or result_path.stat().st_size == 0:
        raise RuntimeError(
            f"{executable.name} exited successfully without writing {result_path}"
        )
    return {
        "wall_ms": wall_ms,
        "phases_ms": parse_gendb_timings(process.stdout),
    }


def run_duckdb(
    connection: duckdb.DuckDBPyConnection,
    query_id: str,
    output_path: Path,
) -> dict[str, float]:
    started = time.perf_counter()
    query_started = time.perf_counter()
    cursor = connection.execute(QUERIES[query_id])
    rows = cursor.fetchall()
    query_fetch_ms = (time.perf_counter() - query_started) * 1000.0
    headers = tuple(column[0].lower() for column in cursor.description)
    output_started = time.perf_counter()
    write_rows(output_path, headers, rows)
    output_ms = (time.perf_counter() - output_started) * 1000.0
    return {
        "query_fetch_ms": query_fetch_ms,
        "output_ms": output_ms,
        "end_to_end_ms": (time.perf_counter() - started) * 1000.0,
    }


def distribution(samples: list[float]) -> dict[str, Any]:
    ordered = sorted(samples)
    p95_index = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return {
        "samples_ms": samples,
        "min_ms": min(samples),
        "median_ms": statistics.median(samples),
        "p95_ms": ordered[p95_index],
        "max_ms": max(samples),
    }


def counterbalanced_order(sample: int) -> tuple[str, str]:
    return ("duckdb", "gendb") if sample % 2 == 0 else ("gendb", "duckdb")


def executable_digest(path: Path) -> dict[str, Any]:
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def command_output(command: list[str]) -> str:
    try:
        return subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        return f"unavailable: {error}"


def benchmark(args: argparse.Namespace) -> None:
    if args.runs < 2 or args.runs % 2 != 0:
        raise ValueError("--runs must be a positive even number for AB/BA blocks")

    database = args.database.resolve()
    gendb_dir = args.gendb_dir.resolve()
    bin_dir = args.bin_dir.resolve()
    reference_dir = args.reference_dir.resolve()
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    duckdb_results = output_dir / "results" / "duckdb"
    gendb_results = output_dir / "results" / "gendb"
    duckdb_results.mkdir(parents=True, exist_ok=True)
    gendb_results.mkdir(parents=True, exist_ok=True)

    connection = duckdb.connect(str(database), read_only=True)
    connection.execute(f"SET threads = {args.threads}")

    raw: dict[str, Any] = {}
    for query_id in QUERY_IDS:
        executable = bin_dir / query_id.lower()
        if not executable.is_file():
            raise FileNotFoundError(executable)
        duckdb_csv = duckdb_results / f"{query_id}.csv"
        gendb_csv = gendb_results / f"{query_id}.csv"

        for _ in range(args.warmups):
            run_duckdb(connection, query_id, duckdb_csv)
            run_gendb(
                executable,
                gendb_dir,
                gendb_results,
                gendb_csv,
                args.timeout,
            )

        correctness_samples: list[dict[str, Any]] = []
        correctness = validate_results(
            query_id,
            duckdb_csv,
            gendb_csv,
            reference_dir / f"{query_id}.csv",
        )
        correctness_samples.append({"phase": "post_warmup", "result": correctness})
        if correctness["status"] != "pass":
            if not (
                args.record_correctness_failures
                and correctness_is_bounded_numeric_only(correctness)
            ):
                raise RuntimeError(
                    f"{query_id} correctness failed:\n"
                    f"{json.dumps(correctness, indent=2)}"
                )
            print(f"{query_id}: recording bounded-numeric correctness failure")

        duck_samples: list[dict[str, Any]] = []
        gendb_samples: list[dict[str, Any]] = []
        execution_orders: list[str] = []
        for sample in range(args.runs):
            order = counterbalanced_order(sample)
            execution_orders.append("_then_".join(order))
            for position, system in enumerate(order, start=1):
                if system == "duckdb":
                    # Give each timed execution the same immediately preceding
                    # workload to reduce frequency and cache-position effects.
                    run_duckdb(connection, query_id, duckdb_csv)
                    measured = run_duckdb(connection, query_id, duckdb_csv)
                    measured["position"] = position
                    duck_samples.append(measured)
                else:
                    run_gendb(
                        executable,
                        gendb_dir,
                        gendb_results,
                        gendb_csv,
                        args.timeout,
                    )
                    measured = run_gendb(
                        executable,
                        gendb_dir,
                        gendb_results,
                        gendb_csv,
                        args.timeout,
                    )
                    measured["position"] = position
                    gendb_samples.append(measured)

            correctness = validate_results(
                query_id,
                duckdb_csv,
                gendb_csv,
                reference_dir / f"{query_id}.csv",
            )
            correctness_samples.append(
                {"phase": "measured", "sample": sample, "result": correctness}
            )
            if correctness["status"] != "pass":
                if not (
                    args.record_correctness_failures
                    and correctness_is_bounded_numeric_only(correctness)
                ):
                    raise RuntimeError(
                        f"{query_id}: sample {sample} failed correctness:\n"
                        f"{json.dumps(correctness, indent=2)}"
                    )
                print(
                    f"{query_id}: sample {sample} recording bounded-numeric "
                    "correctness failure"
                )

        raw[query_id] = {
            "correctness": summarize_correctness(
                correctness_samples,
                require_exact=True,
            ),
            "execution_orders": execution_orders,
            "duckdb": {
                "query_fetch": distribution(
                    [sample["query_fetch_ms"] for sample in duck_samples]
                ),
                "output": distribution(
                    [sample["output_ms"] for sample in duck_samples]
                ),
                "end_to_end": distribution(
                    [sample["end_to_end_ms"] for sample in duck_samples]
                ),
                "end_to_end_by_position": {
                    str(position): distribution(
                        [
                            sample["end_to_end_ms"]
                            for sample in duck_samples
                            if sample["position"] == position
                        ]
                    )
                    for position in (1, 2)
                },
            },
            "gendb": {
                "internal_total": distribution(
                    [sample["phases_ms"]["total"] for sample in gendb_samples]
                ),
                "internal_output": distribution(
                    [sample["phases_ms"]["output"] for sample in gendb_samples]
                ),
                "process_wall": distribution(
                    [sample["wall_ms"] for sample in gendb_samples]
                ),
                "internal_total_by_position": {
                    str(position): distribution(
                        [
                            sample["phases_ms"]["total"]
                            for sample in gendb_samples
                            if sample["position"] == position
                        ]
                    )
                    for position in (1, 2)
                },
                "phase_samples_ms": [
                    sample["phases_ms"] for sample in gendb_samples
                ],
            },
        }
        duck_median = raw[query_id]["duckdb"]["end_to_end"]["median_ms"]
        gendb_median = raw[query_id]["gendb"]["internal_total"]["median_ms"]
        raw[query_id]["normalized_end_to_end_speedup"] = (
            duck_median / gendb_median
        )
        raw[query_id]["process_wall_speedup"] = (
            duck_median
            / raw[query_id]["gendb"]["process_wall"]["median_ms"]
        )
        print(
            f"{query_id}: DuckDB {duck_median:.2f} ms, "
            f"GenDB {gendb_median:.2f} ms, "
            f"speedup {raw[query_id]['normalized_end_to_end_speedup']:.2f}x"
        )

    connection.close()

    duckdb_sum = sum(
        raw[query_id]["duckdb"]["end_to_end"]["median_ms"]
        for query_id in QUERY_IDS
    )
    gendb_sum = sum(
        raw[query_id]["gendb"]["internal_total"]["median_ms"]
        for query_id in QUERY_IDS
    )
    gendb_wall_sum = sum(
        raw[query_id]["gendb"]["process_wall"]["median_ms"]
        for query_id in QUERY_IDS
    )

    binaries = {
        query_id: executable_digest(bin_dir / query_id.lower())
        for query_id in QUERY_IDS
    }
    correctness_passed = all(
        query["correctness"]["status"] == "pass" for query in raw.values()
    )
    result = {
        "schema_version": 3,
        "status": "pass" if correctness_passed else "correctness_failures_recorded",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "methodology": {
            "workload": "TPC-H SF10 selected Q1/Q3/Q6/Q9/Q18",
            "warmups": args.warmups,
            "measured_runs": args.runs,
            "threads": args.threads,
            "query_order": (
                "per query; systems use counterbalanced AB/BA blocks; each timed "
                "execution immediately follows an untimed run of the same system"
            ),
            "primary_comparison": (
                "DuckDB execute+fetch+CSV write vs GenDB internal total including CSV output"
            ),
            "secondary_comparison": (
                "DuckDB execute+fetch+CSV write vs GenDB process wall"
            ),
            "correctness": (
                "strict canonical hash after normalizing numeric values to each "
                "generated program's emitted precision; record-only mode may retain "
                "timings for bounded one-cent numeric failures without admitting them"
            ),
        },
        "environment": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "duckdb_python": duckdb.__version__,
            "affinity": (
                sorted(os.sched_getaffinity(0))
                if hasattr(os, "sched_getaffinity")
                else "unavailable"
            ),
            "omp_num_threads": os.environ.get("OMP_NUM_THREADS"),
            "omp_proc_bind": os.environ.get("OMP_PROC_BIND"),
            "omp_places": os.environ.get("OMP_PLACES"),
            "lscpu": command_output(["lscpu"]),
            "compiler": command_output(["g++", "--version"]).splitlines()[0],
        },
        "inputs": {
            "database": executable_digest(database),
            "gendb_dir": str(gendb_dir),
            "gendb_commit": args.gendb_commit,
            "dbgen_commit": args.dbgen_commit,
            "compile_flags": args.compile_flags,
            "binaries": binaries,
        },
        "queries": raw,
        "aggregate": {
            "duckdb_sum_of_medians_ms": duckdb_sum,
            "gendb_internal_sum_of_medians_ms": gendb_sum,
            "gendb_process_wall_sum_of_medians_ms": gendb_wall_sum,
            "normalized_end_to_end_speedup": duckdb_sum / gendb_sum,
            "process_wall_speedup": duckdb_sum / gendb_wall_sum,
        },
    }
    atomic_json(output_dir / "benchmark-results.json", result)


def benchmark_ablation(args: argparse.Namespace) -> None:
    database = args.database.resolve()
    gendb_dir = args.gendb_dir.resolve()
    bin_dir = args.bin_dir.resolve()
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    duckdb_results = output_dir / "results" / "duckdb"
    gendb_results = output_dir / "results" / "gendb"

    connection = duckdb.connect(str(database), read_only=True)
    connection.execute(f"SET threads = {args.threads}")

    results: dict[str, Any] = {}
    for query_id in QUERY_IDS:
        duckdb_csv = duckdb_results / f"{query_id}.csv"
        run_duckdb(connection, query_id, duckdb_csv)
        variants: dict[str, Any] = {}
        for executable in sorted((bin_dir / query_id).glob("iter_*")):
            variant_results = gendb_results / query_id / executable.name
            variant_results.mkdir(parents=True, exist_ok=True)
            gendb_csv = variant_results / f"{query_id}.csv"
            for _ in range(args.warmups):
                run_gendb(
                    executable,
                    gendb_dir,
                    variant_results,
                    gendb_csv,
                    args.timeout,
                )

            correctness_samples: list[dict[str, Any]] = []
            correctness = validate_results(
                query_id,
                duckdb_csv,
                gendb_csv,
                allow_numeric_tolerance=True,
            )
            correctness_samples.append(
                {"phase": "post_warmup", "result": correctness}
            )
            if correctness["status"] != "pass":
                raise RuntimeError(
                    f"{query_id}/{executable.name} correctness failed:\n"
                    f"{json.dumps(correctness, indent=2)}"
                )

            samples = []
            for sample in range(args.runs):
                samples.append(
                    run_gendb(
                        executable,
                        gendb_dir,
                        variant_results,
                        gendb_csv,
                        args.timeout,
                    )
                )
                correctness = validate_results(
                    query_id,
                    duckdb_csv,
                    gendb_csv,
                    allow_numeric_tolerance=True,
                )
                correctness_samples.append(
                    {"phase": "measured", "sample": sample, "result": correctness}
                )
                if correctness["status"] != "pass":
                    raise RuntimeError(
                        f"{query_id}/{executable.name}: sample {sample} "
                        "failed correctness"
                    )
            variants[executable.name] = {
                "correctness": summarize_correctness(
                    correctness_samples,
                    require_exact=False,
                ),
                "internal_total": distribution(
                    [sample["phases_ms"]["total"] for sample in samples]
                ),
                "process_wall": distribution(
                    [sample["wall_ms"] for sample in samples]
                ),
                "phase_samples_ms": [sample["phases_ms"] for sample in samples],
                "binary": executable_digest(executable),
            }
            print(
                f"{query_id}/{executable.name}: "
                f"{variants[executable.name]['internal_total']['median_ms']:.2f} ms"
            )

        first = variants["iter_0"]["internal_total"]["median_ms"]
        best_name = BEST_ITERATIONS[query_id]
        best = variants[best_name]["internal_total"]["median_ms"]
        results[query_id] = {
            "best_iteration": best_name,
            "iter0_to_best_speedup": first / best,
            "variants": variants,
        }

    connection.close()
    atomic_json(
        output_dir / "ablation-results.json",
        {
            "schema_version": 2,
            "status": (
                "pass"
                if all(
                    variant["correctness"]["strict_status"] == "pass"
                    for query in results.values()
                    for variant in query["variants"].values()
                )
                else "bounded_numeric_failures_recorded"
            ),
            "created_at": datetime.now(timezone.utc).isoformat(),
            "methodology": {
                "workload": "TPC-H SF10 selected Q1/Q3/Q6/Q9/Q18",
                "warmups": args.warmups,
                "measured_runs": args.runs,
                "threads": args.threads,
                "comparison": "GenDB iteration 0 vs author-selected best iteration",
                "correctness_oracle": "DuckDB output under the same canonical comparator",
            },
            "queries": results,
        },
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    setup = subparsers.add_parser("setup-duckdb")
    setup.add_argument("--data-dir", type=Path, required=True)
    setup.add_argument("--database", type=Path, required=True)
    setup.add_argument("--schema", type=Path, required=True)
    setup.add_argument("--evidence", type=Path, required=True)
    setup.add_argument("--threads", type=int, default=64)
    setup.add_argument("--force", action="store_true")
    setup.set_defaults(function=setup_duckdb)

    run = subparsers.add_parser("run")
    run.add_argument("--database", type=Path, required=True)
    run.add_argument("--gendb-dir", type=Path, required=True)
    run.add_argument("--bin-dir", type=Path, required=True)
    run.add_argument("--reference-dir", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--threads", type=int, default=64)
    run.add_argument("--warmups", type=int, default=1)
    run.add_argument("--runs", type=int, default=10)
    run.add_argument("--timeout", type=float, default=300.0)
    run.add_argument("--gendb-commit", required=True)
    run.add_argument("--dbgen-commit", required=True)
    run.add_argument("--compile-flags", required=True)
    run.add_argument("--record-correctness-failures", action="store_true")
    run.set_defaults(function=benchmark)

    ablation = subparsers.add_parser("ablation")
    ablation.add_argument("--database", type=Path, required=True)
    ablation.add_argument("--gendb-dir", type=Path, required=True)
    ablation.add_argument("--bin-dir", type=Path, required=True)
    ablation.add_argument("--output", type=Path, required=True)
    ablation.add_argument("--threads", type=int, default=64)
    ablation.add_argument("--warmups", type=int, default=1)
    ablation.add_argument("--runs", type=int, default=10)
    ablation.add_argument("--timeout", type=float, default=300.0)
    ablation.set_defaults(function=benchmark_ablation)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    args.function(args)


if __name__ == "__main__":
    main()
