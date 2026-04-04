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
  - `PROJECT_SUBDIR`: Path within repo (e.g., `crown` or `crown/craq`)
  - `NODE_HOST`: Bind address (default: `0.0.0.0`)
  - `NODE_PORT`: Server port (default: `5001`)
- ✅ User-specific session names: `${SSH_USER}_node_${port}`
- ✅ User-specific log/pid directories: `${PROJECT_DIR}/run/${SSH_USER}/`
- ✅ Preflight checks: Validates git, cmake, g++, tmux installed
- ✅ Checks if process already running; saves pid for later cleanup
- ✅ Pipe logs to file with `tmux pipe-pane`

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
log: /path/to/crown/run/${SSH_USER}/server_5001.log
pid: /path/to/crown/run/${SSH_USER}/server_5001.pid
attach: tmux attach -t ${SSH_USER}_node_5001
```

**Attach to running server:**
```bash
tmux attach -t ${SSH_USER}_node_5001
```

### 4. `kill.bash` (Process Cleanup)
Stop all nodes for a given user.

**Changes:**
- ✅ Uses `SSH_USER` from environment (or current user fallback)
- ✅ Kills user-scoped tmux sessions (matches `${SSH_USER}_node_*` pattern)
- ✅ Kills user processes matching `server` or `craq_node`
- ✅ Cleans up pid files in user-specific `run/` directories
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

For this repository, `PROJECT_SUBDIR` should normally be `crown` because the top-level `CMakeLists.txt` lives at the repository root. The launcher will fall back to the root automatically if `PROJECT_SUBDIR` is wrong.

Edit `.env`:
```bash
SSH_USER=your-username              # Default SSH user
REPO_URL=https://github.com/.../crown.git
REPO_BRANCH=master
PROJECT_SUBDIR=crown                # or crown/craq for standalone
PROJECT_MODE=crown                  # or craq
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
tmux list-sessions | grep "$SSH_USER"

# Attach to first node (example):
tmux attach -t ${SSH_USER}_node_5001

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
    └── run/                      (user-specific logs/pids)
      └── ${SSH_USER}/
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
ps aux | grep craq_node
ps aux | grep server

# Manually attach tmux:
tmux attach -t ${SSH_USER}_node_5001
```

### Old pid file prevents restart
```bash
# Manually cleanup:
rm /path/to/crown/run/${SSH_USER}/server_5001.pid

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
| REPO_BRANCH | main | develop | Git branch to deploy |
| REPO_NAME | crown | crown | Directory name after clone |
| REMOTE_BASE_DIR | $HOME | /opt/research | Where to clone repo on VM |
| PROJECT_SUBDIR | crown | crown/craq | Subdir within repo to build |
| PROJECT_MODE | crown | craq | crown or craq (affects binary name) |
| BUILD_TYPE | Release | Debug | CMake build type |
| NODE_HOST | 0.0.0.0 | 127.0.0.1 | Bind address |
| NODE_PORT | 5001 | 5002 | Server port |
| TMUX_SESSION_NAME | (auto) | my_session | Custom tmux session name |
| SSH_USER | (required) | alice | SSH user for all VMs |
