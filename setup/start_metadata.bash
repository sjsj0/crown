#!/usr/bin/env bash
set -euo pipefail

# Deploys + builds the repo on this VM and starts the metadata_server in a tmux
# session. The metadata server is the single source of truth for cluster
# topology: it reads $METADATA_CONFIG, validates it, pushes Configure to every
# node, runs a ping-ack failure detector, and serves MetadataStore.GetCluster.
#
# Run this on exactly ONE VM (see METADATA_HOST in setup/.env). Mirrors
# start_server.bash for the clone/build steps.

DEPLOY_USER="${SSH_USER:-$(whoami)}"
echo "=== Metadata store deploy starting (user: $DEPLOY_USER) ==="

# ---------------------------
# 1) Clone or refresh repo
# ---------------------------
REPO_URL="${REPO_URL:?REPO_URL is required}"
REPO_BRANCH="${REPO_BRANCH:-main}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-/home}"
REPO_NAME="${REPO_NAME:-$(basename "${REPO_URL%.git}")}"
PROJECT_SUBDIR="${PROJECT_SUBDIR:-.}"
PROJECT_MODE="${PROJECT_MODE:-crown}"

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
if [[ -e "$REPO_DIR" && ! -w "$REPO_DIR" ]]; then
  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    sudo chown -R "$DEPLOY_USER":"$DEPLOY_USER" "$REPO_DIR"
    sudo chmod -R 777 "$REPO_DIR"
  else
    echo "ERROR: $REPO_DIR is not writable for $DEPLOY_USER."
    exit 1
  fi
fi

# Shared $REPO_DIR is often owned by whichever user first cloned it; mark it as
# a trusted path so git fetch/checkout/pull don't abort with "dubious ownership".
git config --global --add safe.directory "$REPO_DIR" >/dev/null 2>&1 || true

if [[ -d "$REPO_NAME/.git" ]]; then
  echo "Repo exists; pulling latest branch: $REPO_BRANCH"
  git -C "$REPO_NAME" fetch --all --prune
  git -C "$REPO_NAME" checkout -f "$REPO_BRANCH"
  git -C "$REPO_NAME" pull --ff-only origin "$REPO_BRANCH"
else
  echo "Cloning fresh: $REPO_URL"
  git clone -b "$REPO_BRANCH" "$REPO_URL" "$REPO_NAME"
fi

if [[ -e "$REPO_DIR" ]]; then
  chmod -R 777 "$REPO_DIR" 2>/dev/null || \
    { command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null && sudo chmod -R 777 "$REPO_DIR"; } || \
    echo "Warning: unable to set shared permissions on $REPO_DIR (need sudo)."
fi

# ---------------------------
# 2) Resolve project path
# ---------------------------
PRIMARY_PROJECT_DIR="$REMOTE_BASE_DIR/$REPO_NAME/$PROJECT_SUBDIR"
ROOT_PROJECT_DIR="$REMOTE_BASE_DIR/$REPO_NAME"

if [[ -f "$PRIMARY_PROJECT_DIR/CMakeLists.txt" ]]; then
  PROJECT_DIR="$PRIMARY_PROJECT_DIR"
elif [[ -f "$ROOT_PROJECT_DIR/CMakeLists.txt" ]]; then
  PROJECT_DIR="$ROOT_PROJECT_DIR"
else
  echo "ERROR: Project not found. Checked:"
  echo "  - $PRIMARY_PROJECT_DIR/CMakeLists.txt"
  echo "  - $ROOT_PROJECT_DIR/CMakeLists.txt"
  exit 1
fi

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

if [[ -d "build" ]]; then
  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    sudo chown -R "$DEPLOY_USER":"$DEPLOY_USER" build || true
    sudo chmod -R u+rwX,go+rwX build || true
  else
    chmod -R u+rwX,go+rwX build 2>/dev/null || true
  fi
  if [[ -d "build/_deps" || -f "build/CMakeCache.txt" || -d "build/CMakeFiles" ]]; then
    echo "Clearing stale CMake/FetchContent state..."
    rm -rf build/_deps build/CMakeCache.txt build/CMakeFiles 2>/dev/null || \
      { command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null && sudo rm -rf build/_deps build/CMakeCache.txt build/CMakeFiles; }
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

META_BIN=""
if [[ -x "build/metadata_server" ]]; then
  META_BIN="build/metadata_server"
elif [[ -x "build/Debug/metadata_server" ]]; then
  META_BIN="build/Debug/metadata_server"
fi
if [[ -z "$META_BIN" ]]; then
  echo "ERROR: metadata_server binary not found after build."
  exit 1
fi

# ---------------------------
# 4) Run metadata_server in tmux
# ---------------------------
META_HOST="${METADATA_BIND_HOST:-0.0.0.0}"
META_PORT="${METADATA_PORT:-50050}"
META_CONFIG="${METADATA_CONFIG:-config.json}"
PING_INTERVAL_MS="${METADATA_PING_INTERVAL_MS:-1000}"
PING_TIMEOUT_MS="${METADATA_PING_TIMEOUT_MS:-500}"
FAILURE_THRESHOLD="${METADATA_FAILURE_THRESHOLD:-3}"

RUN_SCOPE="${RUN_SCOPE:-shared}"
RUN_DIR="$PROJECT_DIR/run/$RUN_SCOPE"
mkdir -p "$RUN_DIR"
PID_FILE="$RUN_DIR/metadata_${META_PORT}.pid"
LOG_FILE="$RUN_DIR/metadata_${META_PORT}.log"
OUT_FILE="$RUN_DIR/metadata_${META_PORT}.out"
SESSION_NAME="${TMUX_METADATA_SESSION_NAME:-crown_metadata_${META_PORT}}"
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
echo "  config: $META_CONFIG"

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
pkill -u "$DEPLOY_USER" -f "metadata_server .*--port $META_PORT" >/dev/null 2>&1 || true

META_CMD="cd '$PROJECT_DIR' && exec '$META_BIN' --config '$META_CONFIG' --host '$META_HOST' --port '$META_PORT' --ping-interval-ms '$PING_INTERVAL_MS' --ping-timeout-ms '$PING_TIMEOUT_MS' --failure-threshold '$FAILURE_THRESHOLD' --log"
echo "Run command: $META_CMD"
printf '[launch] %s\n' "$META_CMD" | tee -a "$LOG_FILE" >> "$OUT_FILE"
"${TMUX_CMD[@]}" new-session -d -s "$SESSION_NAME" "$META_CMD"
chmod 666 "$TMUX_SOCKET" 2>/dev/null || true
"${TMUX_CMD[@]}" pipe-pane -o -t "$SESSION_NAME:0.0" "cat | tee -a '$LOG_FILE' >> '$OUT_FILE'"

NEW_PID="$("${TMUX_CMD[@]}" display-message -p -t "$SESSION_NAME:0.0" "#{pane_pid}")"
echo "$NEW_PID" > "$PID_FILE"

echo "metadata_server started in tmux session: $SESSION_NAME"
echo "metadata pane pid: $NEW_PID"
echo "log: $LOG_FILE"
echo "pid: $PID_FILE"
echo "attach: tmux -S $TMUX_SOCKET attach -t $SESSION_NAME"
