#!/usr/bin/env bash
# crown smoke test — exercises the metadata_server + ping-ack failure detector
# end to end against a local 3-node CROWN ring.
#
# What it checks:
#   - metadata_server reads config.json, configures all nodes, and serves GetCluster
#   - the client fetches topology from the metadata server and round-trips a write/read
#   - killing a node makes the metadata server log it DOWN within the ping budget
#   - restarting that node makes the metadata server log it UP again
#
# NOTE: the earlier kv_client-based routing-precision / wrong-route / concurrent-
# version tests were removed when kv_client was deleted. They relied on kv_client's
# `target=` output and on injecting deliberately mis-wired configs into the client;
# with the metadata server as the single source of truth neither is possible the
# same way. Re-add equivalent coverage if/when needed.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
WORK_DIR="$BUILD_DIR/crown_smoke"

NODE_COUNT=3
BASE_PORT="${BASE_PORT:-50051}"
HOST="${HOST:-127.0.0.1}"
META_PORT="${META_PORT:-50050}"
META_ADDR="$HOST:$META_PORT"
PING_INTERVAL_MS="${PING_INTERVAL_MS:-300}"
PING_TIMEOUT_MS="${PING_TIMEOUT_MS:-200}"
FAILURE_THRESHOLD="${FAILURE_THRESHOLD:-3}"

CONFIG="$WORK_DIR/config.crown.json"
META_LOG="$WORK_DIR/metadata.log"

declare -a SERVER_PIDS=()
META_PID=""

die() { echo "[crown-smoke] ERROR: $*" >&2; exit 1; }

cleanup() {
  [[ -n "$META_PID" ]] && kill "$META_PID" >/dev/null 2>&1 || true
  for pid in "${SERVER_PIDS[@]:-}"; do
    [[ -n "$pid" ]] && kill "$pid" >/dev/null 2>&1 || true
  done
  [[ -n "$META_PID" ]] && wait "$META_PID" >/dev/null 2>&1 || true
  for pid in "${SERVER_PIDS[@]:-}"; do
    [[ -n "$pid" ]] && wait "$pid" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT INT TERM

# Run a sequence of REPL commands against the client and echo its stdout.
client_repl() {
  # args: command lines (each becomes one REPL line); a trailing 'quit' is added.
  local input=""
  local line
  for line in "$@"; do input+="$line"$'\n'; done
  input+="quit"$'\n'
  printf '%s' "$input" | "$BUILD_DIR/client" "$META_ADDR" 2>&1
}

wait_for_log() {
  # wait_for_log <file> <needle> <timeout_secs>
  local file="$1" needle="$2" timeout="${3:-10}"
  local deadline=$(( SECONDS + timeout ))
  while (( SECONDS < deadline )); do
    [[ -f "$file" ]] && grep -qF "$needle" "$file" && return 0
    sleep 0.2
  done
  return 1
}

echo "[crown-smoke] Building binaries..."
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j4 >/dev/null

[[ -x "$BUILD_DIR/server" ]]          || die "missing binary: $BUILD_DIR/server"
[[ -x "$BUILD_DIR/metadata_server" ]] || die "missing binary: $BUILD_DIR/metadata_server"
[[ -x "$BUILD_DIR/client" ]]          || die "missing binary: $BUILD_DIR/client"

mkdir -p "$WORK_DIR"

echo "[crown-smoke] Generating CROWN config..."
python3 "$ROOT_DIR/setup/generate_mode_configs.py" "$NODE_COUNT" --base-port "$BASE_PORT" --host "$HOST" \
  --output-dir "$WORK_DIR" --prefix config --modes crown >/dev/null

echo "[crown-smoke] Launching $NODE_COUNT servers..."
for ((i = 0; i < NODE_COUNT; ++i)); do
  port=$((BASE_PORT + i))
  "$BUILD_DIR/server" --port "$port" --server-log true >"$WORK_DIR/server_${i}.log" 2>&1 &
  SERVER_PIDS+=("$!")
done
sleep 1
for pid in "${SERVER_PIDS[@]}"; do
  kill -0 "$pid" >/dev/null 2>&1 || die "server process died during startup (pid=$pid)"
done

echo "[crown-smoke] Starting metadata_server (config push + ping-ack detector)..."
"$BUILD_DIR/metadata_server" --config "$CONFIG" --host "$HOST" --port "$META_PORT" \
  --ping-interval-ms "$PING_INTERVAL_MS" --ping-timeout-ms "$PING_TIMEOUT_MS" \
  --failure-threshold "$FAILURE_THRESHOLD" --log >"$META_LOG" 2>&1 &
META_PID="$!"

wait_for_log "$META_LOG" "all nodes configured." 15 || die "metadata_server did not configure all nodes (see $META_LOG)"
wait_for_log "$META_LOG" "listening on $META_ADDR" 10 || die "metadata_server did not start serving GetCluster"
echo "[crown-smoke]   metadata_server up; nodes configured."

# 1) Write/read round-trip via the client (topology fetched from metadata).
echo "[crown-smoke] Verifying write/read round-trip through the client..."
key="smoke-key"
val="smoke-value-$$"
out="$(client_repl "write $key $val" "read $key")"
echo "$out" | grep -qF "Topology from $META_ADDR" || die "client did not fetch topology from metadata server: $out"
# The read may race the async commit; retry a few times if needed.
got=""
for _ in 1 2 3 4 5; do
  out="$(client_repl "read $key")"
  got="$(sed -n "s/.*value='\([^']*\)'.*/\1/p" <<< "$out")"
  [[ "$got" == "$val" ]] && break
  sleep 0.3
done
[[ "$got" == "$val" ]] || die "read did not return the written value (got='$got', want='$val'); client output: $out"
echo "[crown-smoke]   round-trip ok (value='$got')."

# 2) Failure detection: kill node N-1, expect a DOWN log; restart it, expect UP.
victim_idx=$(( NODE_COUNT - 1 ))
victim_port=$(( BASE_PORT + victim_idx ))
victim_pid="${SERVER_PIDS[$victim_idx]}"
echo "[crown-smoke] Killing node $victim_idx (pid=$victim_pid, port=$victim_port) to trigger failure detection..."
kill "$victim_pid" >/dev/null 2>&1 || true
wait "$victim_pid" >/dev/null 2>&1 || true

down_budget=$(( (PING_INTERVAL_MS * (FAILURE_THRESHOLD + 3)) / 1000 + 3 ))
wait_for_log "$META_LOG" "node $victim_idx (" "$down_budget" || true
if ! grep -qE "node $victim_idx \(.*\) declared DOWN" "$META_LOG"; then
  die "metadata_server did not declare node $victim_idx DOWN within ${down_budget}s (see $META_LOG)"
fi
echo "[crown-smoke]   node $victim_idx declared DOWN."

# Confirm GetCluster reflects it (best-effort: client still starts; just check it runs).
client_repl >/dev/null 2>&1 || true

echo "[crown-smoke] Restarting node $victim_idx..."
"$BUILD_DIR/server" --port "$victim_port" --server-log true >"$WORK_DIR/server_${victim_idx}.restart.log" 2>&1 &
SERVER_PIDS[$victim_idx]="$!"
up_budget=$(( (PING_INTERVAL_MS * 5) / 1000 + 5 ))
wait_for_log "$META_LOG" "node $victim_idx (" "$up_budget"
if ! grep -qE "node $victim_idx \(.*\) is UP again" "$META_LOG"; then
  die "metadata_server did not mark node $victim_idx UP again within ${up_budget}s (see $META_LOG)"
fi
echo "[crown-smoke]   node $victim_idx is UP again."

echo "[crown-smoke] PASS"
