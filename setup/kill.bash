#!/usr/bin/env bash
set -euo pipefail

DEPLOY_USER="${SSH_USER:-$(whoami)}"
echo "=== Server stop (user: $DEPLOY_USER on $(hostname -f 2>/dev/null || hostname)) ==="
TMUX_SOCKET="${TMUX_SOCKET:-/tmp/crown-shared/tmux.sock}"
TMUX_CMD=(tmux -S "$TMUX_SOCKET")

# Stop shared tmux sessions
if command -v tmux >/dev/null 2>&1; then
  while IFS= read -r session_name; do
    [[ -n "$session_name" ]] || continue
    if [[ "$session_name" =~ ^crown_node_ ]]; then
      echo "Killing tmux session: $session_name"
      "${TMUX_CMD[@]}" kill-session -t "$session_name" >/dev/null 2>&1 || true
    fi
  done < <("${TMUX_CMD[@]}" list-sessions -F "#{session_name}" 2>/dev/null || true)
fi

# Stop server processes for this user
echo "Killing server processes for user $DEPLOY_USER"
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
