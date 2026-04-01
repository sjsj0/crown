#!/usr/bin/env bash
set -euo pipefail

echo "=== VM setup starting (CRAQ mode) ==="

# ---------------------------
# 1) Packages for C++ CRAQ
# ---------------------------
echo "[1/3] Installing system packages..."
if command -v dnf >/dev/null 2>&1; then
  sudo dnf install -y git cmake make gcc-c++ rsync openssh-clients wget vim
elif command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update -y
  sudo apt-get install -y git cmake make g++ rsync openssh-client wget vim
else
  echo "Warning: no known package manager found; skipping installs."
fi

# ---------------------------
# 2) Clone or refresh GitHub repo
# ---------------------------
echo "[2/3] Cloning/updating repo..."
: "${REPO_URL:?REPO_URL is required}"
REPO_BRANCH="${REPO_BRANCH:-main}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-$HOME/research}"
REPO_NAME="${REPO_NAME:-$(basename "${REPO_URL%.git}")}"

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
# 3) Prime CRAQ build directory
# ---------------------------
echo "[3/3] Preparing CRAQ build folder..."
PROJECT_SUBDIR="${PROJECT_SUBDIR:-crown/craq}"
CRAQ_DIR="$REMOTE_BASE_DIR/$REPO_NAME/$PROJECT_SUBDIR"
if [[ ! -f "$CRAQ_DIR/CMakeLists.txt" ]]; then
  echo "ERROR: CRAQ project not found at $CRAQ_DIR"
  exit 1
fi
mkdir -p "$CRAQ_DIR/build"
echo "CRAQ source: $CRAQ_DIR"

echo "=== VM setup completed ==="

## Legacy (old project) behavior retained as comment:
## - install tmux + go dependencies
## - setup gitlab host key + private key agent
## - clone old hydfs-g33 repo in /home/mp3
