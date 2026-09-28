#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("tpch_bench.py")
DUCKDB_STUB = types.ModuleType("duckdb")
DUCKDB_STUB.DuckDBPyConnection = object
DUCKDB_STUB.__version__ = "test"
sys.modules.setdefault("duckdb", DUCKDB_STUB)

SPEC = importlib.util.spec_from_file_location("tpch_bench", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
tpch_bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tpch_bench)


def write_q6(path: Path, revenue: str) -> None:
    path.write_text(f"revenue\n{revenue}\n", encoding="utf-8")


def write_q3(path: Path, rows: list[tuple[str, str, str, str]]) -> None:
    lines = ["l_orderkey,revenue,o_orderdate,o_shippriority"]
    lines.extend(",".join(row) for row in rows)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class CorrectnessContractTest(unittest.TestCase):
    def test_counterbalanced_order(self) -> None:
        self.assertEqual(
            tpch_bench.counterbalanced_order(0),
            ("duckdb", "gendb"),
        )
        self.assertEqual(
            tpch_bench.counterbalanced_order(1),
            ("gendb", "duckdb"),
        )

    def test_selected_binary_requires_exact_canonical_hash(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duckdb_csv = root / "duckdb.csv"
            gendb_csv = root / "gendb.csv"
            write_q6(duckdb_csv, "123.456")
            write_q6(gendb_csv, "123.465")

            selected = tpch_bench.validate_results(
                "Q6", duckdb_csv, gendb_csv
            )
            legacy = tpch_bench.validate_results(
                "Q6",
                duckdb_csv,
                gendb_csv,
                allow_numeric_tolerance=True,
            )

            self.assertEqual(selected["status"], "fail")
            self.assertEqual(selected["comparison"], "bounded_numeric")
            self.assertTrue(
                tpch_bench.correctness_is_bounded_numeric_only(selected)
            )
            self.assertEqual(
                selected["mismatch_samples"],
                [
                    {
                        "comparison": "duckdb_vs_gendb",
                        "row": 1,
                        "column": "revenue",
                        "expected": "123.46",
                        "actual": "123.47",
                    }
                ],
            )
            self.assertEqual(selected["bounded_mismatch_samples"], [])
            self.assertEqual(legacy["status"], "pass")
            self.assertEqual(legacy["comparison"], "bounded_numeric")
            self.assertEqual(
                legacy["mismatch_samples"],
                selected["mismatch_samples"],
            )

    def test_legacy_tolerance_is_one_cent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duckdb_csv = root / "duckdb.csv"
            gendb_csv = root / "gendb.csv"
            write_q6(duckdb_csv, "123.456")
            write_q6(gendb_csv, "123.475")

            result = tpch_bench.validate_results(
                "Q6",
                duckdb_csv,
                gendb_csv,
                allow_numeric_tolerance=True,
            )

            self.assertEqual(result["status"], "fail")

    def test_structural_mismatches_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duckdb_csv = root / "duckdb.csv"
            gendb_csv = root / "gendb.csv"
            write_q6(duckdb_csv, "123.45")
            gendb_csv.write_text(
                "revenue\n123.45\n123.45\n",
                encoding="utf-8",
            )

            result = tpch_bench.validate_results(
                "Q6",
                duckdb_csv,
                gendb_csv,
                allow_numeric_tolerance=True,
            )

            self.assertEqual(result["status"], "fail")
            self.assertEqual(result["comparison"], "mismatch")
            self.assertEqual(
                result["mismatch_samples"][0]["column"],
                "__row_count__",
            )

            gendb_csv.write_text("wrong_header\n123.45\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "header mismatch"):
                tpch_bench.validate_results(
                    "Q6",
                    duckdb_csv,
                    gendb_csv,
                    allow_numeric_tolerance=True,
                )

            gendb_csv.write_text("revenue\n123.45,extra\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "2 cells"):
                tpch_bench.validate_results(
                    "Q6",
                    duckdb_csv,
                    gendb_csv,
                    allow_numeric_tolerance=True,
                )

            gendb_csv.write_text("", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "empty CSV"):
                tpch_bench.validate_results(
                    "Q6",
                    duckdb_csv,
                    gendb_csv,
                    allow_numeric_tolerance=True,
                )

    def test_row_order_and_nonnumeric_fields_are_exact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duckdb_csv = root / "duckdb.csv"
            gendb_csv = root / "gendb.csv"
            rows = [
                ("1", "10.00", "1995-01-01", "0"),
                ("2", "9.00", "1995-01-02", "0"),
            ]
            write_q3(duckdb_csv, rows)
            write_q3(gendb_csv, list(reversed(rows)))

            reordered = tpch_bench.validate_results(
                "Q3",
                duckdb_csv,
                gendb_csv,
                allow_numeric_tolerance=True,
            )
            self.assertEqual(reordered["status"], "fail")
            self.assertEqual(reordered["comparison"], "mismatch")
            self.assertEqual(
                reordered["mismatch_samples"][0]["column"],
                "l_orderkey",
            )

            changed_date = [rows[0], ("2", "9.00", "1995-02-02", "0")]
            write_q3(gendb_csv, changed_date)
            nonnumeric = tpch_bench.validate_results(
                "Q3",
                duckdb_csv,
                gendb_csv,
                allow_numeric_tolerance=True,
            )
            self.assertEqual(nonnumeric["status"], "fail")
            self.assertEqual(nonnumeric["comparison"], "mismatch")
            self.assertEqual(
                nonnumeric["mismatch_samples"][0]["column"],
                "o_orderdate",
            )

    def test_author_reference_mismatch_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duckdb_csv = root / "duckdb.csv"
            gendb_csv = root / "gendb.csv"
            reference_csv = root / "reference.csv"
            write_q6(duckdb_csv, "123.45")
            write_q6(gendb_csv, "123.45")
            write_q6(reference_csv, "999.99")

            result = tpch_bench.validate_results(
                "Q6",
                duckdb_csv,
                gendb_csv,
                reference_csv,
            )

            self.assertEqual(result["status"], "fail")
            self.assertEqual(result["reference_comparison"], "mismatch")
            self.assertEqual(
                {
                    sample["comparison"]
                    for sample in result["reference_mismatch_samples"]
                },
                {"author_vs_duckdb", "author_vs_gendb"},
            )

    def test_correctness_summary_preserves_bounded_samples(self) -> None:
        exact = {
            "status": "pass",
            "comparison": "exact_hash",
            "reference_comparison": "exact_hash",
        }
        bounded = {
            "status": "pass",
            "comparison": "bounded_numeric",
            "reference_comparison": "bounded_numeric",
        }
        samples = [
            {"phase": "post_warmup", "result": exact},
            {"phase": "measured", "sample": 0, "result": bounded},
            {"phase": "measured", "sample": 1, "result": exact},
        ]

        strict = tpch_bench.summarize_correctness(samples, require_exact=True)
        diagnostic = tpch_bench.summarize_correctness(
            samples,
            require_exact=False,
        )

        self.assertEqual(strict["status"], "fail")
        self.assertEqual(diagnostic["status"], "pass")
        self.assertEqual(strict["strict_status"], "fail")
        self.assertEqual(strict["tolerant_status"], "pass")
        self.assertEqual(strict["exact_samples"], 2)
        self.assertEqual(strict["bounded_numeric_only_samples"], 1)
        self.assertEqual(strict["failed_samples"], 0)

    def test_correctness_summary_rejects_structural_failure(self) -> None:
        mismatch = {
            "status": "fail",
            "comparison": "mismatch",
            "reference_comparison": "mismatch",
        }
        summary = tpch_bench.summarize_correctness(
            [{"phase": "measured", "sample": 0, "result": mismatch}],
            require_exact=False,
        )

        self.assertEqual(summary["status"], "fail")
        self.assertEqual(summary["failed_samples"], 1)

    def test_missing_output_phase_is_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "output"):
            tpch_bench.parse_gendb_timings("[TIMING] total: 1.00 ms\n")

    def test_success_without_new_result_does_not_reuse_stale_csv(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "no-output"
            executable.write_text(
                "#!/bin/sh\n"
                "printf '[TIMING] output: 0.10 ms\\n'\n"
                "printf '[TIMING] total: 1.00 ms\\n'\n",
                encoding="utf-8",
            )
            executable.chmod(0o755)
            result_path = root / "results" / "Q6.csv"
            result_path.parent.mkdir()
            write_q6(result_path, "123.45")

            with self.assertRaisesRegex(RuntimeError, "without writing"):
                tpch_bench.run_gendb(
                    executable,
                    root,
                    result_path.parent,
                    result_path,
                    5.0,
                )
            self.assertFalse(result_path.exists())

    def test_benchmark_records_every_correctness_sample(self) -> None:
        class Connection:
            def execute(self, _query: str) -> "Connection":
                return self

            def close(self) -> None:
                return None

        exact = {
            "status": "pass",
            "comparison": "exact_hash",
            "reference_comparison": "exact_hash",
        }
        bounded = {
            "status": "fail",
            "comparison": "bounded_numeric",
            "reference_comparison": "bounded_numeric",
        }

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "tpch.duckdb"
            database.write_bytes(b"db")
            gendb_dir = root / "gendb"
            bin_dir = root / "bin"
            reference_dir = root / "reference"
            output = root / "output"
            for path in (gendb_dir, bin_dir, reference_dir):
                path.mkdir()
            for query_id in tpch_bench.QUERY_IDS:
                executable = bin_dir / query_id.lower()
                executable.write_bytes(query_id.encode())

            validation_calls: dict[str, int] = {
                query_id: 0 for query_id in tpch_bench.QUERY_IDS
            }

            def validate(query_id: str, *_args: object, **_kwargs: object) -> dict:
                validation_calls[query_id] += 1
                if query_id == "Q3" and validation_calls[query_id] == 2:
                    return bounded
                return exact

            captured: dict[str, object] = {}

            def capture(_path: Path, value: object) -> None:
                captured["result"] = value

            args = types.SimpleNamespace(
                database=database,
                gendb_dir=gendb_dir,
                bin_dir=bin_dir,
                reference_dir=reference_dir,
                output=output,
                threads=64,
                warmups=1,
                runs=2,
                timeout=1.0,
                gendb_commit="gendb",
                dbgen_commit="dbgen",
                compile_flags="-O3",
                record_correctness_failures=True,
            )
            with (
                mock.patch.object(
                    tpch_bench.duckdb,
                    "connect",
                    return_value=Connection(),
                    create=True,
                ),
                mock.patch.object(
                    tpch_bench,
                    "run_duckdb",
                    side_effect=lambda *_args, **_kwargs: {
                        "query_fetch_ms": 1.0,
                        "output_ms": 0.5,
                        "end_to_end_ms": 1.5,
                    },
                ),
                mock.patch.object(
                    tpch_bench,
                    "run_gendb",
                    side_effect=lambda *_args, **_kwargs: {
                        "wall_ms": 3.0,
                        "phases_ms": {"total": 2.0, "output": 0.5},
                    },
                ),
                mock.patch.object(
                    tpch_bench,
                    "validate_results",
                    side_effect=validate,
                ),
                mock.patch.object(
                    tpch_bench,
                    "command_output",
                    return_value="test",
                ),
                mock.patch.object(
                    tpch_bench,
                    "atomic_json",
                    side_effect=capture,
                ),
            ):
                tpch_bench.benchmark(args)

            result = captured["result"]
            assert isinstance(result, dict)
            self.assertEqual(result["status"], "correctness_failures_recorded")
            for query_id in tpch_bench.QUERY_IDS:
                self.assertEqual(validation_calls[query_id], 3)
                self.assertEqual(
                    result["queries"][query_id]["correctness"]["total_samples"],
                    3,
                )
            self.assertEqual(
                result["queries"]["Q3"]["correctness"][
                    "bounded_numeric_only_samples"
                ],
                1,
            )
            self.assertEqual(
                result["queries"]["Q3"]["execution_orders"],
                ["duckdb_then_gendb", "gendb_then_duckdb"],
            )

    def test_ablation_records_every_correctness_sample(self) -> None:
        class Connection:
            def execute(self, _query: str) -> "Connection":
                return self

            def close(self) -> None:
                return None

        exact = {"status": "pass", "comparison": "exact_hash"}
        bounded = {"status": "pass", "comparison": "bounded_numeric"}

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "tpch.duckdb"
            database.write_bytes(b"db")
            gendb_dir = root / "gendb"
            bin_dir = root / "bin"
            output = root / "output"
            (output / "results" / "duckdb").mkdir(parents=True)
            gendb_dir.mkdir()
            for query_id, best in tpch_bench.BEST_ITERATIONS.items():
                query_dir = bin_dir / query_id
                query_dir.mkdir(parents=True)
                for iteration in {"iter_0", best}:
                    (query_dir / iteration).write_bytes(iteration.encode())

            validation_calls: dict[tuple[str, str], int] = {}

            def validate(
                query_id: str,
                _duckdb_path: Path,
                gendb_path: Path,
                **_kwargs: object,
            ) -> dict:
                key = (query_id, gendb_path.parent.name)
                validation_calls[key] = validation_calls.get(key, 0) + 1
                if key == ("Q3", "iter_1") and validation_calls[key] == 2:
                    return bounded
                return exact

            captured: dict[str, object] = {}

            def capture(_path: Path, value: object) -> None:
                captured["result"] = value

            args = types.SimpleNamespace(
                database=database,
                gendb_dir=gendb_dir,
                bin_dir=bin_dir,
                output=output,
                threads=64,
                warmups=1,
                runs=2,
                timeout=1.0,
            )
            with (
                mock.patch.object(
                    tpch_bench.duckdb,
                    "connect",
                    return_value=Connection(),
                    create=True,
                ),
                mock.patch.object(tpch_bench, "run_duckdb"),
                mock.patch.object(
                    tpch_bench,
                    "run_gendb",
                    side_effect=lambda *_args, **_kwargs: {
                        "wall_ms": 3.0,
                        "phases_ms": {"total": 2.0, "output": 0.5},
                    },
                ),
                mock.patch.object(
                    tpch_bench,
                    "validate_results",
                    side_effect=validate,
                ),
                mock.patch.object(
                    tpch_bench,
                    "atomic_json",
                    side_effect=capture,
                ),
            ):
                tpch_bench.benchmark_ablation(args)

            result = captured["result"]
            assert isinstance(result, dict)
            self.assertEqual(result["status"], "bounded_numeric_failures_recorded")
            for query in result["queries"].values():
                for variant in query["variants"].values():
                    self.assertEqual(
                        variant["correctness"]["total_samples"],
                        3,
                    )
            self.assertEqual(
                result["queries"]["Q3"]["variants"]["iter_1"]["correctness"][
                    "bounded_numeric_only_samples"
                ],
                1,
            )


if __name__ == "__main__":
    unittest.main()
