#!/usr/bin/env python3
"""
Walk a directory of gem5 output directories and emit one CSV row per run.

Usage:
    scripts/scrape.py [m5out_dir] [-o results/data.csv]

Each gem5 run writes a stats.txt and a config.json. The run parameters come
from config.json rather than from the directory name wherever possible, so
the CSV describes what was actually simulated rather than what the sweep
script intended to simulate.

A note on stats field names: they drift between gem5 releases, so nothing
here trusts a name from memory. The names below were read out of an actual
stats.txt produced by this build and are recorded in NOTES.md. If a future
gem5 renames them, this script reports the runs as unparseable rather than
silently emitting zeros.
"""

import argparse
import csv
import json
import os
import re
import sys

# Confirmed against stats.txt from gem5 25.x, build/ALL.
STAT_MISSES = "board.cache_hierarchy.l2-cache-0.overallMisses::total"
STAT_ACCESSES = "board.cache_hierarchy.l2-cache-0.overallAccesses::total"
STAT_INSTS = "simInsts"
STAT_SECONDS = "simSeconds"

WANTED = {STAT_MISSES, STAT_ACCESSES, STAT_INSTS, STAT_SECONDS}

# Directory names look like "chase-BRRIP-2048" or "mixed-LRU-8192".
DIRNAME_RE = re.compile(r"^(?P<bench>[a-z]+)-(?P<policy>[A-Za-z]+)-(?P<ws>\d+)$")


def parse_stats(path):
    """
    Pull the wanted scalars out of a gem5 stats.txt.

    gem5 appends a fresh block of stats every time stats are dumped, and the
    warmup runs dump twice: once at the reset and once at the end. Later
    values overwrite earlier ones, so what comes back is the final dump,
    which for a warmup run is exactly the measured window.
    """
    found = {}
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            if not line or line.startswith("-"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            if parts[0] in WANTED:
                found[parts[0]] = parts[1]
    return found


def parse_config(path):
    """Read L2 geometry and policy out of gem5's config.json."""
    info = {}
    try:
        with open(path, "r") as handle:
            config = json.load(handle)
    except (OSError, ValueError):
        return info

    def walk(node):
        if isinstance(node, dict):
            if node.get("name") == "l2-cache-0" or (
                "assoc" in node and "replacement_policy" in node
            ):
                info.setdefault("l2_assoc", node.get("assoc"))
                info.setdefault("l2_size", node.get("size"))
                policy = node.get("replacement_policy")
                if isinstance(policy, dict):
                    info.setdefault("l2_policy_type", policy.get("type"))
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(config)
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "m5out",
        nargs="?",
        default=os.path.expanduser("~/srrip-work/m5out"),
        help="directory containing one subdirectory per gem5 run",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="results/data.csv",
        help="CSV to write",
    )
    args = parser.parse_args()

    rows = []
    skipped = []

    for entry in sorted(os.listdir(args.m5out)):
        run_dir = os.path.join(args.m5out, entry)
        stats_path = os.path.join(run_dir, "stats.txt")
        if not os.path.isfile(stats_path):
            continue

        match = DIRNAME_RE.match(entry)
        if match is None:
            # Smoke tests and acceptance-gate runs live here too; they are
            # not part of the dataset.
            continue

        stats = parse_stats(stats_path)
        if STAT_MISSES not in stats or STAT_INSTS not in stats:
            skipped.append(f"{entry}: stats.txt has no L2 counters")
            continue

        misses = float(stats[STAT_MISSES])
        accesses = float(stats.get(STAT_ACCESSES, 0.0))
        insts = float(stats[STAT_INSTS])

        if insts <= 0:
            skipped.append(f"{entry}: zero instructions simulated")
            continue

        config = parse_config(os.path.join(run_dir, "config.json"))

        rows.append(
            {
                "policy": match.group("policy"),
                "benchmark": match.group("bench"),
                "working_set_kb": int(match.group("ws")),
                "l2_size": config.get("l2_size", ""),
                "l2_assoc": config.get("l2_assoc", ""),
                "misses": int(misses),
                "accesses": int(accesses),
                "insts": int(insts),
                # Misses per thousand instructions. This is the standard
                # metric for replacement policies because it normalises
                # across workloads that run for different lengths.
                "mpki": round(misses / (insts / 1000.0), 4),
                "miss_rate": round(misses / accesses, 6) if accesses else "",
                "sim_seconds": stats.get(STAT_SECONDS, ""),
            }
        )

    if not rows:
        print(f"error: no usable runs found under {args.m5out}", file=sys.stderr)
        return 1

    rows.sort(key=lambda r: (r["benchmark"], r["policy"], r["working_set_kb"]))

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(args.output, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    print(f"wrote {len(rows)} rows to {args.output}")
    for note in skipped:
        print(f"  skipped {note}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
