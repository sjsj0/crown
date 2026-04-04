#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import IO, List, Sequence


class RunnerError(RuntimeError):
    pass


@dataclass
class StartedProcess:
    process: subprocess.Popen
    log_handle: IO[str]


@dataclass
class RunnerConfig:
    root_dir: Path
    build_dir: Path
    work_dir: Path
    log_dir: Path
    cfg_dir: Path
    base_config: Path
    client_bin: Path
    server_bin: Path
    modes: List[str]
    ops: List[str]
    client_counts: List[int]
    duration_sec: float
    write_rate_rps: float
    read_rate_rps: float
    key_count: int
    key_prefix_base: str
    value_prefix_base: str
    ack_base_port: int
    craq_read_node_id: int
    build_first: bool
    start_servers: bool
    reconfigure_each_run: bool
    summary_csv: Path


SERVER_PROCS: List[StartedProcess] = []


def log(msg: str) -> None:
    print(f"[throughput-runner] {msg}", flush=True)


def env_str(name: str, default: str) -> str:
    return os.environ.get(name, default)


def env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise RunnerError(f"invalid integer for {name}: {raw}") from exc


def env_float(name: str, default: float) -> float:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise RunnerError(f"invalid float for {name}: {raw}") from exc


def env_words(name: str, default: str) -> List[str]:
    raw = os.environ.get(name, default)
    return [item for item in raw.split() if item]


def parse_bool(raw: str, label: str) -> bool:
    value = (raw or "").strip().lower()
    if value in {"1", "true", "yes", "y", "on"}:
        return True
    if value in {"0", "false", "no", "n", "off", ""}:
        return False
    raise RunnerError(f"invalid boolean value for {label}: {raw} (expected 1/0/true/false/yes/no)")


def argparse_bool(raw: str) -> bool:
    try:
        return parse_bool(raw, "CLI argument")
    except RunnerError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def parse_args(root_dir: Path) -> argparse.Namespace:
    build_dir_default = env_str("BUILD_DIR", str(root_dir / "build"))
    work_dir_default = env_str("WORK_DIR", str(Path(build_dir_default) / "throughput_runs"))

    p = argparse.ArgumentParser(
        description="Run throughput experiments across chain/craq/crown modes.",
    )

    p.add_argument("--build-dir", default=build_dir_default)
    p.add_argument("--work-dir", default=work_dir_default)
    p.add_argument("--base-config", default=env_str("BASE_CONFIG", str(root_dir / "config.json")))
    p.add_argument("--client-bin", default=env_str("CLIENT_BIN", str(Path(build_dir_default) / "client")))
    p.add_argument("--server-bin", default=env_str("SERVER_BIN", str(Path(build_dir_default) / "server")))

    p.add_argument("--modes", nargs="+", default=env_words("MODES", "chain craq crown"))
    p.add_argument("--ops", nargs="+", default=env_words("OPS", "write read"))
    p.add_argument(
        "--client-counts",
        nargs="+",
        type=int,
        default=[int(v) for v in env_words("CLIENT_COUNTS", "1 2 4 8")],
    )

    p.add_argument("--duration-sec", type=float, default=env_float("DURATION_SEC", 10.0))
    p.add_argument("--write-rate-rps", type=float, default=env_float("WRITE_RATE_RPS", 100.0))
    p.add_argument("--read-rate-rps", type=float, default=env_float("READ_RATE_RPS", 100.0))
    p.add_argument("--key-count", type=int, default=env_int("KEY_COUNT", 64))

    p.add_argument("--key-prefix-base", default=env_str("KEY_PREFIX_BASE", "bench-key"))
    p.add_argument("--value-prefix-base", default=env_str("VALUE_PREFIX_BASE", "bench-value"))

    p.add_argument("--ack-base-port", type=int, default=env_int("ACK_BASE_PORT", 61000))
    p.add_argument("--craq-read-node-id", type=int, default=env_int("CRAQ_READ_NODE_ID", -1))

    p.add_argument(
        "--build-first",
        type=argparse_bool,
        default=parse_bool(env_str("BUILD_FIRST", "1"), "BUILD_FIRST"),
    )
    p.add_argument(
        "--start-servers",
        type=argparse_bool,
        default=parse_bool(env_str("START_SERVERS", "1"), "START_SERVERS"),
    )
    p.add_argument(
        "--reconfigure-each-run",
        type=argparse_bool,
        default=parse_bool(env_str("RECONFIGURE_EACH_RUN", "1"), "RECONFIGURE_EACH_RUN"),
    )

    return p.parse_args()


def build_config(args: argparse.Namespace, root_dir: Path) -> RunnerConfig:
    modes = [m.strip().lower() for m in args.modes if m.strip()]
    ops = [o.strip().lower() for o in args.ops if o.strip()]
    allowed_modes = {"chain", "craq", "crown"}
    allowed_ops = {"write", "read"}

    if not modes:
        raise RunnerError("--modes cannot be empty")
    if not ops:
        raise RunnerError("--ops cannot be empty")

    bad_modes = [m for m in modes if m not in allowed_modes]
    bad_ops = [o for o in ops if o not in allowed_ops]
    if bad_modes:
        raise RunnerError(f"unknown modes: {' '.join(bad_modes)}")
    if bad_ops:
        raise RunnerError(f"unknown ops: {' '.join(bad_ops)}")

    if not args.client_counts:
        raise RunnerError("--client-counts cannot be empty")
    if any(n <= 0 for n in args.client_counts):
        raise RunnerError("--client-counts values must be positive")

    if args.duration_sec <= 0:
        raise RunnerError("--duration-sec must be > 0")
    if args.write_rate_rps <= 0:
        raise RunnerError("--write-rate-rps must be > 0")
    if args.read_rate_rps <= 0:
        raise RunnerError("--read-rate-rps must be > 0")
    if args.key_count <= 0:
        raise RunnerError("--key-count must be > 0")
    if not (1 <= args.ack_base_port <= 65535):
        raise RunnerError("--ack-base-port must be in [1, 65535]")

    build_dir = Path(args.build_dir)
    work_dir = Path(args.work_dir)
    log_dir = work_dir / "logs"
    cfg_dir = work_dir / "configs"
    summary_csv = work_dir / "summary.csv"

    return RunnerConfig(
        root_dir=root_dir,
        build_dir=build_dir,
        work_dir=work_dir,
        log_dir=log_dir,
        cfg_dir=cfg_dir,
        base_config=Path(args.base_config),
        client_bin=Path(args.client_bin),
        server_bin=Path(args.server_bin),
        modes=modes,
        ops=ops,
        client_counts=list(args.client_counts),
        duration_sec=args.duration_sec,
        write_rate_rps=args.write_rate_rps,
        read_rate_rps=args.read_rate_rps,
        key_count=args.key_count,
        key_prefix_base=args.key_prefix_base,
        value_prefix_base=args.value_prefix_base,
        ack_base_port=args.ack_base_port,
        craq_read_node_id=args.craq_read_node_id,
        build_first=args.build_first,
        start_servers=args.start_servers,
        reconfigure_each_run=args.reconfigure_each_run,
        summary_csv=summary_csv,
    )


def wait_for_port(host: str, port: int, timeout_sec: float) -> bool:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.settimeout(0.2)
            sock.connect((host, port))
            return True
        except OSError:
            time.sleep(0.1)
        finally:
            sock.close()
    return False


def run_cmd(args: Sequence[str], quiet: bool = False) -> None:
    kwargs = {}
    if quiet:
        kwargs["stdout"] = subprocess.DEVNULL
        kwargs["stderr"] = subprocess.DEVNULL
    try:
        subprocess.run(args, check=True, **kwargs)
    except subprocess.CalledProcessError as exc:
        joined = " ".join(args)
        raise RunnerError(f"command failed ({exc.returncode}): {joined}") from exc


def cleanup_servers() -> None:
    for sp in SERVER_PROCS:
        if sp.process.poll() is None:
            sp.process.terminate()

    for sp in SERVER_PROCS:
        if sp.process.poll() is None:
            try:
                sp.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                sp.process.kill()
                sp.process.wait(timeout=5)

    for sp in SERVER_PROCS:
        try:
            sp.log_handle.close()
        except Exception:
            pass

    SERVER_PROCS.clear()


def install_signal_handlers() -> None:
    def _handle_signal(signum: int, _frame) -> None:  # type: ignore[override]
        cleanup_servers()
        sys.exit(128 + signum)

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)


def prepare_mode_configs(base_config: Path, cfg_chain: Path, cfg_craq: Path) -> tuple[str, int, int]:
    with base_config.open("r", encoding="utf-8") as f:
        cfg = json.load(f)

    nodes = cfg.get("nodes", [])
    if not isinstance(nodes, list) or not nodes:
        raise RunnerError("invalid base config: missing non-empty nodes array")

    ports = [int(node["port"]) for node in nodes]
    base_port = min(ports)
    expected = list(range(base_port, base_port + len(ports)))
    if sorted(ports) != expected:
        raise RunnerError("base config ports must be contiguous for crown generation")

    cfg_chain_data = dict(cfg)
    cfg_chain_data["mode"] = "chain"

    cfg_craq_data = dict(cfg)
    cfg_craq_data["mode"] = "craq"

    with cfg_chain.open("w", encoding="utf-8") as f:
        json.dump(cfg_chain_data, f, indent=2)
        f.write("\n")

    with cfg_craq.open("w", encoding="utf-8") as f:
        json.dump(cfg_craq_data, f, indent=2)
        f.write("\n")

    host = str(nodes[0]["host"])
    node_count = len(nodes)
    return host, base_port, node_count


def ensure_executable(path: Path, label: str) -> None:
    if not path.is_file() or not os.access(path, os.X_OK):
        raise RunnerError(f"missing {label}: {path}")


def start_servers(server_bin: Path, work_dir: Path, host: str, base_port: int, node_count: int) -> None:
    log(f"Starting {node_count} server processes...")

    for i in range(node_count):
        port = base_port + i
        server_log = work_dir / f"server_{port}.log"
        log_handle = server_log.open("w", encoding="utf-8")

        process = subprocess.Popen(
            [str(server_bin), "--port", str(port)],
            stdout=log_handle,
            stderr=subprocess.STDOUT,
        )
        SERVER_PROCS.append(StartedProcess(process=process, log_handle=log_handle))

    time.sleep(1)

    for i in range(node_count):
        port = base_port + i
        process = SERVER_PROCS[i].process

        if process.poll() is not None:
            raise RunnerError(f"server process died during startup (pid={process.pid})")

        if not wait_for_port(host, port, timeout_sec=10):
            raise RunnerError(f"server did not open {host}:{port} within startup timeout")


def run_clients_for_case(
    cfg: RunnerConfig,
    cfg_path: Path,
    mode: str,
    op: str,
    nclients: int,
    run_idx: int,
) -> None:
    log(f"Running mode={mode} op={op} clients={nclients} duration={cfg.duration_sec}s")

    started: List[StartedProcess] = []

    try:
        for i in range(nclients):
            ack_port = cfg.ack_base_port + run_idx * 100 + i
            configure_flag = "true" if cfg.reconfigure_each_run and i == 0 else "false"

            key_prefix = f"{cfg.key_prefix_base}-{mode}-{op}-"
            value_prefix = f"{cfg.value_prefix_base}-{mode}-{op}-"
            log_file = cfg.log_dir / f"{mode}_{op}_c{nclients}_i{i}.log"
            log_handle = log_file.open("w", encoding="utf-8")

            cmd = [
                str(cfg.client_bin),
                str(cfg_path),
                configure_flag,
                str(ack_port),
            ]

            if op == "write":
                cmd.extend(
                    [
                        "bench-write",
                        str(cfg.duration_sec),
                        str(cfg.write_rate_rps),
                        str(cfg.key_count),
                        str(i),
                        str(nclients),
                        key_prefix,
                        value_prefix,
                    ]
                )
            elif op == "read":
                cmd.extend(
                    [
                        "bench-read",
                        str(cfg.duration_sec),
                        str(cfg.read_rate_rps),
                        str(cfg.key_count),
                        str(i),
                        str(nclients),
                        str(cfg.craq_read_node_id),
                        key_prefix,
                    ]
                )
            else:
                raise RunnerError(f"unknown op in OPS: {op}")

            process = subprocess.Popen(cmd, stdout=log_handle, stderr=subprocess.STDOUT)
            started.append(StartedProcess(process=process, log_handle=log_handle))

        failed = False
        for sp in started:
            if sp.process.wait() != 0:
                failed = True

        if failed:
            raise RunnerError(f"one or more client processes failed for mode={mode} op={op} clients={nclients}")
    finally:
        for sp in started:
            try:
                sp.log_handle.close()
            except Exception:
                pass


def main() -> int:
    install_signal_handlers()

    root_dir = Path(__file__).resolve().parent.parent
    args = parse_args(root_dir)
    cfg = build_config(args, root_dir)

    cfg.log_dir.mkdir(parents=True, exist_ok=True)
    cfg.cfg_dir.mkdir(parents=True, exist_ok=True)

    if not cfg.base_config.is_file():
        raise RunnerError(f"missing base config: {cfg.base_config}")

    if cfg.build_first:
        log("Building project...")
        run_cmd(["cmake", "-S", str(cfg.root_dir), "-B", str(cfg.build_dir)], quiet=True)
        run_cmd(["cmake", "--build", str(cfg.build_dir), "-j4"], quiet=True)

    ensure_executable(cfg.client_bin, "client binary")
    ensure_executable(cfg.server_bin, "server binary")

    cfg_chain = cfg.cfg_dir / "config.chain.json"
    cfg_craq = cfg.cfg_dir / "config.craq.json"
    cfg_crown = cfg.cfg_dir / "config.crown.json"

    log(f"Preparing CHAIN/CRAQ configs from {cfg.base_config}...")
    host, base_port, node_count = prepare_mode_configs(cfg.base_config, cfg_chain, cfg_craq)

    log(f"Generating CROWN config (host={host} base_port={base_port} nodes={node_count})...")
    run_cmd(
        [
            sys.executable,
            str(cfg.root_dir / "setup" / "generate_crown_config.py"),
            str(node_count),
            str(base_port),
            "--host",
            host,
            "--output",
            str(cfg_crown),
        ],
        quiet=True,
    )

    if cfg.start_servers:
        start_servers(cfg.server_bin, cfg.work_dir, host, base_port, node_count)

    run_idx = 0
    mode_to_cfg = {
        "chain": cfg_chain,
        "craq": cfg_craq,
        "crown": cfg_crown,
    }

    for mode in cfg.modes:
        cfg_path = mode_to_cfg.get(mode)
        if cfg_path is None:
            raise RunnerError(f"unknown mode in MODES: {mode}")

        for op in cfg.ops:
            for nclients in cfg.client_counts:
                run_clients_for_case(
                    cfg=cfg,
                    cfg_path=cfg_path,
                    mode=mode,
                    op=op,
                    nclients=nclients,
                    run_idx=run_idx,
                )
                run_idx += 1

    log("Aggregating BENCH_SUMMARY lines into CSV...")
    run_cmd(
        [
            sys.executable,
            str(cfg.root_dir / "setup" / "aggregate_bench_results.py"),
            "--logs-dir",
            str(cfg.log_dir),
            "--output",
            str(cfg.summary_csv),
        ]
    )

    log("Completed.")
    log(f"Logs: {cfg.log_dir}")
    log(f"Summary: {cfg.summary_csv}")

    cleanup_servers()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunnerError as exc:
        print(f"[throughput-runner] ERROR: {exc}", file=sys.stderr)
        cleanup_servers()
        raise SystemExit(1)
