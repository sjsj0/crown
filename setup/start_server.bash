#!/usr/bin/env bash
set -euo pipefail

DEPLOY_USER="${SSH_USER:-$(whoami)}"
echo "=== Server deploy starting (user: $DEPLOY_USER) ==="

# ---------------------------
# 1) Clone or refresh repo
# ---------------------------
REPO_URL="${REPO_URL:?REPO_URL is required}"
REPO_BRANCH="${REPO_BRANCH:-main}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-/home}"
REPO_NAME="${REPO_NAME:-$(basename "${REPO_URL%.git}")}"
# Support both standalone CRAQ and main crown implementation
PROJECT_SUBDIR="${PROJECT_SUBDIR:-.}"
PROJECT_MODE="${PROJECT_MODE:-crown}"  # 'crown' for main src, 'craq' for standalone

if [[ ! -d "$REMOTE_BASE_DIR" ]]; then
  if mkdir -p "$REMOTE_BASE_DIR" 2>/dev/null; then
    :
  elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    sudo mkdir -p "$REMOTE_BASE_DIR"
  else
    echo "ERROR: cannot create $REMOTE_BASE_DIR. Grant write access or configure passwordless sudo."
    exit 1
  fi
fi

mkdir -p "$REMOTE_BASE_DIR"
cd "$REMOTE_BASE_DIR"

REPO_DIR="$REMOTE_BASE_DIR/$REPO_NAME"
if [[ ! -e "$REPO_DIR" && ! -w "$REMOTE_BASE_DIR" ]]; then
  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    echo "Preparing shared repo path with sudo: $REPO_DIR"
    sudo mkdir -p "$REPO_DIR"
    sudo chown -R "$DEPLOY_USER":"$DEPLOY_USER" "$REPO_DIR"
    sudo chmod -R 777 "$REPO_DIR"
  else
    echo "ERROR: $REMOTE_BASE_DIR is not writable for $DEPLOY_USER."
    echo "       Configure passwordless sudo or choose a writable REMOTE_BASE_DIR."
    exit 1
  fi
fi

if [[ -e "$REPO_DIR" && ! -w "$REPO_DIR" ]]; then
  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    echo "Fixing repo ownership with sudo: $REPO_DIR"
    sudo chown -R "$DEPLOY_USER":"$DEPLOY_USER" "$REPO_DIR"
    sudo chmod -R 777 "$REPO_DIR"
  else
    echo "ERROR: $REPO_DIR is not writable for $DEPLOY_USER."
    echo "       Configure passwordless sudo or fix ownership manually."
    exit 1
  fi
fi

if [[ -d "$REPO_NAME/.git" ]]; then
  echo "Repo exists; pulling latest branch: $REPO_BRANCH"
  git -C "$REPO_NAME" fetch --all --prune
  git -C "$REPO_NAME" checkout -f "$REPO_BRANCH"
  git -C "$REPO_NAME" pull --ff-only origin "$REPO_BRANCH"
else
  echo "Cloning fresh: $REPO_URL"
  git clone -b "$REPO_BRANCH" "$REPO_URL" "$REPO_NAME"
fi

# Always enforce shared access on the repo directory.
if [[ -e "$REPO_DIR" ]]; then
  if chmod -R 777 "$REPO_DIR" 2>/dev/null; then
    :
  elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    sudo chmod -R 777 "$REPO_DIR"
  else
    echo "Warning: unable to set shared permissions on $REPO_DIR (need sudo)."
  fi
fi

# ---------------------------
# 2) Resolve project path based on mode
# ---------------------------
PRIMARY_PROJECT_DIR="$REMOTE_BASE_DIR/$REPO_NAME/$PROJECT_SUBDIR"
ROOT_PROJECT_DIR="$REMOTE_BASE_DIR/$REPO_NAME"
LEGACY_CRAQ_DIR="$REMOTE_BASE_DIR/$REPO_NAME/craq"

if [[ -f "$PRIMARY_PROJECT_DIR/CMakeLists.txt" ]]; then
  PROJECT_DIR="$PRIMARY_PROJECT_DIR"
elif [[ -f "$ROOT_PROJECT_DIR/CMakeLists.txt" ]]; then
  if [[ "$PROJECT_SUBDIR" != "." ]]; then
    echo "Warning: $PRIMARY_PROJECT_DIR/CMakeLists.txt not found; falling back to repo root."
  fi
  PROJECT_DIR="$ROOT_PROJECT_DIR"
elif [[ "$PROJECT_MODE" == "craq" && -f "$LEGACY_CRAQ_DIR/CMakeLists.txt" ]]; then
  echo "Warning: falling back to legacy CRAQ subdirectory at $LEGACY_CRAQ_DIR"
  PROJECT_DIR="$LEGACY_CRAQ_DIR"
else
  echo "ERROR: Project not found. Checked:"
  echo "  - $PRIMARY_PROJECT_DIR/CMakeLists.txt"
  echo "  - $ROOT_PROJECT_DIR/CMakeLists.txt"
  echo "  - $LEGACY_CRAQ_DIR/CMakeLists.txt"
  exit 1
fi

echo "Project mode: $PROJECT_MODE"
echo "Project source: $PROJECT_DIR"
cd "$PROJECT_DIR"

# ---------------------------
# 2.5) Preflight tool checks
# ---------------------------
for tool in git cmake g++ tmux; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: $tool is not installed on this VM. Run: ./vm_setup.bash setup"
    exit 1
  fi
done

# ---------------------------
# 3) Configure + build
# ---------------------------
BUILD_TYPE="${BUILD_TYPE:-Release}"
echo "Configuring with CMake (type=$BUILD_TYPE)..."
cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

CPU_COUNT=2
if command -v nproc >/dev/null 2>&1; then
  CPU_COUNT="$(nproc)"
fi
echo "Building with $CPU_COUNT parallel jobs..."
cmake --build build -j "$CPU_COUNT"

echo "Build completed."

NODE_BIN=""
if [[ -x "build/server" ]]; then
  NODE_BIN="build/server"
elif [[ -x "build/Debug/server" ]]; then
  NODE_BIN="build/Debug/server"
fi

if [[ -z "$NODE_BIN" ]]; then
  echo "ERROR: server binary not found after build."
  echo "       Looked for build/server (and build/Debug/server)."
  exit 1
fi

# ---------------------------
# 4) Run server in tmux
# ---------------------------
NODE_HOST="${NODE_HOST:-0.0.0.0}"
NODE_PORT="${NODE_PORT:-5001}"
SERVER_LOG_RAW="${SERVER_LOG:-false}"
case "${SERVER_LOG_RAW,,}" in
  1|true|yes|y|on) SERVER_LOG="true" ;;
  0|false|no|n|off|"") SERVER_LOG="false" ;;
  *)
    echo "ERROR: SERVER_LOG must be true/false (or 1/0, yes/no). Got: $SERVER_LOG_RAW"
    exit 1
    ;;
esac
RUN_SCOPE="${RUN_SCOPE:-shared}"
RUN_DIR="$PROJECT_DIR/run/$RUN_SCOPE"
mkdir -p "$RUN_DIR"
PID_FILE="$RUN_DIR/server_${NODE_PORT}.pid"
LOG_FILE="$RUN_DIR/server_${NODE_PORT}.log"
# SESSION_NAME="${TMUX_SESSION_NAME:-crown_node_${NODE_PORT}}"
SESSION_NAME="${TMUX_SESSION_NAME:-crown}"
TMUX_SOCKET="${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}"
TMUX_SOCKET_DIR="$(dirname "$TMUX_SOCKET")"
mkdir -p "$TMUX_SOCKET_DIR"
chmod 1777 "$TMUX_SOCKET_DIR" 2>/dev/null || true
TMUX_CMD=(tmux -S "$TMUX_SOCKET")

echo "Shared paths:"
echo "  run_dir: $RUN_DIR"
echo "  pid_file: $PID_FILE"
echo "  log_file: $LOG_FILE"
echo "  session: $SESSION_NAME"
echo "  tmux_socket: $TMUX_SOCKET"

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

# Kill any remaining server process for this user/port before starting.
pkill -u "$DEPLOY_USER" -f "server --host .* --port $NODE_PORT" >/dev/null 2>&1 || true

echo "Starting $NODE_BIN --host $NODE_HOST --port $NODE_PORT --server-log $SERVER_LOG"
"${TMUX_CMD[@]}" new-session -d -s "$SESSION_NAME" "cd '$PROJECT_DIR' && exec '$NODE_BIN' --host '$NODE_HOST' --port '$NODE_PORT' --server-log '$SERVER_LOG'"
chmod 666 "$TMUX_SOCKET" 2>/dev/null || true
"${TMUX_CMD[@]}" pipe-pane -o -t "$SESSION_NAME:0.0" "cat >> '$LOG_FILE'"

NEW_PID="$("${TMUX_CMD[@]}" display-message -p -t "$SESSION_NAME:0.0" "#{pane_pid}")"
echo "$NEW_PID" > "$PID_FILE"

echo "Server started in tmux session: $SESSION_NAME"
echo "Server pane pid: $NEW_PID"
echo "log: $LOG_FILE"
echo "pid: $PID_FILE"
echo "attach: tmux -S $TMUX_SOCKET attach -t $SESSION_NAME"

## Legacy (old project) behavior retained as comment:
## - clone old hydfs-g33 repo
## - create tmux sessions
## - run go daemons in src/main and src/ctl
