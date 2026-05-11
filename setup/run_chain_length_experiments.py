#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence


class RunnerError(RuntimeError):
    pass


RAW_FIELDNAMES = [
    "experiment_id",
    "group_experiment_id",
    "chain_length",
    "mode",
    "operation",
    "client_count",
    "trial",
    "key_count",
    "op_count",
    "server_hosts",
    "client_hosts",
    "config_file",
    "remote_config_file",
    "case_work_dir",
    "source_summary",
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
    "throughput_ops_per_sec",
    "weighted_avg_ack_latency_ms",
    "complete",
    "status",
    "error",
]


SUMMARY_FIELDNAMES = [
    "experiment_id",
    "chain_length",
    "mode",
    "operation",
    "client_count",
    "key_count",
    "op_count",
    "trials_expected",
    "trials_completed",
    "throughput_mean",
    "throughput_stddev",
    "latency_mean_ms",
    "latency_stddev_ms",
    "read_failures_mean",
    "write_rpc_failures_mean",
    "all_trials_complete",
]


@dataclass(frozen=True)
class ExperimentConfig:
    root_dir: Path
    setup_dir: Path
    work_dir: Path
    configs_dir: Path
    cases_dir: Path
    lifecycle_log_dir: Path
    raw_csv: Path
    summary_csv: Path

    server_hosts: List[str]
    client_hosts: List[str]
    teardown_hosts: List[str]

    chain_lengths: List[int]
    modes: List[str]
    client_counts: List[int]
    ops: List[str]
    trials: int

    key_count: int
    write_op_count: int
    read_op_count: int
    craq_read_node_id: int
    crown_hot_head_pct: int
    read_hot_key_pct: int

    ssh_user: str
    ssh_opts: List[str]

    repo_url: str
    repo_branch: str
    remote_base_dir: str
    repo_name: str
    project_subdir: str
    project_mode: str
    build_type: str
    remote_repo_dir: str
    remote_client_bin: str

    metadata_host: str
    metadata_port: int
    metadata_addr: str
    metadata_bind_host: str
    metadata_ping_interval_ms: int
    metadata_ping_timeout_ms: int
    metadata_failure_threshold: int

    node_host: str
    node_port: int
    server_log: str
    tmux_session_name: str
    tmux_metadata_session_name: str
    tmux_socket: str
    run_scope: str

    remote_config_dir: str
    remote_log_dir: str
    ack_base_port: int
    stabilization_seconds: float
    dry_run: bool
    fail_fast: bool


@dataclass(frozen=True)
class ExperimentCase:
    index: int
    chain_length: int
    mode: str
    client_count: int
    trial: int
    op: str

    @property
    def experiment_id(self) -> str:
        return (
            f"chainlen_n{self.chain_length}_mode_{self.mode}"
            f"_clients{self.client_count}_op_{self.op}_trial{self.trial}"
        )

    @property
    def group_experiment_id(self) -> str:
        return (
            f"chainlen_n{self.chain_length}_mode_{self.mode}"
            f"_clients{self.client_count}_op_{self.op}"
        )


def log(msg: str) -> None:
    print(f"[chain-length-runner] {msg}", flush=True)


def shell_quote(s: str) -> str:
    return shlex.quote(s)


def format_shell_cmd(args: Sequence[str]) -> str:
    return " ".join(shell_quote(a) for a in args)


def parse_bool(raw: str, label: str) -> bool:
    value = (raw or "").strip().lower()
    if value in {"1", "true", "yes", "y", "on"}:
        return True
    if value in {"0", "false", "no", "n", "off", ""}:
        return False
    raise RunnerError(f"invalid boolean for {label}: {raw}")


def argparse_bool(raw: str) -> bool:
    try:
        return parse_bool(raw, "CLI argument")
    except RunnerError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def load_dotenv(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values

    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :].strip()
        if "=" not in line:
            continue

        key, raw_value = line.split("=", 1)
        key = key.strip()
        if not key:
            continue

        raw_value = raw_value.strip()
        try:
            parsed = shlex.split(raw_value, comments=True, posix=True)
            values[key] = parsed[0] if parsed else ""
        except ValueError:
            values[key] = raw_value.strip("'\"")
    return values


def env_get(dotenv: dict[str, str], name: str, default: str = "") -> str:
    return os.environ.get(name, dotenv.get(name, default))


def env_int(dotenv: dict[str, str], name: str, default: int) -> int:
    raw = env_get(dotenv, name, "")
    if raw == "":
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise RunnerError(f"invalid integer for {name}: {raw}") from exc


def env_float(dotenv: dict[str, str], name: str, default: float) -> float:
    raw = env_get(dotenv, name, "")
    if raw == "":
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise RunnerError(f"invalid float for {name}: {raw}") from exc


def env_words(dotenv: dict[str, str], name: str, default: str) -> List[str]:
    raw = env_get(dotenv, name, default)
    return [item for item in raw.split() if item]


def parse_hosts_text(text: str) -> List[str]:
    hosts: List[str] = []
    for line in text.splitlines():
        cleaned = line.split("#", 1)[0].strip()
        if not cleaned:
            continue
        for token in cleaned.split(","):
            host = token.strip()
            if host:
                hosts.append(host)
    return hosts


def unique_preserve_order(items: Iterable[str]) -> List[str]:
    seen = set()
    out: List[str] = []
    for item in items:
        if item in seen:
            continue
        seen.add(item)
        out.append(item)
    return out


def read_hosts_arg_or_file(raw_hosts: str, hosts_file: Path) -> List[str]:
    hosts: List[str] = []
    if raw_hosts:
        hosts.extend(parse_hosts_text(raw_hosts.replace(",", "\n")))
    elif hosts_file.is_file():
        hosts.extend(parse_hosts_text(hosts_file.read_text(encoding="utf-8", errors="replace")))
    return unique_preserve_order(hosts)


def repo_name_from_url(repo_url: str) -> str:
    name = repo_url.rstrip("/").rsplit("/", 1)[-1]
    if name.endswith(".git"):
        name = name[:-4]
    return name or "crown"


def remote_join(*parts: str) -> str:
    cleaned: List[str] = []
    absolute = False
    for idx, part in enumerate(parts):
        if not part:
            continue
        if idx == 0 and part.startswith("/"):
            absolute = True
        cleaned.append(part.strip("/"))
    joined = "/".join(cleaned)
    return "/" + joined if absolute else joined


def remote_project_dir(remote_base_dir: str, repo_name: str, project_subdir: str) -> str:
    if project_subdir in {"", "."}:
        return remote_join(remote_base_dir, repo_name)
    return remote_join(remote_base_dir, repo_name, project_subdir)


def split_metadata_addr(metadata: str) -> tuple[str, int]:
    if ":" not in metadata:
        raise RunnerError(f"metadata endpoint must be host:port, got: {metadata}")
    host, port_text = metadata.rsplit(":", 1)
    if not host:
        raise RunnerError(f"metadata endpoint is missing host: {metadata}")
    try:
        port = int(port_text)
    except ValueError as exc:
        raise RunnerError(f"metadata endpoint has invalid port: {metadata}") from exc
    return host, port


def build_ssh_opts(ssh_key: str) -> List[str]:
    opts = [
        "-o",
        "BatchMode=yes",
        "-o",
        "IdentitiesOnly=yes",
        "-o",
        "StrictHostKeyChecking=accept-new",
    ]
    if ssh_key:
        key_path = Path(os.path.expanduser(ssh_key))
        if not key_path.is_file():
            raise RunnerError(f"ssh key not found: {ssh_key}")
        opts = ["-i", str(key_path)] + opts
    return opts


def parse_args(root_dir: Path, dotenv: dict[str, str]) -> argparse.Namespace:
    default_work_dir = env_get(dotenv, "CHAIN_LENGTH_WORK_DIR", str(root_dir / "build" / "chain_length_throughput_runs"))
    default_server_hosts_file = env_get(dotenv, "SERVER_HOSTS_FILE", str(root_dir / "setup" / "prod_hosts.csv"))
    default_client_hosts_file = env_get(dotenv, "CLIENT_HOSTS_FILE", str(root_dir / "setup" / "client_hosts.csv"))

    p = argparse.ArgumentParser(
        description="Run chain-length throughput experiments across chain/craq/crown."
    )
    p.add_argument("--work-dir", default=default_work_dir)
    p.add_argument("--chain-lengths", nargs="+", type=int, default=[int(x) for x in env_words(dotenv, "CHAIN_LENGTHS", "3 5 7")])
    p.add_argument("--modes", nargs="+", default=env_words(dotenv, "MODES", "chain craq crown"))
    p.add_argument("--client-counts", nargs="+", type=int, default=[int(x) for x in env_words(dotenv, "CLIENT_COUNTS", "1 3 5")])
    p.add_argument("--ops", nargs="+", default=env_words(dotenv, "OPS", "write read"))
    p.add_argument("--trials", type=int, default=env_int(dotenv, "TRIALS", 3))

    p.add_argument("--key-count", type=int, default=env_int(dotenv, "KEY_COUNT", 64))
    p.add_argument("--write-op-count", type=int, default=env_int(dotenv, "WRITE_OP_COUNT", 50000))
    p.add_argument("--read-op-count", type=int, default=env_int(dotenv, "READ_OP_COUNT", 50000))
    p.add_argument("--craq-read-node-id", type=int, default=env_int(dotenv, "CRAQ_READ_NODE_ID", -1))
    p.add_argument("--crown-hot-head-pct", type=int, default=env_int(dotenv, "CROWN_HOT_HEAD_PCT", 0))
    p.add_argument("--read-hot-key-pct", type=int, default=env_int(dotenv, "READ_HOT_KEY_PCT", 0))

    p.add_argument("--server-hosts", default=env_get(dotenv, "SERVER_HOSTS", ""))
    p.add_argument("--server-hosts-file", default=default_server_hosts_file)
    p.add_argument("--client-hosts", default=env_get(dotenv, "CLIENT_HOSTS", ""))
    p.add_argument("--client-hosts-file", default=default_client_hosts_file)

    p.add_argument("--ssh-user", default=env_get(dotenv, "SSH_USER", ""))
    p.add_argument("--ssh-key", default=env_get(dotenv, "SSH_KEY_LOCAL", ""))
    p.add_argument("--repo-url", default=env_get(dotenv, "REPO_URL", ""))
    p.add_argument("--repo-branch", default=env_get(dotenv, "REPO_BRANCH", "main"))
    p.add_argument("--remote-base-dir", default=env_get(dotenv, "REMOTE_BASE_DIR", "/home"))
    p.add_argument("--repo-name", default=env_get(dotenv, "REPO_NAME", ""))
    p.add_argument("--project-subdir", default=env_get(dotenv, "PROJECT_SUBDIR", "."))
    p.add_argument("--project-mode", default=env_get(dotenv, "PROJECT_MODE", "crown"))
    p.add_argument("--build-type", default=env_get(dotenv, "BUILD_TYPE", "Release"))
    p.add_argument("--remote-repo-dir", default=env_get(dotenv, "REMOTE_REPO_DIR", ""))
    p.add_argument("--remote-client-bin", default=env_get(dotenv, "REMOTE_CLIENT_BIN", "build/client"))

    p.add_argument("--metadata", default=env_get(dotenv, "METADATA_ADDR", ""))
    p.add_argument("--metadata-host", default=env_get(dotenv, "METADATA_HOST", ""))
    p.add_argument("--metadata-port", type=int, default=env_int(dotenv, "METADATA_PORT", 50050))
    p.add_argument("--metadata-bind-host", default=env_get(dotenv, "METADATA_BIND_HOST", "0.0.0.0"))
    p.add_argument("--metadata-ping-interval-ms", type=int, default=env_int(dotenv, "METADATA_PING_INTERVAL_MS", 1000))
    p.add_argument("--metadata-ping-timeout-ms", type=int, default=env_int(dotenv, "METADATA_PING_TIMEOUT_MS", 500))
    p.add_argument("--metadata-failure-threshold", type=int, default=env_int(dotenv, "METADATA_FAILURE_THRESHOLD", 3))

    p.add_argument("--node-host", default=env_get(dotenv, "NODE_HOST", "0.0.0.0"))
    p.add_argument("--node-port", type=int, default=env_int(dotenv, "NODE_PORT", 50051))
    p.add_argument("--server-log", default=env_get(dotenv, "SERVER_LOG", "true"))
    p.add_argument("--tmux-session-name", default=env_get(dotenv, "TMUX_SESSION_NAME", ""))
    p.add_argument("--tmux-metadata-session-name", default=env_get(dotenv, "TMUX_METADATA_SESSION_NAME", ""))
    p.add_argument("--tmux-socket", default=env_get(dotenv, "TMUX_SOCKET", "/tmp/crown-shared/tmux.sock"))
    p.add_argument("--run-scope", default=env_get(dotenv, "RUN_SCOPE", "shared"))

    p.add_argument("--remote-config-dir", default=env_get(dotenv, "REMOTE_CONFIG_DIR", "build/chain_length_throughput_runs/configs"))
    p.add_argument("--remote-log-dir", default=env_get(dotenv, "REMOTE_LOG_DIR", "build/chain_length_throughput_runs/client_logs"))
    p.add_argument("--ack-base-port", type=int, default=env_int(dotenv, "ACK_BASE_PORT", 61000))
    p.add_argument("--stabilization-seconds", type=float, default=env_float(dotenv, "STABILIZATION_SECONDS", 3.0))

    p.add_argument("--fail-fast", type=argparse_bool, default=parse_bool(env_get(dotenv, "FAIL_FAST", "1"), "FAIL_FAST"))
    p.add_argument("--keep-going", action="store_true", help="Continue after failed benchmark cases.")
    p.add_argument("--dry-run", action="store_true", help="Print the full plan and commands without SSH/SCP or benchmark execution.")
    return p.parse_args()


def build_config(args: argparse.Namespace, root_dir: Path) -> ExperimentConfig:
    setup_dir = root_dir / "setup"
    work_dir = Path(args.work_dir)

    modes = [m.strip().lower() for m in args.modes if m.strip()]
    ops = [o.strip().lower() for o in args.ops if o.strip()]
    allowed_modes = {"chain", "craq", "crown"}
    allowed_ops = {"write", "read"}

    bad_modes = [m for m in modes if m not in allowed_modes]
    bad_ops = [o for o in ops if o not in allowed_ops]
    if bad_modes:
        raise RunnerError(f"unknown modes: {' '.join(bad_modes)}")
    if bad_ops:
        raise RunnerError(f"unknown ops: {' '.join(bad_ops)}")
    if not modes:
        raise RunnerError("--modes cannot be empty")
    if not ops:
        raise RunnerError("--ops cannot be empty")

    chain_lengths = sorted(unique_ints(args.chain_lengths))
    client_counts = sorted(unique_ints(args.client_counts))
    if not chain_lengths or min(chain_lengths) <= 0:
        raise RunnerError("--chain-lengths must contain positive integers")
    if not client_counts or min(client_counts) <= 0:
        raise RunnerError("--client-counts must contain positive integers")
    if args.trials <= 0:
        raise RunnerError("--trials must be > 0")
    if args.key_count <= 0:
        raise RunnerError("--key-count must be > 0")
    if args.write_op_count <= 0:
        raise RunnerError("--write-op-count must be > 0")
    if args.read_op_count <= 0:
        raise RunnerError("--read-op-count must be > 0")
    if not (0 <= args.crown_hot_head_pct <= 100):
        raise RunnerError("--crown-hot-head-pct must be in [0, 100]")
    if not (0 <= args.read_hot_key_pct <= 100):
        raise RunnerError("--read-hot-key-pct must be in [0, 100]")
    if not (1 <= args.node_port <= 65535):
        raise RunnerError("--node-port must be in [1, 65535]")
    if not (1 <= args.metadata_port <= 65535):
        raise RunnerError("--metadata-port must be in [1, 65535]")
    if not (1 <= args.ack_base_port <= 65535):
        raise RunnerError("--ack-base-port must be in [1, 65535]")
    if args.ack_base_port + max(client_counts) - 1 > 65535:
        raise RunnerError("--ack-base-port plus max client count exceeds 65535")
    if args.stabilization_seconds < 0:
        raise RunnerError("--stabilization-seconds must be >= 0")

    server_hosts = read_hosts_arg_or_file(args.server_hosts, Path(args.server_hosts_file))
    client_hosts = read_hosts_arg_or_file(args.client_hosts, Path(args.client_hosts_file))
    if len(server_hosts) < max(chain_lengths):
        raise RunnerError(
            f"not enough server hosts: need {max(chain_lengths)}, found {len(server_hosts)} "
            f"(update {args.server_hosts_file} or pass --server-hosts)"
        )
    if len(client_hosts) < max(client_counts):
        raise RunnerError(
            f"not enough client hosts: need {max(client_counts)}, found {len(client_hosts)} "
            f"(update {args.client_hosts_file} or pass --client-hosts)"
        )

    ssh_user = args.ssh_user.strip()
    if not ssh_user:
        raise RunnerError("--ssh-user is required or SSH_USER must be set in setup/.env")
    ssh_opts = build_ssh_opts(args.ssh_key.strip())

    repo_url = args.repo_url.strip()
    if not repo_url:
        raise RunnerError("--repo-url is required or REPO_URL must be set in setup/.env")
    repo_name = args.repo_name.strip() or repo_name_from_url(repo_url)
    remote_repo_dir = args.remote_repo_dir.strip() or remote_project_dir(
        args.remote_base_dir.strip(),
        repo_name,
        args.project_subdir.strip(),
    )

    metadata_host = args.metadata_host.strip()
    metadata_port = args.metadata_port
    if args.metadata.strip():
        metadata_host, metadata_port = split_metadata_addr(args.metadata.strip())
    if not metadata_host:
        raise RunnerError("--metadata-host or --metadata HOST:PORT is required")
    metadata_addr = f"{metadata_host}:{metadata_port}"

    teardown_hosts = unique_preserve_order(
        list(server_hosts[: max(chain_lengths)])
        + list(client_hosts[: max(client_counts)])
        + [metadata_host]
    )

    return ExperimentConfig(
        root_dir=root_dir,
        setup_dir=setup_dir,
        work_dir=work_dir,
        configs_dir=work_dir / "configs",
        cases_dir=work_dir / "cases",
        lifecycle_log_dir=work_dir / "lifecycle_logs",
        raw_csv=work_dir / "raw_trials.csv",
        summary_csv=work_dir / "summary_by_chain_length.csv",
        server_hosts=server_hosts,
        client_hosts=client_hosts,
        teardown_hosts=teardown_hosts,
        chain_lengths=chain_lengths,
        modes=modes,
        client_counts=client_counts,
        ops=ops,
        trials=args.trials,
        key_count=args.key_count,
        write_op_count=args.write_op_count,
        read_op_count=args.read_op_count,
        craq_read_node_id=args.craq_read_node_id,
        crown_hot_head_pct=args.crown_hot_head_pct,
        read_hot_key_pct=args.read_hot_key_pct,
        ssh_user=ssh_user,
        ssh_opts=ssh_opts,
        repo_url=repo_url,
        repo_branch=args.repo_branch.strip() or "main",
        remote_base_dir=args.remote_base_dir.strip() or "/home",
        repo_name=repo_name,
        project_subdir=args.project_subdir.strip() or ".",
        project_mode=args.project_mode.strip() or "crown",
        build_type=args.build_type.strip() or "Release",
        remote_repo_dir=remote_repo_dir,
        remote_client_bin=args.remote_client_bin.strip() or "build/client",
        metadata_host=metadata_host,
        metadata_port=metadata_port,
        metadata_addr=metadata_addr,
        metadata_bind_host=args.metadata_bind_host.strip() or "0.0.0.0",
        metadata_ping_interval_ms=args.metadata_ping_interval_ms,
        metadata_ping_timeout_ms=args.metadata_ping_timeout_ms,
        metadata_failure_threshold=args.metadata_failure_threshold,
        node_host=args.node_host.strip() or "0.0.0.0",
        node_port=args.node_port,
        server_log=args.server_log.strip() or "true",
        tmux_session_name=args.tmux_session_name.strip(),
        tmux_metadata_session_name=args.tmux_metadata_session_name.strip(),
        tmux_socket=args.tmux_socket.strip() or "/tmp/crown-shared/tmux.sock",
        run_scope=args.run_scope.strip() or "shared",
        remote_config_dir=args.remote_config_dir.strip() or "build/chain_length_throughput_runs/configs",
        remote_log_dir=args.remote_log_dir.strip() or "build/chain_length_throughput_runs/client_logs",
        ack_base_port=args.ack_base_port,
        stabilization_seconds=args.stabilization_seconds,
        dry_run=args.dry_run,
        fail_fast=False if args.keep_going else bool(args.fail_fast),
    )


def unique_ints(values: Sequence[int]) -> List[int]:
    out: List[int] = []
    seen = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        out.append(value)
    return out


def endpoint(host: str, port: int) -> str:
    return f"{host}:{port}"


def build_cluster_config(mode: str, hosts: Sequence[str], port: int) -> dict:
    node_count = len(hosts)
    endpoints = [endpoint(hosts[i], port) for i in range(node_count)]
    nodes = []

    if mode in {"chain", "craq"}:
        for i, host in enumerate(hosts):
            nodes.append(
                {
                    "id": i,
                    "host": host,
                    "port": port,
                    "is_head": i == 0,
                    "is_tail": i == node_count - 1,
                    "predecessor": endpoints[i - 1] if i > 0 else None,
                    "successor": endpoints[i + 1] if i + 1 < node_count else None,
                }
            )
    elif mode == "crown":
        for i, host in enumerate(hosts):
            nodes.append(
                {
                    "id": i,
                    "host": host,
                    "port": port,
                    "is_head": False,
                    "is_tail": False,
                    "predecessor": endpoints[(i - 1) % node_count],
                    "successor": endpoints[(i + 1) % node_count],
                }
            )
    else:
        raise RunnerError(f"unknown mode while generating config: {mode}")

    return {"mode": mode, "nodes": nodes}


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def remote_config_paths(cfg: ExperimentConfig, local_config: Path) -> tuple[str, str]:
    filename = local_config.name
    if cfg.remote_config_dir.startswith("/"):
        remote_abs = remote_join(cfg.remote_config_dir, filename)
        return remote_abs, remote_abs
    remote_rel = remote_join(cfg.remote_config_dir, filename)
    remote_abs = remote_join(cfg.remote_repo_dir, remote_rel)
    return remote_abs, remote_rel


def run_cmd(args: Sequence[str],
            *,
            label: str,
            cfg: ExperimentConfig,
            log_path: Optional[Path] = None) -> None:
    if cfg.dry_run:
        log(f"[dry-run] {label}: {format_shell_cmd(args)}")
        return

    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w", encoding="utf-8") as handle:
            try:
                subprocess.run(args, check=True, stdout=handle, stderr=subprocess.STDOUT, text=True)
            except subprocess.CalledProcessError as exc:
                raise RunnerError(
                    f"{label} failed with exit code {exc.returncode}; see {log_path}"
                ) from exc
        return

    try:
        subprocess.run(args, check=True)
    except subprocess.CalledProcessError as exc:
        raise RunnerError(f"{label} failed with exit code {exc.returncode}: {format_shell_cmd(args)}") from exc


def export_env(env: dict[str, str]) -> str:
    return "export " + " ".join(f"{key}={shell_quote(value)}" for key, value in env.items())


def base_remote_env(cfg: ExperimentConfig) -> dict[str, str]:
    return {
        "SSH_USER": cfg.ssh_user,
        "REPO_URL": cfg.repo_url,
        "REPO_BRANCH": cfg.repo_branch,
        "REMOTE_BASE_DIR": cfg.remote_base_dir,
        "REPO_NAME": cfg.repo_name,
        "PROJECT_SUBDIR": cfg.project_subdir,
        "PROJECT_MODE": cfg.project_mode,
        "BUILD_TYPE": cfg.build_type,
        "NODE_HOST": cfg.node_host,
        "NODE_PORT": str(cfg.node_port),
        "SERVER_LOG": cfg.server_log,
        "TMUX_SESSION_NAME": cfg.tmux_session_name,
        "TMUX_METADATA_SESSION_NAME": cfg.tmux_metadata_session_name,
        "TMUX_SOCKET": cfg.tmux_socket,
        "RUN_SCOPE": cfg.run_scope,
        "METADATA_HOST": cfg.metadata_host,
        "METADATA_PORT": str(cfg.metadata_port),
        "METADATA_BIND_HOST": cfg.metadata_bind_host,
        "METADATA_PING_INTERVAL_MS": str(cfg.metadata_ping_interval_ms),
        "METADATA_PING_TIMEOUT_MS": str(cfg.metadata_ping_timeout_ms),
        "METADATA_FAILURE_THRESHOLD": str(cfg.metadata_failure_threshold),
        "BUILD_ONLY": "false",
        "START_ONLY": "true",
    }


def deploy_script_to_host(cfg: ExperimentConfig,
                          *,
                          host: str,
                          script_name: str,
                          label: str,
                          env_overrides: Optional[dict[str, str]] = None,
                          log_stem: str) -> None:
    local_script = cfg.setup_dir / script_name
    if not local_script.is_file():
        raise RunnerError(f"missing local script: {local_script}")

    remote_script = f"/home/{cfg.ssh_user}/{script_name}"
    server = f"{cfg.ssh_user}@{host}"
    scp_cmd = ["scp", *cfg.ssh_opts, str(local_script), f"{server}:{remote_script}"]
    run_cmd(
        scp_cmd,
        label=f"{label} copy {script_name} to {host}",
        cfg=cfg,
        log_path=cfg.lifecycle_log_dir / f"{safe_filename(log_stem)}_{safe_filename(host)}_scp.log",
    )

    env = base_remote_env(cfg)
    if env_overrides:
        env.update(env_overrides)
    remote_cmd = f"{export_env(env)}; tr -d '\\r' < {shell_quote(remote_script)} | bash -s --"
    ssh_cmd = ["ssh", *cfg.ssh_opts, server, f"bash -lc {shell_quote(remote_cmd)}"]
    run_cmd(
        ssh_cmd,
        label=f"{label} run {script_name} on {host}",
        cfg=cfg,
        log_path=cfg.lifecycle_log_dir / f"{safe_filename(log_stem)}_{safe_filename(host)}_ssh.log",
    )


def safe_filename(text: str) -> str:
    return "".join(ch if ch.isalnum() or ch in {"-", "_", "."} else "_" for ch in text)


def kill_cluster(cfg: ExperimentConfig, case: ExperimentCase) -> None:
    log(f"{case.experiment_id}: killing existing server/metadata processes")
    for host in cfg.teardown_hosts:
        deploy_script_to_host(
            cfg,
            host=host,
            script_name="kill.bash",
            label=f"{case.experiment_id} kill",
            log_stem=f"{case.experiment_id}_kill",
        )


def start_servers(cfg: ExperimentConfig, case: ExperimentCase, server_hosts: Sequence[str]) -> None:
    log(f"{case.experiment_id}: starting {len(server_hosts)} server host(s)")
    for host in server_hosts:
        deploy_script_to_host(
            cfg,
            host=host,
            script_name="start_server.bash",
            label=f"{case.experiment_id} start-server",
            log_stem=f"{case.experiment_id}_start_server",
        )


def copy_config_to_metadata(cfg: ExperimentConfig, case: ExperimentCase, local_config: Path) -> str:
    remote_abs, metadata_config_arg = remote_config_paths(cfg, local_config)
    server = f"{cfg.ssh_user}@{cfg.metadata_host}"
    mkdir_cmd = [
        "ssh",
        *cfg.ssh_opts,
        server,
        f"bash -lc {shell_quote('mkdir -p ' + shell_quote(remote_abs.rsplit('/', 1)[0]))}",
    ]
    run_cmd(
        mkdir_cmd,
        label=f"{case.experiment_id} mkdir remote config dir",
        cfg=cfg,
        log_path=cfg.lifecycle_log_dir / f"{case.experiment_id}_metadata_mkdir.log",
    )

    scp_cmd = ["scp", *cfg.ssh_opts, str(local_config), f"{server}:{remote_abs}"]
    run_cmd(
        scp_cmd,
        label=f"{case.experiment_id} copy config to metadata host",
        cfg=cfg,
        log_path=cfg.lifecycle_log_dir / f"{case.experiment_id}_metadata_config_scp.log",
    )
    return metadata_config_arg


def start_metadata(cfg: ExperimentConfig, case: ExperimentCase, metadata_config_arg: str) -> None:
    log(f"{case.experiment_id}: starting metadata with config {metadata_config_arg}")
    deploy_script_to_host(
        cfg,
        host=cfg.metadata_host,
        script_name="start_metadata.bash",
        label=f"{case.experiment_id} start-metadata",
        env_overrides={"METADATA_CONFIG": metadata_config_arg},
        log_stem=f"{case.experiment_id}_start_metadata",
    )


def run_throughput_case(cfg: ExperimentConfig,
                        case: ExperimentCase,
                        client_hosts: Sequence[str],
                        case_work_dir: Path) -> None:
    key_prefix = f"bench-n{case.chain_length}-{case.mode}-c{case.client_count}-t{case.trial}-{case.op}-"
    value_prefix = f"value-n{case.chain_length}-{case.mode}-c{case.client_count}-t{case.trial}-{case.op}-"
    remote_case_log_dir = remote_join(
        cfg.remote_log_dir,
        f"n{case.chain_length}_{case.mode}_c{case.client_count}_t{case.trial}_{case.op}",
    )

    cmd = [
        sys.executable,
        str(cfg.setup_dir / "run_throughput_experiments.py"),
        "--work-dir",
        str(case_work_dir),
        "--hosts",
        ",".join(client_hosts),
        "--modes",
        case.mode,
        "--ops",
        case.op,
        "--write-op-count",
        str(cfg.write_op_count),
        "--read-op-count",
        str(cfg.read_op_count),
        "--key-count",
        str(cfg.key_count),
        "--craq-read-node-id",
        str(cfg.craq_read_node_id),
        "--crown-hot-head-pct",
        str(cfg.crown_hot_head_pct),
        "--read-hot-key-pct",
        str(cfg.read_hot_key_pct),
        "--ssh-user",
        cfg.ssh_user,
        "--remote-repo-dir",
        cfg.remote_repo_dir,
        "--remote-client-bin",
        cfg.remote_client_bin,
        "--metadata",
        cfg.metadata_addr,
        "--remote-log-dir",
        remote_case_log_dir,
        "--key-prefix-exact",
        key_prefix,
        "--value-prefix-exact",
        value_prefix,
        "--ack-base-port",
        str(cfg.ack_base_port),
    ]
    if cfg.ssh_opts and cfg.ssh_opts[0] == "-i":
        cmd.extend(["--ssh-key", cfg.ssh_opts[1]])
    if cfg.dry_run:
        cmd.append("--dry-run")

    run_cmd(
        cmd,
        label=f"{case.experiment_id} throughput runner",
        cfg=cfg,
        log_path=case_work_dir / "runner.log",
    )


def read_case_summary(case: ExperimentCase, summary_csv: Path) -> dict[str, str]:
    if not summary_csv.is_file():
        raise RunnerError(f"case summary not found: {summary_csv}")
    with summary_csv.open("r", encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle))

    expected_operation = "bench-write" if case.op == "write" else "bench-read"
    matches = [
        row for row in rows
        if row.get("operation") == expected_operation
        and row.get("mode") == case.mode
        and int(row.get("num_clients", "0")) == case.client_count
    ]
    if not matches:
        raise RunnerError(
            f"no matching summary row in {summary_csv} for "
            f"operation={expected_operation} mode={case.mode} clients={case.client_count}"
        )
    return matches[0]


def op_count_for_case(cfg: ExperimentConfig, case: ExperimentCase) -> int:
    return cfg.write_op_count if case.op == "write" else cfg.read_op_count


def build_raw_row(cfg: ExperimentConfig,
                  case: ExperimentCase,
                  *,
                  server_hosts: Sequence[str],
                  client_hosts: Sequence[str],
                  local_config: Path,
                  remote_config_file: str,
                  case_work_dir: Path,
                  summary_row: dict[str, str]) -> dict[str, str]:
    throughput = (
        summary_row.get("agg_ack_wps", "0")
        if case.op == "write"
        else summary_row.get("agg_read_resp_rps", "0")
    )
    return {
        "experiment_id": case.experiment_id,
        "group_experiment_id": case.group_experiment_id,
        "chain_length": str(case.chain_length),
        "mode": case.mode,
        "operation": case.op,
        "client_count": str(case.client_count),
        "trial": str(case.trial),
        "key_count": str(cfg.key_count),
        "op_count": str(op_count_for_case(cfg, case)),
        "server_hosts": ";".join(server_hosts),
        "client_hosts": ";".join(client_hosts),
        "config_file": str(local_config),
        "remote_config_file": remote_config_file,
        "case_work_dir": str(case_work_dir),
        "source_summary": str(case_work_dir / "summary.csv"),
        "duration_s_avg": summary_row.get("duration_s_avg", "0"),
        "writes_sent": summary_row.get("writes_sent", "0"),
        "acks_received": summary_row.get("acks_received", "0"),
        "reads_sent": summary_row.get("reads_sent", "0"),
        "reads_ok": summary_row.get("reads_ok", "0"),
        "read_failures": summary_row.get("read_failures", "0"),
        "write_rpc_failures": summary_row.get("write_rpc_failures", "0"),
        "agg_ack_wps": summary_row.get("agg_ack_wps", "0"),
        "agg_read_req_rps": summary_row.get("agg_read_req_rps", "0"),
        "agg_read_resp_rps": summary_row.get("agg_read_resp_rps", "0"),
        "throughput_ops_per_sec": throughput,
        "weighted_avg_ack_latency_ms": summary_row.get("weighted_avg_ack_latency_ms", "0"),
        "complete": summary_row.get("complete", "False"),
        "status": "ok",
        "error": "",
    }


def build_failed_raw_row(cfg: ExperimentConfig,
                         case: ExperimentCase,
                         *,
                         server_hosts: Sequence[str],
                         client_hosts: Sequence[str],
                         local_config: Path,
                         remote_config_file: str,
                         case_work_dir: Path,
                         error: str) -> dict[str, str]:
    row = {field: "" for field in RAW_FIELDNAMES}
    row.update(
        {
            "experiment_id": case.experiment_id,
            "group_experiment_id": case.group_experiment_id,
            "chain_length": str(case.chain_length),
            "mode": case.mode,
            "operation": case.op,
            "client_count": str(case.client_count),
            "trial": str(case.trial),
            "key_count": str(cfg.key_count),
            "op_count": str(op_count_for_case(cfg, case)),
            "server_hosts": ";".join(server_hosts),
            "client_hosts": ";".join(client_hosts),
            "config_file": str(local_config),
            "remote_config_file": remote_config_file,
            "case_work_dir": str(case_work_dir),
            "complete": "False",
            "status": "failed",
            "error": error,
        }
    )
    return row


def write_raw_csv(path: Path, rows: Sequence[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=RAW_FIELDNAMES)
        writer.writeheader()
        writer.writerows(rows)


def to_float(raw: str) -> Optional[float]:
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def mean(values: Sequence[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def sample_stddev(values: Sequence[float]) -> float:
    if len(values) < 2:
        return 0.0
    avg = mean(values)
    variance = sum((value - avg) ** 2 for value in values) / (len(values) - 1)
    return math.sqrt(variance)


def is_complete_row(row: dict[str, str]) -> bool:
    return row.get("status") == "ok" and row.get("complete", "").strip().lower() == "true"


def summarize_rows(cfg: ExperimentConfig, raw_rows: Sequence[dict[str, str]]) -> List[dict[str, str]]:
    grouped: dict[tuple[str, str, str, str, str, str], List[dict[str, str]]] = {}
    for row in raw_rows:
        key = (
            row["chain_length"],
            row["mode"],
            row["operation"],
            row["client_count"],
            row["key_count"],
            row["op_count"],
        )
        grouped.setdefault(key, []).append(row)

    summary_rows: List[dict[str, str]] = []
    for key, rows in sorted(grouped.items(), key=lambda item: (int(item[0][0]), item[0][1], int(item[0][3]), item[0][2])):
        chain_length, mode, operation, client_count, key_count, op_count = key
        complete_rows = [row for row in rows if is_complete_row(row)]

        throughput_values = [
            value for row in complete_rows
            for value in [to_float(row.get("throughput_ops_per_sec", ""))]
            if value is not None
        ]
        latency_values = [
            value for row in complete_rows
            for value in [to_float(row.get("weighted_avg_ack_latency_ms", ""))]
            if value is not None
        ]
        read_failure_values = [
            value for row in complete_rows
            for value in [to_float(row.get("read_failures", ""))]
            if value is not None
        ]
        write_failure_values = [
            value for row in complete_rows
            for value in [to_float(row.get("write_rpc_failures", ""))]
            if value is not None
        ]

        summary_rows.append(
            {
                "experiment_id": (
                    f"chainlen_n{chain_length}_mode_{mode}"
                    f"_clients{client_count}_op_{operation}"
                ),
                "chain_length": chain_length,
                "mode": mode,
                "operation": operation,
                "client_count": client_count,
                "key_count": key_count,
                "op_count": op_count,
                "trials_expected": str(cfg.trials),
                "trials_completed": str(len(complete_rows)),
                "throughput_mean": f"{mean(throughput_values):.6f}",
                "throughput_stddev": f"{sample_stddev(throughput_values):.6f}",
                "latency_mean_ms": f"{mean(latency_values):.6f}",
                "latency_stddev_ms": f"{sample_stddev(latency_values):.6f}",
                "read_failures_mean": f"{mean(read_failure_values):.6f}",
                "write_rpc_failures_mean": f"{mean(write_failure_values):.6f}",
                "all_trials_complete": str(len(complete_rows) == cfg.trials),
            }
        )
    return summary_rows


def write_summary_csv(path: Path, rows: Sequence[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SUMMARY_FIELDNAMES)
        writer.writeheader()
        writer.writerows(rows)


def build_cases(cfg: ExperimentConfig) -> List[ExperimentCase]:
    cases: List[ExperimentCase] = []
    index = 0
    for chain_length in cfg.chain_lengths:
        for mode in cfg.modes:
            for client_count in cfg.client_counts:
                for trial in range(1, cfg.trials + 1):
                    for op in cfg.ops:
                        index += 1
                        cases.append(
                            ExperimentCase(
                                index=index,
                                chain_length=chain_length,
                                mode=mode,
                                client_count=client_count,
                                trial=trial,
                                op=op,
                            )
                        )
    return cases


def prepare_case_config(cfg: ExperimentConfig,
                        case: ExperimentCase,
                        server_hosts: Sequence[str]) -> Path:
    local_config = cfg.configs_dir / f"n{case.chain_length}.{case.mode}.json"
    if cfg.dry_run:
        log(f"[dry-run] {case.experiment_id}: would write config {local_config}")
        return local_config

    payload = build_cluster_config(case.mode, server_hosts, cfg.node_port)
    write_json(local_config, payload)
    return local_config


def run_case(cfg: ExperimentConfig, case: ExperimentCase) -> dict[str, str]:
    server_hosts = cfg.server_hosts[: case.chain_length]
    client_hosts = cfg.client_hosts[: case.client_count]
    case_work_dir = cfg.cases_dir / f"n{case.chain_length}" / case.mode / f"c{case.client_count}" / f"t{case.trial}" / case.op

    local_config = prepare_case_config(cfg, case, server_hosts)
    remote_config_abs, remote_config_arg = remote_config_paths(cfg, local_config)

    try:
        kill_cluster(cfg, case)
        if not cfg.dry_run:
            remote_config_arg = copy_config_to_metadata(cfg, case, local_config)
        else:
            log(f"[dry-run] {case.experiment_id}: would copy config to {remote_config_abs}")

        start_servers(cfg, case, server_hosts)
        start_metadata(cfg, case, remote_config_arg)

        if cfg.stabilization_seconds > 0:
            if cfg.dry_run:
                log(f"[dry-run] {case.experiment_id}: would sleep {cfg.stabilization_seconds:.3f}s for stabilization")
            else:
                log(f"{case.experiment_id}: waiting {cfg.stabilization_seconds:.3f}s for stabilization")
                time.sleep(cfg.stabilization_seconds)

        log(f"{case.experiment_id}: running {case.op} benchmark")
        run_throughput_case(cfg, case, client_hosts, case_work_dir)

        if cfg.dry_run:
            return build_failed_raw_row(
                cfg,
                case,
                server_hosts=server_hosts,
                client_hosts=client_hosts,
                local_config=local_config,
                remote_config_file=remote_config_abs,
                case_work_dir=case_work_dir,
                error="dry-run: benchmark not executed",
            )

        summary_row = read_case_summary(case, case_work_dir / "summary.csv")
        return build_raw_row(
            cfg,
            case,
            server_hosts=server_hosts,
            client_hosts=client_hosts,
            local_config=local_config,
            remote_config_file=remote_config_abs,
            case_work_dir=case_work_dir,
            summary_row=summary_row,
        )
    except Exception as exc:
        error = str(exc)
        if cfg.fail_fast:
            raise
        log(f"{case.experiment_id}: failed, continuing because fail-fast is disabled: {error}")
        return build_failed_raw_row(
            cfg,
            case,
            server_hosts=server_hosts,
            client_hosts=client_hosts,
            local_config=local_config,
            remote_config_file=remote_config_abs,
            case_work_dir=case_work_dir,
            error=error,
        )


def print_plan(cfg: ExperimentConfig, cases: Sequence[ExperimentCase]) -> None:
    log("Chain-length throughput configuration")
    log(f"  chain_lengths={cfg.chain_lengths}")
    log(f"  modes={cfg.modes}")
    log(f"  client_counts={cfg.client_counts}")
    log(f"  ops={cfg.ops}")
    log(f"  trials={cfg.trials}")
    log(f"  planned_benchmark_runs={len(cases)}")
    log(f"  key_count={cfg.key_count}")
    log(f"  write_op_count={cfg.write_op_count}")
    log(f"  read_op_count={cfg.read_op_count}")
    log(f"  server_hosts_available={len(cfg.server_hosts)}")
    log(f"  client_hosts_available={len(cfg.client_hosts)}")
    log(f"  metadata={cfg.metadata_addr}")
    log(f"  work_dir={cfg.work_dir}")


def main() -> int:
    root_dir = Path(__file__).resolve().parent.parent
    dotenv = load_dotenv(root_dir / "setup" / ".env")
    args = parse_args(root_dir, dotenv)
    cfg = build_config(args, root_dir)
    cases = build_cases(cfg)

    print_plan(cfg, cases)

    raw_rows: List[dict[str, str]] = []
    for case in cases:
        log(f"Starting {case.index}/{len(cases)}: {case.experiment_id}")
        row = run_case(cfg, case)
        if not cfg.dry_run:
            raw_rows.append(row)
            write_raw_csv(cfg.raw_csv, raw_rows)
            write_summary_csv(cfg.summary_csv, summarize_rows(cfg, raw_rows))

    if cfg.dry_run:
        log("Dry run complete. No SSH/SCP commands or benchmarks were executed.")
        return 0

    log("Completed chain-length throughput experiments.")
    log(f"Raw trial CSV: {cfg.raw_csv}")
    log(f"Summary CSV: {cfg.summary_csv}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunnerError as exc:
        print(f"[chain-length-runner] ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
