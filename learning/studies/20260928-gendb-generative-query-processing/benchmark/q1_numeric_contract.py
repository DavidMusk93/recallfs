#!/usr/bin/env python3
"""Verify the numeric domain used by GenDB's compact TPC-H Q1 columns."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


INT32_MAX = (1 << 31) - 1
INT64_MAX = (1 << 63) - 1
LINEITEM_ROWS = 59_986_052


def binary64_round_trip(encoded: int, scale: int) -> int:
    """Model parse-to-double followed by GenDB's positive-value rounding."""
    if scale == 1:
        source_text = str(encoded)
    else:
        whole, fractional = divmod(encoded, scale)
        source_text = f"{whole}.{fractional:02d}"
    parsed = float(source_text)
    return int(parsed * scale + 0.5)


def verify_exact_round_trip(name: str, minimum: int, maximum: int, scale: int) -> int:
    checked = 0
    for encoded in range(minimum, maximum + 1):
        actual = binary64_round_trip(encoded, scale)
        if actual != encoded:
            raise AssertionError(
                f"{name}: {encoded}/{scale} encoded as {actual}, expected {encoded}"
            )
        checked += 1
    return checked


def build_report() -> dict[str, object]:
    domains = {
        "quantity_units": {"minimum": 1, "maximum": 50, "scale": 1},
        "discount_cent": {"minimum": 0, "maximum": 10, "scale": 100},
        "tax_cent": {"minimum": 0, "maximum": 8, "scale": 100},
        "extendedprice_cent": {
            "minimum": 90_091,
            "maximum": 10_494_950,
            "scale": 100,
        },
    }
    checked_values = {
        name: verify_exact_round_trip(
            name,
            int(contract["minimum"]),
            int(contract["maximum"]),
            int(contract["scale"]),
        )
        for name, contract in domains.items()
    }

    max_price_cent = int(domains["extendedprice_cent"]["maximum"])
    max_discount_factor = 100
    max_tax_factor = 108
    max_sum_charge_scaled = (
        LINEITEM_ROWS
        * max_price_cent
        * max_discount_factor
        * max_tax_factor
    )
    if max_price_cent > INT32_MAX:
        raise AssertionError("extendedprice cents exceed int32")
    if max_sum_charge_scaled > INT64_MAX:
        raise AssertionError("Q1 sum_charge accumulator exceeds int64")

    return {
        "status": "pass",
        "source_semantics": (
            "TPC-H nonnegative decimal text with at most two fractional digits"
        ),
        "conversion": "int(binary64_value * scale + 0.5)",
        "rounding": "nearest for the declared nonnegative decimal domain",
        "overflow": "widen products and accumulators to signed int64",
        "domains": domains,
        "checked_values": checked_values,
        "first_invalid_values": {
            "quantity_units": [0, 51],
            "discount_cent": [-1, 11],
            "tax_cent": [-1, 9],
            "extendedprice_cent": [90_090, 10_494_951],
        },
        "max_sum_charge_scaled": max_sum_charge_scaled,
        "int64_max": INT64_MAX,
        "int64_headroom": INT64_MAX - max_sum_charge_scaled,
        "portability": "IEEE-754 binary64 and C++ truncation of positive values",
        "guard_boundary": (
            "The upstream builder does not enforce these domains; a production "
            "ingest path must reject or fall back when they do not hold."
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    payload = json.dumps(build_report(), indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    print(payload, end="")


if __name__ == "__main__":
    main()
