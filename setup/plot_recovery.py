#!/usr/bin/env python3
"""
Plot throughput-over-time curves from bench-write-rate logs.

Usage:
    ./plot_recovery.py chain=chain.log craq=craq.log crown=crown.log \
        --kill-at 30 --out recovery.png

Each LOG argument is a "label=path" pair. The script extracts
BENCH_TICK lines of the form:
    BENCH_TICK t_sec=N issued_delta=X acked_delta=Y pending=... total_...

and plots acked_delta (throughput in writes-per-second) vs t_sec.

Use --kill-at SECONDS to draw a vertical line at the failure-injection
time (so you can see the dip and recovery).
"""

import argparse
import re
import sys
from pathlib import Path

TICK_RE = re.compile(
    r"BENCH_TICK\s+t_sec=(\d+)\s+issued_delta=(\d+)\s+acked_delta=(\d+)"
)


def parse_log(path: Path):
    times, issued, acked = [], [], []
    with path.open() as f:
        for line in f:
            m = TICK_RE.search(line)
            if not m:
                continue
            times.append(int(m.group(1)))
            issued.append(int(m.group(2)))
            acked.append(int(m.group(3)))
    return times, issued, acked


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", help='label=path pairs, e.g. chain=chain.log')
    parser.add_argument("--kill-at", type=float, default=None,
                        help="seconds at which a node was killed (vertical marker)")
    parser.add_argument("--out", default="recovery.png", help="output image path")
    parser.add_argument("--metric", choices=["acked", "issued"], default="acked",
                        help="which delta to plot (default: acked = committed wps)")
    parser.add_argument("--show", action="store_true", help="also display the plot")
    args = parser.parse_args()

    import matplotlib
    if not args.show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(10, 5))

    for spec in args.logs:
        if "=" not in spec:
            print(f"bad log spec '{spec}', expected label=path", file=sys.stderr)
            sys.exit(2)
        label, path = spec.split("=", 1)
        times, issued, acked = parse_log(Path(path))
        if not times:
            print(f"warning: no BENCH_TICK lines in {path}", file=sys.stderr)
            continue
        series = acked if args.metric == "acked" else issued
        ax.plot(times, series, label=label, linewidth=1.5)

    if args.kill_at is not None:
        ax.axvline(args.kill_at, color="red", linestyle="--", alpha=0.6,
                   label=f"node killed @ {args.kill_at:g}s")

    ax.set_xlabel("time (s)")
    ax.set_ylabel(f"writes / s ({args.metric})")
    ax.set_title("Write throughput over time — failure & recovery")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(args.out, dpi=130)
    print(f"wrote {args.out}")
    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
