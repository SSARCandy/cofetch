// The smallest cofetch program: one GET, one callback.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>

int main() {
  asio::io_context io;
  cofetch::Client http(io);

  http.async_get("https://postman-echo.com/get",
                 [](std::error_code ec, const cofetch::Response& res) {
                   if (ec) {
                     std::cerr << "transport error: " << ec.message() << "\n";
                     return;
                   }
                   std::cout << "http " << res.http_code_ << "\n"
                             << res.data_ << "\n";
                 });

  io.run();  // returns once nothing is left to do
}
