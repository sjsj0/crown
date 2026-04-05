# Prod Interactive Client Quickstart

This guide shows how to run the client in interactive mode against production hosts.

## 1) Generate prod configs

From the repo root:

`python3 setup/generate_mode_configs.py 5 --base-port 50051 --env prod --output-dir build/prod_configs --prefix config`

This creates:

- `build/prod_configs/config.chain.json`
- `build/prod_configs/config.craq.json`
- `build/prod_configs/config.crown.json`

## 2) Start one server per prod VM (shared port)

Example for 5 nodes (`1201`..`1205`), all listening on `50051`:

`for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; p=50051; ssh <user>@$h "cd /path/to/crown && nohup ./build/server --port $p > server_$p.log 2>&1 &"; done`

## 3) Optional connectivity check

`for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; p=50051; nc -vz $h $p; done`

## 4) Start client interactive loop (first run: configure=true)

Pick one mode config (example: CROWN):

`./build/client build/prod_configs/config.crown.json true 61000`

## 5) Use interactive commands

`write user:1 hello`

`read user:1`

`help`

`quit`

## 6) Start interactive loop again without reconfiguring

`./build/client build/prod_configs/config.crown.json false 61000`

## 7) Switch mode by changing config file

CHAIN:

`./build/client build/prod_configs/config.chain.json true 61000`

CRAQ:

`./build/client build/prod_configs/config.craq.json true 61000`

## 8) Stop servers on prod VMs

`for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; ssh <user>@$h "pkill -f './build/server --port' || true"; done`

## 9) Run distributed throughput tests (single or multiple client VMs)

Run from your controller VM (or from one of the client VMs):

`cd /home/crown`

### Single-client run (one client VM)

`python3 setup/run_throughput_experiments.py --hosts sp26-cs525-1201.cs.illinois.edu --ssh-user ritwikg3 --remote-repo-dir /home/crown --modes chain craq crown --ops write read --write-op-count 5000 --read-op-count 5000 --key-count 64 --work-dir build/prod_throughput_single_client`

### Multi-client simultaneous run (one client process per VM)

`python3 setup/run_throughput_experiments.py --hosts "$(cat setup/prod_hosts.csv)" --ssh-user ritwikg3 --remote-repo-dir /home/crown --modes chain craq crown --ops write read --write-op-count 50000 --read-op-count 50000 --key-count 64 --work-dir build/prod_throughput_multi_client`

Behavior:

- Launches exactly one client process per host in `--hosts`.
- Auto-assigns `client_index=0..N-1` in host order.
- Uses `configure=true` only on the first client in each mode/op case.
- Collects remote logs to local `--work-dir/logs`.
- Writes SSH execution logs to local `--work-dir/ssh_logs`.
- Writes aggregate summary to local `--work-dir/summary.csv`.

## Notes

- NetID in examples above is set to `ritwikg3`.
- Replace `/path/to/crown` with the repo path on each VM.
- Keep `node_count`, host list, and base port consistent.
- For prod configs, all nodes use the same base port.
- If you change node count, regenerate configs and update loop ranges accordingly.
