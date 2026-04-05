#!/usr/bin/env python3
"""Generate CHAIN, CRAQ, and CROWN configs for a given node count.

Examples:
  python3 setup/generate_mode_configs.py 3 --base-port 50051 --env dev
  python3 setup/generate_mode_configs.py 3 --base-port 50051 --env prod --output-dir configs
  python3 setup/generate_mode_configs.py 3 --base-port 50051 --host 127.0.0.1 --output-dir configs --prefix config --modes crown
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


PROD_HOST_PREFIX = "sp26-cs525-"
PROD_HOST_START = 1201
PROD_HOST_SUFFIX = ".cs.illinois.edu"


def endpoint(host: str, port: int) -> str:
    return f"{host}:{port}"


def host_for_index(index: int, env_name: str) -> str:
    if env_name == "dev":
        return "127.0.0.1"
    return f"{PROD_HOST_PREFIX}{PROD_HOST_START + index}{PROD_HOST_SUFFIX}"


def build_hosts(node_count: int, env_name: str, host_override: str | None = None) -> list[str]:
    if host_override:
        return [host_override for _ in range(node_count)]
    return [host_for_index(i, env_name) for i in range(node_count)]


def build_crown_config(
    node_count: int,
    base_port: int,
    env_name: str,
    host_override: str | None = None,
) -> dict:
    hosts = build_hosts(node_count, env_name, host_override)
    endpoints = [endpoint(hosts[i], base_port + i) for i in range(node_count)]

    nodes = []
    for i in range(node_count):
        predecessor_index = (i - 1) % node_count
        successor_index = (i + 1) % node_count

        nodes.append(
            {
                "id": i,
                "host": hosts[i],
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


def build_linear_config(
    mode: str,
    node_count: int,
    base_port: int,
    env_name: str,
    host_override: str | None = None,
) -> dict:
    hosts = build_hosts(node_count, env_name, host_override)
    endpoints = [endpoint(hosts[i], base_port + i) for i in range(node_count)]

    nodes = []
    for i in range(node_count):
        predecessor = endpoints[i - 1] if i > 0 else None
        successor = endpoints[i + 1] if i + 1 < node_count else None

        nodes.append(
            {
                "id": i,
                "host": hosts[i],
                "port": base_port + i,
                "is_head": i == 0,
                "is_tail": i == node_count - 1,
                "predecessor": predecessor,
                "successor": successor,
            }
        )

    return {
        "mode": mode,
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

    if args.host is not None and not args.host.strip():
        raise ValueError("host override must be non-empty when provided")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate CHAIN/CRAQ/CROWN config files with dev/prod host patterns."
    )
    parser.add_argument("node_count", type=int, help="Number of nodes")
    parser.add_argument(
        "--base-port",
        type=int,
        default=50051,
        help="Starting port for node 0 (default: 50051)",
    )
    parser.add_argument(
        "--env",
        choices=["dev", "prod"],
        default="dev",
        help=(
            "Host environment: dev=127.0.0.1 for all nodes, "
            "prod=sp26-cs525-1201.cs.illinois.edu, 1202, ..."
        ),
    )
    parser.add_argument(
        "--host",
        default=None,
        help=(
            "Optional override host for all nodes. "
            "If provided, this takes precedence over --env host generation."
        ),
    )
    parser.add_argument(
        "--modes",
        nargs="+",
        choices=["chain", "craq", "crown"],
        default=["chain", "craq", "crown"],
        help="Which mode config files to generate (default: chain craq crown)",
    )
    parser.add_argument(
        "--output-dir",
        default=".",
        help="Directory where config files will be written (default: current directory)",
    )
    parser.add_argument(
        "--prefix",
        default="config",
        help="Output filename prefix (default: config)",
    )
    return parser.parse_args()


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    validate_args(args)

    output_dir = Path(args.output_dir)

    mode_to_builder = {
        "chain": lambda: build_linear_config("chain", args.node_count, args.base_port, args.env, args.host),
        "craq": lambda: build_linear_config("craq", args.node_count, args.base_port, args.env, args.host),
        "crown": lambda: build_crown_config(args.node_count, args.base_port, args.env, args.host),
    }

    written_paths: list[Path] = []
    for mode in args.modes:
        payload = mode_to_builder[mode]()
        path = output_dir / f"{args.prefix}.{mode}.json"
        write_json(path, payload)
        written_paths.append(path)

    host_desc = args.host if args.host else args.env
    print(
        "Wrote mode configs: "
        f"{', '.join(str(p) for p in written_paths)} "
        f"(nodes={args.node_count}, ports={args.base_port}-{args.base_port + args.node_count - 1}, hosts={host_desc})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
