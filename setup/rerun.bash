#!/usr/bin/env bash
set -euo pipefail

# rerun: kill the existing server node on this VM and start it again (using the
# binary that's already built). Does NOT pull or rebuild -- run
# `./vm_setup.bash build` first if you want fresh code.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/.env" ]]; then
  # shellcheck source=/dev/null
  source "$SCRIPT_DIR/.env"
fi

DEPLOY_USER="${SSH_USER:-$(whoami)}"
REPO_URL="${REPO_URL:-}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-/home}"
REPO_NAME="${REPO_NAME:-$(basename "${REPO_URL%.git}")}"
PROJECT_SUBDIR="${PROJECT_SUBDIR:-.}"
PROJECT_DIR="$REMOTE_BASE_DIR/$REPO_NAME/$PROJECT_SUBDIR"

PROJECT_DIR_CANDIDATES=(
  "$PROJECT_DIR"
  "$REMOTE_BASE_DIR/$REPO_NAME"
  "/home/$DEPLOY_USER/$REPO_NAME/$PROJECT_SUBDIR"
  "/home/$DEPLOY_USER/$REPO_NAME"
  "$SCRIPT_DIR/.."
)

FOUND_PROJECT_DIR=""
for candidate in "${PROJECT_DIR_CANDIDATES[@]}"; do
  if [[ -d "$candidate" && -f "$candidate/CMakeLists.txt" ]]; then
    FOUND_PROJECT_DIR="$candidate"
    break
  fi
done

if [[ -z "$FOUND_PROJECT_DIR" ]]; then
  echo "ERROR: could not locate project directory with CMakeLists.txt"
  echo "Checked:"
  for candidate in "${PROJECT_DIR_CANDIDATES[@]}"; do
    echo "  - $candidate"
  done
  echo "Set REMOTE_BASE_DIR/REPO_NAME/PROJECT_SUBDIR in setup/.env, then retry."
  exit 1
fi

PROJECT_DIR="$FOUND_PROJECT_DIR"

cd "$PROJECT_DIR"

NODE_BIN=""
for candidate in \
  "build/server" \
  "build/Debug/server" \
  "build/Release/server" \
  "build/RelWithDebInfo/server" \
  "build/MinSizeRel/server"
do
  if [[ -x "$candidate" ]]; then
    NODE_BIN="$candidate"
    break
  fi
done

if [[ -z "$NODE_BIN" ]]; then
  echo "ERROR: server binary not found. Build it first: ./vm_setup.bash build"
  echo "Project dir: $PROJECT_DIR"
  echo "Looked for:"
  echo "  - build/server"
  echo "  - build/Debug/server"
  echo "  - build/Release/server"
  echo "  - build/RelWithDebInfo/server"
  echo "  - build/MinSizeRel/server"
  if [[ -d "build" ]]; then
    echo "Top-level build contents:"
    ls -1 build | sed 's/^/  - /'
  fi
  exit 1
fi

NODE_HOST="${NODE_HOST:-0.0.0.0}"
NODE_PORT="${NODE_PORT:-5001}"
SERVER_LOG_RAW="${SERVER_LOG:-true}"
case "${SERVER_LOG_RAW,,}" in
  1|true|yes|y|on) SERVER_LOG="true" ;;
  0|false|no|n|off|"") SERVER_LOG="false" ;;
  *)
    echo "ERROR: SERVER_LOG must be true/false (or 1/0, yes/no). Got: $SERVER_LOG_RAW"
    exit 1
    ;;
esac
RUN_SCOPE="${RUN_SCOPE:-shared}"
TMUX_SOCKET="${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}"
SESSION_NAME="${TMUX_SESSION_NAME:-crown}"

RUN_DIR="$PROJECT_DIR/run/$RUN_SCOPE"
mkdir -p "$RUN_DIR"
PID_FILE="$RUN_DIR/server_${NODE_PORT}.pid"
LOG_FILE="$RUN_DIR/server_${NODE_PORT}.log"
OUT_FILE="$RUN_DIR/server_${NODE_PORT}.out"

TMUX_SOCKET_DIR="$(dirname "$TMUX_SOCKET")"
mkdir -p "$TMUX_SOCKET_DIR"
chmod 1777 "$TMUX_SOCKET_DIR" 2>/dev/null || true
TMUX_CMD=(tmux -S "$TMUX_SOCKET")

# Aggressive cleanup of existing sessions and processes
if "${TMUX_CMD[@]}" has-session -t "$SESSION_NAME" 2>/dev/null; then
  echo "Stopping existing tmux session: $SESSION_NAME"
  "${TMUX_CMD[@]}" kill-session -t "$SESSION_NAME" || true

  # Wait for session to fully terminate (with retry)
  for i in {1..10}; do
    sleep 0.5
    if ! "${TMUX_CMD[@]}" has-session -t "$SESSION_NAME" 2>/dev/null; then
      echo "Tmux session killed after $(( i / 2 )) seconds"
      break
    fi
    if [[ $i -eq 10 ]]; then
      echo "WARNING: Tmux session still exists after 5 seconds, forcing cleanup..."
      # Force kill the pane process
      PANE_PID=$("${TMUX_CMD[@]}" display-message -p -t "$SESSION_NAME:0.0" "#{pane_pid}" 2>/dev/null || true)
      if [[ -n "$PANE_PID" ]]; then
        kill -9 "$PANE_PID" 2>/dev/null || true
      fi
      sleep 1
    fi
  done
fi

# Kill any lingering processes on the port
if [[ -f "$PID_FILE" ]]; then
  OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
  if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
    echo "Stopping existing process pid=$OLD_PID"
    kill "$OLD_PID" || true
    sleep 0.5
    # Force kill if still running
    kill -9 "$OLD_PID" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
fi

# Catch-all: kill any server process on this port
pkill -u "$DEPLOY_USER" -f "server --host .* --port $NODE_PORT" >/dev/null 2>&1 || true
sleep 0.5
pkill -9 -u "$DEPLOY_USER" -f "server --host .* --port $NODE_PORT" >/dev/null 2>&1 || true

# Clean up stale tmux socket if session won't die
if [[ -S "$TMUX_SOCKET" ]] && "${TMUX_CMD[@]}" has-session -t "$SESSION_NAME" 2>/dev/null; then
  echo "WARNING: Stale tmux socket, removing and reconnecting..."
  rm -f "$TMUX_SOCKET"
  sleep 1
fi

SERVER_CMD="cd '$PROJECT_DIR' && exec '$NODE_BIN' --host '$NODE_HOST' --port '$NODE_PORT' --server-log '$SERVER_LOG'"
echo "Run command: $SERVER_CMD"
echo "Starting $NODE_BIN --host $NODE_HOST --port $NODE_PORT --server-log $SERVER_LOG"
printf '[launch] %s\n' "$SERVER_CMD" | tee -a "$LOG_FILE" >> "$OUT_FILE"
"${TMUX_CMD[@]}" new-session -d -s "$SESSION_NAME" "$SERVER_CMD"
chmod 666 "$TMUX_SOCKET" 2>/dev/null || true
"${TMUX_CMD[@]}" pipe-pane -o -t "$SESSION_NAME:0.0" "cat | tee -a '$LOG_FILE' >> '$OUT_FILE'"

NEW_PID="$("${TMUX_CMD[@]}" display-message -p -t "$SESSION_NAME:0.0" "#{pane_pid}")"
echo "$NEW_PID" > "$PID_FILE"

echo "Server started in tmux session: $SESSION_NAME"
echo "Server pane pid: $NEW_PID"
echo "log: $LOG_FILE"
echo "out: $OUT_FILE"
echo "pid: $PID_FILE"
echo "  attach: tmux -S $TMUX_SOCKET attach -t $SESSION_NAME"
