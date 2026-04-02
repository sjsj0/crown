#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
WORK_DIR="$BUILD_DIR/crown_smoke"

NODE_COUNT=3
BASE_PORT="${BASE_PORT:-50051}"
HOST="${HOST:-127.0.0.1}"

CONFIG="$WORK_DIR/config.crown.json"
WRONG_HEAD_CONFIG="$WORK_DIR/config.route_wrong_head.json"
WRONG_TAIL_CONFIG="$WORK_DIR/config.route_wrong_tail.json"

declare -a SERVER_PIDS=()

die() {
  echo "[crown-smoke] ERROR: $*" >&2
  exit 1
}

cleanup() {
  for pid in "${SERVER_PIDS[@]:-}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done

  for pid in "${SERVER_PIDS[@]:-}"; do
    if [[ -n "$pid" ]]; then
      wait "$pid" >/dev/null 2>&1 || true
    fi
  done
}
trap cleanup EXIT INT TERM

extract_field() {
  local line="$1"
  local key="$2"
  local value
  value="$(sed -n "s/.* ${key}=\([^ ]*\).*/\1/p" <<< "$line")"
  echo "$value"
}

kv_write() {
  local cfg="$1"
  local key="$2"
  local value="$3"
  "$BUILD_DIR/kv_client" write "$cfg" "$key" "$value"
}

kv_read() {
  local cfg="$1"
  local key="$2"
  "$BUILD_DIR/kv_client" read "$cfg" "$key"
}

kv_write_concurrent() {
  local cfg="$1"
  local key="$2"
  local value="$3"

  if command -v timeout >/dev/null 2>&1; then
    timeout 20 "$BUILD_DIR/kv_client" write "$cfg" "$key" "$value"
  else
    "$BUILD_DIR/kv_client" write "$cfg" "$key" "$value"
  fi
}

assert_eq() {
  local got="$1"
  local want="$2"
  local msg="$3"
  if [[ "$got" != "$want" ]]; then
    die "$msg (got='$got', want='$want')"
  fi
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  local msg="$3"
  if [[ "$haystack" != *"$needle"* ]]; then
    die "$msg (missing '$needle')"
  fi
}

echo "[crown-smoke] Building binaries..."
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j4 >/dev/null

[[ -x "$BUILD_DIR/server" ]] || die "missing binary: $BUILD_DIR/server"
[[ -x "$BUILD_DIR/client" ]] || die "missing binary: $BUILD_DIR/client"
[[ -x "$BUILD_DIR/kv_client" ]] || die "missing binary: $BUILD_DIR/kv_client"

mkdir -p "$WORK_DIR"

echo "[crown-smoke] Generating CROWN config..."
python3 "$ROOT_DIR/setup/generate_crown_config.py" "$NODE_COUNT" "$BASE_PORT" --host "$HOST" --output "$CONFIG" >/dev/null

# Export KEY_HEAD_<i>, WRAP_KEY, HEAD_ENDPOINT_<i>, TAIL_ENDPOINT_<i>, NODE_COUNT from config.
eval "$(python3 - "$CONFIG" <<'PY'
import json
import shlex
import sys

cfg = json.load(open(sys.argv[1], 'r', encoding='utf-8'))
nodes = cfg['nodes']

if len(nodes) == 0:
    raise SystemExit('no nodes in generated config')


def parse_token(text: str) -> int:
    return int(text, 16) if text.lower().startswith('0x') else int(text)


def token_in_range(token: int, start: int, end: int) -> bool:
    if start <= end:
        return start <= token <= end
    return token >= start or token <= end


def fnv1a64(key: str) -> int:
    h = 14695981039346656037
    for b in key.encode('utf-8'):
        h ^= b
        h = (h * 1099511628211) & ((1 << 64) - 1)
    return h


head_ranges = []
for i, node in enumerate(nodes):
    for r in node.get('head_ranges', []):
        head_ranges.append((i, parse_token(r['start']), parse_token(r['end'])))


def head_owner(key: str):
    token = fnv1a64(key)
    hits = [idx for idx, s, e in head_ranges if token_in_range(token, s, e)]
    if len(hits) != 1:
        return None
    return hits[0]


keys = {}
remaining = set(range(len(nodes)))
probe = 0
while remaining and probe < 2_000_000:
    k = f'smoke-key-{probe}'
    owner = head_owner(k)
    if owner is not None and owner in remaining:
        keys[owner] = k
        remaining.remove(owner)
    probe += 1

if remaining:
    raise SystemExit(f'failed to discover keys for head indexes: {sorted(remaining)}')

print(f"NODE_COUNT={len(nodes)}")
for i, node in enumerate(nodes):
    endpoint = f"{node['host']}:{node['port']}"
    pred = node['predecessor']
    print(f"HEAD_ENDPOINT_{i}={shlex.quote(endpoint)}")
    print(f"TAIL_ENDPOINT_{i}={shlex.quote(pred)}")
    print(f"KEY_HEAD_{i}={shlex.quote(keys[i])}")

print(f"WRAP_KEY={shlex.quote(keys[len(nodes)-1])}")
PY
)"

echo "[crown-smoke] Creating intentionally wrong route configs for negative tests..."
python3 - "$CONFIG" "$WRONG_HEAD_CONFIG" "$WRONG_TAIL_CONFIG" <<'PY'
import copy
import json
import sys

src, out_head, out_tail = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = json.load(open(src, 'r', encoding='utf-8'))
nodes = cfg['nodes']
n = len(nodes)

head_ranges = [copy.deepcopy(node.get('head_ranges', [])) for node in nodes]
tail_ranges = [copy.deepcopy(node.get('tail_ranges', [])) for node in nodes]

cfg_wrong_head = copy.deepcopy(cfg)
cfg_wrong_tail = copy.deepcopy(cfg)

for i in range(n):
    cfg_wrong_head['nodes'][i]['head_ranges'] = copy.deepcopy(head_ranges[(i + 1) % n])
    cfg_wrong_tail['nodes'][i]['tail_ranges'] = copy.deepcopy(tail_ranges[(i + 1) % n])

with open(out_head, 'w', encoding='utf-8') as f:
    json.dump(cfg_wrong_head, f, indent=2)
    f.write('\n')

with open(out_tail, 'w', encoding='utf-8') as f:
    json.dump(cfg_wrong_tail, f, indent=2)
    f.write('\n')
PY

echo "[crown-smoke] Launching $NODE_COUNT servers..."
for ((i = 0; i < NODE_COUNT; ++i)); do
  port=$((BASE_PORT + i))
  log_file="$WORK_DIR/server_${i}.log"

  if command -v stdbuf >/dev/null 2>&1; then
    stdbuf -oL -eL "$BUILD_DIR/server" --port "$port" >"$log_file" 2>&1 &
  else
    "$BUILD_DIR/server" --port "$port" >"$log_file" 2>&1 &
  fi

  SERVER_PIDS+=("$!")
done

sleep 1
for pid in "${SERVER_PIDS[@]}"; do
  kill -0 "$pid" >/dev/null 2>&1 || die "server process died during startup (pid=$pid)"
done

echo "[crown-smoke] Pushing generated topology..."
"$BUILD_DIR/client" "$CONFIG" >/dev/null

# 1) Same key always uses same head and tail.
echo "[crown-smoke] Verifying stable head/tail routing for one key..."
stable_key="$KEY_HEAD_0"
out_w1="$(kv_write "$CONFIG" "$stable_key" "stable-a")"
out_w2="$(kv_write "$CONFIG" "$stable_key" "stable-b")"

stable_head_1="$(extract_field "$out_w1" target)"
stable_head_2="$(extract_field "$out_w2" target)"
assert_eq "$stable_head_1" "$stable_head_2" "same key routed to different heads"
assert_eq "$stable_head_1" "$HEAD_ENDPOINT_0" "same key did not route to expected head"

stable_v1="$(extract_field "$out_w1" version)"
stable_v2="$(extract_field "$out_w2" version)"
(( stable_v2 > stable_v1 )) || die "same-key write versions are not increasing ($stable_v1, $stable_v2)"

out_r1="$(kv_read "$CONFIG" "$stable_key")"
out_r2="$(kv_read "$CONFIG" "$stable_key")"

stable_tail_1="$(extract_field "$out_r1" target)"
stable_tail_2="$(extract_field "$out_r2" target)"
assert_eq "$stable_tail_1" "$stable_tail_2" "same key routed to different tails on read"
assert_eq "$stable_tail_1" "$TAIL_ENDPOINT_0" "same key did not route to expected tail"

# 2) Different keys distribute across different heads/tails.
echo "[crown-smoke] Verifying key distribution across heads/tails..."
declare -A WRITE_TARGETS=()
declare -A READ_TARGETS=()

for ((i = 0; i < NODE_COUNT; ++i)); do
  key_var="KEY_HEAD_${i}"
  head_ep_var="HEAD_ENDPOINT_${i}"
  tail_ep_var="TAIL_ENDPOINT_${i}"

  key="${!key_var}"
  expected_head="${!head_ep_var}"
  expected_tail="${!tail_ep_var}"

  out_w="$(kv_write "$CONFIG" "$key" "dist-$i")"
  out_r="$(kv_read "$CONFIG" "$key")"

  w_target="$(extract_field "$out_w" target)"
  r_target="$(extract_field "$out_r" target)"

  assert_eq "$w_target" "$expected_head" "distribution write did not hit expected head for key '$key'"
  assert_eq "$r_target" "$expected_tail" "distribution read did not hit expected tail for key '$key'"

  WRITE_TARGETS["$w_target"]=1
  READ_TARGETS["$r_target"]=1
done

if (( ${#WRITE_TARGETS[@]} < 2 )); then
  die "different keys did not distribute across multiple heads"
fi
if (( ${#READ_TARGETS[@]} < 2 )); then
  die "different keys did not distribute across multiple tails"
fi

# 3) Wrong-node requests fail clearly.
echo "[crown-smoke] Verifying wrong-node write/read failures..."
if wrong_write_out="$("$BUILD_DIR/kv_client" write "$WRONG_HEAD_CONFIG" "$stable_key" "wrong-path" 2>&1)"; then
  die "wrong-node write unexpectedly succeeded"
fi
assert_contains "$wrong_write_out" "not key head" "wrong-node write error is not clear"

if wrong_read_out="$("$BUILD_DIR/kv_client" read "$WRONG_TAIL_CONFIG" "$stable_key" 2>&1)"; then
  die "wrong-node read unexpectedly succeeded"
fi
assert_contains "$wrong_read_out" "not key tail" "wrong-node read error is not clear"

# 4) Ring wrap-around case works.
echo "[crown-smoke] Verifying ring wrap-around path..."
wrap_out_w="$(kv_write "$CONFIG" "$WRAP_KEY" "wrap-value")"
wrap_out_r="$(kv_read "$CONFIG" "$WRAP_KEY")"

wrap_expected_head_var="HEAD_ENDPOINT_$((NODE_COUNT - 1))"
wrap_expected_tail_var="TAIL_ENDPOINT_$((NODE_COUNT - 1))"
wrap_expected_head="${!wrap_expected_head_var}"
wrap_expected_tail="${!wrap_expected_tail_var}"

assert_eq "$(extract_field "$wrap_out_w" target)" "$wrap_expected_head" "wrap key did not route to last-head node"
assert_eq "$(extract_field "$wrap_out_r" target)" "$wrap_expected_tail" "wrap key did not route to predecessor tail"

# For 3 nodes this specific wrap key should pass through server_0 in propagate path.
if (( NODE_COUNT == 3 )); then
  sleep 0.2
  if ! grep -F "action=handle_propagate key='$WRAP_KEY'" "$WORK_DIR/server_0.log" >/dev/null 2>&1; then
    die "wrap-around evidence missing: server_0 did not log propagate for wrap key"
  fi
fi

# 5) Concurrent writes to same key preserve version order.
echo "[crown-smoke] Verifying concurrent same-key version ordering..."
concurrent_key="smoke-concurrent-key"
concurrent_dir="$WORK_DIR/concurrent"
rm -rf "$concurrent_dir"
mkdir -p "$concurrent_dir"

writer_count=8
declare -a WRITER_PIDS=()
for ((i = 1; i <= writer_count; ++i)); do
  (
    set +e
    out="$(kv_write_concurrent "$CONFIG" "$concurrent_key" "concurrent-$i" 2>&1)"
    status=$?
    printf "%s\n" "$status" >"$concurrent_dir/$i.status"
    printf "%s\n" "$out" >"$concurrent_dir/$i.out"
  ) &
  WRITER_PIDS+=("$!")
done

for pid in "${WRITER_PIDS[@]}"; do
  wait "$pid"
done

versions=()
for ((i = 1; i <= writer_count; ++i)); do
  status="$(cat "$concurrent_dir/$i.status")"
  out="$(cat "$concurrent_dir/$i.out")"
  [[ "$status" == "0" ]] || die "concurrent writer $i failed: $out"

  v="$(extract_field "$out" version)"
  [[ -n "$v" ]] || die "failed to parse version from concurrent writer $i output: $out"
  versions+=("$v")
done

mapfile -t sorted_versions < <(printf "%s\n" "${versions[@]}" | sort -n)
for ((i = 1; i < ${#sorted_versions[@]}; ++i)); do
  prev="${sorted_versions[$((i - 1))]}"
  cur="${sorted_versions[$i]}"
  (( cur > prev )) || die "concurrent versions not strictly increasing after sort: ${sorted_versions[*]}"
done

first_v="${sorted_versions[0]}"
last_v="${sorted_versions[$((writer_count - 1))]}"
(( last_v - first_v + 1 == writer_count )) || die "concurrent versions are not contiguous: ${sorted_versions[*]}"

final_read_out="$(kv_read "$CONFIG" "$concurrent_key")"
final_read_v="$(extract_field "$final_read_out" version)"
assert_eq "$final_read_v" "$last_v" "final read version does not match latest concurrent write"

echo "[crown-smoke] PASS"
