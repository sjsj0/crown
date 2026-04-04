#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


SUMMARY_PREFIX = "BENCH_SUMMARY"
TAG_RE = re.compile(r"^(bench-write|bench-read):([a-z]+):c(\d+)/(\d+)$")


@dataclass
class SummaryRecord:
    operation: str
    mode: str
    client_index: int
    num_clients: int
    duration_s: float
    writes_sent: int
    acks_received: int
    reads_sent: int
    reads_ok: int
    read_failures: int
    write_rpc_failures: int
    ack_wps: float
    read_req_rps: float
    read_resp_rps: float
    avg_ack_latency_ms: float
    source_file: str


def parse_summary_line(line: str, source_file: str) -> SummaryRecord | None:
    line = line.strip()
    marker = SUMMARY_PREFIX + " "
    idx = line.find(marker)
    if idx < 0:
        return None
    line = line[idx:]

    fields: dict[str, str] = {}
    for token in line[len(SUMMARY_PREFIX):].strip().split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value

    tag = fields.get("tag", "")
    m = TAG_RE.match(tag)
    if not m:
        raise ValueError(f"invalid BENCH_SUMMARY tag '{tag}' in {source_file}")

    operation, mode, client_index_text, num_clients_text = m.groups()

    def int_field(name: str) -> int:
        return int(fields.get(name, "0"))

    def float_field(name: str) -> float:
        return float(fields.get(name, "0"))

    return SummaryRecord(
        operation=operation,
        mode=mode,
        client_index=int(client_index_text),
        num_clients=int(num_clients_text),
        duration_s=float_field("duration_s"),
        writes_sent=int_field("writes_sent"),
        acks_received=int_field("acks_received"),
        reads_sent=int_field("reads_sent"),
        reads_ok=int_field("reads_ok"),
        read_failures=int_field("read_failures"),
        write_rpc_failures=int_field("write_rpc_failures"),
        ack_wps=float_field("ack_wps"),
        read_req_rps=float_field("read_req_rps"),
        read_resp_rps=float_field("read_resp_rps"),
        avg_ack_latency_ms=float_field("avg_ack_latency_ms"),
        source_file=source_file,
    )


def collect_records(logs_dir: Path) -> list[SummaryRecord]:
    records: list[SummaryRecord] = []
    for path in sorted(logs_dir.glob("*.log")):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            rec = parse_summary_line(line, str(path))
            if rec is not None:
                records.append(rec)
    return records


def write_aggregate_csv(records: list[SummaryRecord], output: Path) -> None:
    grouped: dict[tuple[str, str, int], list[SummaryRecord]] = defaultdict(list)
    for rec in records:
        grouped[(rec.operation, rec.mode, rec.num_clients)].append(rec)

    mode_order = {"chain": 0, "craq": 1, "crown": 2}
    op_order = {"bench-write": 0, "bench-read": 1}

    rows = []
    for (operation, mode, num_clients), recs in grouped.items():
        clients_reported = len(recs)
        duration_avg = sum(r.duration_s for r in recs) / clients_reported if clients_reported else 0.0

        writes_sent = sum(r.writes_sent for r in recs)
        acks_received = sum(r.acks_received for r in recs)
        reads_sent = sum(r.reads_sent for r in recs)
        reads_ok = sum(r.reads_ok for r in recs)
        read_failures = sum(r.read_failures for r in recs)
        write_rpc_failures = sum(r.write_rpc_failures for r in recs)

        agg_ack_wps = sum(r.ack_wps for r in recs)
        agg_read_req_rps = sum(r.read_req_rps for r in recs)
        agg_read_resp_rps = sum(r.read_resp_rps for r in recs)

        if acks_received > 0:
            weighted_ack_latency_ms = (
                sum(r.avg_ack_latency_ms * r.acks_received for r in recs) / acks_received
            )
        else:
            weighted_ack_latency_ms = 0.0

        complete = clients_reported == num_clients

        rows.append(
            {
                "operation": operation,
                "mode": mode,
                "num_clients": num_clients,
                "clients_reported": clients_reported,
                "complete": complete,
                "duration_s_avg": duration_avg,
                "writes_sent": writes_sent,
                "acks_received": acks_received,
                "reads_sent": reads_sent,
                "reads_ok": reads_ok,
                "read_failures": read_failures,
                "write_rpc_failures": write_rpc_failures,
                "agg_ack_wps": agg_ack_wps,
                "agg_read_req_rps": agg_read_req_rps,
                "agg_read_resp_rps": agg_read_resp_rps,
                "weighted_avg_ack_latency_ms": weighted_ack_latency_ms,
            }
        )

    rows.sort(key=lambda r: (op_order.get(r["operation"], 99), mode_order.get(r["mode"], 99), r["num_clients"]))

    output.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "operation",
        "mode",
        "num_clients",
        "clients_reported",
        "complete",
        "duration_s_avg",
        "writes_sent",
        "acks_received",
        "reads_sent",
        "reads_ok",
        "read_failures",
        "write_rpc_failures",
        "agg_ack_wps",
        "agg_read_req_rps",
        "agg_read_resp_rps",
        "weighted_avg_ack_latency_ms",
    ]

    with output.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Aggregate BENCH_SUMMARY lines from throughput logs")
    p.add_argument("--logs-dir", required=True, help="Directory containing per-client *.log files")
    p.add_argument("--output", required=True, help="CSV output path")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    logs_dir = Path(args.logs_dir)
    output = Path(args.output)

    if not logs_dir.is_dir():
        raise SystemExit(f"logs directory not found: {logs_dir}")

    records = collect_records(logs_dir)
    if not records:
        raise SystemExit("no BENCH_SUMMARY lines found in logs")

    write_aggregate_csv(records, output)
    print(f"Wrote aggregate summary: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
