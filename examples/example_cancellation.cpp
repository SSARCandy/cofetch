// Give a request a time budget with asio::cancel_after: past it, the
// transfer is torn down and completes with operation_aborted.
#include <cofetch.h>

#include <asio.hpp>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

asio::awaitable<void> impatient_get(cofetch::Client& http) {
  // The server takes 2 s to answer; our patience runs out after 500 ms.
  const auto [ec, res] = co_await http.async_get(
      "https://postman-echo.com/delay/2",
      asio::cancel_after(500ms, asio::as_tuple(asio::use_awaitable)));

  if (ec == asio::error::operation_aborted) {
    std::cout << "gave up after 500 ms\n";
  } else {
    std::cout << "finished anyway: http " << res.http_code_ << "\n";
  }
  // The connection stays cached for reuse after the cancel, so io.run()
  // in main() returns a moment later, once the server retires it.
}

int main() {
  asio::io_context io;
  cofetch::Client http(io);
  asio::co_spawn(io, impatient_get(http), asio::detached);
  io.run();
}
