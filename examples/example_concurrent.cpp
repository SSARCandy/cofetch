// Twenty requests in flight at once — one thread, no pool. The event
// loop multiplexes them; completions arrive as each response lands.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>
#include <string>

int main() {
  asio::io_context io;
  cofetch::Client http(io);

  int done = 0;
  for (int i = 1; i <= 20; ++i) {
    http.async_get(
        "https://postman-echo.com/get?n=" + std::to_string(i),
        [&done, i](std::error_code ec, const cofetch::Response& res) {
          std::cout << "#" << i << " -> "
                    << (ec ? ec.message() : std::to_string(res.http_code_))
                    << "\n";
          ++done;
        });
  }

  io.run();
  std::cout << done << " requests completed\n";
}
