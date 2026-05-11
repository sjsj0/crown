# Sample Distributed Throughput Commands

The clients fetch their topology from the metadata server, so bring up the
cluster and the metadata server first, then run the benchmark from the
client-only VMs.

## 0) Run From Repo Root

```bash
cd "/mnt/d/UIUC/Spring '26/CS 525 - Advanced Distributed System/crown"
```

## 1) Bring up the cluster + metadata server

```bash
# `start` launches the server nodes on prod_hosts.csv AND the metadata_server on
# $METADATA_HOST (from setup/.env). Set METADATA_CONFIG to the mode you want to
# benchmark (the config must already exist on that VM):
METADATA_CONFIG=build/prod_configs/config.crown.json ./setup/vm_setup.bash start
```

Switch modes by restarting just the metadata server with a different
`METADATA_CONFIG` (no need to bounce the node servers):

```bash
METADATA_CONFIG=build/prod_configs/config.craq.json ./setup/vm_setup.bash start-metadata
```

## 2) Single-Client (One Client Machine)

One client process on one host, write+read against the running metadata server.

```bash
python3 setup/run_throughput_experiments.py \
  --hosts sp26-cs525-1218.cs.illinois.edu \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes crown \
  --ops write read \
  --write-op-count 5000 \
  --read-op-count 5000 \
  --key-count 64 \
  --work-dir build/prod_throughput_single_client
```

## 3) Multi-Client Simultaneous (One Process Per Client Machine)

Defaults `--hosts` to `setup/client_hosts.csv`; launches all client machines together.

```bash
python3 setup/run_throughput_experiments.py \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes crown \
  --ops write read \
  --write-op-count 50000 \
  --read-op-count 50000 \
  --key-count 64 \
  --work-dir build/prod_throughput_multi_client
```

## 4) Mode-Specific Runs

The metadata server defines the actual mode; `--modes` here only labels logs and
key prefixes. Restart `metadata_server` with the matching config before each run.

### CHAIN

```bash
# metadata server running config.chain.json, then:
python3 setup/run_throughput_experiments.py \
  --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes chain --ops write read \
  --write-op-count 50000 --read-op-count 50000 --key-count 64 \
  --work-dir build/prod_chain_only
```

### CRAQ

```bash
# metadata server running config.craq.json, then:
python3 setup/run_throughput_experiments.py \
  --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes craq --ops write read \
  --write-op-count 50000 --read-op-count 50000 --key-count 64 \
  --craq-read-node-id -1 \
  --work-dir build/prod_craq_only
```

### CROWN

```bash
# metadata server running config.crown.json, then:
python3 setup/run_throughput_experiments.py \
  --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes crown --ops write read \
  --write-op-count 50000 --read-op-count 50000 --key-count 64 \
  --work-dir build/prod_crown_only
```

### CROWN hot-head write skew (example: 60% to one head)

```bash
python3 setup/run_throughput_experiments.py \
  --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes crown --ops write \
  --write-op-count 50000 --key-count 64 \
  --crown-hot-head-pct 60 \
  --work-dir build/prod_crown_hot_60
```

### Read hot-key skew (example: 80% to one key)

```bash
python3 setup/run_throughput_experiments.py \
  --ssh-user ritwikg3 --remote-repo-dir /home/crown \
  --metadata sp26-cs525-1201.cs.illinois.edu:50050 \
  --modes crown --ops read \
  --read-op-count 50000 --key-count 64 \
  --read-hot-key-pct 80 \
  --work-dir build/prod_read_hot_80
```

## Expected Outputs

For any `--work-dir <DIR>`:

```text
<DIR>/logs/*.log
<DIR>/ssh_logs/*.log
<DIR>/summary.csv
```

## Key Arguments

- `--metadata`: metadata_server endpoint `host:port` the clients fetch topology from (default: `$METADATA_HOST:$METADATA_PORT`). The metadata server must already be running.
- `--hosts`: Comma-separated client host list. One client process per host. Defaults to `setup/client_hosts.csv`.
- `--hosts-file`: Host file alternative (CSV or one-host-per-line).
- `--modes`: Label for logs/key-prefixes (`chain`, `craq`, `crown`). The real mode is whatever the metadata server runs — use one mode at a time.
- `--ops`: Workload types to run (`write`, `read`).
- `--write-op-count` / `--read-op-count`: Total distributed operations per write/read case.
- `--key-count`: Number of keys in keyspace.
- `--crown-hot-head-pct`: For CROWN write runs, percentage of writes targeted to one head node (`0`–`100`).
- `--read-hot-key-pct`: For read runs, percentage of reads targeted to one hot key (`0`–`100`).
- `--work-dir`: Local output directory for logs and summary.
- `--dry-run`: Print planned SSH/SCP commands without executing.
