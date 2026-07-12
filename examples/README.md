# Examples

One focused program per file, written for readability. They build with
the default configure (`COFETCH_BUILD_EXAMPLES=ON`) and hit public echo
endpoints, so they need network access to run.

| example | shows |
|---|---|
| [example_hello.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_hello.cpp) | the smallest program: one GET, one callback |
| [example_fluent.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_fluent.cpp) | building a POST with the chainable setters |
| [example_coroutine.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_coroutine.cpp) | dependent requests in linear code (C++20 `co_await`) |
| [example_future.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_future.cpp) | `std::future` style: start, drive, `get()` |
| [example_sync.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_sync.cpp) | wrapping the async client in a blocking `SyncClient` facade |
| [example_concurrent.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_concurrent.cpp) | 20 requests in flight on one thread |
| [example_errors.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_errors.cpp) | transport errors vs HTTP error statuses |
| [example_cancellation.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_cancellation.cpp) | a time budget per request with `asio::cancel_after` |
| [example_redirects.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_redirects.cpp) | 3xx as-is by default; `follow_redirects()` to chase them |
| [example_http2.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_http2.cpp) | requests multiplexed over one HTTP/2 connection; opting a request back to HTTP/1.1 |
| [example_tls.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_tls.cpp) | https out of the box; TLS knobs via the `.curl()` escape hatch |
| [example_compression.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_compression.cpp) | gzip/brotli responses arrive already decoded |
| [example_keepalive.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_keepalive.cpp) | connection pooling: the second request skips the handshake |
| [example_reactors.cpp](https://github.com/SSARCandy/cofetch/blob/main/examples/example_reactors.cpp) | `run()` vs busy-`poll()` vs a foreign epoll loop; io_uring build |
