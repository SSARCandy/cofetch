// Connection pooling. The first request to a host pays for DNS, TCP
// and the TLS handshake; the connection then stays in curl's pool, so
// the second request skips all of it.
#include <cofetch.h>

#include <asio.hpp>
#include <chrono>
#include <iostream>

using Clock = std::chrono::steady_clock;

asio::awaitable<void> twice(cofetch::Client& http) {
  const auto ms_since = [](Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                 start)
        .count();
  };

  auto t = Clock::now();
  co_await http.async_get("https://postman-echo.com/get", asio::use_awaitable);
  std::cout << "first  request: " << ms_since(t) << " ms"
            << "  (DNS + TCP + TLS + HTTP)\n";

  t = Clock::now();
  co_await http.async_get("https://postman-echo.com/get", asio::use_awaitable);
  std::cout << "second request: " << ms_since(t) << " ms"
            << "  (pooled connection, HTTP only)\n";
}

int main() {
  asio::io_context io;
  cofetch::Client http(io);
  asio::co_spawn(io, twice(http), asio::detached);
  io.run();
}
