#!/usr/bin/env bash
set -euo pipefail

echo "=== VM dependency setup (CRAQ mode) ==="

run_as_root() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
    return
  fi
  if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    sudo -n "$@"
    return
  fi
  return 1
}

echo "Installing system packages..."

if command -v dnf >/dev/null 2>&1; then
  if ! run_as_root dnf install -y git cmake make gcc-c++ rsync openssh-clients wget vim; then
    echo "ERROR: cannot run dnf install as root."
    exit 1
  fi
elif command -v apt-get >/dev/null 2>&1; then
  if ! run_as_root apt-get update -y; then
    echo "ERROR: cannot run apt-get update as root."
    exit 1
  fi
  if ! run_as_root apt-get install -y git cmake make g++ rsync openssh-client wget vim; then
    echo "ERROR: cannot run apt-get install as root."
    exit 1
  fi
else
  echo "ERROR: no known package manager found."
  exit 1
fi

echo "=== VM dependency setup completed ==="

## Legacy (old project) behavior retained as comment:
## - install tmux + go dependencies
## - setup gitlab host key + private key agent
## - clone old hydfs-g33 repo in /home/mp3
