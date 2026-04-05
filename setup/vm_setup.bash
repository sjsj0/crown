#!/usr/bin/env bash
set -euo pipefail

# --- load .env from this script's directory ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env"
[[ -f "$ENV_FILE" ]] || { echo "Error: .env not found at $ENV_FILE"; exit 1; }

# Load .env
# shellcheck source=/dev/null
set -a
source "$ENV_FILE"
set +a

# SSH user must come from .env
SSH_USER="${SSH_USER:?Missing SSH_USER in .env}"
echo "Using SSH_USER: $SSH_USER"

# Repo settings for remote setup/build. These are intentionally configurable from .env.
: "${REPO_URL:?Missing REPO_URL in .env}"
REPO_BRANCH="${REPO_BRANCH:-main}"
REMOTE_BASE_DIR="${REMOTE_BASE_DIR:-/home}"
REPO_NAME="${REPO_NAME:-$(basename "${REPO_URL%.git}")}"
PROJECT_SUBDIR="${PROJECT_SUBDIR:-.}"

# Optional: local key used just to reach the VMs. If you've already run
# `ssh-copy-id` or have an agent, you can omit this in .env.
SSH_OPTS=(-o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new)
if [[ -n "${SSH_KEY_LOCAL:-}" && -f "${SSH_KEY_LOCAL:-/dev/null}" ]]; then
  SSH_OPTS=(-i "$SSH_KEY_LOCAL" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new)
fi

# Which local script to send and run remotely
if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <setup|start|build|deploy|kill>"
  echo "  Example: $0 setup"
  exit 1
fi

ACTION="$1"
case "$ACTION" in
  setup) LOCAL_SCRIPT="$SCRIPT_DIR/setup.bash" ;;
  start|build|deploy) LOCAL_SCRIPT="$SCRIPT_DIR/start_server.bash" ;;
  kill)  LOCAL_SCRIPT="$SCRIPT_DIR/kill.bash" ;;
  *) echo "Invalid action: $ACTION"; echo "Usage: $0 <setup|start|build|deploy|kill>"; exit 1 ;;
esac
[[ -f "$LOCAL_SCRIPT" ]] || { echo "Error: $LOCAL_SCRIPT not found"; exit 1; }
REMOTE_SCRIPT="/home/${SSH_USER}/$(basename "$LOCAL_SCRIPT")"
LOCAL_SCRIPT_NAME="$(basename "$LOCAL_SCRIPT")"

# Hosts (without usernames)
hosts=(
 "sp26-cs525-1201.cs.illinois.edu"
 "sp26-cs525-1202.cs.illinois.edu"
 "sp26-cs525-1203.cs.illinois.edu"
#  "sp26-cs525-1204.cs.illinois.edu"
#  "sp26-cs525-1205.cs.illinois.edu"
#  "sp26-cs525-1206.cs.illinois.edu"
#  "sp26-cs525-1207.cs.illinois.edu"
#  "sp26-cs525-1208.cs.illinois.edu"
#  "sp26-cs525-1209.cs.illinois.edu"
#  "sp26-cs525-1210.cs.illinois.edu"
#  "sp26-cs525-1211.cs.illinois.edu"
 "sp26-cs525-1212.cs.illinois.edu"
#  "sp26-cs525-1213.cs.illinois.edu"
#  "sp26-cs525-1214.cs.illinois.edu"
#  "sp26-cs525-1215.cs.illinois.edu"
#  "sp26-cs525-1216.cs.illinois.edu"
#  "sp26-cs525-1217.cs.illinois.edu"
#  "sp26-cs525-1218.cs.illinois.edu"
#  "sp26-cs525-1219.cs.illinois.edu"
#  "sp26-cs525-1220.cs.illinois.edu"
)

# --- loop over servers ---
for host in "${hosts[@]}"; do
  server="${SSH_USER}@${host}"
  echo "==> $server"

  # Copy and run the requested script
  echo "   -> copying $LOCAL_SCRIPT_NAME"
  scp "${SSH_OPTS[@]}" "$LOCAL_SCRIPT" "$server:$REMOTE_SCRIPT"

  echo "   -> running $REMOTE_SCRIPT"
  ssh -t "${SSH_OPTS[@]}" "$server" \
    "SSH_USER='$SSH_USER' REPO_URL='$REPO_URL' REPO_BRANCH='$REPO_BRANCH' REMOTE_BASE_DIR='$REMOTE_BASE_DIR' REPO_NAME='$REPO_NAME' PROJECT_SUBDIR='$PROJECT_SUBDIR' PROJECT_MODE='${PROJECT_MODE:-crown}' BUILD_TYPE='${BUILD_TYPE:-Release}' NODE_HOST='${NODE_HOST:-0.0.0.0}' NODE_PORT='${NODE_PORT:-5001}' SERVER_LOG='${SERVER_LOG:-false}' TMUX_SESSION_NAME='${TMUX_SESSION_NAME:-}' TMUX_SOCKET='${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}' RUN_SCOPE='${RUN_SCOPE:-shared}' bash '$REMOTE_SCRIPT'"
done

