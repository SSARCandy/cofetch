# cofetch

> `await fetch()` for modern C++ — an async HTTP client for event-loop
> applications, built on libcurl's multi interface and standalone ASIO.

Requests **build as a chain** — setters flow off `request()` and the
HTTP verb fires the transfer:

```cpp
const auto res = co_await client.request("https://api.example.com/orders")
                     .headers({"content-type: application/json"})
                     .body(R"({"qty": 1})")
                     .timeout(std::chrono::seconds(2))
                     .post(asio::use_awaitable);
```

Dependent requests **chain like promises**. The JavaScript you write
every day:

```js
const user  = await fetch(api + "/user");
const posts = await fetch(api + "/posts", { method: "POST", body: user.body });
```

and its cofetch equivalent — same shape, C++20 coroutine:

```cpp
const auto user  = co_await client.async_get(api + "/user", asio::use_awaitable);
const auto posts = co_await client.async_post(api + "/posts", user.data_,
                                              asio::use_awaitable);
```

Prefer classic `.then()` chains, or can't use coroutines? The same
pipeline builds from `asio::deferred` — still one thread, still no
callback pyramid:

```cpp
auto chain = client.async_get(api + "/user", asio::deferred)(
    asio::deferred([&](std::error_code ec, cofetch::Response user) {
      return client.async_post(api + "/posts", user.data_, asio::deferred);
    }));
std::move(chain)([](std::error_code ec, cofetch::Response posts) { /*...*/ });
```

Every ASIO completion token works: plain callbacks for the
zero-overhead hot path, `co_await` for flows, `asio::deferred` for
`.then()`-style chains, `std::future` to bridge blocking code — one
implementation behind all of them.

## Why cofetch

- **ASIO-native.** Requests run on the `asio::io_context` you already
  own — compose with timers, sockets, and other coroutines. Drive it
  with `run()`, or `poll()` it from a busy loop that must never block
  (the trading hot path this library grew out of).
- **libcurl underneath.** HTTP/1.1 and HTTP/2 multiplexing, TLS,
  compression, connection pooling, redirects — two decades of protocol
  maturity instead of a hand-rolled client.
- **fetch()-like error model.** Transport failures arrive as
  `std::error_code` (curl error category); HTTP 4xx/5xx are *responses*,
  not errors — check `res.is_ok()`.
- **Header-only.** `#include "http/cofetch.h"`, link against libcurl,
  done.

## Benchmarks

Local nginx on Debian (WSL2, 20 cores), gcc 14 `-O3`. Every client
does the **same 20,000 GETs**, so wall-clock time is directly
comparable. Zero-latency loopback measures client CPU overhead, not
the network. Reproduce with `bench/run_bench.sh`.

**One thread each.** A sync client drives one request at a time; an
async client keeps 100 in flight on that same single thread — that is
the point of async:

| client | in flight | time | req/s |
|---|---:|---:|---:|
| **cofetch** (callbacks, `run()`) | 100 | 1.30 s | 15,388 |
| **cofetch** (callbacks, busy-`poll()`) | 100 | 1.27 s | 15,794 |
| [cpr](https://github.com/libcpr/cpr) (sync session) | 1 | 1.93 s | 10,367 |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) (sync client) | 1 | 1.52 s | 13,186 |
| *epoll ancestor of cofetch (internal reference)* | 100 | 1.04 s | 19,304 |

**One thread per core (20 threads each).** cofetch runs one event loop
per core; the sync libraries get an equal-sized thread pool:

| client | threads | time | req/s |
|---|---:|---:|---:|
| **cofetch** (one event loop per core) | 20 | **0.10 s** | **200,594** |
| cpr (thread pool) | 20 | 0.17 s | 120,055 |
| cpp-httplib (thread pool) | 20 | 0.16 s | 127,565 |

**Sequential chain.** 2,000 dependent requests, one at a time on one
thread — this measures per-request latency, and no client can hide it
with concurrency:

| client | time | req/s |
|---|---:|---:|
| **cofetch** (callback chain) | 0.19 s | 10,466 |
| **cofetch** (coroutine chain) | 0.20 s | 10,254 |
| cpr | 0.20 s | 10,224 |
| cpp-httplib | 0.16 s | 12,604 |
| *epoll ancestor of cofetch (internal reference)* | 0.13 s | 15,487 |

Honest notes: the raw-epoll client cofetch grew out of (kept in
`bench/baseline/`) is still 25–50% faster on loopback — the current
price of the portable ASIO reactor (per-event re-arm, executor
dispatch), not of coroutines (callback and coroutine chains differ by
~2%). An io_uring ASIO backend recovers about half of that gap;
closing the rest is on the roadmap. On a real network, milliseconds of
RTT dwarf these microseconds — what remains is the thread count you
pay: one loop beats a sync client per-thread, and per-core loops beat
a 20-thread pool by ~60%.

## Quick start

```cpp
#include <asio.hpp>
#include "http/cofetch.h"

asio::awaitable<void> demo(cofetch::Client& client) {
  const auto time = co_await client.async_get(
      "https://fapi.binance.com/fapi/v1/time", asio::use_awaitable);

  // Chain a dependent request: plain linear code, no callback nesting.
  const auto echo = co_await client.async_post(
      "https://postman-echo.com/post", "prev=" + time.data_,
      asio::use_awaitable);
  std::cout << echo.data_ << "\n";
}

int main() {
  asio::io_context io;
  cofetch::Client client(io);
  asio::co_spawn(io, demo(client), asio::detached);
  io.run();
}
```

Requests are plain values too — build one ahead of time, fire it later:

```cpp
cofetch::Request req("https://api.example.com/orders");
req.method(cofetch::Request::Method::POST)
   .headers({"content-type: application/json", "x-api-key: k"})
   .body(R"({"qty": 1})");
client.async_perform(std::move(req), token);
```

Error handling, all styles:

```cpp
// coroutine: as_tuple avoids exceptions
auto [ec, res] = co_await client.async_get(url, asio::as_tuple(asio::use_awaitable));
if (ec) { /* transport failed: DNS, TLS, timeout... (curl category) */ }
else if (!res.is_ok()) { /* HTTP error status: res.http_code_ */ }
```

## Installation

Requirements: a C++20 compiler, libcurl ≥ 7.80 (dev headers), and
[standalone ASIO](https://github.com/chriskohlhoff/asio).

### CMake (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(cofetch
    GIT_REPOSITORY https://github.com/SSARCandy/cofetch.git
    GIT_TAG main)
FetchContent_MakeAvailable(cofetch)
target_link_libraries(your_app PRIVATE cofetch::cofetch)
```

cofetch does not impose an ASIO on consumers: point it at yours (any
include path providing `<asio.hpp>`), or enable the vendored submodule
with `-DCOFETCH_USE_VENDORED_ASIO=ON`.

### Manual

Copy `http/cofetch.h`, add asio to your include path, link `libcurl`.

## What cofetch is not

- **Not a server.** Client only. For a server (or a tiny zero-dependency
  client), use [cpp-httplib](https://github.com/yhirose/cpp-httplib).
- **Not single-header-zero-dependency.** Riding libcurl is a deliberate
  trade: dependencies in exchange for protocol maturity.
- **Not thread-safe.** One `Client` per `io_context` thread by design —
  no locks on the hot path. The client must outlive its in-flight
  requests.

## Development

```bash
git submodule update --init          # asio + googletest (dev only)
./build.sh -t                        # Debug build + tests + coverage
./linter.sh                          # clang-format check (v19 pinned in CI)
bench/run_bench.sh                   # benchmarks against local nginx
COFETCH_LIVE_TESTS=1 ./build.sh -t   # also run live-network tests
```

CI runs the linter and the offline test suite (local echo server, no
external endpoints) on Linux gcc/clang and macOS.
