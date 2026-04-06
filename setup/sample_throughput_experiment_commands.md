# Sample Distributed Throughput Commands

## Run From Repo Root

```bash
cd "/mnt/d/UIUC/Spring '26/CS 525 - Advanced Distributed System/crown"
```

## Single-Client (One Client Machine)

Runs one client process on one host, executes write+read for all modes.

```bash
python3 setup/run_throughput_experiments.py \
  --hosts sp26-cs525-1201.cs.illinois.edu \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --modes chain craq crown \
  --ops write read \
  --write-op-count 5000 \
  --read-op-count 5000 \
  --key-count 64 \
  --work-dir build/prod_throughput_single_client
```

## Multi-Client Simultaneous (One Process Per Client Machine)

Reads hosts from `setup/prod_hosts.csv` and launches all client machines together.

```bash
python3 setup/run_throughput_experiments.py \
  --hosts "$(cat setup/prod_hosts.csv)" \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --modes chain craq crown \
  --ops write read \
  --write-op-count 50000 \
  --read-op-count 50000 \
  --key-count 64 \
  --work-dir build/prod_throughput_multi_client
```

## Mode-Specific Runs

### CHAIN only

```bash
python3 setup/run_throughput_experiments.py \
  --hosts "$(cat setup/prod_hosts.csv)" \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --modes chain \
  --ops write read \
  --write-op-count 50000 \
  --read-op-count 50000 \
  --key-count 64 \
  --work-dir build/prod_chain_only
```

### CRAQ only

```bash
python3 setup/run_throughput_experiments.py \
  --hosts "$(cat setup/prod_hosts.csv)" \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --modes craq \
  --ops write read \
  --write-op-count 50000 \
  --read-op-count 50000 \
  --key-count 64 \
  --craq-read-node-id -1 \
  --work-dir build/prod_craq_only
```

### CROWN only

```bash
python3 setup/run_throughput_experiments.py \
  --hosts "$(cat setup/prod_hosts.csv)" \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --modes crown \
  --ops write read \
  --write-op-count 50000 \
  --read-op-count 50000 \
  --key-count 64 \
  --work-dir build/prod_crown_only
```

### CROWN hot-head write skew (example: 60% to one head)

```bash
python3 setup/run_throughput_experiments.py \
  --hosts "$(cat setup/prod_hosts.csv)" \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --modes crown \
  --ops write \
  --write-op-count 50000 \
  --key-count 64 \
  --crown-hot-head-pct 60 \
  --work-dir build/prod_crown_hot_60
```

### Read hot-key skew (example: 80% to one key)

```bash
python3 setup/run_throughput_experiments.py \
  --hosts "$(cat setup/prod_hosts.csv)" \
  --ssh-user ritwikg3 \
  --remote-repo-dir /home/crown \
  --modes chain craq crown \
  --ops read \
  --read-op-count 50000 \
  --key-count 64 \
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

- `--hosts`: Comma-separated client host list. One client process is launched per host.
- `--hosts-file`: Host file alternative (supports CSV or one-host-per-line).
- `--modes`: Replication modes to run (`chain`, `craq`, `crown`).
- `--ops`: Workload types to run (`write`, `read`).
- `--write-op-count`: Total distributed write operations per write case.
- `--read-op-count`: Total distributed read operations per read case.
- `--key-count`: Number of keys in keyspace.
- `--crown-hot-head-pct`: For CROWN write runs, percentage of writes targeted to one head node (`0` to `100`).
- `--read-hot-key-pct`: For read runs, percentage of reads targeted to one hot key (`0` to `100`).
- `--remote-config-template`: Remote config path template (default: `build/prod_configs/config.{mode}.json`).
- `--work-dir`: Local output directory for logs and summary.
- `--dry-run`: Print planned SSH/SCP commands without executing.
