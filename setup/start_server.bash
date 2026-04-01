#!/usr/bin/env bash
set -euo pipefail

echo "=== CRAQ build starting ==="

# ---------------------------
# 1) Resolve project locations
# ---------------------------
PROJECT_SUBDIR="${PROJECT_SUBDIR:-crown/craq}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-$HOME/research}"
REPO_NAME="${REPO_NAME:?REPO_NAME is required}"
CRAQ_DIR="$REMOTE_BASE_DIR/$REPO_NAME/$PROJECT_SUBDIR"

if [[ ! -f "$CRAQ_DIR/CMakeLists.txt" ]]; then
  echo "ERROR: CRAQ project not found at $CRAQ_DIR"
  exit 1
fi

echo "CRAQ source: $CRAQ_DIR"
cd "$CRAQ_DIR"

# ---------------------------
# 2) Configure + build
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

echo "Build done."

if [[ -x "build/craq_node" && -x "build/craq_leader" ]]; then
  echo "Binaries: build/craq_node, build/craq_leader"
elif [[ -x "build/Debug/craq_node" && -x "build/Debug/craq_leader" ]]; then
  echo "Binaries: build/Debug/craq_node, build/Debug/craq_leader"
else
  echo "Note: binaries built, but exact output path depends on CMake generator."
fi

echo "Use crown/craq/scripts/vm_cluster.sh for cluster start/configure/status/down."

## Legacy (old project) behavior retained as comment:
## - clone old hydfs-g33 repo
## - create tmux sessions
## - run go daemons in src/main and src/ctl
