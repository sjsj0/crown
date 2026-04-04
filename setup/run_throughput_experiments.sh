#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/throughput_runs}"
LOG_DIR="$WORK_DIR/logs"
CFG_DIR="$WORK_DIR/configs"

BASE_CONFIG="${BASE_CONFIG:-$ROOT_DIR/config.json}"
CLIENT_BIN="${CLIENT_BIN:-$BUILD_DIR/client}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/server}"

MODES="${MODES:-chain craq crown}"
OPS="${OPS:-write read}"
CLIENT_COUNTS="${CLIENT_COUNTS:-1 2 4 8}"

DURATION_SEC="${DURATION_SEC:-10}"
WRITE_RATE_RPS="${WRITE_RATE_RPS:-100}"
READ_RATE_RPS="${READ_RATE_RPS:-100}"
KEY_COUNT="${KEY_COUNT:-64}"

KEY_PREFIX_BASE="${KEY_PREFIX_BASE:-bench-key}"
VALUE_PREFIX_BASE="${VALUE_PREFIX_BASE:-bench-value}"

ACK_BASE_PORT="${ACK_BASE_PORT:-61000}"
CRAQ_READ_NODE_ID="${CRAQ_READ_NODE_ID:--1}"

BUILD_FIRST="${BUILD_FIRST:-1}"
START_SERVERS="${START_SERVERS:-1}"
RECONFIGURE_EACH_RUN="${RECONFIGURE_EACH_RUN:-1}"

SUMMARY_CSV="$WORK_DIR/summary.csv"

declare -a SERVER_PIDS=()

log() {
  echo "[throughput-runner] $*"
}

die() {
  echo "[throughput-runner] ERROR: $*" >&2
  exit 1
}

wait_for_port() {
  local host="$1"
  local port="$2"
  local timeout_sec="$3"
  local deadline=$((SECONDS + timeout_sec))

  while (( SECONDS < deadline )); do
    if (echo >"/dev/tcp/$host/$port") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done

  return 1
}

is_truthy() {
  local value
  value="$(echo "${1:-}" | tr '[:upper:]' '[:lower:]')"
  case "$value" in
    1|true|yes|y|on) return 0 ;;
    0|false|no|n|off|"") return 1 ;;
    *) die "invalid boolean value: $1 (expected 1/0/true/false/yes/no)" ;;
  esac
}

cleanup() {
  for pid in "${SERVER_PIDS[@]:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done

  for pid in "${SERVER_PIDS[@]:-}"; do
    if [[ -n "$pid" ]]; then
      wait "$pid" >/dev/null 2>&1 || true
    fi
  done
}
trap cleanup EXIT INT TERM

mkdir -p "$LOG_DIR" "$CFG_DIR"

[[ -f "$BASE_CONFIG" ]] || die "missing base config: $BASE_CONFIG"

if is_truthy "$BUILD_FIRST"; then
  log "Building project..."
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" >/dev/null
  cmake --build "$BUILD_DIR" -j4 >/dev/null
fi

[[ -x "$CLIENT_BIN" ]] || die "missing client binary: $CLIENT_BIN"
[[ -x "$SERVER_BIN" ]] || die "missing server binary: $SERVER_BIN"

CFG_CHAIN="$CFG_DIR/config.chain.json"
CFG_CRAQ="$CFG_DIR/config.craq.json"
CFG_CROWN="$CFG_DIR/config.crown.json"

log "Preparing CHAIN/CRAQ configs from $BASE_CONFIG..."
readarray -t BASE_INFO < <(python3 - "$BASE_CONFIG" "$CFG_CHAIN" "$CFG_CRAQ" <<'PY'
import json
import sys

base, out_chain, out_craq = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = json.load(open(base, 'r', encoding='utf-8'))
nodes = cfg.get('nodes', [])
if not isinstance(nodes, list) or not nodes:
    raise SystemExit('invalid base config: missing non-empty nodes array')

ports = [int(n['port']) for n in nodes]
if sorted(ports) != list(range(min(ports), min(ports) + len(ports))):
    raise SystemExit('base config ports must be contiguous for crown generation')

cfg_chain = dict(cfg)
cfg_chain['mode'] = 'chain'
cfg_craq = dict(cfg)
cfg_craq['mode'] = 'craq'

with open(out_chain, 'w', encoding='utf-8') as f:
    json.dump(cfg_chain, f, indent=2)
    f.write('\n')

with open(out_craq, 'w', encoding='utf-8') as f:
    json.dump(cfg_craq, f, indent=2)
    f.write('\n')

first = nodes[0]
print(first['host'])
print(min(ports))
print(len(nodes))
PY
)

HOST="${BASE_INFO[0]}"
BASE_PORT="${BASE_INFO[1]}"
NODE_COUNT="${BASE_INFO[2]}"

log "Generating CROWN config (host=$HOST base_port=$BASE_PORT nodes=$NODE_COUNT)..."
python3 "$ROOT_DIR/setup/generate_crown_config.py" "$NODE_COUNT" "$BASE_PORT" --host "$HOST" --output "$CFG_CROWN" >/dev/null

if is_truthy "$START_SERVERS"; then
  log "Starting $NODE_COUNT server processes..."
  for ((i = 0; i < NODE_COUNT; ++i)); do
    port=$((BASE_PORT + i))
    server_log="$WORK_DIR/server_${port}.log"
    if command -v stdbuf >/dev/null 2>&1; then
      stdbuf -oL -eL "$SERVER_BIN" --port "$port" >"$server_log" 2>&1 &
    else
      "$SERVER_BIN" --port "$port" >"$server_log" 2>&1 &
    fi
    SERVER_PIDS+=("$!")
  done

  sleep 1
  for ((i = 0; i < NODE_COUNT; ++i)); do
    pid="${SERVER_PIDS[$i]}"
    port=$((BASE_PORT + i))
    kill -0 "$pid" >/dev/null 2>&1 || die "server process died during startup (pid=$pid)"
    wait_for_port "$HOST" "$port" 10 || die "server did not open $HOST:$port within startup timeout"
  done
fi

run_idx=0

for mode in $MODES; do
  case "$mode" in
    chain) cfg="$CFG_CHAIN" ;;
    craq) cfg="$CFG_CRAQ" ;;
    crown) cfg="$CFG_CROWN" ;;
    *) die "unknown mode in MODES: $mode" ;;
  esac

  for op in $OPS; do
    for nclients in $CLIENT_COUNTS; do
      log "Running mode=$mode op=$op clients=$nclients duration=${DURATION_SEC}s"
      declare -a CLIENT_PIDS=()

      for ((i = 0; i < nclients; ++i)); do
        ack_port=$((ACK_BASE_PORT + run_idx * 100 + i))
        configure_flag="false"
        if is_truthy "$RECONFIGURE_EACH_RUN" && [[ "$i" -eq 0 ]]; then
          configure_flag="true"
        fi

        key_prefix="${KEY_PREFIX_BASE}-${mode}-${op}-"
        value_prefix="${VALUE_PREFIX_BASE}-${mode}-${op}-"
        log_file="$LOG_DIR/${mode}_${op}_c${nclients}_i${i}.log"

        if [[ "$op" == "write" ]]; then
          "$CLIENT_BIN" "$cfg" "$configure_flag" "$ack_port" \
            bench-write "$DURATION_SEC" "$WRITE_RATE_RPS" "$KEY_COUNT" "$i" "$nclients" \
            "$key_prefix" "$value_prefix" >"$log_file" 2>&1 &
        elif [[ "$op" == "read" ]]; then
          "$CLIENT_BIN" "$cfg" "$configure_flag" "$ack_port" \
            bench-read "$DURATION_SEC" "$READ_RATE_RPS" "$KEY_COUNT" "$i" "$nclients" \
            "$CRAQ_READ_NODE_ID" "$key_prefix" >"$log_file" 2>&1 &
        else
          die "unknown op in OPS: $op"
        fi

        CLIENT_PIDS+=("$!")
      done

      failed=0
      for pid in "${CLIENT_PIDS[@]}"; do
        if ! wait "$pid"; then
          failed=1
        fi
      done

      if [[ "$failed" -ne 0 ]]; then
        die "one or more client processes failed for mode=$mode op=$op clients=$nclients"
      fi

      run_idx=$((run_idx + 1))
    done
  done
done

log "Aggregating BENCH_SUMMARY lines into CSV..."
python3 "$ROOT_DIR/setup/aggregate_bench_results.py" --logs-dir "$LOG_DIR" --output "$SUMMARY_CSV"

log "Completed."
log "Logs: $LOG_DIR"
log "Summary: $SUMMARY_CSV"
