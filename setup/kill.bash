#!/usr/bin/env bash
set -euo pipefail

DEPLOY_USER="${SSH_USER:-$(whoami)}"
echo "=== Server stop (user: $DEPLOY_USER on $(hostname -f 2>/dev/null || hostname)) ==="

# Stop server tmux sessions for this user
if command -v tmux >/dev/null 2>&1; then
  while IFS= read -r session_name; do
    [[ -n "$session_name" ]] || continue
    # Only kill sessions owned by this user
    if tmux has-session -t "$session_name" 2>/dev/null; then
      SESSION_USER="$(tmux display-message -p -t "$session_name" '#{session_name}' | cut -d_ -f1)"
      if [[ "$SESSION_USER" == "${DEPLOY_USER}" ]]; then
        echo "Killing tmux session: $session_name"
        tmux kill-session -t "$session_name" >/dev/null 2>&1 || true
      fi
    fi
  done < <(tmux list-sessions -F "#{session_name}" 2>/dev/null || true)
fi

# Stop server processes for this user (support both project modes)
echo "Killing server/craq_node processes for user $DEPLOY_USER"
pkill -u "$DEPLOY_USER" -f 'server|craq_node' >/dev/null 2>&1 || true

# Remove pid files in user-specific directories
echo "Cleaning up pid files for user $DEPLOY_USER"
for base_dir in "$HOME" "$HOME/research" "$(find ~ -maxdepth 2 -name crown -type d 2>/dev/null || true)"; do
  [[ -d "$base_dir" ]] || continue
  for pidfile in "$base_dir"/*/run/"$DEPLOY_USER"/*.pid "$base_dir"/run/"$DEPLOY_USER"/*.pid; do
    [[ -f "$pidfile" ]] || continue
    echo "Removing $pidfile"
    rm -f "$pidfile" || true
  done
done

echo "Done."

## Legacy (old project) behavior retained as comment:
## - kill tmux sessions cs-425-shared-mp3 and cs-425-shared-mp3-ctl
## - remove tmux socket files under /tmp
