#!/usr/bin/env bash
set -euo pipefail

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
  echo "ERROR: server binary not found after build."
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
RUN_SCOPE="${RUN_SCOPE:-shared}"
TMUX_SOCKET="${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}"
SESSION_NAME="${TMUX_SESSION_NAME:-crown}"

RUN_DIR="$PROJECT_DIR/run/$RUN_SCOPE"
mkdir -p "$RUN_DIR"
PID_FILE="$RUN_DIR/server_${NODE_PORT}.pid"
LOG_FILE="$RUN_DIR/server_${NODE_PORT}.log"

TMUX_SOCKET_DIR="$(dirname "$TMUX_SOCKET")"
mkdir -p "$TMUX_SOCKET_DIR"
chmod 1777 "$TMUX_SOCKET_DIR" 2>/dev/null || true
TMUX_CMD=(tmux -S "$TMUX_SOCKET")

if "${TMUX_CMD[@]}" has-session -t "$SESSION_NAME" 2>/dev/null; then
  echo "Stopping existing tmux session: $SESSION_NAME"
  "${TMUX_CMD[@]}" kill-session -t "$SESSION_NAME" || true
  sleep 1
fi

if [[ -f "$PID_FILE" ]]; then
  OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
  if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
    echo "Stopping existing process pid=$OLD_PID"
    kill "$OLD_PID" || true
    sleep 1
  fi
fi

pkill -u "$DEPLOY_USER" -f "server --host .* --port $NODE_PORT" >/dev/null 2>&1 || true

echo "Starting $NODE_BIN --host $NODE_HOST --port $NODE_PORT"
"${TMUX_CMD[@]}" new-session -d -s "$SESSION_NAME" "cd '$PROJECT_DIR' && exec '$NODE_BIN' --host '$NODE_HOST' --port '$NODE_PORT'"
chmod 666 "$TMUX_SOCKET" 2>/dev/null || true
"${TMUX_CMD[@]}" pipe-pane -o -t "$SESSION_NAME:0.0" "cat >> '$LOG_FILE'"

NEW_PID="$("${TMUX_CMD[@]}" display-message -p -t "$SESSION_NAME:0.0" "#{pane_pid}")"
echo "$NEW_PID" > "$PID_FILE"

echo "Server started in tmux session: $SESSION_NAME"
echo "Server pane pid: $NEW_PID"
echo "log: $LOG_FILE"
echo "pid: $PID_FILE"
echo "  attach: tmux -S $TMUX_SOCKET attach -t $SESSION_NAME"
