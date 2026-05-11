# Crown Deployment Scripts Guide

This directory contains scripts for deploying and managing Crown/CRAQ nodes on remote VMs.

## Prod Interactive Mode

For a focused step-by-step guide to run the interactive client against prod hosts, see:

- `setup/PROD_INTERACTIVE_README.md`

## Scripts Overview

### 1. `vm_setup.bash` (Orchestrator)
Coordinates deployment across multiple VMs via SSH.

**Host inventory:** the script reads two CSV files in this directory (one host per
line, trailing comma allowed, `#` comments ignored):
- `prod_hosts.csv` — VMs that run **server** nodes. One of them (`$METADATA_HOST`
  in `.env`) also runs the metadata store.
- `client_hosts.csv` — VMs that run **only** the `client` binary (benchmark drivers).

The repo is cloned + built on the **union** of both files; server/metadata
processes start only on the relevant subset.

**Usage:**
```bash
./vm_setup.bash setup           # install deps on prod_hosts.csv ∪ client_hosts.csv
./vm_setup.bash build           # clone/build the repo on prod_hosts.csv ∪ client_hosts.csv
./vm_setup.bash start           # start a server node on each prod_hosts.csv VM, THEN the metadata_server on $METADATA_HOST
./vm_setup.bash start-servers   # start a server node on each prod_hosts.csv VM only (skip the metadata_server)
./vm_setup.bash start-metadata  # start the metadata_server on $METADATA_HOST only
./vm_setup.bash kill            # stop servers + metadata on prod_hosts.csv ∪ client_hosts.csv ∪ $METADATA_HOST
```

Typical bring-up order: `setup` → `build` → `start`. `start` launches a server
node on every `prod_hosts.csv` VM and then brings up the `metadata_server` on
`$METADATA_HOST` (it pushes Configure to every node, then runs the ping-ack
detector) — no separate `start-metadata` step needed. If `METADATA_HOST` is
unset, `start` warns and skips the metadata step. Clients then point at
`$METADATA_HOST:$METADATA_PORT`. Use `start-servers` when you want the node
servers without (re)starting the metadata server, and `start-metadata` to
(re)start just the metadata server — e.g. to switch modes with a different
`METADATA_CONFIG`.

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
  - `SERVER_LOG`: `true`/`false` to enable server stdout/stderr logs (default: `false`)
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

# Enable server logs explicitly:
SERVER_LOG=true NODE_PORT=5001 ./start_server.bash
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

### 4. `start_metadata.bash` (Metadata store)
Clone/build the repo on this VM and start `build/metadata_server` in a tmux
session. The metadata server is the single source of truth for cluster topology:
it reads `$METADATA_CONFIG`, validates it, pushes Configure to every node, runs a
ping-ack failure detector (logs a node DOWN after `$METADATA_FAILURE_THRESHOLD`
missed acks), and serves `MetadataStore.GetCluster`. Run it on exactly one VM.

```bash
# Via vm_setup.bash (runs on $METADATA_HOST only):
./vm_setup.bash start-metadata
```

Relevant env vars: `METADATA_PORT`, `METADATA_CONFIG`,
`METADATA_PING_INTERVAL_MS`, `METADATA_PING_TIMEOUT_MS`,
`METADATA_FAILURE_THRESHOLD`. Logs at `run/$RUN_SCOPE/metadata_${METADATA_PORT}.log`,
tmux session `crown_metadata_${METADATA_PORT}`.

### 5. `kill.bash` (Process Cleanup)
Stop all server **and** metadata processes for the deployment user.

- ✅ Kills shared tmux sessions (`crown_node_*`, `crown_metadata_*`, legacy `crown`)
- ✅ Kills `server` and `metadata_server` processes for the deployment user
- ✅ Cleans up pid files in shared `run/shared/` directories
- ✅ Safe: only kills processes owned by the specified user (`pkill -u`)

```bash
./kill.bash            # locally
./vm_setup.bash kill   # across prod_hosts.csv ∪ client_hosts.csv
```

### 6. `run_throughput_experiments.py` (Distributed Throughput Runner)
Runs write/read throughput tests using distributed client VMs. The clients fetch
their topology from the metadata server, so **start the metadata server first**
(`./vm_setup.bash start-metadata`). The runner takes a `--metadata HOST:PORT`
endpoint (defaults to `$METADATA_HOST:$METADATA_PORT`); `--hosts` defaults to the
machines in `setup/client_hosts.csv`.

Because the metadata server defines the replication mode, run one mode at a time
(restart `metadata_server` with a different `config.json` to switch modes). The
`--modes` list is still accepted but is only used for log/key-prefix labels.

**Usage:**
```bash
# 1) bring up the cluster + metadata server (one command: servers, then metadata)
./setup/vm_setup.bash start
#    (to switch modes later, restart just the metadata server with a different config:
#     METADATA_CONFIG=build/prod_configs/config.craq.json ./setup/vm_setup.bash start-metadata)

# 2) run benchmarks from the client VMs
python3 setup/run_throughput_experiments.py \
  --ssh-user <your-netid> \
  --remote-repo-dir /home/crown \
  --metadata "$METADATA_HOST:$METADATA_PORT" \
  --modes crown \
  --ops write read \
  --write-op-count 50000 \
  --read-op-count 50000 \
  --key-count 64 \
  --work-dir build/prod_throughput
```

**Outputs:**
- Local client logs: `<work-dir>/logs`
- Local SSH logs: `<work-dir>/ssh_logs`
- Aggregate CSV: `<work-dir>/summary.csv`

## Configuration (.env file)

Use the existing `.env` file in this directory and customize it. For this
repository, use `PROJECT_SUBDIR=.` because the top-level `CMakeLists.txt` lives at
the repository root.

```bash
SSH_USER=your-username              # Default SSH user
REPO_URL=https://github.com/.../crown.git
REPO_BRANCH=master
REMOTE_BASE_DIR=/home
PROJECT_SUBDIR=.                    # repo root
PROJECT_MODE=crown                  # or craq
TMUX_SOCKET=/tmp/crown-shared/tmux.sock
RUN_SCOPE=shared
NODE_PORT=50051

# Metadata store
METADATA_HOST=sp26-cs525-1201.cs.illinois.edu   # which prod VM runs the metadata store
METADATA_PORT=50050
METADATA_CONFIG=config.json                      # config the metadata server loads + pushes
```

Host inventory files (one host per line, trailing comma OK, `#` comments ignored):
- `prod_hosts.csv`   — server-node VMs (one also runs the metadata store).
- `client_hosts.csv` — client-only VMs (benchmark drivers).

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
| METADATA_HOST | (required for start-metadata) | sp26-cs525-1201.cs.illinois.edu | VM that runs the metadata store |
| METADATA_PORT | 50050 | 50050 | metadata_server gRPC port |
| METADATA_CONFIG | config.json | build/prod_configs/config.crown.json | config the metadata server loads + pushes |
| METADATA_PING_INTERVAL_MS | 1000 | 500 | ping interval for the failure detector |
| METADATA_PING_TIMEOUT_MS | 500 | 300 | per-ping deadline |
| METADATA_FAILURE_THRESHOLD | 3 | 5 | consecutive missed pings → node DOWN |
