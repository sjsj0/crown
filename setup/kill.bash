#!/usr/bin/env bash
set -euo pipefail

echo "=== CRAQ stop on $(hostname -f 2>/dev/null || hostname) ==="

# Stop CRAQ tmux sessions if running.
if command -v tmux >/dev/null 2>&1; then
  while IFS= read -r session_name; do
    [[ -n "$session_name" ]] || continue
    tmux kill-session -t "$session_name" >/dev/null 2>&1 || true
  done < <(tmux list-sessions -F "#{session_name}" 2>/dev/null | grep '^craq_node_' || true)
fi

# Stop CRAQ node if running.
pkill -f "craq_node" >/dev/null 2>&1 || true

# Remove known CRAQ pid files if present.
for pidfile in "$HOME"/craq/run/*.pid "$HOME"/research/*/crown/craq/run/*.pid; do
  [[ -f "$pidfile" ]] || continue
  rm -f "$pidfile" || true
done

echo "Done."

## Legacy (old project) behavior retained as comment:
## - kill tmux sessions cs-425-shared-mp3 and cs-425-shared-mp3-ctl
## - remove tmux socket files under /tmp
