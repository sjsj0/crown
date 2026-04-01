#!/usr/bin/env bash
set -euo pipefail

echo "=== CRAQ stop on $(hostname -f 2>/dev/null || hostname) ==="

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
