# Examples

One focused program per file, written for readability. They build with
the default configure (`COFETCH_BUILD_EXAMPLES=ON`) and hit public echo
endpoints, so they need network access to run.

| example | shows |
|---|---|
| [example_hello.cpp](example_hello.cpp) | the smallest program: one GET, one callback |
| [example_fluent.cpp](example_fluent.cpp) | building a POST with the chainable setters |
| [example_coroutine.cpp](example_coroutine.cpp) | dependent requests in linear code (C++20 `co_await`) |
| [example_future.cpp](example_future.cpp) | `std::future` style: start, drive, `get()` |
| [example_concurrent.cpp](example_concurrent.cpp) | 20 requests in flight on one thread |
| [example_errors.cpp](example_errors.cpp) | transport errors vs HTTP error statuses |
| [example_cancellation.cpp](example_cancellation.cpp) | a time budget per request with `asio::cancel_after` |
| [example_reactors.cpp](example_reactors.cpp) | `run()` vs busy-`poll()` vs a foreign epoll loop; io_uring build |
