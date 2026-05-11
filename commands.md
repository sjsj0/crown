# Crown — Command Reference

Quick reference for building, deploying, and running the cluster, the metadata
server, and the client. Run everything from the repo root unless noted.

```bash
cd "/mnt/d/UIUC/Spring '26/CS 525 - Advanced Distributed System/crown"   # local
# on a VM: cd /home/crown   (REMOTE_BASE_DIR/REPO_NAME from setup/.env)
```

Component summary:
- **`build/server`** — a node process; starts config-agnostic, waits for the metadata server to push its `NodeConfig`.
- **`build/metadata_server`** — single source of truth: reads a `config.*.json`, pushes Configure to every node, runs a ping-ack failure detector, serves `MetadataStore.GetCluster`.
- **`build/client`** — fetches topology/mode from the metadata server (never reads config files). Interactive or benchmark modes.

---

## 1) Build

```bash
# deps — Ubuntu/Debian
sudo apt install cmake libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc nlohmann-json3-dev
# deps — macOS
brew install cmake grpc nlohmann-json

# configure + build (generates protos, builds server / metadata_server / client)
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON      # add -DCMAKE_PREFIX_PATH=/opt/homebrew on macOS
cmake --build build

# build just the client
cmake --build build --target client

# optional: clangd/IDE support
ln -s build/compile_commands.json compile_commands.json
```

---

## 2) Generate mode configs

```bash
# CHAIN + CRAQ + CROWN configs together
python3 setup/generate_mode_configs.py <node_count> --base-port <port> --env <dev|prod> --output-dir <dir>

# prod, 5 nodes (1201..1205), all on port 50051
python3 setup/generate_mode_configs.py 5 --base-port 50051 --env prod --output-dir build/prod_configs --prefix config
#   -> build/prod_configs/config.chain.json, config.craq.json, config.crown.json

# dev (all hosts 127.0.0.1, ports increment), CROWN only
python3 setup/generate_mode_configs.py 3 --base-port 50051 --env dev --output-dir . --prefix config --modes crown
```

`--env dev`: hosts = `127.0.0.1`, ports increment per node. `--env prod`: hosts = `sp26-cs525-1201..`, all nodes share `--base-port`. `--host <v>` overrides the host for all nodes.

---

## 3) Local single-machine run

```bash
# 3 nodes on different ports
./build/server --port 50051 &
./build/server --port 50052 &
./build/server --port 50053 &

# metadata server (the only thing that reads config.json)
./build/metadata_server --config config.json --host 0.0.0.0 --port 50050 --log
# useful flags: --ping-interval-ms (1000) --ping-timeout-ms --failure-threshold (3)

# client (interactive)
./build/client 127.0.0.1:50050
```

---

## 4) Prod VM bring-up (via setup/ scripts)

Driven by `setup/.env` + `setup/prod_hosts.csv` (server VMs; one also runs metadata) + `setup/client_hosts.csv` (client-only VMs). Repo is cloned/built on the **union** of both files.

### 4a) Bring up the whole cluster from your local terminal (TL;DR)

Everything below runs locally; the script SSHes into the VMs for you.

```bash
cd "/mnt/d/UIUC/Spring '26/CS 525 - Advanced Distributed System/crown"

# generate prod configs once (node count must match prod_hosts.csv), all on port 50051:
python3 setup/generate_mode_configs.py 3 --base-port 50051 --env prod --output-dir build/prod_configs --prefix config

./setup/vm_setup.bash setup    # one-time per VM: install build deps
./setup/vm_setup.bash build    # clone + build the repo on prod ∪ client ∪ metadata VMs (no processes started)
# (re)start the server nodes AND the metadata_server, all from the already-built binaries:
METADATA_CONFIG=build/prod_configs/config.crown.json ./setup/vm_setup.bash start
```

Action scopes:
- **`setup`** → install build deps on `prod_hosts.csv ∪ client_hosts.csv ∪ $METADATA_HOST`.
- **`build`** (and `deploy`) → `git clone`/`pull` + `cmake build` on `prod_hosts.csv ∪ client_hosts.csv ∪ $METADATA_HOST`. **Nothing is started** — it just produces `build/server`, `build/metadata_server`, `build/client` on every VM.
- **`start`** → (re)start `./build/server --port $NODE_PORT` (default 50051, tmux `crown`, log `run/shared/server_<port>.log`) on each `prod_hosts.csv` VM — *no build* — then (re)start `./build/metadata_server` on `$METADATA_HOST` (tmux `crown_metadata_<port>`, log `run/shared/metadata_<port>.log`) with `$METADATA_CONFIG`. Client-only VMs are not touched. If `METADATA_HOST` is unset, `start` warns and skips the metadata step. (Each (re)start kills the previous instance first.)

Variants:
- `./setup/vm_setup.bash start-servers` — node servers only (prod hosts), skip the metadata server.
- `METADATA_CONFIG=build/prod_configs/config.craq.json ./setup/vm_setup.bash start-metadata` — (re)start just the metadata server, e.g. to switch modes.
- `./setup/vm_setup.bash rerun` — kill everything (prod ∪ client ∪ metadata), then `start` again (node servers on prod + metadata on `$METADATA_HOST`). No build — run `build` first if you want fresh code.
- `./setup/vm_setup.bash kill` — stop all servers + metadata on `prod_hosts.csv ∪ client_hosts.csv ∪ $METADATA_HOST`.

Check / attach on a VM:

```bash
ssh ritwikg3@sp26-cs525-1201.cs.illinois.edu
tmux -S /tmp/crown-shared/tmux.sock list-sessions
tmux -S /tmp/crown-shared/tmux.sock attach -t crown_node_50051
```

⚠️ Pre-flight for `setup/.env`:
- `setup` / `build` / `kill` now also cover `$METADATA_HOST` even if it's not in `prod_hosts.csv` / `client_hosts.csv` — so it gets build deps + the repo + teardown like everything else. It still needs to be SSH-reachable. Current `.env` has `METADATA_HOST=sp26-cs525-1220...`.
- `start` / `start-metadata` do **not** build — run `build` first (it includes the metadata host).
- `METADATA_CONFIG` must point at a config that exists on the metadata VM. Default `config.json` is the one committed at the repo root; for a specific mode, generate `build/prod_configs/config.*.json` (above) and pass `METADATA_CONFIG=...` inline as shown.

### 4b) Full script reference

```bash
./setup/vm_setup.bash setup            # install deps on prod ∪ client ∪ $METADATA_HOST
./setup/vm_setup.bash build            # clone + build on prod ∪ client ∪ $METADATA_HOST (no processes started)
./setup/vm_setup.bash start            # (re)start node servers on prod, THEN metadata_server on $METADATA_HOST (no build)
./setup/vm_setup.bash start-servers    # (re)start node servers on prod only (skip the metadata_server)
# (re)start just the metadata server; METADATA_CONFIG picks the mode (must exist on that VM)
METADATA_CONFIG=build/prod_configs/config.crown.json ./setup/vm_setup.bash start-metadata
./setup/vm_setup.bash rerun            # kill (prod ∪ client ∪ metadata), then start again (prod servers + metadata)
./setup/vm_setup.bash kill             # stop servers + metadata on prod ∪ client ∪ $METADATA_HOST
```

Manual equivalents (5 nodes 1201..1205, shared port 50051):

```bash
# start servers
for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; \
  ssh <netid>@$h "cd /home/crown && nohup ./build/server --port 50051 --server-log true > server_50051.log 2>&1 &"; done

# connectivity check
for i in 1 2 3 4 5; do nc -vz "sp26-cs525-120${i}.cs.illinois.edu" 50051; done

# metadata server on one VM (e.g. 1201)
./build/metadata_server --config build/prod_configs/config.crown.json --host 0.0.0.0 --port 50050 --log

# stop everything
for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; \
  ssh <netid>@$h "pkill -f 'build/metadata_server'; pkill -f 'build/server --port' || true"; done
```

tmux (when started via the scripts): sessions `crown_node_<port>` / `crown_metadata_<port>` on socket `/tmp/crown-shared/tmux.sock`:

```bash
tmux -S /tmp/crown-shared/tmux.sock list-sessions
tmux -S /tmp/crown-shared/tmux.sock attach -t crown_node_50051
```

---

## 5) Run the client binary on a VM

The cluster + metadata server must already be running. First arg is always the
metadata `host:port` (topology and replication mode come from there).

```bash
# --- interactive ---
./build/client <metadata_host:port> [ack_port]
# e.g.
./build/client sp26-cs525-1201.cs.illinois.edu:50050 61000
#   arg2 = ack-listener port, default 60000
# then: write <key> <value> | read <key> [node_id] | help | quit
```

```bash
# --- benchmark: write ---
./build/client <metadata_host:port> [ack_port] bench-write <total_ops> <key_count> <client_index> <num_clients> [key_prefix] [value_prefix] [hot=<0-100>]
# e.g. 50k writes, 64 keys, this is client 0 of 1
./build/client sp26-cs525-1201.cs.illinois.edu:50050 bench-write 50000 64 0 1

# --- benchmark: read ---
./build/client <metadata_host:port> [ack_port] bench-read <total_ops> <key_count> <client_index> <num_clients> [craq_node_id] [key_prefix] [hot=<0-100>]
# e.g. 50k reads, 64 keys, CRAQ read from any node (-1)
./build/client sp26-cs525-1201.cs.illinois.edu:50050 bench-read 50000 64 0 1 -1
```

Notes: `client_index` in `[0, num_clients)`; `hot=N` (also accepts `crown_hot_head_pct=N` for write / `read_hot_key_pct=N` for read) skews load toward one head/key. The client must already be built on that VM (`cmake --build build --target client`).

---

## 6) Distributed throughput runner

Drives one client process per host (from `--hosts`, default `setup/client_hosts.csv`); auto-assigns `client_index=0..N-1`. Metadata server must already be running. The metadata server defines the mode — run one mode at a time; `--modes` here is only a log/key-prefix label.

```bash
# multi-client (one process per VM in client_hosts.csv)
python3 setup/run_throughput_experiments.py \
  --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes crown --ops write read \
  --write-op-count 50000 --read-op-count 50000 --key-count 64 \
  --work-dir build/prod_throughput_multi_client

# single-client (one VM)
python3 setup/run_throughput_experiments.py \
  --hosts sp26-cs525-1218.cs.illinois.edu \
  --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes crown --ops write read \
  --write-op-count 5000 --read-op-count 5000 --key-count 64 \
  --work-dir build/prod_throughput_single_client

# CHAIN / CRAQ / CROWN — restart metadata_server with the matching config first
python3 setup/run_throughput_experiments.py --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 --modes chain --ops write read \
  --write-op-count 50000 --read-op-count 50000 --key-count 64 --work-dir build/prod_chain_only

python3 setup/run_throughput_experiments.py --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 --modes craq --ops write read \
  --write-op-count 50000 --read-op-count 50000 --key-count 64 --craq-read-node-id -1 --work-dir build/prod_craq_only

# CROWN hot-head write skew (60% of writes to one head)
python3 setup/run_throughput_experiments.py --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 --modes crown --ops write \
  --write-op-count 50000 --key-count 64 --crown-hot-head-pct 60 --work-dir build/prod_crown_hot_60

# read hot-key skew (80% of reads to one key)
python3 setup/run_throughput_experiments.py --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 --modes crown --ops read \
  --read-op-count 50000 --key-count 64 --read-hot-key-pct 80 --work-dir build/prod_read_hot_80

# preview the SSH/SCP plan without running it
python3 setup/run_throughput_experiments.py ... --dry-run
```

Outputs per `--work-dir <DIR>`: `<DIR>/logs/*.log`, `<DIR>/ssh_logs/*.log`, `<DIR>/summary.csv`. Aggregate further with `python3 setup/aggregate_bench_results.py` if needed.

Key args: `--metadata host:port` (default `$METADATA_HOST:$METADATA_PORT`), `--hosts` / `--hosts-file`, `--modes` (chain|craq|crown, label only), `--ops` (write read), `--write-op-count` / `--read-op-count`, `--key-count`, `--craq-read-node-id`, `--crown-hot-head-pct`, `--read-hot-key-pct`, `--work-dir`, `--dry-run`.

---

## 7) Chain-length throughput experiment runner

Runs the full chain-length experiment grid from your laptop by SSHing into the
server/client VMs. The script generates mode configs, kills/restarts remote
servers and metadata, runs the distributed client benchmark, collects logs, and
writes CSVs locally.

```bash
# preview every SSH/SCP/benchmark command without touching remote machines
python3 setup/run_chain_length_experiments.py --dry-run

# full default run
python3 setup/run_chain_length_experiments.py
```

Default experiment grid:

- chain lengths: `3 5 7`
- modes: `chain craq crown`
- client counts: `1 3 5`
- operations: `write read`
- trials: `3`
- unique keys per benchmark run: `64`
- total writes per write benchmark run: `50000`
- total reads per read benchmark run: `50000`

That is `3 * 3 * 3 * 2 * 3 = 162` benchmark runs. Each benchmark
run gets a full kill/start cycle before it runs. A benchmark run is one
operation for one tuple, e.g. `n=5, mode=crown, clients=3, op=write, trial=2`.

Quick smoke run:

```bash
python3 setup/run_chain_length_experiments.py \
  --chain-lengths 3 --modes crown --client-counts 1 --trials 3 \
  --key-count 8 --write-op-count 100 --read-op-count 100
```

Custom workload size:

```bash
python3 setup/run_chain_length_experiments.py \
  --chain-lengths 3 5 7 \
  --client-counts 1 3 5 \
  --trials 3 \
  --key-count 128 \
  --write-op-count 100000 \
  --read-op-count 100000
```

Inputs picked up from files / environment:

- `setup/.env`: `SSH_USER`, `SSH_KEY_LOCAL`, `REPO_URL`, `REPO_BRANCH`,
  `REMOTE_BASE_DIR`, `REPO_NAME`, `PROJECT_SUBDIR`, `REMOTE_REPO_DIR`,
  `METADATA_HOST`, `METADATA_PORT`, `NODE_PORT`, `TMUX_SOCKET`, and related
  deployment settings.
- `setup/prod_hosts.csv`: server inventory. For chain length `N`, the script
  uses the first `N` hosts. The default grid requires at least 7 server hosts.
- `setup/client_hosts.csv`: client inventory. For client count `C`, the script
  uses the first `C` hosts. The default grid requires at least 5 client hosts.

Inputs from code defaults unless overridden:

- `--work-dir build/chain_length_throughput_runs`
- `--chain-lengths 3 5 7`
- `--modes chain craq crown`
- `--client-counts 1 3 5`
- `--ops write read`
- `--trials 3`
- `--key-count 64`
- `--write-op-count 50000`
- `--read-op-count 50000`
- `--stabilization-seconds 3`

Outputs:

- `build/chain_length_throughput_runs/raw_trials.csv` — one row per operation
  per trial, with labels such as `chain_length`, `mode`, `operation`,
  `client_count`, `trial`, `key_count`, `op_count`, server/client host lists,
  config path, and source log path.
- `build/chain_length_throughput_runs/summary_by_chain_length.csv` — grouped
  rows with `throughput_mean`, `throughput_stddev`, `latency_mean_ms`, and
  `latency_stddev_ms` for graph error bars.
- Per-case logs under `build/chain_length_throughput_runs/cases/`.
- Lifecycle SSH/SCP logs under `build/chain_length_throughput_runs/lifecycle_logs/`.

The per-benchmark key/value prefixes are exact and identify the run:

```text
bench-n{chain_length}-{mode}-c{client_count}-t{trial}-{op}-
value-n{chain_length}-{mode}-c{client_count}-t{trial}-{op}-
```

Yes: you run this script from your laptop/local checkout. The benchmark itself
runs on the remote server/client VMs via SSH. Make sure you have already run
the remote build on all server, client, and metadata hosts before the full run:

```bash
./setup/vm_setup.bash build
python3 setup/run_chain_length_experiments.py --dry-run
python3 setup/run_chain_length_experiments.py
```

---

## 8) Switch replication mode

Stop the metadata server and restart it with a different config; clients pick up the new mode on their next start.

```bash
# scripts:
METADATA_CONFIG=build/prod_configs/config.chain.json ./setup/vm_setup.bash start-metadata
# manual:
./build/metadata_server --config build/prod_configs/config.chain.json --host 0.0.0.0 --port 50050 --log
```

---

## 9) setup/.env keys (reference)

```bash
SSH_USER=<netid>
REPO_URL=https://github.com/.../crown.git
REPO_BRANCH=master
REMOTE_BASE_DIR=/home          # remote repo lives at $REMOTE_BASE_DIR/$REPO_NAME (e.g. /home/crown)
PROJECT_SUBDIR=.               # repo root (CMakeLists.txt is at the top level)
PROJECT_MODE=crown             # or craq
NODE_PORT=50051
TMUX_SOCKET=/tmp/crown-shared/tmux.sock
RUN_SCOPE=shared
METADATA_HOST=sp26-cs525-1201.cs.illinois.edu
METADATA_PORT=50050
METADATA_CONFIG=config.json    # config the metadata server loads + pushes
# optional: SSH_KEY_LOCAL, METADATA_PING_INTERVAL_MS, METADATA_PING_TIMEOUT_MS, METADATA_FAILURE_THRESHOLD
```

Host inventory files (one host per line; trailing comma OK; `#` comments ignored):
- `setup/prod_hosts.csv` — server-node VMs (one also runs the metadata store).
- `setup/client_hosts.csv` — client-only VMs (benchmark drivers).
