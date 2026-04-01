# CRAQ Standalone (C++)

Standalone CRAQ implementation in C++ for local and multi-VM testing.

## What Is Implemented

- CRAQ node server (`craq_node`):
  - Receives `CONFIG` from leader/client.
  - Stores dirty versions and a clean version per key.
  - Replicates writes head -> ... -> tail.
  - Tail sends `CLIENT_ACK` directly to client and starts upstream ACK propagation.
  - On ACK receipt, each node marks version clean and removes dirty versions up to that version.
  - Accepts read from any node; non-tail nodes query tail for the latest clean value.

- CRAQ leader/client app (`craq_leader`):
  - Configures all nodes from a local config file.
  - Sends write requests to head and waits for tail ACK.
  - Sends reads to any node.
  - Dumps node state for debug.

## Folder Layout

- `include/`: shared headers
- `src/`: C++ source files
- `configs/cluster.sample.conf`: example chain config
- `scripts/start_node.sh`: build + run node
- `scripts/configure.sh`: build + send config
- `CMakeLists.txt`: build file

## Build

```bash
cd crown/craq
cmake -S . -B build
cmake --build build --config Debug
```

Binaries:

- Windows (Visual Studio generator): `build/Debug/craq_node.exe`, `build/Debug/craq_leader.exe`
- Linux/macOS (single-config generators): `build/craq_node`, `build/craq_leader`

## Config File Format

Example (`configs/cluster.sample.conf`):

```txt
mode CRAQ
node n1 127.0.0.1 5001
node n2 127.0.0.1 5002
node n3 127.0.0.1 5003
```

Meaning:

- `mode`: must be `CRAQ` for this implementation.
- `node`: ordered chain from head to tail.

## Run Locally (3 nodes)

Terminal 1:

```bash
./build/Debug/craq_node.exe --host 0.0.0.0 --port 5001
```

Terminal 2:

```bash
./build/Debug/craq_node.exe --host 0.0.0.0 --port 5002
```

Terminal 3:

```bash
./build/Debug/craq_node.exe --host 0.0.0.0 --port 5003
```

Configure cluster from another terminal:

```bash
./build/Debug/craq_leader.exe configure --config configs/cluster.sample.conf
```

Write via head and wait for tail ACK:

```bash
./build/Debug/craq_leader.exe write \
  --head 127.0.0.1:5001 \
  --key k1 \
  --value hello \
  --leader-host 127.0.0.1 \
  --ack-port 7000 \
  --timeout-ms 5000
```

Read from any node:

```bash
./build/Debug/craq_leader.exe read --node 127.0.0.1:5002 --key k1
```

Dump state:

```bash
./build/Debug/craq_leader.exe dump --node 127.0.0.1:5001
./build/Debug/craq_leader.exe dump --node 127.0.0.1:5002
./build/Debug/craq_leader.exe dump --node 127.0.0.1:5003
```

If you build on Linux/macOS, replace `./build/Debug/craq_node.exe` with `./build/craq_node` and
`./build/Debug/craq_leader.exe` with `./build/craq_leader`.

## Protocol Summary

Messages are line-based text over TCP.
Each line is `TYPE|k=v|k=v...` with escaping for separators.

Key message types:

- `CONFIG`
- `CLIENT_WRITE`
- `REPL_WRITE`
- `ACK`
- `CLIENT_READ`
- `TAIL_READ`
- `CLIENT_ACK`
- `DUMP`
- `OK` / `ERR`

## CRAQ Semantics in This Code

- Dirty write inserted at each node on `REPL_WRITE`.
- Tail marks version clean and immediately ACKs client.
- Tail sends ACK upstream.
- Every upstream node marks version clean on ACK and prunes dirty versions `<= ack_version`.
- Reads from non-tail nodes always go to tail for latest clean version.

## VM Notes

- Open node ports and client ACK port in firewalls/security groups.
- Use VM-private IPs in config and command args.
- Start all nodes before running `configure`.

## Run on VMs (Setup + Deploy + Test)

This section is for the VM workflow used in this project.
Keep using the local section above for local-only runs.

### 1) One-time dependency install on all target VMs

Run from `crown/setup`:

```bash
./vm_setup.bash setup
```

This installs system tools needed by CRAQ (`git`, `cmake`, `g++`, `make`, `rsync`, `tmux`).

### 2) Pull latest code, rebuild, and start CRAQ node on all target VMs

Run from `crown/setup`:

```bash
./vm_setup.bash deploy
```

Notes:

- `deploy` copies and runs `setup/start_server.bash` on each listed VM.
- `start_server.bash` does: clone/pull repo -> cmake configure/build -> restart node in tmux.
- Node process runs inside tmux session `craq_node_<port>`.
- Current `vm_setup.bash` host list controls which VMs are targeted.

### 3) Check that node processes are running (tmux)

On a VM:

```bash
tmux ls
tmux attach -t craq_node_5001
```

Detach from tmux without killing process: `Ctrl+b`, then `d`.

### 4) Configure chain roles/order after nodes are up

Run from `crown/craq` (do not use `sudo`):

```bash
bash ./scripts/vm_cluster.sh configure --map ./configs/vm_hosts.sample.csv
```

### 5) Check cluster status

```bash
bash ./scripts/vm_cluster.sh status --map ./configs/vm_hosts.sample.csv
```

If your remote code path differs, pass it explicitly:

```bash
bash ./scripts/vm_cluster.sh status \
  --map ./configs/vm_hosts.sample.csv \
  --remote-dir /home/sagarj2/crown/craq
```

### 6) Write test (head write + tail ACK)

Run from a controller machine where `craq_leader` exists:

```bash
./build/craq_leader write \
  --head sp26-cs525-1201.cs.illinois.edu:5001 \
  --key k1 --value v1 \
  --leader-host <CONTROLLER_VM_IP> \
  --ack-port 7000
```

Get controller IP:

```bash
hostname -I | awk '{print $1}'
```

Important:

- Use `--leader-host` (not `--client-host`).
- Use a reachable IP for `--leader-host` so tail can connect back for ACK.

### 7) Read test from a non-tail node

```bash
./build/craq_leader read --node sp26-cs525-1203.cs.illinois.edu:5001 --key k1
```

### 8) Dump node state

```bash
./build/craq_leader dump --node sp26-cs525-1203.cs.illinois.edu:5001
```

### 9) Stop nodes

From `crown/setup`:

```bash
./vm_setup.bash kill
```

Or from `crown/craq`:

```bash
bash ./scripts/vm_cluster.sh down --map ./configs/vm_hosts.sample.csv
```

### Common gotchas

- Do not run `vm_cluster.sh` with `sudo`.
- Ensure mapping file contains only nodes that are actually running, or reads may fail at tail lookup.
- If multiline commands use `\`, keep `\` as the last character on the line (no trailing spaces).

## Cases

### Case: Change head and tail (example: 1203 as head, 1201 as tail)

Head and tail are decided by row order in `configs/vm_hosts.sample.csv`.

To make `1203` head and `1201` tail:

1. Open `configs/vm_hosts.sample.csv`.
2. Move the row for `1203` to the top (first node row after header).
3. Move the row for `1201` to the bottom (last node row).
4. Keep all other rows in any middle order you want.
5. Re-run configure (no rebuild needed):

```bash
bash ./scripts/vm_cluster.sh configure --map ./configs/vm_hosts.sample.csv
```

6. Verify chain status:

```bash
bash ./scripts/vm_cluster.sh status --map ./configs/vm_hosts.sample.csv
```

Example first/last rows:

```csv
n3,sagarj2,sp26-cs525-1203.cs.illinois.edu,22,sp26-cs525-1203.cs.illinois.edu,5001
n1,sagarj2,sp26-cs525-1201.cs.illinois.edu,22,sp26-cs525-1201.cs.illinois.edu,5001
```

That is all you need: reorder map rows and run configure again.

### Case: `write --head` must match configured head

After `configure`, the first row in `configs/vm_hosts.sample.csv` is the configured head.
When running write, `--head` should point to that same node.

If `--head` points to a different node than the configured head, writes may fail or be rejected.

Checklist:

1. Run status to confirm current chain:

```bash
bash ./scripts/vm_cluster.sh status --map ./configs/vm_hosts.sample.csv
```

2. Use the reported/configured head in write command:

```bash
./build/craq_leader write \
  --head sp26-cs525-1203.cs.illinois.edu:5001 \
  --key k1 --value v1 \
  --leader-host <CONTROLLER_VM_IP> \
  --ack-port 7000
```

3. If you changed row order, re-run configure before writing:

```bash
bash ./scripts/vm_cluster.sh configure --map ./configs/vm_hosts.sample.csv
```
