// Two different kinds of failure, deliberately kept apart:
//   - transport errors (DNS, TLS, timeout) arrive in the error_code
//   - HTTP error statuses are *successful* transfers; check res.is_ok()
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>

int main() {
  asio::io_context io;
  cofetch::Client client(io);

  // DNS failure: ec is set, the response carries the curl error text.
  client.async_get("https://no-such-host.invalid/",
                   [](std::error_code ec, cofetch::Response) {
                     std::cout << "transport error: " << ec.message() << "\n";
                   });

  // 404: the transfer worked, the server just said no.
  client.async_get("https://postman-echo.com/status/404",
                   [](std::error_code ec, cofetch::Response res) {
                     if (!ec && !res.is_ok()) {
                       std::cout << "http status: " << res.http_code_ << "\n";
                     }
                   });

  io.run();
}
