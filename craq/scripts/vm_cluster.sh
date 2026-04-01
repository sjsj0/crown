#!/usr/bin/env bash
set -euo pipefail

# One-script orchestration for multi-VM CRAQ clusters.
#
# Supports:
#   up        - sync source, build remotely, start nodes, configure cluster
#   configure - reconfigure cluster only
#   status    - run leader dump against all mapped nodes
#   down      - stop nodes on all mapped VMs
#
# Mapping file format (CSV):
#   node_id,ssh_user,ssh_host,ssh_port,node_host,node_port

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

MAP_FILE="$ROOT_DIR/configs/vm_hosts.sample.csv"
REMOTE_DIR="/home/sagarj2/crown/craq"
BIND_HOST="0.0.0.0"
REMOTE_BUILD_TYPE="Release"
SSH_EXTRA_OPTS=""

usage() {
  cat <<EOF
Usage:
  $0 <up|down|configure|status> [options]

Options:
  --map <path>           VM mapping CSV file
  --remote-dir <path>    Remote CRAQ directory (default: /home/sagarj2/crown/craq)
  --bind-host <host>     Host craq_node binds to on VM (default: 0.0.0.0)
  --build-type <type>    CMake build type on VM (default: Release)
  --ssh-opt <opt>        Extra SSH option (can be repeated)

Examples:
  $0 up --map "$ROOT_DIR/configs/vm_hosts.sample.csv"
  $0 status --map "$ROOT_DIR/configs/vm_hosts.sample.csv"
  $0 down --map "$ROOT_DIR/configs/vm_hosts.sample.csv"
EOF
}

log() {
  printf '[vm_cluster] %s\n\n' "$*"
}

declare -a NODE_IDS=()
declare -a SSH_USERS=()
declare -a SSH_HOSTS=()
declare -a SSH_PORTS=()
declare -a NODE_HOSTS=()
declare -a NODE_PORTS=()

trim() {
  local s="$1"
  s="${s#${s%%[![:space:]]*}}"
  s="${s%${s##*[![:space:]]}}"
  printf '%s' "$s"
}

load_map() {
  if [[ ! -f "$MAP_FILE" ]]; then
    echo "mapping file not found: $MAP_FILE" >&2
    exit 1
  fi

  NODE_IDS=()
  SSH_USERS=()
  SSH_HOSTS=()
  SSH_PORTS=()
  NODE_HOSTS=()
  NODE_PORTS=()

  while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
    local line
    line="$(trim "$raw_line")"
    [[ -z "$line" ]] && continue
    [[ "$line" =~ ^# ]] && continue

    IFS=',' read -r node_id ssh_user ssh_host ssh_port node_host node_port <<<"$line"
    node_id="$(trim "${node_id:-}")"
    ssh_user="$(trim "${ssh_user:-}")"
    ssh_host="$(trim "${ssh_host:-}")"
    ssh_port="$(trim "${ssh_port:-}")"
    node_host="$(trim "${node_host:-}")"
    node_port="$(trim "${node_port:-}")"

    if [[ -z "$node_id" || -z "$ssh_user" || -z "$ssh_host" || -z "$ssh_port" || -z "$node_host" || -z "$node_port" ]]; then
      echo "invalid map row: $line" >&2
      exit 1
    fi

    NODE_IDS+=("$node_id")
    SSH_USERS+=("$ssh_user")
    SSH_HOSTS+=("$ssh_host")
    SSH_PORTS+=("$ssh_port")
    NODE_HOSTS+=("$node_host")
    NODE_PORTS+=("$node_port")
  done < "$MAP_FILE"

  if (( ${#NODE_IDS[@]} < 3 )); then
    echo "mapping must contain at least 3 nodes for VM cluster orchestration" >&2
    exit 1
  fi
}

ssh_target() {
  local i="$1"
  printf '%s@%s' "${SSH_USERS[$i]}" "${SSH_HOSTS[$i]}"
}

ssh_cmd() {
  local i="$1"
  shift
  local -a opts=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -p "${SSH_PORTS[$i]}")
  if [[ -n "$SSH_EXTRA_OPTS" ]]; then
    # shellcheck disable=SC2206
    local extra=( $SSH_EXTRA_OPTS )
    opts+=("${extra[@]}")
  fi
  ssh "${opts[@]}" "$(ssh_target "$i")" "$@"
}

scp_to() {
  local i="$1"
  local src="$2"
  local dst="$3"
  local -a opts=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -P "${SSH_PORTS[$i]}")
  if [[ -n "$SSH_EXTRA_OPTS" ]]; then
    # shellcheck disable=SC2206
    local extra=( $SSH_EXTRA_OPTS )
    opts+=("${extra[@]}")
  fi
  scp "${opts[@]}" "$src" "$(ssh_target "$i"):$dst"
}

generate_runtime_config() {
  local out_file="$1"
  {
    echo "mode CRAQ"
    local i
    for ((i=0; i<${#NODE_IDS[@]}; ++i)); do
      echo "node ${NODE_IDS[$i]} ${NODE_HOSTS[$i]} ${NODE_PORTS[$i]}"
    done
  } > "$out_file"
}

sync_source_and_build_remote() {
  local i="$1"
  log "sync+build | node=${NODE_IDS[$i]} | ssh=${SSH_USERS[$i]}@${SSH_HOSTS[$i]}:${SSH_PORTS[$i]} | node_endpoint=${NODE_HOSTS[$i]}:${NODE_PORTS[$i]}"

  ssh_cmd "$i" "mkdir -p '$REMOTE_DIR'"

  tar -C "$ROOT_DIR" -czf - CMakeLists.txt include src configs | \
    ssh_cmd "$i" "tar -xzf - -C '$REMOTE_DIR'"

  ssh_cmd "$i" "cmake -S '$REMOTE_DIR' -B '$REMOTE_DIR/build' -DCMAKE_BUILD_TYPE='$REMOTE_BUILD_TYPE'"
  ssh_cmd "$i" "cmake --build '$REMOTE_DIR/build' -j"
}

start_remote_node() {
  local i="$1"
  local node_id="${NODE_IDS[$i]}"
  local node_port="${NODE_PORTS[$i]}"

  log "start node | node=$node_id | route=controller -> ${SSH_USERS[$i]}@${SSH_HOSTS[$i]}:${SSH_PORTS[$i]} | bind=${BIND_HOST}:$node_port"

  ssh_cmd "$i" "mkdir -p '$REMOTE_DIR/run' '$REMOTE_DIR/logs'"
  ssh_cmd "$i" "if [[ -f '$REMOTE_DIR/run/${node_id}.pid' ]]; then kill \"\$(cat '$REMOTE_DIR/run/${node_id}.pid')\" >/dev/null 2>&1 || true; rm -f '$REMOTE_DIR/run/${node_id}.pid'; fi"

  ssh_cmd "$i" "nohup '$REMOTE_DIR/build/craq_node' --host '$BIND_HOST' --port '$node_port' > '$REMOTE_DIR/logs/${node_id}.log' 2>&1 & echo \$! > '$REMOTE_DIR/run/${node_id}.pid'"
}

configure_cluster() {
  local controller=0
  local tmp_cfg
  tmp_cfg="$(mktemp)"
  generate_runtime_config "$tmp_cfg"

  log "configure cluster | controller=${NODE_IDS[$controller]} (${SSH_HOSTS[$controller]}) | map=$MAP_FILE"

  scp_to "$controller" "$tmp_cfg" "$REMOTE_DIR/configs/cluster.runtime.conf"
  rm -f "$tmp_cfg"

  ssh_cmd "$controller" "'$REMOTE_DIR/build/craq_leader' configure --config '$REMOTE_DIR/configs/cluster.runtime.conf'"
}

status_cluster() {
  local controller=0
  local i
  log "cluster status | controller=${NODE_IDS[$controller]} (${SSH_HOSTS[$controller]})"

  for ((i=0; i<${#NODE_IDS[@]}; ++i)); do
    local endpoint="${NODE_HOSTS[$i]}:${NODE_PORTS[$i]}"
    echo "----- ${NODE_IDS[$i]} ($endpoint) -----"
    echo
    ssh_cmd "$controller" "'$REMOTE_DIR/build/craq_leader' dump --node '$endpoint'" || true
    echo
  done
}

stop_remote_node() {
  local i="$1"
  local node_id="${NODE_IDS[$i]}"
  local node_port="${NODE_PORTS[$i]}"

  log "stop node | node=$node_id | route=controller -> ${SSH_USERS[$i]}@${SSH_HOSTS[$i]}:${SSH_PORTS[$i]} | bind=${BIND_HOST}:$node_port"

  ssh_cmd "$i" "if [[ -f '$REMOTE_DIR/run/${node_id}.pid' ]]; then kill \"\$(cat '$REMOTE_DIR/run/${node_id}.pid')\" >/dev/null 2>&1 || true; rm -f '$REMOTE_DIR/run/${node_id}.pid'; fi"
  ssh_cmd "$i" "pkill -f 'craq_node --host $BIND_HOST --port $node_port' >/dev/null 2>&1 || true"
}

main() {
  if [[ $# -lt 1 ]]; then
    usage
    exit 1
  fi

  local cmd="$1"
  shift

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --map)
        MAP_FILE="$2"
        shift 2
        ;;
      --remote-dir)
        REMOTE_DIR="$2"
        shift 2
        ;;
      --bind-host)
        BIND_HOST="$2"
        shift 2
        ;;
      --build-type)
        REMOTE_BUILD_TYPE="$2"
        shift 2
        ;;
      --ssh-opt)
        if [[ -n "$SSH_EXTRA_OPTS" ]]; then
          SSH_EXTRA_OPTS+=" "
        fi
        SSH_EXTRA_OPTS+="$2"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        echo "unknown option: $1" >&2
        usage
        exit 1
        ;;
    esac
  done

  load_map

  local i
  case "$cmd" in
    up)
      for ((i=0; i<${#NODE_IDS[@]}; ++i)); do
        sync_source_and_build_remote "$i"
      done
      for ((i=0; i<${#NODE_IDS[@]}; ++i)); do
        start_remote_node "$i"
      done
      sleep 1
      configure_cluster
      status_cluster
      ;;
    configure)
      configure_cluster
      ;;
    status)
      status_cluster
      ;;
    down)
      for ((i=0; i<${#NODE_IDS[@]}; ++i)); do
        stop_remote_node "$i"
      done
      ;;
    *)
      echo "unknown command: $cmd" >&2
      usage
      exit 1
      ;;
  esac
}

main "$@"
