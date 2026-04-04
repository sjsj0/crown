# crown

A C++ implementation of three chain replication variants:
- **Chain Replication** — classic head/tail linear chain
- **CRAQ** (Chain Replication with Apportioned Queries) — reads from any node with version consistency
- **CROWN** — circular topology where key ownership is computed by `hash(key) % node_count`

Inter-node communication uses gRPC and Protocol Buffers. Topology config is pushed to nodes at runtime by a client — nodes themselves are config-agnostic at startup.

For CHAIN and CROWN in this version, `WriteResponse` means the head accepted the write (assigned a version), not that the write is already committed. Commit happens later when `Ack` returns from the tail.

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
2. Compile `server.cpp`, `client.cpp`, and `kv_client.cpp`
3. Output binaries at `build/server`, `build/client`, and `build/kv_client`

To symlink `compile_commands.json` for clangd/IDE support:
```bash
ln -s build/compile_commands.json compile_commands.json
```

---

## Running

### 1. Start node servers

Each node is a server process that starts config-agnostic and waits for the client to push its topology. Start one process per node, each on a different port:

```bash
./build/server --port 50051 &
./build/server --port 50052 &
./build/server --port 50053 &
```

### 2. Push topology config

Write a `config.json` describing the chain (see format below), then run the client to configure all nodes:

```bash
./build/client config.json
```

The client loops through every node in the config file and sends each one its `NodeConfig` via the `Configure` RPC.

`src/client/client.cpp` is intentionally limited to topology/config push and was not changed for replication write/commit behavior.

### Generate a CROWN ring config automatically

Use the helper script to generate a valid closed CROWN ring config:

```bash
python3 setup/generate_crown_config.py <node_count> <base_port> --output config.crown.sample.json
```

Example (3 nodes, ports 50051-50053):

```bash
python3 setup/generate_crown_config.py 3 50051 --output config.crown.sample.json
```

A checked-in 3-node sample is provided at `config.crown.sample.json`.

### 3. Run key/value workload client

`kv_client` uses the same `config.json` for routing and then sends actual `Write` / `Read` RPCs.

Write:

```bash
./build/kv_client write config.json user:1 hello
```

Read:

```bash
./build/kv_client read config.json user:1
```

Routing rules used by `kv_client`:

- CHAIN: writes -> global head, reads -> global tail.
- CROWN: `head_index = hash(key) % N`; writes -> node with `id=head_index`; reads -> predecessor of that head.
- CRAQ: currently writes -> head, reads -> tail (temporary baseline).

The client prints the contacted node endpoint and returned version.

For `write`, `WriteResponse.version` is the head-assigned version number for the accepted write. It is not by itself proof of commit.

### 4. Run practical CROWN smoke test

Use the smoke-test harness to validate a local 3-node CROWN ring end-to-end:

```bash
bash setup/crown_smoke_test.sh
```

What it checks:

- launches 3 local servers and pushes a generated CROWN config
- writes keys mapped to different heads and reads from corresponding tails
- same key always routes to the same head/tail
- different keys distribute across different heads/tails
- wrong-node write/read requests fail with clear errors
- ring wrap-around path works
- concurrent writes to the same key produce strictly increasing contiguous versions
- cleans up server processes on exit

### CHAIN/CROWN write and commit semantics in this repo

- Only the write head for a key accepts client `Write` RPCs.
- The head assigns a monotonic per-key version and records that version as dirty.
- The head immediately returns `WriteResponse { success=true, version=... }` to indicate accepted-by-head.
- Dirty versions propagate node-by-node using `Propagate` (CHAIN: successor along the chain, CROWN: clockwise toward the key tail).
- The key tail marks the version clean/committed and initiates upstream `Ack`.
- Upstream nodes mark clean on `Ack`; the key head logs final commit when `Ack` arrives.
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
│   │   └── server.cpp                      # Entry point, gRPC service, RPC handlers
│   ├── client/
│   │   └── client.cpp                      # Reads config.json, configures all nodes
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
