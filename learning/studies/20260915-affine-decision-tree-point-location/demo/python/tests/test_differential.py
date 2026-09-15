from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

from point_location_oracle import (
    PAIRWISE_BISECTOR_MIDPOINTS,
    SITES,
    UNCERTAINTY_REGRESSION_POINT,
    VORONOI_VERTEX,
    generate_cases,
    nextafter_neighborhood,
)


class PointLocationDifferentialTest(unittest.TestCase):
    def test_20054_points_respect_independent_nearest_site_oracle(self) -> None:
        binary_text = os.environ.get("POINT_LOCATION_BIN")
        self.assertIsNotNone(binary_text, "POINT_LOCATION_BIN must name the C example executable")
        binary = Path(binary_text).resolve()

        cases = generate_cases(count=20_000, seed=20260915)
        self.assertEqual(len(cases), 20_054)
        case_points = {(case.x, case.y) for case in cases}
        for critical_point in (
            *PAIRWISE_BISECTOR_MIDPOINTS,
            VORONOI_VERTEX,
            UNCERTAINTY_REGRESSION_POINT,
        ):
            self.assertTrue(
                set(nextafter_neighborhood(*critical_point)) <= case_points,
                f"missing nextafter neighborhood for {critical_point}",
            )
        regression_cases = [
            case
            for case in cases
            if (case.x, case.y) == UNCERTAINTY_REGRESSION_POINT
        ]
        self.assertEqual(len(regression_cases), 1)
        self.assertEqual(regression_cases[0].expected_region, 1)
        self.assertTrue(regression_cases[0].within_uncertainty_band)

        input_text = "".join(f"{case.x:.17g} {case.y:.17g}\n" for case in cases)
        try:
            completed = subprocess.run(
                [str(binary), "--machine"],
                input=input_text,
                text=True,
                capture_output=True,
            )
        except OSError as error:
            self.fail(f"cannot execute C example {binary}: {error}")
        self.assertEqual(completed.returncode, 0, completed.stderr)

        rows = completed.stdout.splitlines()
        self.assertEqual(len(rows), len(cases))
        for index, (case, row) in enumerate(zip(cases, rows, strict=True)):
            region_text, _ = row.split()
            actual_region = int(region_text)
            if case.within_uncertainty_band:
                self.assertIn(actual_region, range(len(SITES)), f"case {index}: {case}")
            else:
                self.assertEqual(actual_region, case.expected_region, f"case {index}: {case}")


if __name__ == "__main__":
    unittest.main()
