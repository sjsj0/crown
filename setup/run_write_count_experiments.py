#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Sequence


MODE_ORDER = ["chain", "craq", "crown"]
MODE_STYLES = {
    "chain": {"label": "CHAIN", "color": "#3f73b8", "linestyle": "-", "marker": "o"},
    "craq": {"label": "CRAQ", "color": "#f28e2b", "linestyle": "--", "marker": "s"},
    "crown": {"label": "CROWN", "color": "#4e9a3a", "linestyle": "-.", "marker": "D"},
}

OUTPUT_FIELDNAMES = [
    "experiment_id",
    "total_write_count",
    "ops_per_client",
    "chain_length",
    "mode",
    "client_count",
    "key_count",
    "trials_expected",
    "trials_completed",
    "throughput_mean",
    "throughput_stddev",
    "latency_mean_ms",
    "latency_stddev_ms",
    "latency_p50_mean_ms",
    "latency_p95_mean_ms",
    "latency_p99_mean_ms",
    "write_rpc_failures_mean",
    "all_trials_complete",
    "source_summary",
]


def log(msg: str) -> None:
    print(f"[write-count-runner] {msg}", flush=True)


def parse_args() -> tuple[argparse.Namespace, list[str]]:
    root_dir = Path(__file__).resolve().parent.parent
    default_work_dir = root_dir / "build" / "write_count_throughput_runs"

    p = argparse.ArgumentParser(
        description=(
            "Sweep total write count and plot average write throughput for "
            "CHAIN, CRAQ, and CROWN."
        )
    )
    p.add_argument(
        "--write-counts",
        nargs="+",
        type=int,
        default=[50000, 100000, 200000, 400000],
        help="Aggregate requested writes per benchmark case.",
    )
    p.add_argument("--modes", nargs="+", default=MODE_ORDER, choices=MODE_ORDER)
    p.add_argument("--chain-length", type=int, default=3)
    p.add_argument("--client-count", type=int, default=1)
    p.add_argument("--trials", type=int, default=3)
    p.add_argument("--key-count", type=int, default=64)
    p.add_argument("--work-dir", type=Path, default=default_work_dir)
    p.add_argument(
        "--runner",
        type=Path,
        default=root_dir / "setup" / "run_chain_length_experiments.py",
        help="Path to the chain-length experiment runner to reuse.",
    )
    p.add_argument("--dry-run", action="store_true", help="Print planned commands without running them.")
    p.add_argument("--keep-going", action="store_true", help="Continue if one write-count run fails.")
    p.add_argument("--no-plot", action="store_true", help="Only write the combined CSV.")
    p.add_argument("--formats", nargs="+", default=["png"], help="Plot output formats, e.g. png pdf svg.")
    p.add_argument("--dpi", type=int, default=150)
    args, extra_runner_args = p.parse_known_args()
    return args, extra_runner_args


def validate_args(args: argparse.Namespace) -> None:
    if not args.write_counts:
        raise SystemExit("--write-counts cannot be empty")
    if any(count <= 0 for count in args.write_counts):
        raise SystemExit("--write-counts must all be positive")
    if args.chain_length <= 0:
        raise SystemExit("--chain-length must be > 0")
    if args.client_count <= 0:
        raise SystemExit("--client-count must be > 0")
    if args.trials <= 0:
        raise SystemExit("--trials must be > 0")
    if args.key_count <= 0:
        raise SystemExit("--key-count must be > 0")
    for count in args.write_counts:
        if count % args.client_count != 0:
            raise SystemExit(
                f"total write count {count} is not divisible by --client-count {args.client_count}; "
                "pick divisible counts so each client gets equal work"
            )


def shell_join(cmd: Sequence[str]) -> str:
    return " ".join(shlex.quote(part) for part in cmd)


def run_for_write_count(
    args: argparse.Namespace,
    extra_runner_args: Sequence[str],
    total_writes: int,
) -> Path:
    ops_per_client = total_writes // args.client_count
    case_work_dir = args.work_dir / f"writes_{total_writes}"
    cmd = [
        sys.executable,
        str(args.runner),
        "--work-dir",
        str(case_work_dir),
        "--chain-lengths",
        str(args.chain_length),
        "--modes",
        *args.modes,
        "--client-counts",
        str(args.client_count),
        "--ops",
        "write",
        "--trials",
        str(args.trials),
        "--key-count",
        str(args.key_count),
        "--write-op-count",
        str(ops_per_client),
        "--read-op-count",
        "1",
        *extra_runner_args,
    ]
    if args.keep_going and "--keep-going" not in extra_runner_args:
        cmd.append("--keep-going")
    if args.dry_run:
        cmd.append("--dry-run")
        log(f"[dry-run] {shell_join(cmd)}")
        return case_work_dir / "summary_by_chain_length.csv"

    log(
        f"running total_writes={total_writes} "
        f"(ops_per_client={ops_per_client}, modes={args.modes})"
    )
    result = subprocess.run(cmd, cwd=args.runner.resolve().parent.parent)
    if result.returncode != 0:
        message = f"write-count run failed for total_writes={total_writes} with exit code {result.returncode}"
        if args.keep_going:
            log(message)
        else:
            raise SystemExit(message)
    return case_work_dir / "summary_by_chain_length.csv"


def collect_rows(args: argparse.Namespace, summaries: Sequence[tuple[int, Path]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for total_writes, summary_path in summaries:
        if not summary_path.is_file():
            if args.keep_going or args.dry_run:
                log(f"summary missing for total_writes={total_writes}: {summary_path}")
                continue
            raise SystemExit(f"summary missing for total_writes={total_writes}: {summary_path}")

        with summary_path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            for record in reader:
                if record.get("operation") != "write":
                    continue
                mode = record.get("mode", "")
                if mode not in args.modes:
                    continue

                ops_per_client = str(total_writes // args.client_count)
                rows.append(
                    {
                        "experiment_id": (
                            f"writecount_total{total_writes}_mode_{mode}"
                            f"_chainlen{args.chain_length}_clients{args.client_count}"
                        ),
                        "total_write_count": str(total_writes),
                        "ops_per_client": ops_per_client,
                        "chain_length": record.get("chain_length", str(args.chain_length)),
                        "mode": mode,
                        "client_count": record.get("client_count", str(args.client_count)),
                        "key_count": record.get("key_count", str(args.key_count)),
                        "trials_expected": record.get("trials_expected", str(args.trials)),
                        "trials_completed": record.get("trials_completed", "0"),
                        "throughput_mean": record.get("throughput_mean", "0"),
                        "throughput_stddev": record.get("throughput_stddev", "0"),
                        "latency_mean_ms": record.get("latency_mean_ms", "0"),
                        "latency_stddev_ms": record.get("latency_stddev_ms", "0"),
                        "latency_p50_mean_ms": record.get("latency_p50_mean_ms", "0"),
                        "latency_p95_mean_ms": record.get("latency_p95_mean_ms", "0"),
                        "latency_p99_mean_ms": record.get("latency_p99_mean_ms", "0"),
                        "write_rpc_failures_mean": record.get("write_rpc_failures_mean", "0"),
                        "all_trials_complete": record.get("all_trials_complete", "False"),
                        "source_summary": str(summary_path),
                    }
                )

    rows.sort(
        key=lambda row: (
            int(row["total_write_count"]),
            MODE_ORDER.index(row["mode"]) if row["mode"] in MODE_ORDER else 99,
        )
    )
    return rows


def write_csv(path: Path, rows: Sequence[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=OUTPUT_FIELDNAMES)
        writer.writeheader()
        writer.writerows(rows)


def parse_float(raw: str) -> float:
    try:
        return float(raw)
    except (TypeError, ValueError):
        return 0.0


def make_plot(args: argparse.Namespace, rows: Sequence[dict[str, str]], csv_path: Path) -> list[Path]:
    if args.no_plot or not rows:
        return []

    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import FuncFormatter
    except ImportError:
        log("matplotlib is not installed; skipped plot generation")
        return []

    by_mode: dict[str, list[dict[str, str]]] = {mode: [] for mode in args.modes}
    for row in rows:
        by_mode.setdefault(row["mode"], []).append(row)

    fig, ax = plt.subplots(figsize=(8.7, 5.25))
    max_y = 0.0
    for mode in MODE_ORDER:
        mode_rows = sorted(by_mode.get(mode, []), key=lambda row: int(row["total_write_count"]))
        if not mode_rows:
            continue

        xs = [int(row["total_write_count"]) for row in mode_rows]
        ys = [parse_float(row["throughput_mean"]) for row in mode_rows]
        yerrs = [parse_float(row["throughput_stddev"]) for row in mode_rows]
        max_y = max(max_y, max((y + err for y, err in zip(ys, yerrs)), default=0.0))

        style = MODE_STYLES[mode]
        ax.errorbar(
            xs,
            ys,
            yerr=yerrs if any(err > 0.0 for err in yerrs) else None,
            label=style["label"],
            color=style["color"],
            linestyle=style["linestyle"],
            marker=style["marker"],
            linewidth=2.0,
            markersize=6.5,
            capsize=4,
            elinewidth=1.2,
        )

    ax.set_title("Write Throughput vs Total Writes", fontsize=13, fontweight="bold")
    ax.set_xlabel("Total Writes Requested")
    ax.set_ylabel("Average Write Throughput (ops/s)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(sorted({int(row["total_write_count"]) for row in rows}))
    ax.xaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{int(value / 1000)}k"))
    ax.set_ylim(bottom=0, top=max(max_y * 1.18, 1.0))
    ax.grid(True, linestyle="--", alpha=0.3, linewidth=0.6)
    ax.legend(title="Protocol", loc="best", frameon=True)
    fig.text(
        0.5,
        0.01,
        f"chain_length={args.chain_length}, clients={args.client_count}, "
        f"keys={args.key_count}, trials={args.trials}",
        ha="center",
        fontsize=9,
    )
    fig.tight_layout(rect=(0, 0.035, 1, 1))

    out_paths: list[Path] = []
    for fmt in args.formats:
        path = csv_path.with_suffix(f".{fmt}")
        fig.savefig(path, dpi=args.dpi)
        out_paths.append(path)
    plt.close(fig)
    return out_paths


def main() -> int:
    args, extra_runner_args = parse_args()
    validate_args(args)

    args.work_dir.mkdir(parents=True, exist_ok=True)
    summaries: list[tuple[int, Path]] = []
    for total_writes in args.write_counts:
        summary = run_for_write_count(args, extra_runner_args, total_writes)
        summaries.append((total_writes, summary))

    if args.dry_run:
        log("Dry run complete. No experiments were executed.")
        return 0

    rows = collect_rows(args, summaries)
    csv_path = args.work_dir / "write_count_throughput.csv"
    write_csv(csv_path, rows)
    log(f"Wrote CSV: {csv_path}")

    for path in make_plot(args, rows, csv_path):
        log(f"Wrote plot: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
