# crown

A C++ implementation of three chain replication variants:
- **Chain Replication** — classic head/tail linear chain
- **CRAQ** (Chain Replication with Apportioned Queries) — reads from any node with version consistency
- **CROWN** — circular topology where key ownership is computed by `hash(key) % node_count`

Inter-node communication uses gRPC and Protocol Buffers. Nodes start config-agnostic; a standalone **`metadata_server`** is the single source of truth for cluster topology. It reads `config.json`, validates it, pushes each node its `NodeConfig` via the `Configure` RPC, runs a **ping-ack failure detector** (it pings every node on an interval; after N missed acks a node is logged as DOWN), and serves `MetadataStore.GetCluster`. The `client` binary fetches its routing tables from `MetadataStore.GetCluster` — it never reads config files.

For CHAIN and CROWN in this version, `WriteResponse` means the head accepted the write (assigned a version), not that the write is already committed. Commit happens later when the tail commits and sends `Ack` directly to the client.

This prototype intentionally avoids mutexes/condition variables in replication helpers and assumes serialized writes from a single active client.

---

## Prerequisites

Install the following before building:

**macOS (Homebrew):**
```bash
brew install cmake grpc nlohmann-json
```

**Ubuntu/Debian:**
```bash
sudo apt install cmake libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc nlohmann-json3-dev
```

---

## Clone

```bash
git clone https://github.com/sjsj0/crown.git
cd crown
```

---

## Build

CMake handles proto generation and compilation in one step.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

This will:
1. Run `protoc` to generate `chain.pb.h/cc` and `chain.grpc.pb.h/cc` from `proto/chain.proto`
2. Compile `server.cpp`, `metadata_server.cpp`, and `client.cpp`
3. Output binaries at `build/server`, `build/metadata_server`, and `build/client`

To symlink `compile_commands.json` for clangd/IDE support:
```bash
ln -s build/compile_commands.json compile_commands.json
```

---

## Running

### 1. Start node servers

Each node is a server process that starts config-agnostic and waits for the metadata server to push its topology. Start one process per node, each on a different port:

```bash
./build/server --port 50051 &
./build/server --port 50052 &
./build/server --port 50053 &
```

### 2. Start the metadata server

Write a `config.json` describing the chain (see format below), then start the metadata server. It validates the config, pushes each node its `NodeConfig` via the `Configure` RPC, then keeps running: it pings every node (`--ping-interval-ms`, default 1000) and logs a node DOWN after `--failure-threshold` (default 3) missed acks, and serves `MetadataStore.GetCluster`.

```bash
./build/metadata_server --config config.json --host 0.0.0.0 --port 50050 --log
```

Useful flags: `--ping-interval-ms`, `--ping-timeout-ms`, `--failure-threshold`. The metadata server is the only component that reads `config.json`.

### Generate mode configs automatically

Use the helper script to generate CHAIN, CRAQ, and CROWN configs together:

```bash
python3 setup/generate_mode_configs.py <node_count> --base-port <port> --env <dev|prod> --output-dir <dir>
```

Example:

```bash
python3 setup/generate_mode_configs.py 3 --base-port 50051 --env prod --output-dir configs
```

This writes:

- `configs/config.chain.json`
- `configs/config.craq.json`
- `configs/config.crown.json`

To generate only CROWN config from the same script:

```bash
python3 setup/generate_mode_configs.py 3 --base-port 50051 --env dev --output-dir . --prefix config --modes crown
```

Host generation rules:

- `--env dev`: all node hosts are `127.0.0.1`
- `--env prod`: node hosts are `sp26-cs525-1201.cs.illinois.edu`, `sp26-cs525-1202.cs.illinois.edu`, ...

Port generation rules:

- `--env dev`: ports increment per node from `--base-port` (e.g., 50051, 50052, ...)
- `--env prod`: all nodes use the same `--base-port` (e.g., 50051 on each host)

You can optionally pass `--host <value>` to override and use one host for all generated nodes.

A checked-in 3-node sample is provided at `config.crown.sample.json`.

### 3. Run the workload client

The `client` binary takes the metadata server's `host:port` as its first argument; it calls `MetadataStore.GetCluster` to learn the topology, then drops into an interactive `read`/`write` REPL (or runs a benchmark):

```bash
./build/client localhost:50050
# then type:
#   write user:1 hello
#   read  user:1
#   quit
```

Pipe commands on stdin for one-shot use:

```bash
printf 'write user:1 hello\nread user:1\n' | ./build/client localhost:50050
```

Benchmark mode:

```bash
./build/client localhost:50050 61000 bench-write 5000 64 0 1
./build/client localhost:50050 61000 bench-read  5000 64 0 1
```

(The optional second positional is the ack-listener port; default `60000`.)

Routing the client applies (using the topology from the metadata server):

- CHAIN: writes -> global head, reads -> global tail.
- CROWN: `head_index = hash(key) % N`; writes -> node with `id=head_index`; reads -> predecessor of that head.
- CRAQ: currently writes -> head, reads -> tail (temporary baseline).

For `write`, `WriteResponse.version` is the head-assigned version number for the accepted write. It is not by itself proof of commit.

### 4. Run practical CROWN smoke test

Use the smoke-test harness to validate a local 3-node CROWN ring end-to-end:

```bash
bash setup/crown_smoke_test.sh
```

What it checks:

- launches 3 local servers and a `metadata_server` that pushes a generated CROWN config
- writes keys mapped to different heads and reads from corresponding tails (via the `client` REPL)
- same key always routes to the same head/tail
- different keys distribute across different heads/tails
- ring wrap-around path works
- concurrent writes to the same key produce strictly increasing contiguous versions
- cleans up server + metadata processes on exit

### CHAIN/CROWN write and commit semantics in this repo

- Only the write head for a key accepts client `Write` RPCs.
- The head assigns a monotonic per-key version and records that version as dirty.
- The head immediately returns `WriteResponse { success=true, version=... }` to indicate accepted-by-head.
- Dirty versions propagate node-by-node using `Propagate` (CHAIN: successor along the chain, CROWN: clockwise toward the key tail).
- The key tail marks the version clean/committed and sends `Ack` directly to the client.
- CHAIN and CROWN do not use inter-node `Ack` propagation in this version. CRAQ still uses upstream inter-node `Ack` propagation.
- Only the read tail for a key serves client `Read` RPCs (CHAIN: configured tail, CROWN: key tail).
- Reads return the latest clean/committed value/version only.
- Client writes sent to non-head nodes fail with a clear error.
- Client reads sent to non-tail nodes fail with a clear error.

---

## Config file format

```json
{
  "mode": "chain",
  "nodes": [
    {
      "id":          0,
      "host":        "127.0.0.1",
      "port":        50051,
      "is_head":     true,
      "is_tail":     false,
      "predecessor": null,
      "successor":   "127.0.0.1:50052"
    },
    {
      "id":          1,
      "host":        "127.0.0.1",
      "port":        50052,
      "is_head":     false,
      "is_tail":     false,
      "predecessor": "127.0.0.1:50051",
      "successor":   "127.0.0.1:50053"
    },
    {
      "id":          2,
      "host":        "127.0.0.1",
      "port":        50053,
      "is_head":     false,
      "is_tail":     true,
      "predecessor": "127.0.0.1:50052",
      "successor":   null
    }
  ]
}
```

`mode` can be `"chain"`, `"craq"`, or `"crown"`.

CROWN mapping in this repo:

- `head_index(key) = fnv1a64(key) % N`, where `N = number of crown nodes`.
- `head(key)` = node with `id == head_index(key)`.
- `tail(key)` = predecessor(`head(key)`) in the configured ring.
- CROWN node ids must be unique and contiguous in `[0, N-1]`.

Valid CROWN sample (`config.crown.sample.json`):

```json
{
  "mode": "crown",
  "nodes": [
    {
      "id": 0,
      "host": "127.0.0.1",
      "port": 50051,
      "is_head": false,
      "is_tail": false,
      "predecessor": "127.0.0.1:50053",
      "successor": "127.0.0.1:50052"
    },
    {
      "id": 1,
      "host": "127.0.0.1",
      "port": 50052,
      "is_head": false,
      "is_tail": false,
      "predecessor": "127.0.0.1:50051",
      "successor": "127.0.0.1:50053"
    },
    {
      "id": 2,
      "host": "127.0.0.1",
      "port": 50053,
      "is_head": false,
      "is_tail": false,
      "predecessor": "127.0.0.1:50052",
      "successor": "127.0.0.1:50051"
    }
  ]
}
```

---

## Project structure

```
crown/
├── proto/
│   └── chain.proto                         # gRPC service + message definitions
├── src/
│   ├── node/
│   │   └── node.cpp                        # Node identity and topology (no store)
│   ├── server/
│   │   └── server.cpp                      # Node entry point: gRPC service, RPC handlers (incl. Ping)
│   ├── metadata/
│   │   └── metadata_server.cpp             # Reads config.json, configures nodes, ping-ack detector, GetCluster
│   ├── client/
│   │   └── client.cpp                      # Benchmark / interactive client; topology from MetadataStore.GetCluster
│   └── replication/
│       ├── replication_strategy.h          # Abstract interface for all strategies
│       ├── replication_strategy.cpp
│       ├── chain/
|       │   └── chain_replication.h 
│       │   └── chain_replication.cpp
│       ├── craq/
│       │   └── craq_replication.h
│       │   └── craq_replication.cpp
│       └── crown/
│           └── crown_replication.h
│           └── crown_replication.cpp
└── CMakeLists.txt
```

---

## Recompiling after changes

```bash
cmake --build build
```

Only changed files are recompiled. Re-run the full `cmake -S . -B build` step only if you modify `CMakeLists.txt` or `chain.proto`.
