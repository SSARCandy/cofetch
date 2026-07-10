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
asio 1.38. Raw data: `results.csv`.

**One thread each, 20,000 GETs**

| client | in flight | time | req/s |
|---|---:|---:|---:|
| cofetch (callbacks, `run()`) | 100 | 1.30 s | 15,388 |
| cofetch (callbacks, busy-`poll()`) | 100 | 1.27 s | 15,794 |
| cpr (sync session) | 1 | 1.93 s | 10,367 |
| cpp-httplib (sync client) | 1 | 1.52 s | 13,186 |
| epoll baseline (internal reference) | 100 | 1.04 s | 19,304 |

**One thread per core (20 threads each), 20,000 GETs**

| client | threads | time | req/s |
|---|---:|---:|---:|
| cofetch (one event loop per core) | 20 | 0.10 s | 200,594 |
| cpr (thread pool) | 20 | 0.17 s | 120,055 |
| cpp-httplib (thread pool) | 20 | 0.16 s | 127,565 |

**Sequential chain, 2,000 dependent requests, one thread**

| client | time | req/s |
|---|---:|---:|
| cofetch (callback chain) | 0.19 s | 10,466 |
| cofetch (coroutine chain) | 0.20 s | 10,254 |
| cpr | 0.20 s | 10,224 |
| cpp-httplib | 0.16 s | 12,604 |
| epoll baseline (internal reference) | 0.13 s | 15,487 |

## Notes for maintainers

- The epoll baseline is the performance ceiling cofetch must not drift
  from. Current gap on loopback: ~25% throughput, ~48% chain — the cost
  of the portable ASIO reactor (per-event `async_wait` re-arm, executor
  dispatch), not of coroutines (callback vs coroutine chain differ ~2%).
- asio's io_uring backend (`-DASIO_HAS_IO_URING -DASIO_DISABLE_EPOLL`,
  link `-luring`) recovers about half the throughput gap (+14%,
  measured on WSL2 kernel 5.15). Closing the rest needs persistent
  socket registration — see ROADMAP.
