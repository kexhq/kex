#!/usr/bin/env bash
# make bench-net: Net.HTTP.Server throughput (kexhq/kex#670).
#
#   bash tools/bench/net/run.sh [KEX] [PORT]
#
# Compiles server.kex with KEX (build/kex by default), starts it pinned to
# CPUs 0-3, and drives it from load.escript pinned to CPUs 4-7, so the two
# don't compete for cores. Each scenario runs three times; read the median.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
kex=${1:-$root/build/kex}
port=${2:-18950}
out=$(mktemp -d)
trap 'kill "$server" 2>/dev/null || true; rm -rf "$out"' EXIT INT TERM

if (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null; then
  echo "port $port is already in use; pass another as the second argument" >&2
  exit 1
fi
"$kex" -c -o "$out" "$here/server.kex" >/dev/null
PORT=$port taskset -c 0-3 erl +Bi -noshell -pa "$out" -eval "kex_server:main()" >/dev/null 2>&1 &
server=$!
for _ in $(seq 1 100); do
  (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null && break
  sleep 0.1
done
if ! (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null; then
  echo "the server did not start listening on $port" >&2
  exit 1
fi

scenario() {
  mode=$1; conns=$2; per=$3
  for _ in 1 2 3; do
    taskset -c 4-7 escript "$here/load.escript" "$mode" "$port" "$conns" "$per"
  done
}

scenario ws 1 2000
scenario ws 100 200
scenario ws 1000 20
scenario http 100 200
scenario http 1000 20
