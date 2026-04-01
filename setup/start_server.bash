#!/usr/bin/env bash
set -euo pipefail

echo "=== CRAQ deploy starting ==="

# ---------------------------
# 1) Clone or refresh repo
# ---------------------------
REPO_URL="${REPO_URL:?REPO_URL is required}"
REPO_BRANCH="${REPO_BRANCH:-main}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-$HOME}"
REPO_NAME="${REPO_NAME:-$(basename "${REPO_URL%.git}")}"
PROJECT_SUBDIR="${PROJECT_SUBDIR:-crown/craq}"

mkdir -p "$REMOTE_BASE_DIR"
cd "$REMOTE_BASE_DIR"

if [[ -d "$REPO_NAME/.git" ]]; then
  echo "Repo exists; pulling latest branch: $REPO_BRANCH"
  git -C "$REPO_NAME" fetch --all --prune
  git -C "$REPO_NAME" checkout -f "$REPO_BRANCH"
  git -C "$REPO_NAME" pull --ff-only origin "$REPO_BRANCH"
else
  echo "Cloning fresh: $REPO_URL"
  git clone -b "$REPO_BRANCH" "$REPO_URL" "$REPO_NAME"
fi

# ---------------------------
# 2) Resolve CRAQ project path
# ---------------------------
CRAQ_DIR="$REMOTE_BASE_DIR/$REPO_NAME/$PROJECT_SUBDIR"

if [[ ! -f "$CRAQ_DIR/CMakeLists.txt" ]]; then
  echo "ERROR: CRAQ project not found at $CRAQ_DIR"
  exit 1
fi

echo "CRAQ source: $CRAQ_DIR"
cd "$CRAQ_DIR"

# ---------------------------
# 2.5) Preflight tool checks
# ---------------------------
if ! command -v git >/dev/null 2>&1; then
  echo "ERROR: git is not installed on this VM. Run: ./vm_setup.bash setup"
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake is not installed on this VM. Run: ./vm_setup.bash setup"
  exit 1
fi

if ! command -v g++ >/dev/null 2>&1; then
  echo "ERROR: g++ is not installed on this VM. Run: ./vm_setup.bash setup"
  exit 1
fi

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
if [[ -x "build/craq_node" ]]; then
  NODE_BIN="build/craq_node"
elif [[ -x "build/Debug/craq_node" ]]; then
  NODE_BIN="build/Debug/craq_node"
fi

if [[ -z "$NODE_BIN" ]]; then
  echo "ERROR: craq_node binary not found after build."
  exit 1
fi

# ---------------------------
# 4) Run node
# ---------------------------
NODE_HOST="${NODE_HOST:-0.0.0.0}"
NODE_PORT="${NODE_PORT:-5001}"
RUN_DIR="$CRAQ_DIR/run"
mkdir -p "$RUN_DIR"
PID_FILE="$RUN_DIR/craq_node.pid"
LOG_FILE="$RUN_DIR/craq_node.log"

if [[ -f "$PID_FILE" ]]; then
  OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
  if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
    echo "Stopping existing craq_node pid=$OLD_PID"
    kill "$OLD_PID" || true
    sleep 1
  fi
fi

pkill -f "craq_node --host .* --port $NODE_PORT" >/dev/null 2>&1 || true

echo "Starting $NODE_BIN --host $NODE_HOST --port $NODE_PORT"
nohup "$NODE_BIN" --host "$NODE_HOST" --port "$NODE_PORT" >"$LOG_FILE" 2>&1 &
NEW_PID=$!
echo "$NEW_PID" > "$PID_FILE"

echo "CRAQ node started: pid=$NEW_PID"
echo "log: $LOG_FILE"
echo "pid: $PID_FILE"

## Legacy (old project) behavior retained as comment:
## - clone old hydfs-g33 repo
## - create tmux sessions
## - run go daemons in src/main and src/ctl
