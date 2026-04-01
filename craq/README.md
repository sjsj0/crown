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

## One-Click Multi-VM Cluster (3+ Nodes)

Use `scripts/vm_cluster.sh` for remote orchestration with host mapping.

### 1) Prepare mapping

Copy and edit `configs/vm_hosts.sample.csv`:

```txt
node_id,ssh_user,ssh_host,ssh_port,node_host,node_port
n1,sagarj2,sp26-cs525-1201.cs.illinois.edu,22,sp26-cs525-1201.cs.illinois.edu,5001
n2,sagarj2,sp26-cs525-1202.cs.illinois.edu,22,sp26-cs525-1202.cs.illinois.edu,5001
n3,sagarj2,sp26-cs525-1203.cs.illinois.edu,22,sp26-cs525-1203.cs.illinois.edu,5001
```

You can extend this pattern through `sp26-cs525-1220.cs.illinois.edu`.

Field meanings:

- `node_id`: CRAQ node id.
- `ssh_user`, `ssh_host`, `ssh_port`: SSH login endpoint for that VM.
- `node_host`, `node_port`: address other CRAQ nodes should use for replication.

### 2) Bring cluster up (one command)

```bash
./scripts/vm_cluster.sh up --map ./configs/vm_hosts.sample.csv
```

What `up` does:

- Syncs `CMakeLists.txt`, `include/`, `src/`, and `configs/` to each VM.
- Builds CRAQ on each VM.
- Starts each node with `nohup` and PID files under `<remote-dir>/run/`.
- Generates a runtime chain config from the mapping order.
- Configures the cluster using `craq_leader` on the first mapped VM.
- Prints node dumps for quick validation.

### 3) Check status

```bash
./scripts/vm_cluster.sh status --map ./configs/vm_hosts.sample.csv
```

### 4) Reconfigure only

```bash
./scripts/vm_cluster.sh configure --map ./configs/vm_hosts.sample.csv
```

### 5) Stop cluster

```bash
./scripts/vm_cluster.sh down --map ./configs/vm_hosts.sample.csv
```

### Optional flags

- `--remote-dir <path>`: remote CRAQ directory (default `~/craq`).
- `--bind-host <host>`: bind host for remote `craq_node` (default `0.0.0.0`).
- `--build-type <type>`: CMake build type on VMs (default `Release`).
- `--ssh-opt <opt>`: pass extra SSH option; repeat as needed.

Example with identity file:

```bash
./scripts/vm_cluster.sh up \
  --map ./configs/vm_hosts.sample.csv \
  --ssh-opt "-i ~/.ssh/my_vm_key"
```
