#!/usr/bin/env python3
"""
Draw the crossover plot from results/data.csv.

Usage:
    scripts/plot.py [-i results/data.csv] [-o plots/crossover.png]
                    [--capacity-kb 1024]

The main output is L2 miss rate against working-set size for the three
policies, with the L2 capacity marked. A second panel shows MPKI, which is
the metric the literature reports, on the same x axis.

matplotlib only. No pandas, so the repo has one fewer dependency for the
amount of data involved.
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

# One colour and marker per policy, held constant across every figure so the
# same line means the same thing wherever it appears.
STYLE = {
    "LRU": {"color": "#4c6ef5", "marker": "o", "label": "LRU"},
    "SRRIP": {"color": "#e8590c", "marker": "s", "label": "SRRIP"},
    "BRRIP": {"color": "#2f9e44", "marker": "^", "label": "BRRIP"},
    "Random": {"color": "#868e96", "marker": "x", "label": "Random"},
}

ORDER = ["LRU", "SRRIP", "BRRIP", "Random"]


def read_rows(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def format_kb(value, _pos=None):
    """Axis labels in KiB or MiB, whichever reads better."""
    if value >= 1024:
        mib = value / 1024.0
        return f"{mib:g}M"
    return f"{value:g}K"


def pretty_kb(value):
    """
    A size for prose, rounded to one decimal.

    The crossover points are interpolated, so they arrive with far more
    digits than the measurement justifies. Printing "1.00022 MiB" would
    claim a precision the sweep spacing does not support.
    """
    if value >= 1024:
        return f"{value / 1024.0:.1f} MiB"
    return f"{value:.0f} KiB"


def series_for(rows, benchmark):
    """Return {policy: (xs, miss_rates, mpkis)} sorted by working set."""
    grouped = defaultdict(list)
    for row in rows:
        if row["benchmark"] != benchmark:
            continue
        if not row["miss_rate"]:
            continue
        grouped[row["policy"]].append(
            (
                int(row["working_set_kb"]),
                float(row["miss_rate"]),
                float(row["mpki"]),
            )
        )

    out = {}
    for policy, points in grouped.items():
        points.sort()
        out[policy] = (
            [p[0] for p in points],
            [p[1] for p in points],
            [p[2] for p in points],
        )
    return out


# Below this difference in miss rate the two policies are treated as tied.
# Well below capacity SRRIP and BRRIP agree to four decimal places, and
# calling that a crossing would be reading noise as a result.
TIE = 1e-3


def find_crossover(series, a="SRRIP", b="BRRIP"):
    """
    Find the working-set size where b separates from a and becomes better.

    Returns (crossing_kb, end_kb): the point where BRRIP first goes clearly
    below SRRIP, and the point where the advantage closes back up again, or
    None for either if it does not happen in the measured range.

    The two are not merely swapping order here. Below capacity they are
    identical, so the crossing is a separation from a tie rather than a
    clean sign change, and the detection has to allow for that. Returning
    None rather than guessing matters: a crossover that is not in the data
    must not be drawn on the plot.
    """
    if a not in series or b not in series:
        return None, None

    xs_a, ys_a, _ = series[a]
    xs_b, ys_b, _ = series[b]
    common = sorted(set(xs_a) & set(xs_b))
    if len(common) < 2:
        return None, None

    lookup_a = dict(zip(xs_a, ys_a))
    lookup_b = dict(zip(xs_b, ys_b))
    deltas = [(x, lookup_b[x] - lookup_a[x]) for x in common]

    start = None
    end = None
    for (x_left, d_left), (x_right, d_right) in zip(deltas, deltas[1:]):
        if start is None and d_left >= -TIE and d_right < -TIE:
            # Interpolate the point where the gap reaches the tie threshold.
            span = d_left - d_right
            frac = (d_left + TIE) / span if span else 1.0
            start = x_left + frac * (x_right - x_left)
        elif start is not None and end is None and d_left < -TIE <= d_right:
            span = d_right - d_left
            frac = (-TIE - d_left) / span if span else 1.0
            end = x_left + frac * (x_right - x_left)

    return start, end


def draw(rows, out_path, capacity_kb, benchmark, title_suffix=""):
    series = series_for(rows, benchmark)
    if not series:
        print(f"error: no {benchmark} rows in the data", file=sys.stderr)
        return None, None

    fig, (ax_rate, ax_mpki) = plt.subplots(
        1, 2, figsize=(12.5, 5.2), constrained_layout=True
    )

    crossover, closes = find_crossover(series)

    for axis, index, ylabel in (
        (ax_rate, 1, "L2 miss rate"),
        (ax_mpki, 2, "L2 MPKI"),
    ):
        for policy in ORDER:
            if policy not in series:
                continue
            xs = series[policy][0]
            ys = series[policy][index]
            style = STYLE[policy]
            axis.plot(
                xs,
                ys,
                color=style["color"],
                marker=style["marker"],
                markersize=5.5,
                linewidth=1.9,
                label=style["label"],
            )

        axis.axvline(
            capacity_kb,
            color="#adb5bd",
            linestyle="--",
            linewidth=1.2,
            zorder=0,
        )
        # Sits to the left of the capacity line, where the curves are flat
        # and there is nothing to collide with.
        axis.annotate(
            f"L2 capacity\n{pretty_kb(capacity_kb)}",
            xy=(capacity_kb, axis.get_ylim()[1]),
            xytext=(-6, -4),
            textcoords="offset points",
            va="top",
            ha="right",
            fontsize=8,
            color="#495057",
        )

        if crossover is not None:
            right = closes if closes is not None else max(series["BRRIP"][0])
            axis.axvspan(
                crossover,
                right,
                color="#2f9e44",
                alpha=0.07,
                zorder=0,
            )
            axis.axvline(
                crossover,
                color="#2f9e44",
                linestyle=":",
                linewidth=1.4,
                zorder=0,
            )

        axis.set_xscale("log", base=2)
        axis.xaxis.set_major_formatter(FuncFormatter(format_kb))
        axis.set_xlabel("Working set (log scale)")
        axis.set_ylabel(ylabel)
        axis.grid(True, which="major", alpha=0.25, linewidth=0.7)
        axis.set_axisbelow(True)

    ax_rate.legend(frameon=False, loc="lower right")

    if crossover is not None and closes is not None:
        note = (
            f"BRRIP overtakes SRRIP at about {pretty_kb(crossover)} "
            f"and the advantage closes again by {pretty_kb(closes)}"
        )
    elif crossover is not None:
        note = f"BRRIP overtakes SRRIP at about {pretty_kb(crossover)}"
    else:
        note = "No SRRIP/BRRIP crossover in the measured range"

    fig.suptitle(
        f"L2 replacement policy vs working-set size ({benchmark}.c)"
        f"{title_suffix}\n{note}",
        fontsize=12,
    )

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")
    return crossover, closes


def draw_bars(rows, out_path):
    """
    The two supporting workloads, side by side.

    Left: mixed.c, swept over hot-set size. This is the other side of the
    argument. On chase.c, past capacity, BRRIP wins. Here, where there is a
    hot set small enough to be worth keeping, BRRIP loses, and loses by more
    as the hot set grows. A policy that refuses to retain most of what it
    sees cannot build up a working set it should have kept.

    Right: stream.c, the control. Pure sequential streaming past capacity has
    no reuse for any policy to exploit, so all three land on the same number.
    If they did not, the harness would be suspect.
    """
    mixed = series_for(rows, "mixed")
    stream_rows = [r for r in rows if r["benchmark"] == "stream"]
    if not mixed and not stream_rows:
        return

    fig, (ax_mixed, ax_stream) = plt.subplots(
        1,
        2,
        figsize=(11.5, 4.8),
        constrained_layout=True,
        gridspec_kw={"width_ratios": [1.7, 1.0]},
    )

    for policy in ORDER:
        if policy not in mixed:
            continue
        xs, miss_rates, _ = mixed[policy]
        ax_mixed.plot(
            xs,
            miss_rates,
            color=STYLE[policy]["color"],
            marker=STYLE[policy]["marker"],
            markersize=5.5,
            linewidth=1.9,
            label=policy,
        )

    ax_mixed.set_xscale("log", base=2)
    ax_mixed.xaxis.set_major_formatter(FuncFormatter(format_kb))
    ax_mixed.set_xlabel("Hot-set size (2 MiB streamed per phase)")
    ax_mixed.set_ylabel("L2 miss rate")
    ax_mixed.set_title(
        "mixed.c: BRRIP loses when there is a hot set worth keeping",
        fontsize=11,
    )
    ax_mixed.legend(frameon=False)
    ax_mixed.grid(True, alpha=0.25, linewidth=0.7)
    ax_mixed.set_axisbelow(True)

    policies = [p for p in ORDER if p != "Random"]
    values = []
    for policy in policies:
        match = [
            float(r["miss_rate"])
            for r in stream_rows
            if r["policy"] == policy and r["miss_rate"]
        ]
        values.append(match[0] if match else 0.0)

    bars = ax_stream.bar(
        range(len(policies)),
        values,
        width=0.6,
        color=[STYLE[p]["color"] for p in policies],
    )
    ax_stream.bar_label(bars, fmt="%.4f", fontsize=9, padding=2)
    ax_stream.set_xticks(range(len(policies)))
    ax_stream.set_xticklabels(policies)
    ax_stream.set_ylim(0, 1.15)
    ax_stream.set_ylabel("L2 miss rate")
    ax_stream.set_title(
        "stream.c control: no reuse, so no policy helps", fontsize=11
    )
    ax_stream.grid(True, axis="y", alpha=0.25, linewidth=0.7)
    ax_stream.set_axisbelow(True)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def draw_improvement(rows, out_path, capacity_kb, benchmark="chase"):
    """
    How much BRRIP actually wins by, against each baseline.

    The crossover plot shows three absolute curves and leaves the reader to
    eyeball the vertical distance between them. This one plots that distance
    directly, which is the quantity the project is actually about.

    Two panels, because the percentage on its own is misleading. Below
    capacity every policy already hits nearly everything, so a large relative
    improvement there would be a large share of almost nothing. The right
    panel shows misses actually avoided per thousand instructions, which puts
    that region back in proportion.
    """
    series = series_for(rows, benchmark)
    for needed in ("LRU", "SRRIP", "BRRIP"):
        if needed not in series:
            print(f"error: no {needed} rows for {benchmark}", file=sys.stderr)
            return

    rate = {p: dict(zip(series[p][0], series[p][1])) for p in series}
    mpki = {p: dict(zip(series[p][0], series[p][2])) for p in series}
    xs = sorted(set(rate["BRRIP"]) & set(rate["LRU"]) & set(rate["SRRIP"]))

    def pct(base, x):
        b = rate[base][x]
        return 100.0 * (b - rate["BRRIP"][x]) / b if b else 0.0

    fig, (ax_pct, ax_abs) = plt.subplots(
        1, 2, figsize=(12.5, 5.0), constrained_layout=True
    )

    for base in ("LRU", "SRRIP"):
        colour = STYLE[base]["color"]
        ys_pct = [pct(base, x) for x in xs]
        ys_abs = [mpki[base][x] - mpki["BRRIP"][x] for x in xs]

        for axis, ys in ((ax_pct, ys_pct), (ax_abs, ys_abs)):
            axis.plot(
                xs,
                ys,
                color=colour,
                marker=STYLE[base]["marker"],
                markersize=5.5,
                linewidth=1.9,
                label=f"vs {base}",
            )
            axis.fill_between(xs, 0, ys, color=colour, alpha=0.08)

    peak_x = max(xs, key=lambda x: pct("LRU", x))
    peak_y = pct("LRU", peak_x)

    for axis, ylabel, title in (
        (
            ax_pct,
            "Reduction in L2 misses (%)",
            "Relative: share of misses BRRIP avoids",
        ),
        (
            ax_abs,
            "MPKI avoided",
            "Absolute: misses avoided per 1000 instructions",
        ),
    ):
        axis.axhline(0, color="#495057", linewidth=1.0, zorder=1)
        axis.axvline(
            capacity_kb,
            color="#adb5bd",
            linestyle="--",
            linewidth=1.2,
            zorder=0,
        )
        axis.set_xscale("log", base=2)
        axis.xaxis.set_major_formatter(FuncFormatter(format_kb))
        axis.set_xlabel("Working set (log scale)")
        axis.set_ylabel(ylabel)
        axis.set_title(title, fontsize=11)
        axis.grid(True, alpha=0.25, linewidth=0.7)
        axis.set_axisbelow(True)
        axis.legend(frameon=False, loc="upper right")
        axis.annotate(
            f"L2 capacity\n{pretty_kb(capacity_kb)}",
            xy=(capacity_kb, axis.get_ylim()[1]),
            xytext=(-6, -4),
            textcoords="offset points",
            va="top",
            ha="right",
            fontsize=8,
            color="#495057",
        )

    ax_pct.annotate(
        f"peak {peak_y:.0f}% at {pretty_kb(peak_x)}",
        xy=(peak_x, peak_y),
        xytext=(18, -6),
        textcoords="offset points",
        fontsize=9,
        color="#212529",
        arrowprops={"arrowstyle": "-", "color": "#868e96", "linewidth": 0.9},
    )

    fig.suptitle(
        "How much BRRIP improves on LRU and SRRIP "
        f"({benchmark}.c, 1 MiB 16-way L2)\n"
        "The entire benefit lives in a narrow band just past capacity",
        fontsize=12,
    )

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-i", "--input", default="results/data.csv")
    parser.add_argument("-o", "--output", default="plots/crossover.png")
    parser.add_argument("--bars-output", default="plots/workloads.png")
    parser.add_argument(
        "--improvement-output", default="plots/brrip_improvement.png"
    )
    parser.add_argument("--capacity-kb", type=int, default=1024)
    parser.add_argument("--benchmark", default="chase")
    parser.add_argument("--title-suffix", default="")
    parser.add_argument(
        "--no-bars", action="store_true", help="skip the bar chart"
    )
    args = parser.parse_args()

    rows = read_rows(args.input)
    crossover, _closes = draw(
        rows,
        args.output,
        args.capacity_kb,
        args.benchmark,
        args.title_suffix,
    )
    if not args.no_bars:
        draw_bars(rows, args.bars_output)
    draw_improvement(
        rows, args.improvement_output, args.capacity_kb, args.benchmark
    )

    if crossover is None:
        print("note: SRRIP and BRRIP do not cross in this dataset")
    else:
        print(f"crossover at approximately {crossover:.0f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
