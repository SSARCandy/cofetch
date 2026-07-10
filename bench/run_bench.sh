#!/bin/bash
# Build and run all benchmarks against a local nginx.
# Usage: bench/run_bench.sh [total] [concurrency] [chain_count]
set -e

CURDIR="$( cd "$(dirname "$0")/.." ; pwd -P )"
TOTAL="${1:-20000}"
CONC="${2:-100}"
CHAIN="${3:-2000}"
URL="http://127.0.0.1:18081"
NGINX_PREFIX=/tmp/cofetch-nginx

cd "${CURDIR}"
mkdir -p build-bench "${NGINX_PREFIX}"
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DCOFETCH_BUILD_BENCH=ON \
      -DCOFETCH_BUILD_EXAMPLES=OFF > /dev/null
cmake --build build-bench --target \
      bench_cofetch bench_cpr bench_httplib bench_epoll -j "$(nproc)" \
      > /dev/null

nginx -p "${NGINX_PREFIX}" -c "${CURDIR}/bench/nginx.conf"
trap 'nginx -p "${NGINX_PREFIX}" -c "${CURDIR}/bench/nginx.conf" -s stop' EXIT
sleep 0.3

BIN=build-bench/bench
echo "name,scenario,total,concurrency,seconds,req_per_sec,failed"
for _ in 1 2 3; do ${BIN}/bench_cofetch throughput "${URL}" 2000 100 > /dev/null; done  # warmup

${BIN}/bench_cofetch throughput "${URL}" "${TOTAL}" "${CONC}"
COFETCH_BENCH_POLL=1 ${BIN}/bench_cofetch throughput "${URL}" "${TOTAL}" "${CONC}"
COFETCH_BENCH_LOOPS=$(nproc) ${BIN}/bench_cofetch throughput "${URL}" "${TOTAL}" "${CONC}"
${BIN}/bench_epoll   throughput "${URL}" "${TOTAL}" "${CONC}"
${BIN}/bench_cpr     throughput "${URL}" "${TOTAL}" "${CONC}"
${BIN}/bench_httplib throughput "${URL}" "${TOTAL}" "${CONC}"

${BIN}/bench_cofetch chain "${URL}" "${CHAIN}"
${BIN}/bench_epoll   chain "${URL}" "${CHAIN}"
${BIN}/bench_cpr     chain "${URL}" "${CHAIN}"
${BIN}/bench_httplib chain "${URL}" "${CHAIN}"
