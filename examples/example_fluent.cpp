// Build a request as a chain: setters flow off request(), and the HTTP
// verb at the end fires the transfer.
#include <cofetch.h>

#include <asio.hpp>
#include <chrono>
#include <iostream>

int main() {
  asio::io_context io;
  cofetch::Client http(io);

  http.request("https://postman-echo.com/post")
      .headers({"content-type: application/json"})
      .body(R"({"greeting": "hello cofetch"})")
      .timeout(std::chrono::seconds(2))
      .post([](std::error_code ec, const cofetch::Response& res) {
        if (ec) {
          std::cerr << "transport error: " << ec.message() << "\n";
          return;
        }
        std::cout << "http " << res.http_code_ << "\n" << res.data_ << "\n";
      });

  io.run();
}
