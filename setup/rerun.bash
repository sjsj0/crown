#!/usr/bin/env bash
set -euo pipefail

# rerun: pull latest $REPO_BRANCH, rebuild, then restart the server node in tmux.
# Pass --skip-build to skip the cmake build (still pulls + restarts) -- useful
# when the binary is already current and you just want a fast restart.

usage() {
  echo "Usage: $(basename "${BASH_SOURCE[0]}")  [--skip-build]"
  echo "  Pull latest \$REPO_BRANCH, rebuild (skipped with --skip-build), then restart the server node."
}

SKIP_BUILD=false
for arg in "$@"; do
  case "$arg" in
    --skip-build) SKIP_BUILD=true ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown argument: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/.env" ]]; then
  # shellcheck source=/dev/null
  source "$SCRIPT_DIR/.env"
fi

DEPLOY_USER="${SSH_USER:-$(whoami)}"
REPO_URL="${REPO_URL:-}"
REPO_BRANCH="${REPO_BRANCH:-main}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-/home}"
REPO_NAME="${REPO_NAME:-$(basename "${REPO_URL%.git}")}"
PROJECT_SUBDIR="${PROJECT_SUBDIR:-.}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

REPO_DIR="$REMOTE_BASE_DIR/$REPO_NAME"

# ---------------------------
# 1) Pull latest
# ---------------------------
if [[ -d "$REPO_DIR/.git" ]]; then
  echo "Pulling latest branch '$REPO_BRANCH' in $REPO_DIR"
  # Shared $REPO_DIR is often owned by whichever user first cloned it; mark it
  # trusted so git doesn't abort with "dubious ownership".
  git config --global --add safe.directory "$REPO_DIR" >/dev/null 2>&1 || true
  git -C "$REPO_DIR" fetch --all --prune
  git -C "$REPO_DIR" checkout -f "$REPO_BRANCH"
  git -C "$REPO_DIR" pull --ff-only origin "$REPO_BRANCH"
else
  echo "Warning: $REPO_DIR/.git not found; skipping git pull (run './vm_setup.bash build' first)."
fi

# ---------------------------
# 2) Locate project directory
# ---------------------------
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

# ---------------------------
# 3) Build (unless --skip-build)
# ---------------------------
if [[ "$SKIP_BUILD" == "true" ]]; then
  echo "--skip-build: not rebuilding; restarting the existing binary."
else
  for tool in cmake g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      echo "ERROR: $tool is not installed on this VM. Run: ./vm_setup.bash setup"
      exit 1
    fi
  done

  # Shared deployments can leave build/_deps owned by a different user;
  # normalize perms and clear stale FetchContent cache before configuring.
  if [[ -d "build" ]]; then
    chmod -R u+rwX,go+rwX build 2>/dev/null || true
    if [[ -d "build/_deps" || -f "build/CMakeCache.txt" || -d "build/CMakeFiles" ]]; then
      echo "Clearing stale CMake/FetchContent state..."
      rm -rf build/_deps build/CMakeCache.txt build/CMakeFiles 2>/dev/null || true
    fi
  fi

  echo "Configuring with CMake (type=$BUILD_TYPE)..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

  CPU_COUNT=2
  if command -v nproc >/dev/null 2>&1; then
    CPU_COUNT="$(nproc)"
  fi
  echo "Building with $CPU_COUNT parallel jobs..."
  cmake --build build -j "$CPU_COUNT"
  echo "Build completed."
fi

# ---------------------------
# 4) Locate server binary
# ---------------------------
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
  if [[ "$SKIP_BUILD" == "true" ]]; then
    echo "ERROR: server binary not found (--skip-build was set; re-run without it to build first)."
  else
    echo "ERROR: server binary not found after build."
  fi
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

# ---------------------------
# 5) Restart server in tmux
# ---------------------------
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
