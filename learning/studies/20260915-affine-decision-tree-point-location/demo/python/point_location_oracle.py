from __future__ import annotations

import argparse
import csv
import math
import random
import sys
from dataclasses import dataclass
from fractions import Fraction
from typing import Iterable

WIDTH = 10.0
HEIGHT = 8.0
SITES = ((2, 2), (8, 3), (4, 7))
OUTSIDE = -1
DISTANCE_UNCERTAINTY_ULPS = 8
PAIRWISE_BISECTOR_MIDPOINTS = (
    (5.0, 2.5),
    (3.0, 4.5),
    (6.0, 5.0),
)
VORONOI_VERTEX = (67.0 / 14.0, 53.0 / 14.0)
UNCERTAINTY_REGRESSION_POINT = (5.414124727934967, 0.01525163239020344)


@dataclass(frozen=True)
class Case:
    x: float
    y: float
    expected_region: int
    within_uncertainty_band: bool


def analyze_nearest_site(x: float, y: float) -> tuple[int, bool]:
    if x < 0.0 or x > WIDTH or y < 0.0 or y > HEIGHT:
        return OUTSIDE, False

    exact_x = Fraction.from_float(x)
    exact_y = Fraction.from_float(y)
    distances = sorted(
        ((exact_x - site_x) ** 2 + (exact_y - site_y) ** 2, region)
        for region, (site_x, site_y) in enumerate(SITES)
    )
    nearest_distance, nearest_region = distances[0]
    second_distance = distances[1][0]
    distance_ulp = max(
        Fraction.from_float(math.ulp(float(nearest_distance))),
        Fraction.from_float(math.ulp(float(second_distance))),
    )
    within_uncertainty_band = (
        second_distance - nearest_distance <= DISTANCE_UNCERTAINTY_ULPS * distance_ulp
    )
    return nearest_region, within_uncertainty_band


def locate_brute_force(x: float, y: float) -> int:
    return analyze_nearest_site(x, y)[0]


def make_case(x: float, y: float) -> Case:
    expected_region, within_uncertainty_band = analyze_nearest_site(x, y)
    return Case(x, y, expected_region, within_uncertainty_band)


def nextafter_neighborhood(x: float, y: float) -> Iterable[tuple[float, float]]:
    x_values = (math.nextafter(x, -math.inf), x, math.nextafter(x, math.inf))
    y_values = (math.nextafter(y, -math.inf), y, math.nextafter(y, math.inf))
    for neighbor_x in x_values:
        for neighbor_y in y_values:
            yield neighbor_x, neighbor_y


def iter_cases(count: int, seed: int) -> Iterable[Case]:
    anchors = [
        (2.0, 2.0),
        (8.0, 3.0),
        (4.0, 7.0),
        (5.0, 2.5),
        (3.0, 4.5),
        (6.0, 5.0),
        (0.0, 0.0),
        (10.0, 8.0),
        (-0.01, 4.0),
        (10.01, 4.0),
        (5.0, -0.01),
        (5.0, 8.01),
    ]
    critical_points = (
        *PAIRWISE_BISECTOR_MIDPOINTS,
        VORONOI_VERTEX,
        UNCERTAINTY_REGRESSION_POINT,
    )
    seen = set(anchors)
    rng = random.Random(seed)
    for x, y in anchors:
        yield make_case(x, y)
    for critical_x, critical_y in critical_points:
        for x, y in nextafter_neighborhood(critical_x, critical_y):
            if (x, y) not in seen:
                seen.add((x, y))
                yield make_case(x, y)
    for _ in range(count):
        x = rng.uniform(-2.0, 12.0)
        y = rng.uniform(-2.0, 10.0)
        yield make_case(x, y)


def generate_cases(count: int, seed: int) -> list[Case]:
    return list(iter_cases(count, seed))


def write_csv(cases: Iterable[Case]) -> None:
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(("x", "y", "expected_region", "within_uncertainty_band"))
    writer.writerows(
        (
            (
                format(case.x, ".17g"),
                format(case.y, ".17g"),
                case.expected_region,
                int(case.within_uncertainty_band),
            )
            for case in cases
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate independent 2D point-location cases")
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20260915)
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must be non-negative")
    write_csv(iter_cases(args.count, args.seed))


if __name__ == "__main__":
    main()
