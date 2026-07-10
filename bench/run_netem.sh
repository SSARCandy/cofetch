#!/bin/bash
# Tier-1 benchmark with real network latency injected on loopback.
# Usage: bench/run_netem.sh [rtt_ms]     (default 10; needs sudo for tc)
#
# Loopback RTT is ~0, which flatters sync clients: every request returns
# instantly, so throughput is pure CPU. With RTT, a sync thread is
# parked for a full round trip per request while an async client keeps
# its pipeline full. Same framing as tier 1 (one thread each), plus
# 100-thread pools to show what sync needs to match one cofetch thread.
#
# Requires the sch_netem kernel module (stock WSL2 kernels lack it; see
# bench/README.md).
set -e

CURDIR="$( cd "$(dirname "$0")/.." ; pwd -P )"
RTT="${1:-10}"
HALF_MS=$(( RTT / 2 ))                # netem delays each direction once
URL="http://127.0.0.1:18081"
NGINX_PREFIX=/tmp/cofetch-nginx

# Keep every run near ~10s of steady state: in-flight/RTT caps req/s.
TOTAL_ASYNC=$(( 200000 / RTT ))       # 100 in flight -> ceiling 100000/RTT
TOTAL_SYNC1=$(( 10000 / RTT ))        # 1 in flight   -> ceiling  1000/RTT

cd "${CURDIR}"
mkdir -p build-bench "${NGINX_PREFIX}"
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DCOFETCH_BUILD_BENCH=ON \
      -DCOFETCH_BUILD_EXAMPLES=OFF > /dev/null
cmake --build build-bench --target bench_cofetch bench_cpr bench_httplib \
      -j "$(nproc)" > /dev/null

sudo modprobe sch_netem 2>/dev/null || true
sudo tc qdisc add dev lo root netem delay "${HALF_MS}ms"

nginx -p "${NGINX_PREFIX}" -c "${CURDIR}/bench/nginx.conf"
cleanup() {
    nginx -p "${NGINX_PREFIX}" -c "${CURDIR}/bench/nginx.conf" -s stop
    sudo tc qdisc del dev lo root
}
trap cleanup EXIT
sleep 0.3

ping -c 3 -q 127.0.0.1 | tail -1 >&2   # show the effective RTT

BIN=build-bench/bench
TAG="s/,throughput,/,throughput-rtt${RTT},/"
echo "name,scenario,total,concurrency,seconds,req_per_sec,failed"
${BIN}/bench_cofetch throughput "${URL}" 2000 100 > /dev/null  # warmup

# One thread each: cofetch multiplexes 100 in flight, sync clients wait
# out one RTT per request.
${BIN}/bench_cofetch throughput "${URL}" "${TOTAL_ASYNC}" 100 | sed "${TAG}"
${BIN}/bench_httplib throughput "${URL}" "${TOTAL_SYNC1}" 1  | sed "${TAG}"
${BIN}/bench_cpr     throughput "${URL}" "${TOTAL_SYNC1}" 1  | sed "${TAG}"

# What it takes for sync to match: a 100-thread pool (same 100 in flight).
${BIN}/bench_httplib throughput "${URL}" "${TOTAL_ASYNC}" 100 | sed "${TAG}"
${BIN}/bench_cpr     throughput "${URL}" "${TOTAL_ASYNC}" 100 | sed "${TAG}"
