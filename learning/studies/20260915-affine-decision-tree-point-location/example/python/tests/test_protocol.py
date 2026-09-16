from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import shutil
import subprocess
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

import generate_dataset
import point_location_oracle


PYTHON_ROOT = Path(__file__).resolve().parents[1]
EXAMPLE_ROOT = PYTHON_ROOT.parent
DATA_ROOT = EXAMPLE_ROOT / "data"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class GeneratedDatasetTest(unittest.TestCase):
    def test_committed_dataset_is_canonical_and_reproducible(self) -> None:
        with (
            tempfile.TemporaryDirectory() as first_text,
            tempfile.TemporaryDirectory() as second_text,
        ):
            first = Path(first_text)
            second = Path(second_text)
            first_counts = generate_dataset.generate(first)
            second_counts = generate_dataset.generate(second)

            self.assertEqual(first_counts, second_counts)
            self.assertEqual(first_counts["sites"], 1000)
            self.assertEqual(
                (first / "sites.csv").read_bytes(),
                (second / "sites.csv").read_bytes(),
            )
            self.assertEqual(
                (first / "queries.csv").read_bytes(), (second / "queries.csv").read_bytes()
            )
            self.assertEqual(
                (first / "sites.csv").read_bytes(), (DATA_ROOT / "sites.csv").read_bytes()
            )
            self.assertEqual(
                (first / "queries.csv").read_bytes(), (DATA_ROOT / "queries.csv").read_bytes()
            )

    def test_sites_are_unique_nonuniform_ordinal_binary64_values(self) -> None:
        with (DATA_ROOT / "sites.csv").open(newline="", encoding="ascii") as stream:
            rows = list(csv.DictReader(stream))

        self.assertEqual(
            list(rows[0]),
            ["ordinal", "region_id", "x_hex", "y_hex"],
        )
        self.assertEqual(len(rows), 1000)
        coordinates: set[tuple[float, float]] = set()
        x_bands = [0, 0, 0, 0]
        y_bands = [0, 0, 0, 0]
        for ordinal, row in enumerate(rows):
            self.assertEqual(int(row["ordinal"]), ordinal)
            self.assertEqual(int(row["region_id"]), ordinal)
            x = float.fromhex(row["x_hex"])
            y = float.fromhex(row["y_hex"])
            self.assertEqual(x.hex(), row["x_hex"])
            self.assertEqual(y.hex(), row["y_hex"])
            self.assertTrue(0.0 <= x <= 12.0)
            self.assertTrue(0.0 <= y <= 8.0)
            coordinates.add((x, y))
            x_bands[min(int(x / 3.0), 3)] += 1
            y_bands[min(int(y / 2.0), 3)] += 1

        self.assertEqual(len(coordinates), 1000)
        self.assertGreater(max(x_bands) - min(x_bands), 100)
        self.assertGreater(max(y_bands) - min(y_bands), 100)

    def test_queries_cover_every_protocol_class(self) -> None:
        with (DATA_ROOT / "sites.csv").open(newline="", encoding="ascii") as stream:
            sites = list(csv.DictReader(stream))
        with (DATA_ROOT / "queries.csv").open(newline="", encoding="ascii") as stream:
            rows = list(csv.DictReader(stream))

        self.assertEqual(
            list(rows[0]),
            [
                "query_id",
                "class",
                "x_hex",
                "y_hex",
                "site_a",
                "site_b",
                "expected_kind",
                "expected_status",
                "expected_region_id",
            ],
        )
        classes: dict[str, list[dict[str, str]]] = {}
        for query_id, row in enumerate(rows):
            self.assertEqual(int(row["query_id"]), query_id)
            classes.setdefault(row["class"], []).append(row)
            for key in ("expected_kind", "expected_status", "expected_region_id"):
                self.assertEqual(row[key], "")

        self.assertEqual(set(classes), set(generate_dataset.QUERY_CLASSES))
        self.assertEqual(len(classes["site"]), 1000)
        self.assertGreaterEqual(len(classes["random"]), 2000)
        self.assertGreaterEqual(len(classes["exact_bisector"]), 32)
        self.assertGreaterEqual(len(classes["nonfinite"]), 6)

        site_points = {(row["x_hex"], row["y_hex"]) for row in sites}
        query_site_points = {(row["x_hex"], row["y_hex"]) for row in classes["site"]}
        self.assertEqual(query_site_points, site_points)

        boundary_points = {
            (float.fromhex(row["x_hex"]), float.fromhex(row["y_hex"]))
            for row in classes["boundary"]
        }
        self.assertTrue(any(x == 0.0 for x, _ in boundary_points))
        self.assertTrue(any(x == 12.0 for x, _ in boundary_points))
        self.assertTrue(any(y == 0.0 for _, y in boundary_points))
        self.assertTrue(any(y == 8.0 for _, y in boundary_points))

        outside_points = [
            (float.fromhex(row["x_hex"]), float.fromhex(row["y_hex"]))
            for row in classes["outside"]
        ]
        self.assertTrue(any(x == math.nextafter(0.0, -math.inf) for x, _ in outside_points))
        self.assertTrue(any(x == math.nextafter(12.0, math.inf) for x, _ in outside_points))
        self.assertTrue(any(y == math.nextafter(0.0, -math.inf) for _, y in outside_points))
        self.assertTrue(any(y == math.nextafter(8.0, math.inf) for _, y in outside_points))

        site_values = [
            (Fraction.from_float(float.fromhex(row["x_hex"])),
             Fraction.from_float(float.fromhex(row["y_hex"])))
            for row in sites
        ]
        for row in classes["exact_bisector"]:
            point_x = Fraction.from_float(float.fromhex(row["x_hex"]))
            point_y = Fraction.from_float(float.fromhex(row["y_hex"]))
            first = site_values[int(row["site_a"])]
            second = site_values[int(row["site_b"])]
            first_distance = (point_x - first[0]) ** 2 + (point_y - first[1]) ** 2
            second_distance = (point_x - second[0]) ** 2 + (point_y - second[1]) ** 2
            self.assertEqual(first_distance, second_distance)

    def test_committed_digests_are_pinned(self) -> None:
        self.assertEqual(sha256(DATA_ROOT / "sites.csv"), generate_dataset.SITES_SHA256)
        self.assertEqual(sha256(DATA_ROOT / "queries.csv"), generate_dataset.QUERIES_SHA256)


@unittest.skipUnless(os.environ.get("ODT_EXAMPLE_BIN"), "ODT_EXAMPLE_BIN is required")
class CProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.binary = Path(os.environ["ODT_EXAMPLE_BIN"]).resolve()

    def run_example(
        self,
        work: Path,
        *,
        mode: str,
        queries: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(self.binary),
                "--mode",
                mode,
                "--sites",
                str(DATA_ROOT / "sites.csv"),
                "--queries",
                str(queries or DATA_ROOT / "queries.csv"),
                "--results",
                str(work / "results.jsonl"),
                "--summary",
                str(work / "summary.json"),
                "--snapshot",
                str(work / "generation.odt"),
            ],
            text=True,
            capture_output=True,
        )

    def test_machine_protocol_and_full_differential(self) -> None:
        with tempfile.TemporaryDirectory() as work_text:
            work = Path(work_text)
            completed = self.run_example(work, mode="machine")
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stderr, "")
            stdout_summary = json.loads(completed.stdout)
            file_summary = json.loads((work / "summary.json").read_text(encoding="ascii"))
            self.assertEqual(stdout_summary, file_summary)
            self.assertEqual(file_summary["schema"], "odt-example-summary/v1")
            self.assertEqual(file_summary["inputs"]["site_count"], 1000)
            self.assertEqual(file_summary["mismatches"]["total"], 0)

            result_rows = [
                json.loads(line)
                for line in (work / "results.jsonl").read_text(encoding="ascii").splitlines()
            ]
            self.assertEqual(len(result_rows), file_summary["inputs"]["query_count"])
            self.assertTrue(result_rows)
            self.assertEqual(result_rows[0]["schema"], "odt-example-result/v1")
            self.assertEqual(
                set(result_rows[0]),
                {
                    "schema",
                    "query_id",
                    "class",
                    "x_hex",
                    "y_hex",
                    "scalar",
                    "batch",
                    "restored",
                },
            )

            oracle = subprocess.run(
                [
                    os.fspath(Path(os.environ.get("PYTHON", os.sys.executable))),
                    os.fspath(PYTHON_ROOT / "point_location_oracle.py"),
                    "--sites",
                    os.fspath(DATA_ROOT / "sites.csv"),
                    "--queries",
                    os.fspath(DATA_ROOT / "queries.csv"),
                    "--results",
                    os.fspath(work / "results.jsonl"),
                    "--summary",
                    os.fspath(work / "summary.json"),
                ],
                text=True,
                capture_output=True,
            )
            self.assertEqual(oracle.returncode, 0, oracle.stderr)
            self.assertIn("mismatches=0", oracle.stdout)
            self.assertIn("representative rows:", oracle.stdout)

    def test_human_mode_is_visible_without_jsonl_noise(self) -> None:
        with tempfile.TemporaryDirectory() as work_text:
            completed = self.run_example(Path(work_text), mode="human")
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("Oblique Decision Tree example", completed.stdout)
            self.assertIn("tree:", completed.stdout)
            self.assertIn("result totals:", completed.stdout)
            self.assertIn("representative rows:", completed.stdout)
            self.assertIn("artifacts:", completed.stdout)
            self.assertNotIn('"schema":"odt-example-result/v1"', completed.stdout)

    def test_semantic_mismatch_exits_one_after_writing_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as work_text:
            work = Path(work_text)
            queries = work / "mismatch.csv"
            shutil.copyfile(DATA_ROOT / "queries.csv", queries)
            rows = list(csv.reader(queries.read_text(encoding="ascii").splitlines()))
            rows[1][6:] = ["outside", "ok", "0"]
            with queries.open("w", newline="", encoding="ascii") as stream:
                csv.writer(stream, lineterminator="\n").writerows(rows)

            completed = self.run_example(work, mode="machine", queries=queries)
            self.assertEqual(completed.returncode, 1, completed.stderr)
            summary = json.loads((work / "summary.json").read_text(encoding="ascii"))
            self.assertGreater(summary["mismatches"]["expectation"], 0)
            self.assertGreater(summary["mismatches"]["total"], 0)

    def test_malformed_input_exits_two(self) -> None:
        with tempfile.TemporaryDirectory() as work_text:
            work = Path(work_text)
            queries = work / "malformed.csv"
            queries.write_text("wrong,header\n", encoding="ascii")
            completed = self.run_example(work, mode="machine", queries=queries)
            self.assertEqual(completed.returncode, 2)
            self.assertEqual(completed.stdout, "")
            self.assertIn("invalid query CSV header", completed.stderr)


class OracleImplementationTest(unittest.TestCase):
    def test_oracle_uses_fraction_from_float_without_tree_internals(self) -> None:
        source = (PYTHON_ROOT / "point_location_oracle.py").read_text(encoding="ascii")
        self.assertIn("Fraction.from_float", source)
        for forbidden in ("ctypes", "nodes_offset", "leaf", "affine", "crc32"):
            self.assertNotIn(forbidden, source.lower())

    def test_oracle_protocol_failure_exits_two(self) -> None:
        with tempfile.TemporaryDirectory() as work_text:
            work = Path(work_text)
            bad_results = work / "bad.jsonl"
            bad_results.write_text("{}\n", encoding="ascii")
            completed = subprocess.run(
                [
                    os.fspath(Path(os.environ.get("PYTHON", os.sys.executable))),
                    os.fspath(PYTHON_ROOT / "point_location_oracle.py"),
                    "--sites",
                    os.fspath(DATA_ROOT / "sites.csv"),
                    "--queries",
                    os.fspath(DATA_ROOT / "queries.csv"),
                    "--results",
                    os.fspath(bad_results),
                    "--summary",
                    os.fspath(bad_results),
                ],
                text=True,
                capture_output=True,
            )
            self.assertEqual(completed.returncode, 2)


if __name__ == "__main__":
    unittest.main()
