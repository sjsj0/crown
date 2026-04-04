#!/usr/bin/env bash
set -euo pipefail

DEPLOY_USER="${SSH_USER:-$(whoami)}"
echo "=== VM dependency setup (user: $DEPLOY_USER) ==="

run_as_root() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
    return
  fi
  if command -v sudo >/dev/null 2>&1; then
    sudo "$@"
    return
  fi
  return 1
}

ensure_passwordless_sudo() {
  if [[ "$(id -u)" -eq 0 ]]; then
    return
  fi
  if ! command -v sudo >/dev/null 2>&1; then
    echo "ERROR: sudo is not installed on this VM. Run this as root or install sudo first."
    exit 1
  fi
  if ! sudo -n true 2>/dev/null; then
    echo "ERROR: sudo requires a password on this VM."
    echo "       Configure passwordless sudo for $DEPLOY_USER, or run this script as root."
    exit 1
  fi
}

echo "Installing system packages..."

# Prefer apt-get on Debian/Ubuntu; fallback to dnf on Fedora/RHEL.
if command -v apt-get >/dev/null 2>&1; then
  ensure_passwordless_sudo
  if ! run_as_root apt-get update -y; then
    echo "ERROR: cannot run apt-get update as root."
    exit 1
  fi
  if ! run_as_root apt-get install -y git cmake make g++ rsync openssh-client wget vim tmux libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc; then
    echo "ERROR: cannot run apt-get install as root."
    exit 1
  fi
elif command -v dnf >/dev/null 2>&1; then
  ensure_passwordless_sudo
  if ! run_as_root dnf install -y git cmake make gcc-c++ rsync openssh-clients wget vim tmux grpc-devel protobuf-devel protobuf-compiler; then
    echo "ERROR: cannot run dnf install as root."
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
