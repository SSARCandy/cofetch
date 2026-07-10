// Dependent requests in linear code with co_await (C++20).
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>

asio::awaitable<void> fetch_then_post(cofetch::Client& http) {
  // First request: ask the server for its time.
  const auto time = co_await http.async_get(
      "https://fapi.binance.com/fapi/v1/time", asio::use_awaitable);
  std::cout << "server time: " << time.data_ << "\n";

  // Second request uses the first one's result — still top to bottom,
  // no callback nesting.
  const auto echo = co_await http.request("https://postman-echo.com/post")
                        .body("prev=" + time.data_)
                        .post(asio::use_awaitable);
  std::cout << "echo: http " << echo.http_code_ << "\n";
}

int main() {
  asio::io_context io;
  cofetch::Client http(io);
  asio::co_spawn(io, fetch_then_post(http), asio::detached);
  io.run();
}
