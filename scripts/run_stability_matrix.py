#!/usr/bin/env python3
"""Run WorldSim long-run experiments across seed/resolution combinations.

The per-case acceptance envelope remains owned by worldsim_long_run. This driver
adds reproducible orchestration and cross-resolution diagnostics without
inventing a convergence threshold before a baseline exists.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Iterable

COMPARISON_METRICS = (
    "land_area_m2",
    "veg_ratio_initial",
    "veg_density_kg_m2",
    "grass_share",
    "shrub_share",
    "tree_share",
    "litter_density_kg_m2",
    "soil_density_kg_m2",
    "mean_npp_PgC_yr",
    "npp_density_kg_m2_yr",
    "annual_burn_land_fraction",
    "fauna_carbon_ratio",
    "fauna_carbon_PgC",
    "fauna_carbon_density_kg_m2",
    "mean_fertility",
    "mineral_nitrogen_kg_m2",
    "mean_land_temp_k",
    "mean_land_precip_mm_day",
    "atmospheric_co2_ppm",
    "planet_carbon_rel_residual",
    "tracked_nitrogen_rel_residual",
)

VALID_MODES = (
    "coupled",
    "no-fire",
    "no-fauna",
    "no-fire-no-fauna",
)

UINT64_MAX = (1 << 64) - 1


def parse_uint_list(value: str, name: str, maximum: int | None = None) -> list[int]:
    items: list[int] = []
    seen: set[int] = set()
    for raw in value.split(","):
        token = raw.strip()
        if not token:
            raise argparse.ArgumentTypeError(f"{name} contains an empty value")
        try:
            parsed = int(token, 10)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                f"{name} must contain comma-separated integers"
            ) from exc
        if parsed < 0 or (maximum is not None and parsed > maximum):
            suffix = f" in [0, {maximum}]" if maximum is not None else " >= 0"
            raise argparse.ArgumentTypeError(f"{name} values must be{suffix}")
        if parsed not in seen:
            seen.add(parsed)
            items.append(parsed)
    if not items:
        raise argparse.ArgumentTypeError(f"{name} must not be empty")
    return items


def parse_modes(value: str) -> list[str]:
    modes: list[str] = []
    seen: set[str] = set()
    for raw in value.split(","):
        mode = raw.strip()
        if mode not in VALID_MODES:
            raise argparse.ArgumentTypeError(
                f"unknown mode {mode!r}; expected one of {', '.join(VALID_MODES)}"
            )
        if mode not in seen:
            seen.add(mode)
            modes.append(mode)
    if not modes:
        raise argparse.ArgumentTypeError("modes must not be empty")
    return modes


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run worldsim_long_run across a seed/resolution matrix"
    )
    parser.add_argument(
        "--binary",
        type=Path,
        default=Path("./out/dev/worldsim_long_run"),
        help="path to worldsim_long_run",
    )
    parser.add_argument("--years", type=int, default=100)
    parser.add_argument("--seeds", default="0,42,999")
    parser.add_argument("--levels", default="1,2")
    parser.add_argument("--modes", default="coupled")
    parser.add_argument(
        "--assert-stable",
        action="store_true",
        help="pass the existing per-case stability envelope to worldsim_long_run",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("out/stability-matrix"),
    )
    args = parser.parse_args(argv)

    if args.years < 1 or args.years > 1000:
        parser.error("--years must be in [1, 1000]")
    try:
        args.seeds = parse_uint_list(args.seeds, "seeds", maximum=UINT64_MAX)
        args.levels = parse_uint_list(args.levels, "levels", maximum=5)
        args.modes = parse_modes(args.modes)
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))
    args.levels.sort()
    return args


def read_final_row(path: Path, expected_year: int) -> dict[str, str]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise RuntimeError(f"{path} has no CSV header")
        rows = list(reader)
    if not rows:
        raise RuntimeError(f"{path} has no data rows")
    row = rows[-1]
    try:
        year = int(row["year"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError(f"{path} has an invalid final year") from exc
    if year != expected_year:
        raise RuntimeError(
            f"{path} ended at year {year}, expected {expected_year}"
        )
    return row


def numeric(row: dict[str, str], key: str) -> float:
    try:
        value = float(row[key])
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError(f"missing or invalid numeric metric {key!r}") from exc
    if not math.isfinite(value):
        raise RuntimeError(f"metric {key!r} is not finite")
    return value


def enrich_row(row: dict[str, str]) -> dict[str, str]:
    enriched = dict(row)
    land_area = numeric(enriched, "land_area_m2")
    if not land_area > 0.0:
        raise RuntimeError("land_area_m2 must be positive for stability comparison")
    enriched["npp_density_kg_m2_yr"] = str(
        numeric(enriched, "mean_npp_PgC_yr") * 1.0e12 / land_area
    )
    enriched["fauna_carbon_density_kg_m2"] = str(
        numeric(enriched, "fauna_carbon_PgC") * 1.0e12 / land_area
    )
    return enriched


def write_csv(path: Path, rows: Iterable[dict[str, object]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = args.binary.resolve()
    if not binary.is_file():
        raise RuntimeError(f"worldsim_long_run not found: {binary}")

    output = args.output.resolve()
    cases_dir = output / "cases"
    cases_dir.mkdir(parents=True, exist_ok=True)

    case_rows: list[dict[str, str]] = []
    failures: list[dict[str, object]] = []

    for mode in args.modes:
        for seed in args.seeds:
            for level in args.levels:
                stem = f"{mode}-seed{seed}-level{level}"
                csv_path = cases_dir / f"{stem}.csv"
                log_path = cases_dir / f"{stem}.log"
                command = [
                    str(binary),
                    "--years",
                    str(args.years),
                    "--seed",
                    str(seed),
                    "--level",
                    str(level),
                    "--mode",
                    mode,
                    "--output",
                    str(csv_path),
                ]
                if args.assert_stable:
                    command.append("--assert-stable")

                print(
                    f"stability case mode={mode} seed={seed} level={level} "
                    f"years={args.years}",
                    flush=True,
                )
                result = subprocess.run(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    check=False,
                )
                log_path.write_text(result.stdout, encoding="utf-8")
                if result.returncode != 0:
                    failures.append(
                        {
                            "mode": mode,
                            "seed": seed,
                            "level": level,
                            "returncode": result.returncode,
                            "log": str(log_path.relative_to(output)),
                        }
                    )
                    continue

                try:
                    row = enrich_row(read_final_row(csv_path, args.years))
                    for metric in COMPARISON_METRICS:
                        numeric(row, metric)
                except RuntimeError as exc:
                    failures.append(
                        {
                            "mode": mode,
                            "seed": seed,
                            "level": level,
                            "returncode": 0,
                            "error": str(exc),
                            "log": str(log_path.relative_to(output)),
                        }
                    )
                    continue
                case_rows.append(row)

    matrix_path = output / "matrix.csv"
    if case_rows:
        write_csv(matrix_path, case_rows, list(case_rows[0].keys()))

    indexed: dict[tuple[str, int, int], dict[str, str]] = {}
    for row in case_rows:
        indexed[(row["mode"], int(row["seed"]), int(row["level"]))] = row

    comparisons: list[dict[str, object]] = []
    maxima: dict[str, float] = {metric: 0.0 for metric in COMPARISON_METRICS}
    for mode in args.modes:
        for seed in args.seeds:
            available_levels = [
                level
                for level in args.levels
                if (mode, seed, level) in indexed
            ]
            for low_level, high_level in zip(
                available_levels, available_levels[1:]
            ):
                low = indexed[(mode, seed, low_level)]
                high = indexed[(mode, seed, high_level)]
                for metric in COMPARISON_METRICS:
                    low_value = numeric(low, metric)
                    high_value = numeric(high, metric)
                    absolute_delta = abs(high_value - low_value)
                    scale = max(abs(low_value), abs(high_value), 1.0e-12)
                    relative_delta = absolute_delta / scale
                    maxima[metric] = max(maxima[metric], relative_delta)
                    comparisons.append(
                        {
                            "mode": mode,
                            "seed": seed,
                            "low_level": low_level,
                            "high_level": high_level,
                            "metric": metric,
                            "low_value": low_value,
                            "high_value": high_value,
                            "absolute_delta": absolute_delta,
                            "relative_delta": relative_delta,
                        }
                    )

    comparison_path = output / "resolution.csv"
    write_csv(
        comparison_path,
        comparisons,
        [
            "mode",
            "seed",
            "low_level",
            "high_level",
            "metric",
            "low_value",
            "high_value",
            "absolute_delta",
            "relative_delta",
        ],
    )

    summary = {
        "binary": str(binary),
        "years": args.years,
        "seeds": args.seeds,
        "levels": args.levels,
        "modes": args.modes,
        "assert_stable": args.assert_stable,
        "expected_cases": len(args.modes) * len(args.seeds) * len(args.levels),
        "completed_cases": len(case_rows),
        "failures": failures,
        "comparison_metrics": list(COMPARISON_METRICS),
        "max_adjacent_level_relative_delta": maxima,
        "convergence_gate": None,
        "convergence_note": (
            "Cross-resolution deltas are diagnostic only. No acceptance threshold "
            "is imposed until a multi-seed baseline defines one."
        ),
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    if failures:
        print(
            f"stability matrix failed: {len(failures)} of "
            f"{summary['expected_cases']} cases failed",
            file=sys.stderr,
        )
        return 1

    print(
        f"stability matrix complete: {len(case_rows)} cases, "
        f"{len(comparisons)} resolution comparisons -> {output}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (OSError, RuntimeError) as exc:
        print(f"run_stability_matrix.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
