from __future__ import annotations

import argparse
import csv
import hashlib
import math
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path


SEED = 20260916
WIDTH = 12.0
HEIGHT = 8.0
SITE_COUNT = 1000
RANDOM_QUERY_COUNT = 2048
BISECTOR_QUERY_COUNT = 32
QUERY_CLASSES = (
    "random",
    "boundary",
    "site",
    "exact_bisector",
    "outside",
    "nonfinite",
)

# Pinned after canonical generation. Protocol tests compare these values with
# both a fresh generation and the tracked fixtures.
SITES_SHA256 = "5a7f73ad433ccc8006d6a7ebd44dda6da4a2d8d4787e41f5daeb8ba1ea8cf28f"
QUERIES_SHA256 = "02f65827993c43d931650997e41ca1d5ea1bec5b0ba3fee58e48900d989344f0"


class PCG32:
    _MASK_32 = (1 << 32) - 1
    _MASK_64 = (1 << 64) - 1
    _MULTIPLIER = 6364136223846793005

    def __init__(self, seed: int) -> None:
        self.state = 0
        self.increment = ((seed << 1) | 1) & self._MASK_64
        self.next_u32()
        self.state = (self.state + seed) & self._MASK_64
        self.next_u32()

    def next_u32(self) -> int:
        old_state = self.state
        self.state = (
            old_state * self._MULTIPLIER + self.increment
        ) & self._MASK_64
        xor_shifted = (((old_state >> 18) ^ old_state) >> 27) & self._MASK_32
        rotation = old_state >> 59
        return (
            (xor_shifted >> rotation)
            | (xor_shifted << ((-rotation) & 31))
        ) & self._MASK_32

    def unit(self) -> float:
        return (self.next_u32() + 0.5) / 4294967296.0


@dataclass(frozen=True)
class Site:
    ordinal: int
    x: float
    y: float


@dataclass(frozen=True)
class Query:
    query_class: str
    x: float
    y: float
    site_a: int | None = None
    site_b: int | None = None


def generate_sites(generator: PCG32) -> list[Site]:
    sites: list[Site] = []
    for ordinal in range(SITE_COUNT):
        column = ordinal % 40
        row = ordinal // 40
        grid_x = (column + 0.1 + 0.8 * generator.unit()) / 40.0
        grid_y = (row + 0.1 + 0.8 * generator.unit()) / 25.0
        x = WIDTH * grid_x * grid_x
        y = HEIGHT * grid_y * (2.0 - grid_y)
        sites.append(Site(ordinal, x, y))

    if len({(site.x, site.y) for site in sites}) != SITE_COUNT:
        raise RuntimeError("PCG32 site generation produced a duplicate")
    return sites


def _exact_midpoint(first: Site, second: Site) -> tuple[float, float] | None:
    exact_x = (Fraction.from_float(first.x) + Fraction.from_float(second.x)) / 2
    exact_y = (Fraction.from_float(first.y) + Fraction.from_float(second.y)) / 2
    x = float(exact_x)
    y = float(exact_y)
    if Fraction.from_float(x) != exact_x or Fraction.from_float(y) != exact_y:
        return None
    return x, y


def _exact_distance(
    point: tuple[Fraction, Fraction],
    site: tuple[Fraction, Fraction],
) -> Fraction:
    return (point[0] - site[0]) ** 2 + (point[1] - site[1]) ** 2


def generate_exact_bisectors(sites: list[Site]) -> list[Query]:
    exact_sites = [
        (Fraction.from_float(site.x), Fraction.from_float(site.y)) for site in sites
    ]
    candidates: list[tuple[int, int]] = []
    for ordinal in range(SITE_COUNT):
        if ordinal % 40 != 39:
            candidates.append((ordinal, ordinal + 1))
        if ordinal + 40 < SITE_COUNT:
            candidates.append((ordinal, ordinal + 40))

    queries: list[Query] = []
    for first_ordinal, second_ordinal in candidates:
        midpoint = _exact_midpoint(sites[first_ordinal], sites[second_ordinal])
        if midpoint is None:
            continue
        exact_point = (
            Fraction.from_float(midpoint[0]),
            Fraction.from_float(midpoint[1]),
        )
        tied_distance = _exact_distance(exact_point, exact_sites[first_ordinal])
        if tied_distance != _exact_distance(exact_point, exact_sites[second_ordinal]):
            continue
        if any(
            _exact_distance(exact_point, site) < tied_distance for site in exact_sites
        ):
            continue
        queries.append(
            Query(
                "exact_bisector",
                midpoint[0],
                midpoint[1],
                first_ordinal,
                second_ordinal,
            )
        )
        if len(queries) == BISECTOR_QUERY_COUNT:
            return queries
    raise RuntimeError(
        f"found only {len(queries)} exact nearest-site bisectors; "
        f"need {BISECTOR_QUERY_COUNT}"
    )


def generate_queries(generator: PCG32, sites: list[Site]) -> list[Query]:
    queries = [
        Query("random", WIDTH * generator.unit(), HEIGHT * generator.unit())
        for _ in range(RANDOM_QUERY_COUNT)
    ]
    queries.extend(
        Query("boundary", x, y)
        for x, y in (
            (0.0, 0.0),
            (WIDTH, 0.0),
            (0.0, HEIGHT),
            (WIDTH, HEIGHT),
            (0.0, HEIGHT / 2.0),
            (WIDTH, HEIGHT / 2.0),
            (WIDTH / 2.0, 0.0),
            (WIDTH / 2.0, HEIGHT),
        )
    )
    queries.extend(Query("site", site.x, site.y, site.ordinal, site.ordinal) for site in sites)
    queries.extend(generate_exact_bisectors(sites))
    queries.extend(
        Query("outside", x, y)
        for x, y in (
            (math.nextafter(0.0, -math.inf), HEIGHT / 2.0),
            (math.nextafter(WIDTH, math.inf), HEIGHT / 2.0),
            (WIDTH / 2.0, math.nextafter(0.0, -math.inf)),
            (WIDTH / 2.0, math.nextafter(HEIGHT, math.inf)),
            (math.nextafter(0.0, -math.inf), math.nextafter(0.0, -math.inf)),
            (math.nextafter(WIDTH, math.inf), math.nextafter(HEIGHT, math.inf)),
            (-1.0, HEIGHT / 2.0),
            (WIDTH + 1.0, HEIGHT / 2.0),
        )
    )
    queries.extend(
        Query("nonfinite", x, y)
        for x, y in (
            (math.nan, 0.0),
            (math.inf, 0.0),
            (-math.inf, 0.0),
            (0.0, math.nan),
            (0.0, math.inf),
            (0.0, -math.inf),
        )
    )
    return queries


def _write_sites(path: Path, sites: list[Site]) -> None:
    with path.open("w", newline="", encoding="ascii") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("ordinal", "region_id", "x_hex", "y_hex"))
        writer.writerows(
            (site.ordinal, site.ordinal, site.x.hex(), site.y.hex()) for site in sites
        )


def _write_queries(path: Path, queries: list[Query]) -> None:
    with path.open("w", newline="", encoding="ascii") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(
            (
                "query_id",
                "class",
                "x_hex",
                "y_hex",
                "site_a",
                "site_b",
                "expected_kind",
                "expected_status",
                "expected_region_id",
            )
        )
        writer.writerows(
            (
                query_id,
                query.query_class,
                query.x.hex(),
                query.y.hex(),
                "" if query.site_a is None else query.site_a,
                "" if query.site_b is None else query.site_b,
                "",
                "",
                "",
            )
            for query_id, query in enumerate(queries)
        )


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(output_directory: Path) -> dict[str, int]:
    output_directory.mkdir(parents=True, exist_ok=True)
    generator = PCG32(SEED)
    sites = generate_sites(generator)
    queries = generate_queries(generator, sites)
    _write_sites(output_directory / "sites.csv", sites)
    _write_queries(output_directory / "queries.csv", queries)
    counts = {"sites": len(sites), "queries": len(queries)}
    counts.update(
        {
            query_class: sum(query.query_class == query_class for query in queries)
            for query_class in QUERY_CLASSES
        }
    )
    return counts


def main() -> int:
    default_output = Path(__file__).resolve().parents[1] / "data"
    parser = argparse.ArgumentParser(description="Generate the canonical ODT example dataset")
    parser.add_argument("--output", type=Path, default=default_output)
    args = parser.parse_args()

    counts = generate(args.output)
    sites_path = args.output / "sites.csv"
    queries_path = args.output / "queries.csv"
    print(f"seed={SEED}")
    print(f"sites={counts['sites']} sha256={_sha256(sites_path)} path={sites_path}")
    print(f"queries={counts['queries']} sha256={_sha256(queries_path)} path={queries_path}")
    print(
        "query_classes="
        + ",".join(f"{name}:{counts[name]}" for name in QUERY_CLASSES)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
