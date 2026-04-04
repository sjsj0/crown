# Crown Deployment Scripts Guide

This directory contains scripts for deploying and managing Crown/CRAQ nodes on remote VMs.

## Scripts Overview

### 1. `vm_setup.bash` (Orchestrator)
Coordinates deployment across multiple VMs via SSH.

**Changes:**
- ✅ Uses `SSH_USER` from `.env` (no username argument in command)
- ✅ Loads configuration from `.env` file in this directory
- ✅ Supports actions: `setup` | `start` | `build` | `deploy` | `kill`
- ✅ Forwards all env vars to remote scripts (per-user aware)

**Usage:**
```bash
./vm_setup.bash setup    # Setup VMs for SSH_USER from .env
./vm_setup.bash start    # Start nodes for SSH_USER from .env
./vm_setup.bash kill     # Kill all nodes for SSH_USER from .env
```

### 2. `setup.bash` (Dependency Installation)
One-time setup of system dependencies on each VM.

**Changes:**
- ✅ Uses `SSH_USER` from environment (or current user fallback)
- ✅ Detects package manager (apt-get / dnf)
- ✅ Installs all required dependencies:
  - Build tools: `git`, `cmake`, `make`, `g++`
  - Communication: `openssh-client`, `wget`
  - Deployment: `rsync`, `tmux`
  - gRPC/protobuf: `libgrpc++-dev`, `libprotobuf-dev`, `protobuf-compiler`, `protobuf-compiler-grpc` on Debian/Ubuntu; `grpc-devel`, `protobuf-devel`, `protobuf-compiler` on Fedora/RHEL
- ✅ Requires passwordless `sudo` or running as root on the VM

**Usage:**
```bash
./setup.bash         # Setup system packages
# OR via vm_setup.bash:
./vm_setup.bash setup
```

### 3. `start_server.bash` (Build & Deploy)
Clone/pull repo, build project, start node in tmux.

**Changes:**
- ✅ Uses `SSH_USER` from environment (or current user fallback)
- ✅ Supports both `crown` (main src) and `craq` (standalone) project modes
- ✅ Environment variables for customization:
  - `PROJECT_MODE`: `crown` or `craq`
  - `PROJECT_SUBDIR`: Path within repo (use `.` for repo root)
  - `NODE_HOST`: Bind address (default: `0.0.0.0`)
  - `NODE_PORT`: Server port (default: `5001`)
- ✅ Shared session names: `crown_node_${port}` by default
- ✅ Shared log/pid directories: `${PROJECT_DIR}/run/shared/`
- ✅ Shared tmux socket: `/tmp/crown-shared/tmux.sock` (others can attach)
- ✅ Preflight checks: Validates git, cmake, g++, tmux installed
- ✅ Checks if process already running; saves pid for later cleanup
- ✅ Pipe logs to file with `tmux pipe-pane`
- ✅ Launches the current `server` binary from `build/server`

**Usage:**
```bash
# Start server on port 5001
./start_server.bash

# With custom port and project mode:
NODE_PORT=5002 PROJECT_MODE=crown ./start_server.bash

# Multiple nodes on same VM:
NODE_PORT=5001 ./start_server.bash
NODE_PORT=5002 ./start_server.bash
```

**Output:**
```
Server started in tmux session: ${SSH_USER}_node_5001
Server pane pid: 12345
log: /path/to/crown/run/shared/server_5001.log
pid: /path/to/crown/run/shared/server_5001.pid
attach: tmux -S /tmp/crown-shared/tmux.sock attach -t crown_node_5001
```

**Attach to running server:**
```bash
tmux -S /tmp/crown-shared/tmux.sock attach -t crown_node_5001
```

### 4. `kill.bash` (Process Cleanup)
Stop all nodes for a given user.

**Changes:**
- ✅ Uses `SSH_USER` from environment (or current user fallback)
- ✅ Kills shared tmux sessions (matches `crown_node_*` pattern)
- ✅ Kills server processes for the deployment user
- ✅ Cleans up pid files in shared `run/shared/` directories
- ✅ Safe: Only kills processes owned by the specified user (uses `pkill -u`)
- ✅ Works across multiple project modes and locations

**Usage:**
```bash
./kill.bash          # Kill all nodes for SSH_USER from .env

# Via vm_setup.bash:
./vm_setup.bash kill
```

## Configuration (.env file)

Use the existing `.env` file in this directory and customize it:

For this repository, use `PROJECT_SUBDIR=.` because the top-level `CMakeLists.txt` lives at the repository root.

Edit `.env`:
```bash
SSH_USER=your-username              # Default SSH user
REPO_URL=https://github.com/.../crown.git
REPO_BRANCH=master
REMOTE_BASE_DIR=/home
PROJECT_SUBDIR=.                    # repo root
PROJECT_MODE=crown                  # or craq
TMUX_SOCKET=/tmp/crown-shared/tmux.sock
RUN_SCOPE=shared
NODE_PORT=5001
```

## Multi-User Multi-Node Example

Run multiple nodes for the user in `.env` (`SSH_USER`) on different ports:

```bash
# Setup once:
./vm_setup.bash setup

# Start two nodes:
NODE_PORT=5001 ./vm_setup.bash start
NODE_PORT=5002 ./vm_setup.bash start

# Check running nodes for SSH_USER from .env (example: alice):
tmux -S /tmp/crown-shared/tmux.sock list-sessions | grep crown_node_

# Attach to first node (example):
tmux -S /tmp/crown-shared/tmux.sock attach -t crown_node_5001

# Stop all nodes:
./kill.bash
```

## Directory Layout After Deployment

```
$REMOTE_BASE_DIR/
└── crown/                        (REPO_NAME)
    ├── CMakeLists.txt
    ├── src/                      (or crown/craq for standalone mode)
    │   ├── replication/
    │   └── ...
    └── run/                      (shared logs/pids)
      └── shared/
            ├── server_5001.log
            ├── server_5001.pid
            ├── server_5002.log
            └── server_5002.pid
      └── ...
```

## Troubleshooting

### Script won't execute
```bash
chmod +x setup/vm_setup.bash
chmod +x setup/setup.bash
chmod +x setup/start_server.bash
chmod +x setup/kill.bash
```

### Missing .env
```bash
# Create or edit setup/.env with your configuration
```

### SSH connection fails
- Check `.env` has correct `SSH_USER`
- Verify SSH key in `~/.ssh/id_rsa`
- Test: `ssh -i ~/.ssh/id_rsa user@hostname`

### Node fails to start
```bash
# Check logs:
tail -f /remote/path/to/crown/run/username/server_5001.log

# Check process:
ps aux | grep server

# Manually attach tmux:
tmux -S /tmp/crown-shared/tmux.sock attach -t crown_node_5001
```

### Old pid file prevents restart
```bash
# Manually cleanup:
rm /path/to/crown/run/shared/server_5001.pid

# Or use kill script:
./kill.bash
```

## Advanced: Direct Script Execution

Run scripts directly on local/remote without vm_setup.bash:

```bash
# On remote VM:
ssh user@vm1
cd crown/setup

# Setup (one-time):
./setup.bash

# Build and start:
./start_server.bash

# Stop:
./kill.bash
```

## Environment Variables Reference

| Variable | Default | Example | Purpose |
|----------|---------|---------|---------|
| REPO_URL | (required) | github.com/org/crown | Git clone URL |
| REPO_BRANCH | master | develop | Git branch to deploy |
| REPO_NAME | crown | crown | Directory name after clone |
| REMOTE_BASE_DIR | /home | /home | Shared base directory on VM |
| PROJECT_SUBDIR | . | . | Subdir within repo to build (repo root) |
| PROJECT_MODE | crown | craq | crown or craq (affects binary name) |
| BUILD_TYPE | Release | Debug | CMake build type |
| NODE_HOST | 0.0.0.0 | 127.0.0.1 | Bind address |
| NODE_PORT | 5001 | 5002 | Server port |
| TMUX_SESSION_NAME | crown_node_${NODE_PORT} | crown_node_5001 | Shared tmux session name |
| TMUX_SOCKET | /tmp/crown-shared/tmux.sock | /tmp/crown-shared/tmux.sock | Shared tmux socket path |
| RUN_SCOPE | shared | shared | Run directory scope under run/ |
| SSH_USER | (required) | alice | SSH user for all VMs |
