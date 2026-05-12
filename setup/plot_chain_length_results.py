#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

try:
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
except ImportError as exc:  # pragma: no cover - depends on local environment
    raise SystemExit(
        "matplotlib is required for plotting. Install it with: python3 -m pip install matplotlib"
    ) from exc


MODE_ORDER = ["chain", "craq", "crown"]
MODE_STYLES = {
    "chain": {
        "label": "CHAIN",
        "color": "#3f73b8",
        "linestyle": "-",
        "marker": "o",
    },
    "craq": {
        "label": "CRAQ",
        "color": "#f28e2b",
        "linestyle": "--",
        "marker": "s",
    },
    "crown": {
        "label": "CROWN",
        "color": "#4e9a3a",
        "linestyle": "-.",
        "marker": "D",
    },
}

TAIL_LATENCY_SERIES = [
    {
        "key": "p50",
        "label": "P50",
        "mean": "latency_p50_mean_ms",
        "stddev": "latency_p50_stddev_ms",
        "linestyle": ":",
        "marker": "^",
        "linewidth": 1.8,
    },
    {
        "key": "p95",
        "label": "P95",
        "mean": "latency_p95_mean_ms",
        "stddev": "latency_p95_stddev_ms",
        "linestyle": "--",
        "marker": "s",
        "linewidth": 2.0,
    },
    {
        "key": "p99",
        "label": "P99",
        "mean": "latency_p99_mean_ms",
        "stddev": "latency_p99_stddev_ms",
        "linestyle": "-",
        "marker": "D",
        "linewidth": 2.2,
    },
]

METRIC_DEFS = {
    "throughput": {
        "mean": "throughput_mean",
        "stddev": "throughput_stddev",
        "ylabel": "{operation_title} Throughput (ops/s)",
        "title": "{operation_title} Throughput vs Chain Length",
        "filename": "{operation}_throughput_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
    "latency": {
        "mean": "latency_mean_ms",
        "stddev": "latency_stddev_ms",
        "ylabel": "{operation_title} Latency (ms)",
        "title": "{operation_title} Latency vs Chain Length",
        "filename": "{operation}_latency_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
    "latency_p50": {
        "mean": "latency_p50_mean_ms",
        "stddev": "latency_p50_stddev_ms",
        "ylabel": "{operation_title} P50 Latency (ms)",
        "title": "{operation_title} P50 Latency vs Chain Length",
        "filename": "{operation}_latency_p50_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
    "latency_p95": {
        "mean": "latency_p95_mean_ms",
        "stddev": "latency_p95_stddev_ms",
        "ylabel": "{operation_title} P95 Latency (ms)",
        "title": "{operation_title} P95 Latency vs Chain Length",
        "filename": "{operation}_latency_p95_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
    "latency_p99": {
        "mean": "latency_p99_mean_ms",
        "stddev": "latency_p99_stddev_ms",
        "ylabel": "{operation_title} P99 Latency (ms)",
        "title": "{operation_title} P99 Latency vs Chain Length",
        "filename": "{operation}_latency_p99_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
    "latency_percentiles": {
        "combined": "tail_latency",
        "ylabel": "{operation_title} Tail Latency (ms)",
        "title": "{operation_title} P50/P95/P99 Latency vs Chain Length",
        "filename": "{operation}_tail_latency_percentiles_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
    "read_failures": {
        "mean": "read_failures_mean",
        "stddev": "",
        "ylabel": "Read Failures",
        "title": "Read Failures vs Chain Length",
        "filename": "{operation}_read_failures_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
    "write_rpc_failures": {
        "mean": "write_rpc_failures_mean",
        "stddev": "",
        "ylabel": "Write RPC Failures",
        "title": "Write RPC Failures vs Chain Length",
        "filename": "{operation}_write_rpc_failures_clients{client_count}_keys{key_count}_ops{ops_per_client}",
    },
}


@dataclass(frozen=True)
class SummaryRow:
    experiment_id: str
    chain_length: int
    mode: str
    operation: str
    client_count: int
    key_count: int
    ops_per_client: int
    total_requested_ops: int
    trials_expected: int
    trials_completed: int
    throughput_mean: float
    throughput_stddev: float
    latency_mean_ms: float
    latency_stddev_ms: float
    latency_p50_mean_ms: float
    latency_p50_stddev_ms: float
    latency_p95_mean_ms: float
    latency_p95_stddev_ms: float
    latency_p99_mean_ms: float
    latency_p99_stddev_ms: float
    read_failures_mean: float
    write_rpc_failures_mean: float
    all_trials_complete: bool


def parse_int(raw: str, default: int = 0) -> int:
    try:
        return int(raw)
    except (TypeError, ValueError):
        return default


def parse_float(raw: str, default: float = 0.0) -> float:
    try:
        value = float(raw)
    except (TypeError, ValueError):
        return default
    if math.isnan(value) or math.isinf(value):
        return default
    return value


def parse_bool(raw: str) -> bool:
    return str(raw).strip().lower() in {"1", "true", "yes", "y", "on"}


def read_summary(path: Path) -> list[SummaryRow]:
    if not path.is_file():
        raise SystemExit(f"summary CSV not found: {path}")

    rows: list[SummaryRow] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        for record in reader:
            mode = record.get("mode", "").strip().lower()
            operation = record.get("operation", "").strip().lower()
            if mode not in MODE_STYLES or operation not in {"write", "read"}:
                continue

            ops_per_client = parse_int(
                record.get("ops_per_client", "") or record.get("op_count", "")
            )
            rows.append(
                SummaryRow(
                    experiment_id=record.get("experiment_id", ""),
                    chain_length=parse_int(record.get("chain_length", "")),
                    mode=mode,
                    operation=operation,
                    client_count=parse_int(record.get("client_count", "")),
                    key_count=parse_int(record.get("key_count", "")),
                    ops_per_client=ops_per_client,
                    total_requested_ops=parse_int(record.get("total_requested_ops", "")),
                    trials_expected=parse_int(record.get("trials_expected", "")),
                    trials_completed=parse_int(record.get("trials_completed", "")),
                    throughput_mean=parse_float(record.get("throughput_mean", "")),
                    throughput_stddev=parse_float(record.get("throughput_stddev", "")),
                    latency_mean_ms=parse_float(record.get("latency_mean_ms", "")),
                    latency_stddev_ms=parse_float(record.get("latency_stddev_ms", "")),
                    latency_p50_mean_ms=parse_float(record.get("latency_p50_mean_ms", "")),
                    latency_p50_stddev_ms=parse_float(record.get("latency_p50_stddev_ms", "")),
                    latency_p95_mean_ms=parse_float(record.get("latency_p95_mean_ms", "")),
                    latency_p95_stddev_ms=parse_float(record.get("latency_p95_stddev_ms", "")),
                    latency_p99_mean_ms=parse_float(record.get("latency_p99_mean_ms", "")),
                    latency_p99_stddev_ms=parse_float(record.get("latency_p99_stddev_ms", "")),
                    read_failures_mean=parse_float(record.get("read_failures_mean", "")),
                    write_rpc_failures_mean=parse_float(record.get("write_rpc_failures_mean", "")),
                    all_trials_complete=parse_bool(record.get("all_trials_complete", "")),
                )
            )
    return rows


def read_summary_fieldnames(path: Path) -> set[str]:
    if not path.is_file():
        raise SystemExit(f"summary CSV not found: {path}")
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        return set(reader.fieldnames or [])


def operation_title(operation: str) -> str:
    return operation.capitalize()


def safe_filename(raw: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", raw.strip())
    return safe.strip("_") or "plot"


def filter_rows(
    rows: Sequence[SummaryRow],
    *,
    operations: set[str],
    client_counts: set[int] | None,
    modes: set[str],
    only_complete: bool,
) -> list[SummaryRow]:
    out: list[SummaryRow] = []
    for row in rows:
        if row.operation not in operations:
            continue
        if client_counts is not None and row.client_count not in client_counts:
            continue
        if row.mode not in modes:
            continue
        if only_complete and not row.all_trials_complete:
            continue
        if row.trials_completed <= 0:
            continue
        out.append(row)
    return out


def group_rows(rows: Iterable[SummaryRow]) -> dict[tuple[str, int, int, int], list[SummaryRow]]:
    grouped: dict[tuple[str, int, int, int], list[SummaryRow]] = defaultdict(list)
    for row in rows:
        grouped[(row.operation, row.client_count, row.key_count, row.ops_per_client)].append(row)
    return grouped


def metric_value(row: SummaryRow, field: str) -> float:
    return {
        "throughput_mean": row.throughput_mean,
        "throughput_stddev": row.throughput_stddev,
        "latency_mean_ms": row.latency_mean_ms,
        "latency_stddev_ms": row.latency_stddev_ms,
        "latency_p50_mean_ms": row.latency_p50_mean_ms,
        "latency_p50_stddev_ms": row.latency_p50_stddev_ms,
        "latency_p95_mean_ms": row.latency_p95_mean_ms,
        "latency_p95_stddev_ms": row.latency_p95_stddev_ms,
        "latency_p99_mean_ms": row.latency_p99_mean_ms,
        "latency_p99_stddev_ms": row.latency_p99_stddev_ms,
        "read_failures_mean": row.read_failures_mean,
        "write_rpc_failures_mean": row.write_rpc_failures_mean,
    }[field]


def add_value_labels(ax, xs: Sequence[int], ys: Sequence[float], yerrs: Sequence[float]) -> None:
    if not xs:
        return
    max_top = max((y + err for y, err in zip(ys, yerrs)), default=0.0)
    offset = max(max_top * 0.025, 0.01)
    for x, y, err in zip(xs, ys, yerrs):
        ax.text(
            x,
            y + err + offset,
            f"{y:.0f}",
            ha="center",
            va="bottom",
            fontsize=8,
            fontweight="bold",
            color="black",
        )


def make_plot(
    rows: Sequence[SummaryRow],
    *,
    metric: str,
    out_dir: Path,
    formats: Sequence[str],
    dpi: int,
    show_labels: bool,
) -> list[Path]:
    metric_def = METRIC_DEFS[metric]
    operation, client_count, key_count, ops_per_client = (
        rows[0].operation,
        rows[0].client_count,
        rows[0].key_count,
        rows[0].ops_per_client,
    )
    op_title = operation_title(operation)

    by_mode: dict[str, list[SummaryRow]] = defaultdict(list)
    for row in rows:
        by_mode[row.mode].append(row)

    fig, ax = plt.subplots(figsize=(8.7, 5.25))

    max_y = 0.0
    chain_lengths: set[int] = set()
    for mode in MODE_ORDER:
        mode_rows = sorted(by_mode.get(mode, []), key=lambda r: r.chain_length)
        if not mode_rows:
            continue

        xs = [row.chain_length for row in mode_rows]
        ys = [metric_value(row, metric_def["mean"]) for row in mode_rows]
        yerrs = (
            [metric_value(row, metric_def["stddev"]) for row in mode_rows]
            if metric_def["stddev"]
            else [0.0 for _ in mode_rows]
        )
        chain_lengths.update(xs)
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
        if show_labels:
            add_value_labels(ax, xs, ys, yerrs)

    title = metric_def["title"].format(operation_title=op_title)
    ylabel = metric_def["ylabel"].format(operation_title=op_title)
    ax.set_title(title, fontsize=13, fontweight="bold")
    ax.set_xlabel("Chain Length")
    ax.set_ylabel(ylabel)
    if chain_lengths:
        ax.set_xticks(sorted(chain_lengths))
    ax.set_ylim(bottom=0, top=max(max_y * 1.18, 1.0))
    ax.grid(True, linestyle="--", alpha=0.3, linewidth=0.6)
    ax.legend(title="Protocol", loc="best", frameon=True)

    subtitle = f"clients={client_count}, keys={key_count}, ops/client={ops_per_client}"
    incomplete = sorted({row.mode for row in rows if not row.all_trials_complete})
    if incomplete:
        subtitle += f", incomplete={','.join(MODE_STYLES[m]['label'] for m in incomplete)}"
    fig.text(0.5, 0.01, subtitle, ha="center", fontsize=9)
    fig.tight_layout(rect=(0, 0.035, 1, 1))

    basename = metric_def["filename"].format(
        operation=operation,
        client_count=client_count,
        key_count=key_count,
        ops_per_client=ops_per_client,
    )
    out_paths: list[Path] = []
    for fmt in formats:
        path = out_dir / f"{safe_filename(basename)}.{fmt}"
        fig.savefig(path, dpi=dpi)
        out_paths.append(path)
    plt.close(fig)
    return out_paths


def make_tail_latency_plot(
    rows: Sequence[SummaryRow],
    *,
    series_list: Sequence[dict[str, object]],
    out_dir: Path,
    formats: Sequence[str],
    dpi: int,
) -> list[Path]:
    metric_def = METRIC_DEFS["latency_percentiles"]
    operation, client_count, key_count, ops_per_client = (
        rows[0].operation,
        rows[0].client_count,
        rows[0].key_count,
        rows[0].ops_per_client,
    )
    op_title = operation_title(operation)

    by_mode: dict[str, list[SummaryRow]] = defaultdict(list)
    for row in rows:
        by_mode[row.mode].append(row)

    fig, ax = plt.subplots(figsize=(9.3, 5.35))

    max_y = 0.0
    chain_lengths: set[int] = set()
    for mode in MODE_ORDER:
        mode_rows = sorted(by_mode.get(mode, []), key=lambda r: r.chain_length)
        if not mode_rows:
            continue

        xs = [row.chain_length for row in mode_rows]
        chain_lengths.update(xs)
        mode_style = MODE_STYLES[mode]
        for series in series_list:
            ys = [metric_value(row, series["mean"]) for row in mode_rows]
            yerrs = [metric_value(row, series["stddev"]) for row in mode_rows]
            max_y = max(max_y, max((y + err for y, err in zip(ys, yerrs)), default=0.0))
            ax.errorbar(
                xs,
                ys,
                yerr=yerrs if any(err > 0.0 for err in yerrs) else None,
                color=mode_style["color"],
                linestyle=series["linestyle"],
                marker=series["marker"],
                linewidth=series["linewidth"],
                markersize=5.6,
                capsize=3,
                elinewidth=0.9,
                alpha=0.95,
            )

    percentile_title = "/".join(str(series["label"]) for series in series_list)
    title = f"{op_title} {percentile_title} Latency vs Chain Length"
    ylabel = metric_def["ylabel"].format(operation_title=op_title)
    ax.set_title(title, fontsize=13, fontweight="bold")
    ax.set_xlabel("Chain Length")
    ax.set_ylabel(ylabel)
    if chain_lengths:
        ax.set_xticks(sorted(chain_lengths))
    ax.set_ylim(bottom=0, top=max(max_y * 1.18, 1.0))
    ax.grid(True, linestyle="--", alpha=0.3, linewidth=0.6)

    protocol_handles = [
        Line2D([0], [0], color=MODE_STYLES[mode]["color"], linewidth=2.4, label=MODE_STYLES[mode]["label"])
        for mode in MODE_ORDER
        if mode in by_mode
    ]
    percentile_handles = [
        Line2D(
            [0],
            [0],
            color="#303030",
            linestyle=series["linestyle"],
            marker=series["marker"],
            linewidth=series["linewidth"],
            markersize=5.6,
            label=series["label"],
        )
        for series in series_list
    ]
    protocol_legend = ax.legend(handles=protocol_handles, title="Protocol", loc="upper left", frameon=True)
    ax.add_artist(protocol_legend)
    ax.legend(handles=percentile_handles, title="Percentile", loc="upper right", frameon=True)

    subtitle = f"clients={client_count}, keys={key_count}, ops/client={ops_per_client}"
    incomplete = sorted({row.mode for row in rows if not row.all_trials_complete})
    if incomplete:
        subtitle += f", incomplete={','.join(MODE_STYLES[m]['label'] for m in incomplete)}"
    fig.text(0.5, 0.01, subtitle, ha="center", fontsize=9)
    fig.tight_layout(rect=(0, 0.035, 1, 1))

    basename = metric_def["filename"].format(
        operation=operation,
        client_count=client_count,
        key_count=key_count,
        ops_per_client=ops_per_client,
    )
    out_paths: list[Path] = []
    for fmt in formats:
        path = out_dir / f"{safe_filename(basename)}.{fmt}"
        fig.savefig(path, dpi=dpi)
        out_paths.append(path)
    plt.close(fig)
    return out_paths


def write_index(path: Path, rows: Sequence[dict[str, str]]) -> None:
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "plot_file",
                "operation",
                "metric",
                "client_count",
                "key_count",
                "ops_per_client",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def parse_args() -> argparse.Namespace:
    root_dir = Path(__file__).resolve().parent.parent
    default_work_dir = root_dir / "build" / "chain_length_throughput_runs"

    p = argparse.ArgumentParser(
        description="Plot chain-length experiment summaries with stable per-mode styles."
    )
    p.add_argument(
        "--summary",
        type=Path,
        default=default_work_dir / "summary_by_chain_length.csv",
        help="Input summary CSV from setup/run_chain_length_experiments.py.",
    )
    p.add_argument(
        "--out-dir",
        type=Path,
        default=default_work_dir / "plots",
        help="Directory for generated plot files.",
    )
    p.add_argument(
        "--metrics",
        nargs="+",
        choices=sorted(METRIC_DEFS),
        default=["throughput", "latency", "latency_percentiles"],
        help="Metrics to plot.",
    )
    p.add_argument(
        "--operations",
        nargs="+",
        choices=["write", "read"],
        default=["write", "read"],
        help="Operations to plot.",
    )
    p.add_argument(
        "--client-counts",
        nargs="+",
        type=int,
        default=None,
        help="Client counts to include. Defaults to all counts in the summary.",
    )
    p.add_argument(
        "--modes",
        nargs="+",
        choices=MODE_ORDER,
        default=MODE_ORDER,
        help="Modes to include. Styles stay fixed even if a subset is chosen.",
    )
    p.add_argument(
        "--formats",
        nargs="+",
        default=["png"],
        help="Output formats supported by matplotlib, e.g. png pdf svg.",
    )
    p.add_argument("--dpi", type=int, default=150)
    p.add_argument("--only-complete", action="store_true", help="Skip rows with incomplete trials.")
    p.add_argument("--no-labels", action="store_true", help="Do not draw numeric labels above points.")
    return p.parse_args()


def available_tail_latency_series(fieldnames: set[str]) -> list[dict[str, object]]:
    return [
        series for series in TAIL_LATENCY_SERIES
        if series["mean"] in fieldnames and series["stddev"] in fieldnames
    ]


def required_metric_fields(metric: str) -> list[str]:
    metric_def = METRIC_DEFS[metric]
    fields = [metric_def["mean"]]
    if metric_def["stddev"]:
        fields.append(metric_def["stddev"])
    return fields


def main() -> int:
    args = parse_args()
    summary_fieldnames = read_summary_fieldnames(args.summary)
    tail_latency_series = available_tail_latency_series(summary_fieldnames)
    metrics = []
    for metric in args.metrics:
        if METRIC_DEFS[metric].get("combined") == "tail_latency":
            if not tail_latency_series:
                print(
                    "Skipping latency_percentiles: summary CSV is missing all percentile latency columns",
                    file=sys.stderr,
                )
                continue
            omitted = [
                series["label"] for series in TAIL_LATENCY_SERIES
                if series not in tail_latency_series
            ]
            if omitted:
                included = ", ".join(series["label"] for series in tail_latency_series)
                print(
                    f"Plotting latency_percentiles with {included}; "
                    f"missing {', '.join(omitted)} in summary CSV",
                    file=sys.stderr,
                )
            metrics.append(metric)
            continue

        missing = [field for field in required_metric_fields(metric) if field not in summary_fieldnames]
        if missing:
            print(
                f"Skipping {metric}: summary CSV is missing {', '.join(missing)}",
                file=sys.stderr,
            )
            continue
        metrics.append(metric)
    if not metrics:
        raise SystemExit("none of the requested metrics exist in the summary CSV")

    rows = read_summary(args.summary)
    filtered = filter_rows(
        rows,
        operations=set(args.operations),
        client_counts=set(args.client_counts) if args.client_counts else None,
        modes=set(args.modes),
        only_complete=args.only_complete,
    )
    if not filtered:
        raise SystemExit("no summary rows matched the requested filters")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    grouped = group_rows(filtered)

    index_rows: list[dict[str, str]] = []
    for key in sorted(grouped, key=lambda k: (k[1], k[0], k[2], k[3])):
        group = grouped[key]
        operation, client_count, key_count, ops_per_client = key
        for metric in metrics:
            if METRIC_DEFS[metric].get("combined") == "tail_latency":
                paths = make_tail_latency_plot(
                    group,
                    series_list=tail_latency_series,
                    out_dir=args.out_dir,
                    formats=args.formats,
                    dpi=args.dpi,
                )
            else:
                paths = make_plot(
                    group,
                    metric=metric,
                    out_dir=args.out_dir,
                    formats=args.formats,
                    dpi=args.dpi,
                    show_labels=not args.no_labels,
                )
            for path in paths:
                index_rows.append(
                    {
                        "plot_file": str(path),
                        "operation": operation,
                        "metric": metric,
                        "client_count": str(client_count),
                        "key_count": str(key_count),
                        "ops_per_client": str(ops_per_client),
                    }
                )
                print(f"Wrote {path}")

    write_index(args.out_dir / "plot_index.csv", index_rows)
    print(f"Wrote {args.out_dir / 'plot_index.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
