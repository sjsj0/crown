#!/usr/bin/env bash

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

# Metadata store placement (single source of truth for topology + liveness).
METADATA_HOST="${METADATA_HOST:-}"
METADATA_PORT="${METADATA_PORT:-50050}"
METADATA_CONFIG="${METADATA_CONFIG:-config.json}"

# Optional: local key used just to reach the VMs. If you've already run
# `ssh-copy-id` or have an agent, you can omit this in .env.
SSH_OPTS=(-o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new)
if [[ -n "${SSH_KEY_LOCAL:-}" && -f "${SSH_KEY_LOCAL:-/dev/null}" ]]; then
  SSH_OPTS=(-i "$SSH_KEY_LOCAL" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new)
fi

# --- host inventory ---------------------------------------------------------
# prod_hosts.csv  : VMs that run server nodes (one of them often also runs metadata).
# client_hosts.csv: VMs that run only the client binary (benchmark drivers).
# $METADATA_HOST  : VM that runs the metadata_server (may be one of the above,
#                   or a host listed in neither file).
# Scopes:
#   setup / build / kill : prod_hosts ∪ client_hosts ∪ $METADATA_HOST
#                          (deps / clone+build / teardown -- no processes started by `build`)
#   start / rerun        : node servers on prod_hosts; metadata_server on $METADATA_HOST
#                          (no client_hosts -- they only run the client binary)
read_hosts_csv() {
  local file="$1"
  [[ -f "$file" ]] || return 0
  # Strip comments (#...), CR, leading field before first comma, surrounding
  # whitespace; drop blank lines.
  sed -e 's/#.*$//' -e 's/\r//' "$file" \
    | awk -F',' '{ gsub(/^[ \t]+|[ \t]+$/, "", $1); if ($1 != "") print $1 }'
}

mapfile -t prod_hosts     < <(read_hosts_csv "$SCRIPT_DIR/prod_hosts.csv")
mapfile -t client_hosts   < <(read_hosts_csv "$SCRIPT_DIR/client_hosts.csv")
mapfile -t all_hosts      < <(printf '%s\n' "${prod_hosts[@]}" "${client_hosts[@]}" | awk 'NF' | sort -u)
# prod ∪ client ∪ metadata host -- the full set of VMs that need build deps /
# get torn down (the metadata host may not appear in either CSV).
mapfile -t all_hosts_meta < <(printf '%s\n' "${all_hosts[@]}" "${METADATA_HOST:-}" | awk 'NF' | sort -u)

usage() {
  echo "Usage: $0 <setup|build|deploy|start|start-servers|start-metadata|rerun|kill>"
  echo "  setup          : install build deps on prod_hosts.csv ∪ client_hosts.csv ∪ \$METADATA_HOST"
  echo "  build|deploy   : clone + build the repo on prod_hosts.csv ∪ client_hosts.csv ∪ \$METADATA_HOST (no processes started)"
  echo "  start          : (re)start a server node on each prod_hosts.csv VM, then the metadata_server on \$METADATA_HOST"
  echo "  start-servers  : (re)start a server node on each prod_hosts.csv VM only (skip the metadata_server)"
  echo "  start-metadata : (re)start the metadata_server on \$METADATA_HOST only"
  echo "  rerun          : kill, then start again (node servers on prod_hosts.csv + metadata_server on \$METADATA_HOST)"
  echo "  kill           : stop everything on prod_hosts.csv ∪ client_hosts.csv ∪ \$METADATA_HOST"
}

# --- remote deploy helper ---------------------------------------------------
# Copies a local script to a host and runs it there with the deployment env.
# Usage: deploy_to_host <local_script_path> <host> <label>
# The BUILD_ONLY / START_ONLY shell vars (if set by the caller) are forwarded to
# the remote script:  BUILD_ONLY=true deploy_to_host ...   /   START_ONLY=true deploy_to_host ...
deploy_to_host() {
  local local_script="$1" host="$2" label="$3"
  [[ -f "$local_script" ]] || { echo "Error: $local_script not found"; exit 1; }
  local script_name remote_script server
  script_name="$(basename "$local_script")"
  remote_script="/home/${SSH_USER}/${script_name}"
  server="${SSH_USER}@${host}"

  echo "==> $server  ($label)"

  echo "   -> copying $script_name"
  scp "${SSH_OPTS[@]}" "$local_script" "$server:$remote_script"

  echo "   -> running $remote_script"
  ssh -t "${SSH_OPTS[@]}" "$server" \
    "export SSH_USER='$SSH_USER' REPO_URL='$REPO_URL' REPO_BRANCH='$REPO_BRANCH' REMOTE_BASE_DIR='$REMOTE_BASE_DIR' REPO_NAME='$REPO_NAME' PROJECT_SUBDIR='$PROJECT_SUBDIR' PROJECT_MODE='${PROJECT_MODE:-crown}' BUILD_TYPE='${BUILD_TYPE:-Release}' NODE_HOST='${NODE_HOST:-0.0.0.0}' NODE_PORT='${NODE_PORT:-50051}' SERVER_LOG='${SERVER_LOG:-true}' TMUX_SESSION_NAME='${TMUX_SESSION_NAME:-}' TMUX_SOCKET='${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}' RUN_SCOPE='${RUN_SCOPE:-shared}' METADATA_HOST='${METADATA_HOST:-}' METADATA_PORT='${METADATA_PORT:-50050}' METADATA_CONFIG='${METADATA_CONFIG:-config.json}' BUILD_ONLY='${BUILD_ONLY:-false}' START_ONLY='${START_ONLY:-false}'; tr -d '\r' < '$remote_script' | bash -s --"
}

# Clone + build the repo on a host without starting anything.
build_on_host() { BUILD_ONLY=true deploy_to_host "$SCRIPT_DIR/start_server.bash" "$1" "build (clone + build)"; }

# (Re)start the node server on a host using the already-built binary.
start_server_on_host() { START_ONLY=true deploy_to_host "$SCRIPT_DIR/start_server.bash" "$1" "start (server node)"; }

# (Re)start the metadata_server on $METADATA_HOST using the already-built binary.
# (For a fresh VM, run `build` first; the metadata host is included in `build`.)
start_metadata_server() {
  if [[ -z "${METADATA_HOST:-}" ]]; then
    echo "WARNING: METADATA_HOST is not set in .env -- skipping metadata_server."
    echo "         Set METADATA_HOST in $ENV_FILE, then run: $0 start-metadata"
    return 0
  fi
  echo "-- (re)starting metadata_server on METADATA_HOST=$METADATA_HOST (config: $METADATA_CONFIG) --"
  START_ONLY=true deploy_to_host "$SCRIPT_DIR/start_metadata.bash" "$METADATA_HOST" "start-metadata"
}

# --- action -----------------------------------------------------------------
if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

ACTION="$1"
case "$ACTION" in
  setup)
    [[ ${#all_hosts_meta[@]} -gt 0 ]] || { echo "No target hosts (check prod_hosts.csv / client_hosts.csv / METADATA_HOST)."; exit 1; }
    for host in "${all_hosts_meta[@]}"; do deploy_to_host "$SCRIPT_DIR/setup.bash" "$host" "$ACTION"; done
    ;;
  build|deploy)
    # clone + build only -- no node server / metadata_server started here.
    [[ ${#all_hosts_meta[@]} -gt 0 ]] || { echo "No target hosts (check prod_hosts.csv / client_hosts.csv / METADATA_HOST)."; exit 1; }
    for host in "${all_hosts_meta[@]}"; do build_on_host "$host"; done
    ;;
  start|start-servers)
    [[ ${#prod_hosts[@]} -gt 0 ]] || { echo "No server hosts (check prod_hosts.csv)."; exit 1; }
    for host in "${prod_hosts[@]}"; do start_server_on_host "$host"; done
    # `start` also (re)starts the metadata_server so the cluster is fully
    # configured in one shot; `start-servers` stops after the node servers.
    if [[ "$ACTION" == "start" ]]; then
      start_metadata_server
    fi
    ;;
  start-metadata)
    : "${METADATA_HOST:?Set METADATA_HOST in .env}"
    start_metadata_server
    ;;
  rerun)
    # "kill, then start again" -- stop everything, then bring the servers + metadata back up.
    [[ ${#all_hosts_meta[@]} -gt 0 ]] || { echo "No target hosts (check prod_hosts.csv / client_hosts.csv / METADATA_HOST)."; exit 1; }
    for host in "${all_hosts_meta[@]}"; do deploy_to_host "$SCRIPT_DIR/kill.bash" "$host" "rerun (kill)"; done
    [[ ${#prod_hosts[@]} -gt 0 ]] || { echo "No server hosts (check prod_hosts.csv)."; exit 1; }
    for host in "${prod_hosts[@]}"; do start_server_on_host "$host"; done
    start_metadata_server
    ;;
  kill)
    [[ ${#all_hosts_meta[@]} -gt 0 ]] || { echo "No target hosts (check prod_hosts.csv / client_hosts.csv / METADATA_HOST)."; exit 1; }
    for host in "${all_hosts_meta[@]}"; do deploy_to_host "$SCRIPT_DIR/kill.bash" "$host" "$ACTION"; done
    ;;
  *)
    echo "Invalid action: $ACTION"
    usage
    exit 1
    ;;
esac
