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

# Number of remote hosts to process at once for setup/build/start/kill actions.
# 0 or unset means "all target hosts in parallel".
VM_SETUP_PARALLELISM="${VM_SETUP_PARALLELISM:-0}"
if ! [[ "$VM_SETUP_PARALLELISM" =~ ^[0-9]+$ ]]; then
  echo "Error: VM_SETUP_PARALLELISM must be a non-negative integer, got: $VM_SETUP_PARALLELISM"
  exit 1
fi

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
  scp "${SSH_OPTS[@]}" "$local_script" "$server:$remote_script" || return 1

  echo "   -> running $remote_script"
  ssh -T "${SSH_OPTS[@]}" "$server" \
    "export SSH_USER='$SSH_USER' REPO_URL='$REPO_URL' REPO_BRANCH='$REPO_BRANCH' REMOTE_BASE_DIR='$REMOTE_BASE_DIR' REPO_NAME='$REPO_NAME' PROJECT_SUBDIR='$PROJECT_SUBDIR' PROJECT_MODE='${PROJECT_MODE:-crown}' BUILD_TYPE='${BUILD_TYPE:-Release}' NODE_HOST='${NODE_HOST:-0.0.0.0}' NODE_PORT='${NODE_PORT:-50051}' SERVER_LOG='${SERVER_LOG:-true}' TMUX_SESSION_NAME='${TMUX_SESSION_NAME:-}' TMUX_SOCKET='${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}' RUN_SCOPE='${RUN_SCOPE:-shared}' METADATA_HOST='${METADATA_HOST:-}' METADATA_PORT='${METADATA_PORT:-50050}' METADATA_CONFIG='${METADATA_CONFIG:-config.json}' BUILD_ONLY='${BUILD_ONLY:-false}' START_ONLY='${START_ONLY:-false}'; tr -d '\r' < '$remote_script' | bash -s --"
}

wait_parallel_batch() {
  local -n batch_pids_ref="$1"
  local -n batch_hosts_ref="$2"
  local -n failures_ref="$3"
  local i pid host

  for i in "${!batch_pids_ref[@]}"; do
    pid="${batch_pids_ref[$i]}"
    host="${batch_hosts_ref[$i]}"
    if wait "$pid"; then
      echo "[ok] $host completed"
    else
      echo "[failed] $host failed"
      failures_ref=$((failures_ref + 1))
    fi
  done

  batch_pids_ref=()
  batch_hosts_ref=()
}

run_hosts_parallel() {
  local label="$1" fn="$2"
  shift 2
  local hosts=("$@")
  local total="${#hosts[@]}"
  local max_parallel="$VM_SETUP_PARALLELISM"
  local failures=0
  local -a batch_pids=()
  local -a batch_hosts=()
  local host

  [[ "$total" -gt 0 ]] || return 0
  if [[ "$max_parallel" -le 0 || "$max_parallel" -gt "$total" ]]; then
    max_parallel="$total"
  fi

  echo "-- $label on $total host(s), parallelism=$max_parallel --"
  for host in "${hosts[@]}"; do
    (
      echo "---- [$host] $label started ----"
      "$fn" "$host"
      rc=$?
      if [[ "$rc" -eq 0 ]]; then
        echo "---- [$host] $label finished ----"
      else
        echo "---- [$host] $label failed rc=$rc ----"
      fi
      exit "$rc"
    ) &
    batch_pids+=("$!")
    batch_hosts+=("$host")

    if [[ "${#batch_pids[@]}" -ge "$max_parallel" ]]; then
      wait_parallel_batch batch_pids batch_hosts failures
    fi
  done

  wait_parallel_batch batch_pids batch_hosts failures
  if [[ "$failures" -ne 0 ]]; then
    echo "ERROR: $label failed on $failures/$total host(s)."
    return 1
  fi
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
    setup_on_host() { deploy_to_host "$SCRIPT_DIR/setup.bash" "$1" "$ACTION"; }
    run_hosts_parallel "$ACTION" setup_on_host "${all_hosts_meta[@]}" || exit 1
    ;;
  build|deploy)
    # clone + build only -- no node server / metadata_server started here.
    [[ ${#all_hosts_meta[@]} -gt 0 ]] || { echo "No target hosts (check prod_hosts.csv / client_hosts.csv / METADATA_HOST)."; exit 1; }
    run_hosts_parallel "build" build_on_host "${all_hosts_meta[@]}" || exit 1
    ;;
  start|start-servers)
    [[ ${#prod_hosts[@]} -gt 0 ]] || { echo "No server hosts (check prod_hosts.csv)."; exit 1; }
    run_hosts_parallel "start servers" start_server_on_host "${prod_hosts[@]}" || exit 1
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
    kill_for_rerun_on_host() { deploy_to_host "$SCRIPT_DIR/kill.bash" "$1" "rerun (kill)"; }
    run_hosts_parallel "rerun kill" kill_for_rerun_on_host "${all_hosts_meta[@]}" || exit 1
    [[ ${#prod_hosts[@]} -gt 0 ]] || { echo "No server hosts (check prod_hosts.csv)."; exit 1; }
    run_hosts_parallel "rerun start servers" start_server_on_host "${prod_hosts[@]}" || exit 1
    start_metadata_server
    ;;
  kill)
    [[ ${#all_hosts_meta[@]} -gt 0 ]] || { echo "No target hosts (check prod_hosts.csv / client_hosts.csv / METADATA_HOST)."; exit 1; }
    kill_on_host() { deploy_to_host "$SCRIPT_DIR/kill.bash" "$1" "$ACTION"; }
    run_hosts_parallel "$ACTION" kill_on_host "${all_hosts_meta[@]}" || exit 1
    ;;
  *)
    echo "Invalid action: $ACTION"
    usage
    exit 1
    ;;
esac
