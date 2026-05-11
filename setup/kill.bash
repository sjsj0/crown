#!/usr/bin/env bash
set -euo pipefail

DEPLOY_USER="${SSH_USER:-$(whoami)}"
echo "=== Server stop (user: $DEPLOY_USER on $(hostname -f 2>/dev/null || hostname)) ==="
TMUX_SOCKET="${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}"
TMUX_CMD=(tmux -S "$TMUX_SOCKET")

wait_for_pid_exit() {
  local pid="$1" label="$2"
  local i
  for i in {1..30}; do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "WARNING: $label pid=$pid is still running after waiting."
  return 1
}

remove_stale_tmux_socket_if_safe() {
  [[ -S "$TMUX_SOCKET" ]] || return 0

  # If tmux can answer on the socket, it is live and should not be removed here.
  if "${TMUX_CMD[@]}" list-sessions >/dev/null 2>&1; then
    return 0
  fi

  echo "Removing stale tmux socket: $TMUX_SOCKET"
  if rm -f "$TMUX_SOCKET" 2>/dev/null; then
    return 0
  fi

  echo "Unable to remove stale tmux socket as $DEPLOY_USER; checking ownership:"
  ls -ld "$(dirname "$TMUX_SOCKET")" "$TMUX_SOCKET" 2>/dev/null || true

  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    echo "Removing stale tmux socket with sudo: $TMUX_SOCKET"
    sudo rm -f "$TMUX_SOCKET"
    return 0
  fi

  echo "ERROR: unable to remove stale tmux socket: $TMUX_SOCKET"
  echo "       The socket is stale/unresponsive but not removable by $DEPLOY_USER."
  echo "       Remove it manually on this host or grant passwordless sudo for cleanup."
  return 1
}

# Stop shared tmux sessions (server nodes, the metadata store, and the legacy
# single 'crown' session name used by start_server.bash).
if command -v tmux >/dev/null 2>&1; then
  tmux_server_pid="$("${TMUX_CMD[@]}" display-message -p '#{pid}' 2>/dev/null || true)"
  if [[ -n "$tmux_server_pid" ]]; then
    echo "Found tmux server for socket $TMUX_SOCKET: pid=$tmux_server_pid"
  elif [[ -S "$TMUX_SOCKET" ]]; then
    echo "WARNING: tmux socket exists but the server did not respond: $TMUX_SOCKET"
  fi

  while IFS= read -r session_name; do
    [[ -n "$session_name" ]] || continue
    if [[ "$session_name" =~ ^crown_node_ || "$session_name" =~ ^crown_metadata_ || "$session_name" == "crown" ]]; then
      echo "Killing tmux session: $session_name"
      "${TMUX_CMD[@]}" kill-session -t "$session_name" >/dev/null 2>&1 || true
    fi
  done < <("${TMUX_CMD[@]}" list-sessions -F "#{session_name}" 2>/dev/null || true)

  # Once the crown sessions are gone, stop the tmux server for this socket too.
  # This avoids leaving a live or stale /tmp/crown-shared/tmux.sock behind for
  # the next experiment iteration.
  if [[ -n "${tmux_server_pid:-}" ]]; then
    echo "Stopping tmux server for socket: $TMUX_SOCKET"
    "${TMUX_CMD[@]}" kill-server >/dev/null 2>&1 || true
    if ! wait_for_pid_exit "$tmux_server_pid" "tmux server"; then
      echo "Killing lingering tmux server pid=$tmux_server_pid"
      kill "$tmux_server_pid" 2>/dev/null || true
      if ! wait_for_pid_exit "$tmux_server_pid" "tmux server"; then
        echo "Force-killing lingering tmux server pid=$tmux_server_pid"
        kill -9 "$tmux_server_pid" 2>/dev/null || true
        wait_for_pid_exit "$tmux_server_pid" "tmux server" || {
          echo "ERROR: tmux server pid=$tmux_server_pid is still running."
          exit 1
        }
      fi
    fi
  fi

  remove_stale_tmux_socket_if_safe
fi

# Stop server + metadata_server processes for this user.
echo "Killing server / metadata_server processes for user $DEPLOY_USER"
pkill -u "$DEPLOY_USER" -f 'metadata_server' >/dev/null 2>&1 || true
pkill -u "$DEPLOY_USER" -f 'server' >/dev/null 2>&1 || true

# Remove pid files in shared run directories
echo "Cleaning up shared pid files"
for base_dir in "$HOME" "$HOME/research" "$(find ~ -maxdepth 2 -name crown -type d 2>/dev/null || true)"; do
  [[ -d "$base_dir" ]] || continue
  for pidfile in "$base_dir"/*/run/shared/*.pid "$base_dir"/run/shared/*.pid; do
    [[ -f "$pidfile" ]] || continue
    echo "Removing $pidfile"
    rm -f "$pidfile" || true
  done
done

echo "Done."

## Legacy (old project) behavior retained as comment:
## - kill tmux sessions cs-425-shared-mp3 and cs-425-shared-mp3-ctl
## - remove tmux socket files under /tmp
