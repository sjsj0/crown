# crown

A C++ implementation of three chain replication variants:
- **Chain Replication** — classic head/tail linear chain
- **CRAQ** (Chain Replication with Apportioned Queries) — reads from any node with version consistency
- **CROWN** — circular topology where each node acts as head/tail based on key partitioning

Inter-node communication uses gRPC and Protocol Buffers. Topology config is pushed to nodes at runtime by a client — nodes themselves are config-agnostic at startup.

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
2. Compile `server.cpp` and `client.cpp`
3. Output binaries at `build/server` and `build/client`

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

For crown mode, each node entry also accepts `head_ranges` and `tail_ranges`:
```json
"head_ranges": [{ "start": "a", "end": "m" }],
"tail_ranges": [{ "start": "n", "end": "z" }]
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
