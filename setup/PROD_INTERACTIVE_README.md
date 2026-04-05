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

## 9) Run distributed write benchmark (one client per VM)

Step 4 command to run from a controller VM:

`cd /home/ritwikg3/crown && bash ./setup/run_prod_distributed_write_bench.bash --hosts "$(cat setup/prod_hosts.csv)" --ssh-user ritwikg3 --remote-repo-dir /home/ritwikg3/crown --config-path build/prod_configs/config.crown.json --write-op-count 50000 --key-count 64 --ack-port 61000`

The hosts list is read from `setup/prod_hosts.csv` in this repo.

Alternative host list format is still supported:

`cp setup/prod_hosts.sample.txt setup/prod_hosts.txt && bash ./setup/run_prod_distributed_write_bench.bash --hosts-file setup/prod_hosts.txt --ssh-user ritwikg3 --remote-repo-dir /home/ritwikg3/crown --config-path build/prod_configs/config.crown.json --write-op-count 50000 --key-count 64`

Behavior:

- SSHes to each host and launches one `bench-write` client per host.
- Assigns `client_index` automatically (`0..N-1`) based on host order.
- Uses `configure=true` only on the first host/client; others run with `configure=false`.
- Waits for all clients to finish and reports per-host success/failure.
- Writes remote client logs under `build/prod_bench_logs` on each VM.
- Writes local SSH launcher logs under `build/prod_ssh_launcher_logs`.

## Notes

- NetID in examples above is set to `ritwikg3`.
- Replace `/path/to/crown` with the repo path on each VM.
- Keep `node_count`, host list, and base port consistent.
- For prod configs, all nodes use the same base port.
- If you change node count, regenerate configs and update loop ranges accordingly.
