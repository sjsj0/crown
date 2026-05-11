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

`for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; p=50051; ssh <user>@$h "cd /path/to/crown && nohup ./build/server --port $p --server-log true > server_$p.log 2>&1 &"; done`

## 3) Optional connectivity check

`for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; p=50051; nc -vz $h $p; done`

## 4) Start the metadata server (configures all nodes + monitors liveness)

Pick one mode config (example: CROWN). Run this on ONE VM (e.g. `1201`):

`./build/metadata_server --config build/prod_configs/config.crown.json --host 0.0.0.0 --port 50050 --log`

It pushes each node its `NodeConfig` via the `Configure` RPC, then keeps running:
it pings every node and logs a node DOWN after 3 missed acks, and serves
`MetadataStore.GetCluster`. (Or use `./setup/vm_setup.bash start-metadata`, which
runs it on `$METADATA_HOST` from `setup/.env`.)

## 5) Start the client (topology comes from the metadata server)

`./build/client sp26-cs525-1201.cs.illinois.edu:50050 61000`

(First positional = metadata `host:port`; second = ack-listener port, default 60000.)

## 6) Use interactive commands

`write user:1 hello`

`read user:1`

`help`

`quit`

## 7) Switch mode by restarting the metadata server with a different config

Stop the current `metadata_server`, then e.g. for CHAIN:

`./build/metadata_server --config build/prod_configs/config.chain.json --host 0.0.0.0 --port 50050 --log`

The client picks up the new mode automatically on its next start (it reads
`mode` from `MetadataStore.GetCluster`).

## 8) Stop servers + metadata on prod VMs

`for i in 1 2 3 4 5; do h="sp26-cs525-120${i}.cs.illinois.edu"; ssh <user>@$h "pkill -f 'build/metadata_server' ; pkill -f 'build/server --port' || true"; done`

(or `./setup/vm_setup.bash kill`)

## 9) Run distributed throughput tests (single or multiple client VMs)

Run from your controller VM (or from one of the client VMs); the metadata server
must already be running (step 4).

`cd /home/crown`

### Single-client run (one client VM)

`python3 setup/run_throughput_experiments.py --hosts sp26-cs525-1218.cs.illinois.edu --ssh-user ritwikg3 --remote-repo-dir /home/crown --metadata sp26-cs525-1201.cs.illinois.edu:50050 --modes crown --ops write read --write-op-count 5000 --read-op-count 5000 --key-count 64 --work-dir build/prod_throughput_single_client`

### Multi-client simultaneous run (one client process per VM in client_hosts.csv)

`python3 setup/run_throughput_experiments.py --ssh-user ritwikg3 --remote-repo-dir /home/crown --metadata sp26-cs525-1201.cs.illinois.edu:50050 --modes crown --ops write read --write-op-count 50000 --read-op-count 50000 --key-count 64 --work-dir build/prod_throughput_multi_client`

Behavior:

- Launches exactly one client process per host in `--hosts` (defaults to `setup/client_hosts.csv`).
- Auto-assigns `client_index=0..N-1` in host order.
- Each client fetches topology from `--metadata` (no config files on the client side).
- `--modes` is only a label here — the actual mode is whatever the metadata server is running. Run one mode at a time.
- Collects remote logs to local `--work-dir/logs`, SSH logs to `--work-dir/ssh_logs`, aggregate summary to `--work-dir/summary.csv`.

## Notes

- NetID in examples above is set to `ritwikg3`.
- Replace `/path/to/crown` with the repo path on each VM.
- Keep `node_count`, host list, and base port consistent.
- For prod configs, all nodes use the same base port.
- If you change node count, regenerate configs and restart the metadata server.
