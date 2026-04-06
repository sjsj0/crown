#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import IO, List, Sequence


class RunnerError(RuntimeError):
    pass


@dataclass
class RunnerConfig:
    root_dir: Path
    work_dir: Path
    log_dir: Path
    ssh_log_dir: Path
    summary_csv: Path

    hosts: List[str]
    modes: List[str]
    ops: List[str]

    write_op_count: int
    read_op_count: int
    key_count: int
    craq_read_node_id: int
    write_hot_head_pct: int
    read_hot_key_pct: int

    ssh_user: str
    ssh_opts: List[str]
    remote_repo_dir: str
    remote_client_bin: str
    remote_config_template: str
    remote_log_dir: str

    key_prefix_base: str
    value_prefix_base: str
    ack_base_port: int
    reconfigure_each_run: bool

    dry_run: bool
    fail_fast: bool


@dataclass
class ActiveLaunch:
    host: str
    client_index: int
    remote_log_file: str
    local_client_log: Path
    local_ssh_log: Path
    process: subprocess.Popen[bytes]
    log_handle: IO[str]


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


def unique_preserve_order(items: Sequence[str]) -> List[str]:
    seen = set()
    out: List[str] = []
    for item in items:
        if item in seen:
            continue
        seen.add(item)
        out.append(item)
    return out


def shell_quote(s: str) -> str:
    return shlex.quote(s)


def format_shell_cmd(args: Sequence[str]) -> str:
    return " ".join(shell_quote(a) for a in args)


def run_cmd(args: Sequence[str], quiet: bool = False) -> None:
    kwargs = {}
    if quiet:
        kwargs["stdout"] = subprocess.DEVNULL
        kwargs["stderr"] = subprocess.DEVNULL
    try:
        subprocess.run(args, check=True, **kwargs)
    except subprocess.CalledProcessError as exc:
        raise RunnerError(f"command failed ({exc.returncode}): {format_shell_cmd(args)}") from exc


def restart_servers(root_dir: Path) -> None:
    """Kill servers and restart them for a clean state between benchmark runs."""
    log("Restarting servers (kill + rerun)...")
    setup_script = root_dir / "setup" / "vm_setup.bash"
    try:
        subprocess.run([str(setup_script), "kill"], check=False, capture_output=True, timeout=60)
        log("Servers killed.")
        subprocess.run([str(setup_script), "rerun"], check=True, timeout=300)
        log("Servers restarted successfully.")
    except subprocess.TimeoutExpired as exc:
        raise RunnerError(f"server restart timed out: {exc}") from exc
    except subprocess.CalledProcessError as exc:
        raise RunnerError(f"server restart failed: {exc}") from exc


def startup_servers(root_dir: Path) -> None:
    """Start servers for the first time (rerun without kill)."""
    log("Starting servers (rerun)...")
    setup_script = root_dir / "setup" / "vm_setup.bash"
    try:
        subprocess.run([str(setup_script), "rerun"], check=True, timeout=300)
        log("Servers started successfully.")
    except subprocess.TimeoutExpired as exc:
        raise RunnerError(f"server startup timed out: {exc}") from exc
    except subprocess.CalledProcessError as exc:
        raise RunnerError(f"server startup failed: {exc}") from exc


def parse_args(root_dir: Path) -> argparse.Namespace:
    work_dir_default = env_str("WORK_DIR", str(root_dir / "build" / "distributed_throughput_runs"))

    p = argparse.ArgumentParser(
        description=(
            "Run distributed throughput experiments across chain/craq/crown with one client process per host."
        ),
    )

    p.add_argument("--work-dir", default=work_dir_default)

    p.add_argument("--hosts", default=env_str("HOSTS", ""), help="Comma-separated host list")
    p.add_argument("--hosts-file", default=env_str("HOSTS_FILE", ""), help="Host file (CSV or one-per-line)")

    p.add_argument("--modes", nargs="+", default=env_words("MODES", "chain craq crown"))
    p.add_argument("--ops", nargs="+", default=env_words("OPS", "write read"))

    p.add_argument("--write-op-count", type=int, default=env_int("WRITE_OP_COUNT", 50000))
    p.add_argument("--read-op-count", type=int, default=env_int("READ_OP_COUNT", 50000))
    p.add_argument("--key-count", type=int, default=env_int("KEY_COUNT", 64))
    p.add_argument("--craq-read-node-id", type=int, default=env_int("CRAQ_READ_NODE_ID", -1))
    p.add_argument(
        "--write-hot-head-pct",
        "--crown-hot-head-pct",
        dest="write_hot_head_pct",
        type=int,
        default=env_int("WRITE_HOT_HEAD_PCT", env_int("CROWN_HOT_HEAD_PCT", 0)),
        help=(
            "For bench-write, target this percent of writes to a selected hot head (0-100). "
            "For CHAIN/CRAQ, effective hot-head share is always 100 because all writes use one head."
        ),
    )
    p.add_argument(
        "--read-hot-key-pct",
        type=int,
        default=env_int("READ_HOT_KEY_PCT", 0),
        help="For bench-read, target this percent of reads to one hot key (0-100)",
    )

    p.add_argument("--ssh-user", default=env_str("SSH_USER", ""))
    p.add_argument("--ssh-key", default=env_str("SSH_KEY_LOCAL", ""))
    p.add_argument("--remote-repo-dir", default=env_str("REMOTE_REPO_DIR", ""))
    p.add_argument("--remote-client-bin", default=env_str("REMOTE_CLIENT_BIN", "build/client"))
    p.add_argument(
        "--remote-config-template",
        default=env_str("REMOTE_CONFIG_TEMPLATE", "build/prod_configs/config.{mode}.json"),
        help="Remote config path template, e.g. build/prod_configs/config.{mode}.json",
    )
    p.add_argument("--remote-log-dir", default=env_str("REMOTE_LOG_DIR", "build/prod_bench_logs"))

    p.add_argument("--key-prefix-base", default=env_str("KEY_PREFIX_BASE", "bench-key"))
    p.add_argument("--value-prefix-base", default=env_str("VALUE_PREFIX_BASE", "bench-value"))
    p.add_argument("--ack-base-port", type=int, default=env_int("ACK_BASE_PORT", 61000))

    p.add_argument(
        "--reconfigure-each-run",
        type=argparse_bool,
        default=parse_bool(env_str("RECONFIGURE_EACH_RUN", "1"), "RECONFIGURE_EACH_RUN"),
    )
    p.add_argument(
        "--fail-fast",
        type=argparse_bool,
        default=parse_bool(env_str("FAIL_FAST", "1"), "FAIL_FAST"),
        help="Stop on first failing case when true",
    )
    p.add_argument("--dry-run", action="store_true", help="Print planned SSH/SCP commands without executing")

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

    if args.write_op_count <= 0:
        raise RunnerError("--write-op-count must be > 0")
    if args.read_op_count <= 0:
        raise RunnerError("--read-op-count must be > 0")
    if args.key_count <= 0:
        raise RunnerError("--key-count must be > 0")
    if args.write_hot_head_pct < 0 or args.write_hot_head_pct > 100:
        raise RunnerError("--write-hot-head-pct must be in [0, 100]")
    if args.read_hot_key_pct < 0 or args.read_hot_key_pct > 100:
        raise RunnerError("--read-hot-key-pct must be in [0, 100]")
    if not (1 <= args.ack_base_port <= 65535):
        raise RunnerError("--ack-base-port must be in [1, 65535]")

    raw_hosts: List[str] = []
    if args.hosts:
        raw_hosts.extend(parse_hosts_text(args.hosts.replace(",", "\n")))

    if args.hosts_file:
        hosts_file = Path(args.hosts_file)
        if not hosts_file.is_file():
            raise RunnerError(f"hosts file not found: {hosts_file}")
        raw_hosts.extend(parse_hosts_text(hosts_file.read_text(encoding="utf-8")))

    hosts = unique_preserve_order(raw_hosts)
    if not hosts:
        raise RunnerError("no hosts provided; use --hosts and/or --hosts-file")

    ssh_user = args.ssh_user.strip()
    if not ssh_user:
        raise RunnerError("--ssh-user is required")

    remote_repo_dir = args.remote_repo_dir.strip()
    if not remote_repo_dir:
        remote_repo_dir = "/home/crown"

    ssh_opts = [
        "-o",
        "BatchMode=yes",
        "-o",
        "IdentitiesOnly=yes",
        "-o",
        "StrictHostKeyChecking=accept-new",
    ]
    ssh_key = args.ssh_key.strip()
    if ssh_key:
        key_path = Path(ssh_key)
        if not key_path.is_file():
            raise RunnerError(f"ssh key not found: {ssh_key}")
        ssh_opts = ["-i", str(key_path)] + ssh_opts

    work_dir = Path(args.work_dir)
    log_dir = work_dir / "logs"
    ssh_log_dir = work_dir / "ssh_logs"
    summary_csv = work_dir / "summary.csv"

    return RunnerConfig(
        root_dir=root_dir,
        work_dir=work_dir,
        log_dir=log_dir,
        ssh_log_dir=ssh_log_dir,
        summary_csv=summary_csv,
        hosts=hosts,
        modes=modes,
        ops=ops,
        write_op_count=args.write_op_count,
        read_op_count=args.read_op_count,
        key_count=args.key_count,
        craq_read_node_id=args.craq_read_node_id,
        write_hot_head_pct=args.write_hot_head_pct,
        read_hot_key_pct=args.read_hot_key_pct,
        ssh_user=ssh_user,
        ssh_opts=ssh_opts,
        remote_repo_dir=remote_repo_dir,
        remote_client_bin=args.remote_client_bin,
        remote_config_template=args.remote_config_template,
        remote_log_dir=args.remote_log_dir,
        key_prefix_base=args.key_prefix_base,
        value_prefix_base=args.value_prefix_base,
        ack_base_port=args.ack_base_port,
        reconfigure_each_run=args.reconfigure_each_run,
        dry_run=args.dry_run,
        fail_fast=args.fail_fast,
    )


def cleanup_previous_logs(cfg: RunnerConfig) -> None:
    cfg.log_dir.mkdir(parents=True, exist_ok=True)
    cfg.ssh_log_dir.mkdir(parents=True, exist_ok=True)

    for path in cfg.log_dir.glob("*.log"):
        path.unlink(missing_ok=True)
    for path in cfg.ssh_log_dir.glob("*.log"):
        path.unlink(missing_ok=True)


def build_remote_client_command(
    cfg: RunnerConfig,
    *,
    mode: str,
    op: str,
    client_index: int,
    num_clients: int,
    ack_port: int,
    should_configure: bool,
) -> tuple[List[str], str, str]:
    configure_flag = "true" if should_configure else "false"
    config_path = cfg.remote_config_template.format(mode=mode)

    key_prefix = f"{cfg.key_prefix_base}-{mode}-{op}-"
    value_prefix = f"{cfg.value_prefix_base}-{mode}-{op}-"

    cmd = [
        cfg.remote_client_bin,
        config_path,
        configure_flag,
        str(ack_port),
    ]

    if op == "write":
        cmd.extend(
            [
                "bench-write",
                str(cfg.write_op_count),
                str(cfg.key_count),
                str(client_index),
                str(num_clients),
                key_prefix,
                value_prefix,
                f"hot={cfg.write_hot_head_pct}",
            ]
        )
    else:
        cmd.extend(
            [
                "bench-read",
                str(cfg.read_op_count),
                str(cfg.key_count),
                str(client_index),
                str(num_clients),
                str(cfg.craq_read_node_id),
                key_prefix,
                f"hot={cfg.read_hot_key_pct}",
            ]
        )

    remote_log_file = f"{cfg.remote_log_dir}/{mode}_{op}_c{num_clients}_i{client_index}.log"
    return cmd, config_path, remote_log_file


def launch_case(cfg: RunnerConfig, mode: str, op: str, run_idx: int) -> None:
    num_clients = len(cfg.hosts)
    total_ops = cfg.write_op_count if op == "write" else cfg.read_op_count
    log(f"Running mode={mode} op={op} clients={num_clients} total_ops={total_ops}")

    launches: List[ActiveLaunch] = []

    for client_index, host in enumerate(cfg.hosts):
        should_configure = cfg.reconfigure_each_run and client_index == 0
        ack_port = cfg.ack_base_port + run_idx * 100 + client_index

        client_cmd, config_path, remote_log_file = build_remote_client_command(
            cfg,
            mode=mode,
            op=op,
            client_index=client_index,
            num_clients=num_clients,
            ack_port=ack_port,
            should_configure=should_configure,
        )

        host_tag = host.replace("/", "_").replace(":", "_")
        local_client_log = cfg.log_dir / f"{mode}_{op}_c{num_clients}_i{client_index}.log"
        local_ssh_log = cfg.ssh_log_dir / f"{mode}_{op}_c{num_clients}_i{client_index}_{host_tag}.log"

        remote_script = " ; ".join(
            [
                "set -euo pipefail",
                f"cd {shell_quote(cfg.remote_repo_dir)}",
                (
                    f"if [[ ! -x {shell_quote(cfg.remote_client_bin)} ]]; then "
                    f"echo 'missing client binary: {cfg.remote_client_bin}' >&2; exit 10; fi"
                ),
                (
                    f"if [[ ! -f {shell_quote(config_path)} ]]; then "
                    f"echo 'missing config file: {config_path}' >&2; exit 11; fi"
                ),
                f"mkdir -p {shell_quote(cfg.remote_log_dir)}",
                (
                    "echo "
                    f"{shell_quote(f'[remote] host=$(hostname -f 2>/dev/null || hostname) client_index={client_index}/{num_clients} mode={mode} op={op}')}"
                ),
                f"{format_shell_cmd(client_cmd)} > {shell_quote(remote_log_file)} 2>&1",
                f"echo {shell_quote(f'[remote] done client_index={client_index} log={remote_log_file}')}",
            ]
        )

        ssh_cmd = [
            "ssh",
            *cfg.ssh_opts,
            f"{cfg.ssh_user}@{host}",
            f"bash -lc {shell_quote(remote_script)}",
        ]
        scp_cmd = [
            "scp",
            *cfg.ssh_opts,
            f"{cfg.ssh_user}@{host}:{cfg.remote_repo_dir}/{remote_log_file}",
            str(local_client_log),
        ]

        if cfg.dry_run:
            log(f"[dry-run] launch: {format_shell_cmd(ssh_cmd)}")
            log(f"[dry-run] collect: {format_shell_cmd(scp_cmd)}")
            continue

        log_handle = local_ssh_log.open("w", encoding="utf-8")
        process = subprocess.Popen(ssh_cmd, stdout=log_handle, stderr=subprocess.STDOUT)
        launches.append(
            ActiveLaunch(
                host=host,
                client_index=client_index,
                remote_log_file=remote_log_file,
                local_client_log=local_client_log,
                local_ssh_log=local_ssh_log,
                process=process,
                log_handle=log_handle,
            )
        )

    if cfg.dry_run:
        return

    failures: List[ActiveLaunch] = []
    for launch in launches:
        rc = launch.process.wait()
        launch.log_handle.close()
        if rc != 0:
            failures.append(launch)
            log(
                f"Client failed host={launch.host} client_index={launch.client_index}; "
                f"ssh log={launch.local_ssh_log}"
            )

    if failures:
        raise RunnerError(
            f"one or more distributed clients failed for mode={mode} op={op}; "
            f"failed={len(failures)}/{len(launches)}"
        )

    for launch in launches:
        scp_cmd = [
            "scp",
            *cfg.ssh_opts,
            f"{cfg.ssh_user}@{launch.host}:{cfg.remote_repo_dir}/{launch.remote_log_file}",
            str(launch.local_client_log),
        ]
        try:
            run_cmd(scp_cmd)
        except RunnerError as exc:
            raise RunnerError(
                f"failed to collect remote log for host={launch.host} "
                f"client_index={launch.client_index} remote={launch.remote_log_file}"
            ) from exc


def aggregate_results(cfg: RunnerConfig) -> None:
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


def main() -> int:
    root_dir = Path(__file__).resolve().parent.parent
    args = parse_args(root_dir)
    cfg = build_config(args, root_dir)

    cleanup_previous_logs(cfg)

    log("Distributed benchmark configuration")
    log(f"  hosts={cfg.hosts}")
    log(f"  modes={cfg.modes}")
    log(f"  ops={cfg.ops}")
    log(f"  write_op_count={cfg.write_op_count}")
    log(f"  read_op_count={cfg.read_op_count}")
    log(f"  key_count={cfg.key_count}")
    log(f"  write_hot_head_pct={cfg.write_hot_head_pct}")
    log(f"  read_hot_key_pct={cfg.read_hot_key_pct}")
    log(f"  work_dir={cfg.work_dir}")

    run_idx = 0
    first_run = True
    for mode in cfg.modes:
        for op in cfg.ops:
            # Start/restart servers before each run
            if first_run:
                if not cfg.dry_run:
                    startup_servers(root_dir)
                first_run = False
            else:
                if not cfg.dry_run:
                    restart_servers(root_dir)

            try:
                launch_case(cfg, mode, op, run_idx)
            except RunnerError:
                if cfg.fail_fast:
                    raise
                log(f"Continuing after failed case mode={mode} op={op} because --fail-fast=false")
            run_idx += 1

    if cfg.dry_run:
        log("Dry run complete. No commands executed.")
        return 0

    log("Aggregating BENCH_SUMMARY lines into CSV...")
    aggregate_results(cfg)

    log("Completed distributed throughput runs.")
    log(f"Client logs: {cfg.log_dir}")
    log(f"SSH logs: {cfg.ssh_log_dir}")
    log(f"Summary: {cfg.summary_csv}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunnerError as exc:
        print(f"[throughput-runner] ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
