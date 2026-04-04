#!/usr/bin/env python3
"""Generate a valid CROWN ring config.json.

Example:
  python3 setup/generate_crown_config.py 3 50051 --output config.crown.sample.json
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def endpoint(host: str, port: int) -> str:
    return f"{host}:{port}"


def build_config(node_count: int, base_port: int, host: str) -> dict:
    endpoints = [endpoint(host, base_port + i) for i in range(node_count)]

    nodes = []
    for i in range(node_count):
        predecessor_index = (i - 1) % node_count
        successor_index = (i + 1) % node_count

        nodes.append(
            {
                "id": i,
                "host": host,
                "port": base_port + i,
                "is_head": False,
                "is_tail": False,
                "predecessor": endpoints[predecessor_index],
                "successor": endpoints[successor_index],
            }
        )

    return {
        "mode": "crown",
        "nodes": nodes,
    }


def validate_args(args: argparse.Namespace) -> None:
    if args.node_count < 1:
        raise ValueError("node_count must be >= 1")

    if args.base_port < 1 or args.base_port > 65535:
        raise ValueError("base_port must be in [1, 65535]")

    last_port = args.base_port + args.node_count - 1
    if last_port > 65535:
        raise ValueError(
            f"port range exceeds 65535: base_port={args.base_port}, "
            f"node_count={args.node_count}"
        )

    if not args.host:
        raise ValueError("host must be non-empty")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate a closed CROWN ring config for modulo-based key ownership."
        )
    )
    parser.add_argument("node_count", type=int, help="Number of ring nodes")
    parser.add_argument("base_port", type=int, help="Starting port for node 0")
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="Host for all generated nodes (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--output",
        default="config.crown.sample.json",
        help="Output config path (default: config.crown.sample.json)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    validate_args(args)

    config = build_config(args.node_count, args.base_port, args.host)

    output_path = Path(args.output)
    if output_path.parent and str(output_path.parent) != ".":
        output_path.parent.mkdir(parents=True, exist_ok=True)

    output_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")

    print(
        f"Wrote CROWN config to {output_path} "
        f"(nodes={args.node_count}, ports={args.base_port}-{args.base_port + args.node_count - 1})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
