#!/usr/bin/env python3
"""Compare consecutive text-page renders from two CrossPoint serial logs."""
import argparse
import json
import re
import statistics
from pathlib import Path


def samples(path):
    result = []
    for line in path.read_text(errors="replace").splitlines():
        if "Page render" not in line:
            continue
        values = {key: int(value) for key, value in re.findall(r"\b(\w+)=(\d+)ms", line)}
        if "total" in values:
            result.append(values)
    return result


def summarize(rows, skip, cleanup_every):
    turns = rows[skip:]
    ordinary = [row for i, row in enumerate(turns, 1) if not cleanup_every or i % cleanup_every]
    cleanup = [row for i, row in enumerate(turns, 1) if cleanup_every and i % cleanup_every == 0]

    def group(values):
        if not values:
            return {"count": 0}
        totals = sorted(row["total"] for row in values)
        keys = set.intersection(*(set(row) for row in values))
        return {
            "count": len(values),
            "median_ms": statistics.median(totals),
            "mean_ms": round(statistics.mean(totals), 2),
            "min_ms": totals[0],
            "max_ms": totals[-1],
            "component_medians_ms": {key: statistics.median(row[key] for row in values) for key in sorted(keys)},
        }

    return {"recorded": len(rows), "skipped": skip, "ordinary": group(ordinary), "cleanup": group(cleanup)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("optimized", type=Path)
    parser.add_argument("--skip", type=int, default=1, help="Opening renders to exclude (default: 1)")
    parser.add_argument("--cleanup-every", type=int, default=15,
                        help="Full refresh interval after opening a book (default: 15, 0 groups all turns together)")
    args = parser.parse_args()
    if args.skip < 0 or args.cleanup_every < 0:
        parser.error("--skip and --cleanup-every must be zero or greater")
    before, after = samples(args.baseline), samples(args.optimized)
    if len(before) != len(after):
        parser.error(f"Logs contain different render counts: {len(before)} and {len(after)}")
    if len(before) <= args.skip:
        parser.error("Logs do not contain enough page renders after --skip")
    report = {
        "baseline": summarize(before, args.skip, args.cleanup_every),
        "optimized": summarize(after, args.skip, args.cleanup_every),
    }
    old = report["baseline"]["ordinary"].get("median_ms")
    new = report["optimized"]["ordinary"].get("median_ms")
    if old and new is not None:
        report["ordinary_time_reduction_percent"] = round(100 * (old - new) / old, 2)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
