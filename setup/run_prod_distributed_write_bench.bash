#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Optional: load defaults from setup/.env when present.
if [[ -f "$SCRIPT_DIR/.env" ]]; then
  # shellcheck source=/dev/null
  set -a
  source "$SCRIPT_DIR/.env"
  set +a
fi

usage() {
  cat <<'EOF'
Run distributed bench-write in prod: one client process per host via SSH.

Usage:
  ./setup/run_prod_distributed_write_bench.bash \
    --hosts host1,host2,host3 \
    --ssh-user <netid> \
    --remote-repo-dir /home/<netid>/crown \
    --config-path build/prod_configs/config.crown.json \
    --write-op-count 50000 \
    --key-count 64 \
    [--ack-port 61000] \
    [--client-bin build/client] \
    [--remote-log-dir build/prod_bench_logs] \
    [--local-log-dir build/prod_ssh_launcher_logs] \
    [--hosts-file setup/prod_hosts.txt] \
    [--ssh-key /path/to/key] \
    [--key-prefix bench-key-] \
    [--value-prefix bench-value-] \
    [--dry-run]

Notes:
  - The script launches all hosts in parallel and waits for completion.
  - client_index is assigned automatically as 0..N-1 in host order.
  - num_clients is N (the number of hosts).
  - write-op-count is GLOBAL across all hosts (split by client_index/num_clients).
  - configure=true is used only on client_index=0; others use configure=false.
  - --value-prefix requires --key-prefix (client args are positional).

Examples:
  ./setup/run_prod_distributed_write_bench.bash \
    --hosts sp26-cs525-1201.cs.illinois.edu,sp26-cs525-1202.cs.illinois.edu,sp26-cs525-1203.cs.illinois.edu \
    --ssh-user <netid> \
    --remote-repo-dir /home/<netid>/crown \
    --config-path build/prod_configs/config.crown.json \
    --write-op-count 50000 \
    --key-count 64
EOF
}

trim_spaces() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "$s"
}

shell_quote() {
  local s="$1"
  s=${s//\'/\'\"\'\"\'}
  printf "'%s'" "$s"
}

join_quoted_args() {
  local out=""
  local arg
  for arg in "$@"; do
    if [[ -n "$out" ]]; then
      out+=" "
    fi
    out+="$(shell_quote "$arg")"
  done
  printf '%s' "$out"
}

SSH_USER="${SSH_USER:-}"
SSH_KEY="${SSH_KEY_LOCAL:-}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR:-}"
CLIENT_BIN="build/client"
CONFIG_PATH="build/prod_configs/config.crown.json"
WRITE_OP_COUNT="50000"
KEY_COUNT="64"
ACK_PORT="61000"
REMOTE_LOG_DIR="build/prod_bench_logs"
LOCAL_LOG_DIR="build/prod_ssh_launcher_logs"
HOSTS_CSV=""
HOSTS_FILE=""
KEY_PREFIX=""
VALUE_PREFIX=""
DRY_RUN="false"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hosts)
      HOSTS_CSV="${2:-}"
      shift 2
      ;;
    --hosts-file)
      HOSTS_FILE="${2:-}"
      shift 2
      ;;
    --ssh-user)
      SSH_USER="${2:-}"
      shift 2
      ;;
    --ssh-key)
      SSH_KEY="${2:-}"
      shift 2
      ;;
    --remote-repo-dir)
      REMOTE_REPO_DIR="${2:-}"
      shift 2
      ;;
    --client-bin)
      CLIENT_BIN="${2:-}"
      shift 2
      ;;
    --config-path)
      CONFIG_PATH="${2:-}"
      shift 2
      ;;
    --write-op-count)
      WRITE_OP_COUNT="${2:-}"
      shift 2
      ;;
    --key-count)
      KEY_COUNT="${2:-}"
      shift 2
      ;;
    --ack-port)
      ACK_PORT="${2:-}"
      shift 2
      ;;
    --remote-log-dir)
      REMOTE_LOG_DIR="${2:-}"
      shift 2
      ;;
    --local-log-dir)
      LOCAL_LOG_DIR="${2:-}"
      shift 2
      ;;
    --key-prefix)
      KEY_PREFIX="${2:-}"
      shift 2
      ;;
    --value-prefix)
      VALUE_PREFIX="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN="true"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Error: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$SSH_USER" ]]; then
  echo "Error: --ssh-user is required (or set SSH_USER in setup/.env)." >&2
  exit 1
fi

if [[ -z "$REMOTE_REPO_DIR" ]]; then
  REMOTE_REPO_DIR="/home/${SSH_USER}/crown"
fi

if [[ -n "$VALUE_PREFIX" && -z "$KEY_PREFIX" ]]; then
  echo "Error: --value-prefix requires --key-prefix." >&2
  exit 1
fi

if ! [[ "$WRITE_OP_COUNT" =~ ^[0-9]+$ ]] || [[ "$WRITE_OP_COUNT" -le 0 ]]; then
  echo "Error: --write-op-count must be an integer > 0." >&2
  exit 1
fi
if ! [[ "$KEY_COUNT" =~ ^[0-9]+$ ]] || [[ "$KEY_COUNT" -le 0 ]]; then
  echo "Error: --key-count must be an integer > 0." >&2
  exit 1
fi
if ! [[ "$ACK_PORT" =~ ^[0-9]+$ ]] || [[ "$ACK_PORT" -lt 1 || "$ACK_PORT" -gt 65535 ]]; then
  echo "Error: --ack-port must be in [1, 65535]." >&2
  exit 1
fi

declare -a HOSTS=()

if [[ -n "$HOSTS_CSV" ]]; then
  IFS=',' read -r -a RAW_HOSTS <<< "$HOSTS_CSV"
  for raw in "${RAW_HOSTS[@]}"; do
    host="$(trim_spaces "$raw")"
    [[ -n "$host" ]] && HOSTS+=("$host")
  done
fi

if [[ -n "$HOSTS_FILE" ]]; then
  if [[ ! -f "$HOSTS_FILE" ]]; then
    echo "Error: hosts file not found: $HOSTS_FILE" >&2
    exit 1
  fi

  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    host="$(trim_spaces "$line")"
    [[ -n "$host" ]] && HOSTS+=("$host")
  done < "$HOSTS_FILE"
fi

NUM_CLIENTS="${#HOSTS[@]}"
if [[ "$NUM_CLIENTS" -le 0 ]]; then
  echo "Error: no hosts provided. Use --hosts and/or --hosts-file." >&2
  exit 1
fi

SSH_OPTS=(-o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new)
if [[ -n "$SSH_KEY" ]]; then
  if [[ ! -f "$SSH_KEY" ]]; then
    echo "Error: ssh key not found: $SSH_KEY" >&2
    exit 1
  fi
  SSH_OPTS=(-i "$SSH_KEY" -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new)
fi

mkdir -p "$REPO_ROOT/$LOCAL_LOG_DIR"

echo "Launching distributed bench-write"
echo "  ssh_user:        $SSH_USER"
echo "  remote_repo_dir: $REMOTE_REPO_DIR"
echo "  config_path:     $CONFIG_PATH"
echo "  hosts:           ${HOSTS[*]}"
echo "  num_clients:     $NUM_CLIENTS"
echo "  write_op_count:  $WRITE_OP_COUNT"
echo "  key_count:       $KEY_COUNT"
echo "  ack_port:        $ACK_PORT"
echo "  dry_run:         $DRY_RUN"

declare -a PIDS=()
declare -A PID_TO_HOST=()
declare -A PID_TO_LOG=()
declare -A PID_TO_INDEX=()

for i in "${!HOSTS[@]}"; do
  host="${HOSTS[$i]}"
  configure_flag="false"
  if [[ "$i" -eq 0 ]]; then
    configure_flag="true"
  fi

  remote_log_file="$REMOTE_LOG_DIR/write_c${NUM_CLIENTS}_i${i}.log"

  client_args=(
    "$CLIENT_BIN"
    "$CONFIG_PATH"
    "$configure_flag"
    "$ACK_PORT"
    "bench-write"
    "$WRITE_OP_COUNT"
    "$KEY_COUNT"
    "$i"
    "$NUM_CLIENTS"
  )
  if [[ -n "$KEY_PREFIX" ]]; then
    client_args+=("$KEY_PREFIX")
  fi
  if [[ -n "$VALUE_PREFIX" ]]; then
    client_args+=("$VALUE_PREFIX")
  fi

  client_cmd="$(join_quoted_args "${client_args[@]}")"
  remote_script="set -euo pipefail; \
cd $(shell_quote "$REMOTE_REPO_DIR"); \
if [[ ! -x $(shell_quote "$CLIENT_BIN") ]]; then echo 'missing client binary: $CLIENT_BIN' >&2; exit 10; fi; \
if [[ ! -f $(shell_quote "$CONFIG_PATH") ]]; then echo 'missing config file: $CONFIG_PATH' >&2; exit 11; fi; \
mkdir -p $(shell_quote "$REMOTE_LOG_DIR"); \
echo '[remote] host='\"\$(hostname -f 2>/dev/null || hostname)\"' client_index=$i/$NUM_CLIENTS configure=$configure_flag'; \
$client_cmd > $(shell_quote "$remote_log_file") 2>&1; \
echo '[remote] done client_index=$i log=$remote_log_file'"

  host_tag="${host//[^a-zA-Z0-9._-]/_}"
  local_ssh_log="$REPO_ROOT/$LOCAL_LOG_DIR/${host_tag}_i${i}.log"

  echo "[launch] host=$host client_index=$i configure=$configure_flag remote_log=$remote_log_file"

  if [[ "$DRY_RUN" == "true" ]]; then
    echo "[dry-run] ssh ${SSH_USER}@${host} bash -lc $(shell_quote "$remote_script")"
    continue
  fi

  ssh "${SSH_OPTS[@]}" "${SSH_USER}@${host}" "bash -lc $(shell_quote "$remote_script")" >"$local_ssh_log" 2>&1 &
  pid=$!
  PIDS+=("$pid")
  PID_TO_HOST["$pid"]="$host"
  PID_TO_LOG["$pid"]="$local_ssh_log"
  PID_TO_INDEX["$pid"]="$i"
done

if [[ "$DRY_RUN" == "true" ]]; then
  echo "Dry run complete."
  exit 0
fi

failures=0
for pid in "${PIDS[@]}"; do
  host="${PID_TO_HOST[$pid]}"
  idx="${PID_TO_INDEX[$pid]}"
  log_file="${PID_TO_LOG[$pid]}"

  if wait "$pid"; then
    echo "[ok]   host=$host client_index=$idx"
  else
    echo "[fail] host=$host client_index=$idx (see $log_file)" >&2
    failures=$((failures + 1))
  fi
done

if [[ "$failures" -ne 0 ]]; then
  echo "Distributed write benchmark finished with failures: $failures" >&2
  exit 1
fi

echo "Distributed write benchmark finished successfully."
echo "Remote client logs: $REMOTE_LOG_DIR"
echo "Local SSH logs: $LOCAL_LOG_DIR"
