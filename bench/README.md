# Benchmarks

cofetch vs [cpr](https://github.com/libcpr/cpr) and
[cpp-httplib](https://github.com/yhirose/cpp-httplib), plus the
raw-epoll client cofetch grew out of (`baseline/`, kept as an internal
no-regression reference — not a competitor).

## How to run

One-time setup (Debian/Ubuntu):

```bash
sudo apt install nginx libcurl4-openssl-dev   # nginx serves the load locally
```

Then, from the repo root:

```bash
bench/run_bench.sh | tee bench/results.csv               # build + run everything
python3 bench/plot_bench.py bench/results.csv -o docs    # regenerate README charts
```

- `run_bench.sh [total] [concurrency] [chain_count]` — defaults
  `20000 100 2000`. It configures a Release build with
  `-DCOFETCH_BUILD_BENCH=ON` (FetchContent pulls cpr and cpp-httplib),
  starts nginx on `127.0.0.1:18081` with `bench/nginx.conf`
  (keep-alive, `access_log off`), and prints one CSV row per run.
- `plot_bench.py` needs matplotlib (`sudo apt install python3-matplotlib`)
  and writes `docs/benchmark-{light,dark}.svg`.

Driver knobs (set as env vars for `bench_cofetch`):

- `COFETCH_BENCH_POLL=1` — busy-poll `io_context::poll()` instead of `run()`
- `COFETCH_BENCH_LOOPS=N` — N threads, each with its own event loop and
  `Client`, splitting the workload
- `COFETCH_BENCH_CB=1` — chain scenario with callbacks instead of a coroutine

## Scenarios

- **throughput** — `total` GETs with `concurrency` kept in flight.
  Sync clients (cpr, cpp-httplib) get one thread per unit of
  concurrency; cofetch keeps them in flight on one thread.
- **chain** — sequential dependent requests, one in flight. This is
  per-request latency; concurrency cannot hide it.

Benchmarking is against local nginx on loopback: zero network latency,
so the numbers isolate client CPU overhead. On a real network, RTT
dominates — run with a remote nginx if you want to see that.

## Results — 2026-07-10

Debian 13 (WSL2, 20 logical cores), gcc 14 `-O3`, libcurl 8.14,
asio 1.38. Raw data: `results.csv`. WSL run-to-run variance is
roughly ±10%; interleaved re-runs confirm the ordering is stable.

**One thread each, 20,000 GETs**

| client | in flight | time | req/s |
|---|---:|---:|---:|
| cofetch (callbacks, busy-`poll()`) | 100 | 1.10 s | 18,240 |
| cofetch (callbacks, `run()`) | 100 | 1.15 s | 17,412 |
| cpr (sync session) | 1 | 1.98 s | 10,127 |
| cpp-httplib (sync client) | 1 | 1.55 s | 12,889 |
| epoll baseline (internal reference) | 100 | 0.99 s | 20,270 |

**One thread per core (20 threads each), 20,000 GETs**

| client | threads | time | req/s |
|---|---:|---:|---:|
| cofetch (one event loop per core) | 20 | 0.12 s | 166,646 |
| cpr (thread pool) | 20 | 0.16 s | 122,122 |
| cpp-httplib (thread pool) | 20 | 0.16 s | 127,333 |

**Sequential chain, 2,000 dependent requests, one thread**

| client | time | req/s |
|---|---:|---:|
| cofetch (callbacks, busy-`poll()`) | 0.14 s | 14,804 |
| cofetch (callbacks, `run()`) | 0.18 s | 11,116 |
| cofetch (coroutine, `run()`) | 0.19 s | 10,547 |
| cpr | 0.20 s | 10,209 |
| cpp-httplib | 0.16 s | 12,761 |
| epoll baseline (internal reference) | 0.13 s | 15,566 |

The chain scenario is per-request latency. In blocking `run()` mode a
reactor pays one kernel sleep/wake per request that a raw blocking
`recv()` (cpp-httplib) does not — busy-poll mode removes it, which is
exactly what that mode is for. The README chart uses busy-poll for the
chain panel because it is cofetch's documented latency mode.

## Notes for maintainers

- The epoll baseline is the performance ceiling cofetch must not drift
  from. After the 2026-07-10 optimizations (static curl options set
  once per pooled handle instead of reset+reapply, curl's 0 ms timer
  kicks turned into a deduplicated `asio::post`, timer reschedules
  skipped when a pending expiry is early enough, reactor handlers on
  `asio::recycling_allocator`, completion invoked without the
  type-erased dispatch hop) the loopback gap is ~14% throughput and
  ~5% chain (busy-poll). Callback vs coroutine chains differ ~5%.
- Construct the io_context as `asio::io_context io(1)` in
  single-threaded apps — the concurrency hint removes internal locking.
- asio's io_uring backend (`-DASIO_HAS_IO_URING -DASIO_DISABLE_EPOLL`,
  link `-luring`) was measured +14% throughput before these
  optimizations; re-measure if pursuing.
- Persistent socket registration was tried and **rejected** (2026-07-10):
  a client-owned epoll set with the epoll fd registered in asio measured
  −8% throughput and −3% chain vs the one-shot path. asio's epoll
  reactor already keeps descriptors persistently registered internally;
  nesting a second epoll only added syscalls per batch. Interleaved A/B,
  5 rounds each. Don't retry without new evidence.
