#!/usr/bin/env python3
"""Render the README benchmark charts from run_bench.sh CSV output.

Usage:
    bench/run_bench.sh | tee bench/results.csv
    python3 bench/plot_bench.py bench/results.csv -o docs

Writes benchmark-light.svg and benchmark-dark.svg (GitHub <picture>
swaps them by viewer theme), plus benchmark.png for previewing.
"""
import argparse
import csv
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

COFETCH = "#3b82f6"
RIVAL = "#9ca3af"

# (name, scenario, concurrency) -> bar label; first entry drawn topmost.
PANELS = [
    ("One thread each — 20,000 GETs", [
        (("cofetch", "throughput", 100), "cofetch · run()", COFETCH),
        (("cofetch-poll", "throughput", 100), "cofetch · busy-poll()",
         COFETCH),
        (("cpp-httplib-threads", "throughput", 1), "cpp-httplib · sync",
         RIVAL),
        (("cpr-threads", "throughput", 1), "cpr · sync", RIVAL),
    ]),
    ("One thread per core (20) — 20,000 GETs", [
        (("cofetch-20loops", "throughput", 100), "cofetch · 20 loops",
         COFETCH),
        (("cpp-httplib-threads", "throughput", 20),
         "cpp-httplib · 20 threads", RIVAL),
        (("cpr-threads", "throughput", 20), "cpr · 20 threads", RIVAL),
    ]),
    ("Sequential chain — 2,000 dependent requests", [
        (("cofetch-cb-poll", "chain", 1), "cofetch · busy-poll", COFETCH),
        (("cpp-httplib", "chain", 1), "cpp-httplib", RIVAL),
        (("cpr", "chain", 1), "cpr", RIVAL),
    ]),
]


def load(path):
    rows = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            key = (row["name"], row["scenario"], int(row["concurrency"]))
            rows[key] = float(row["req_per_sec"])
    return rows


def render(rows, path, dark, transparent=True):
    fg = "#e6edf3" if dark else "#1f2328"
    muted = "#8b949e" if dark else "#57606a"
    plt.rcParams.update({
        "svg.fonttype": "path",
        "font.size": 11,
        "text.color": fg,
        "axes.edgecolor": muted,
        "axes.labelcolor": fg,
        "xtick.color": muted,
        "ytick.color": fg,
    })

    heights = [len(bars) for _, bars in PANELS]
    fig, axes = plt.subplots(
        len(PANELS), 1,
        figsize=(8.5, 0.42 * sum(heights) + 1.0 * len(PANELS)),
        gridspec_kw={"height_ratios": heights, "hspace": 0.55})

    for ax, (title, bars) in zip(axes, PANELS):
        bars = bars[::-1]  # barh draws bottom-up; keep declared order on top
        labels = [label for _, label, _ in bars]
        values = [rows[key] for key, _, _ in bars]
        colors = [c for _, _, c in bars]

        drawn = ax.barh(labels, values, color=colors, height=0.62)
        for patch, value in zip(drawn, values):
            ax.text(value + max(values) * 0.02,
                    patch.get_y() + patch.get_height() / 2,
                    f"{value:,.0f}", va="center", color=fg)

        ax.set_title(title, loc="left", fontweight="bold", color=fg)
        ax.set_xlim(0, max(values) * 1.17)
        ax.tick_params(axis="y", length=0)
        ax.xaxis.set_visible(False)
        for side in ("top", "right", "bottom"):
            ax.spines[side].set_visible(False)

    fig.text(0.99, 0.005, "requests / second — higher is better",
             ha="right", color=muted, fontsize=9)
    if transparent:
        # NB: passing any facecolor kwarg would override transparent=True.
        fig.savefig(path, transparent=True, bbox_inches="tight")
    else:
        fig.savefig(path, facecolor="white", bbox_inches="tight")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv")
    parser.add_argument("-o", "--outdir", default="docs")
    args = parser.parse_args()

    rows = load(args.csv)
    os.makedirs(args.outdir, exist_ok=True)
    render(rows, os.path.join(args.outdir, "benchmark-light.svg"), dark=False)
    render(rows, os.path.join(args.outdir, "benchmark-dark.svg"), dark=True)
    # Opaque variant for the doxygen site, readable on any theme.
    render(rows, os.path.join(args.outdir, "benchmark-docs.svg"), dark=False,
           transparent=False)
    render(rows, os.path.join(args.outdir, "benchmark.png"), dark=False)
    print(f"wrote benchmark-{{light,dark}}.svg to {args.outdir}/")


if __name__ == "__main__":
    main()
