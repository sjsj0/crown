# Sample Throughput Experiment Commands

## Run From Repo Root

```bash
cd "/mnt/d/UIUC/Spring '26/CS 525 - Advanced Distributed System/crown"
```

## Optional One-Time Build Step

```bash
cmake -S . -B build
cmake --build build -j4
```

## Common Argument Block

Included in each command below:

```text
--client-counts 1 2 4 8 --write-op-count 5000 --read-op-count 5000 --key-count 64 --start-servers true --build-first false --reconfigure-each-run true
```

## 1) CHAIN: write + read experiments

```bash
./setup/run_throughput_experiments.py --modes chain --ops write read --work-dir build/throughput_runs_chain --client-counts 1 2 4 8 --write-op-count 5000 --read-op-count 5000 --key-count 64 --start-servers true --build-first false --reconfigure-each-run true
```

## 2) CRAQ: write + read experiments

```bash
./setup/run_throughput_experiments.py --modes craq --ops write read --work-dir build/throughput_runs_craq --client-counts 1 2 4 8 --write-op-count 5000 --read-op-count 5000 --key-count 64 --start-servers true --build-first false --reconfigure-each-run true
```

## 3) CROWN: write + read experiments

```bash
./setup/run_throughput_experiments.py --modes crown --ops write read --work-dir build/throughput_runs_crown --client-counts 1 2 4 8 --write-op-count 5000 --read-op-count 5000 --key-count 64 --start-servers true --build-first false --reconfigure-each-run true
```

## 4) All Modes In One Run: CHAIN + CRAQ + CROWN, write + read

```bash
./setup/run_throughput_experiments.py --modes chain craq crown --ops write read --work-dir build/throughput_runs_all_modes --client-counts 1 2 4 8 --write-op-count 5000 --read-op-count 5000 --key-count 64 --start-servers true --build-first false --reconfigure-each-run true
```

## Expected Summary CSV Outputs

```text
build/throughput_runs_chain/summary.csv
build/throughput_runs_craq/summary.csv
build/throughput_runs_crown/summary.csv
build/throughput_runs_all_modes/summary.csv
```

## Parameter Meanings

### --modes
Which replication mode(s) to run. Allowed values: chain, craq, crown.

### --ops
Which operation workload(s) to run. Allowed values: write, read.

### --work-dir
Output directory for that run. Contains generated configs, per-client logs, and summary.csv.

### --client-counts
How many client processes to launch concurrently for each experiment case.
Example: 1 2 4 8 means run four separate cases (1-client, 2-client, 4-client, 8-client).

### --write-op-count
Total number of write operations to issue in each write case, across all clients.

### --read-op-count
Total number of read operations to issue in each read case, across all clients.

### --key-count
Number of distinct keys used in the workload keyspace.

Bench requests are sent as fast as possible (no client-side pacing).

### --start-servers
Whether the runner should automatically start server processes.
true means start servers automatically; false means you must start them yourself.

### --build-first
Whether to build the project before running experiments.
true runs cmake build first; false skips build.

### --reconfigure-each-run
Whether the first client in each case should send Configure RPCs before workload starts.
true is usually recommended to ensure fresh/consistent server configuration per case.

## Boolean Notes

For --start-servers, --build-first, and --reconfigure-each-run, accepted values are true/false (also 1/0).
