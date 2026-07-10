// HTTP/2 multiplexing. cofetch enables CURLOPT_PIPEWAIT, so concurrent
// requests to the same h2 server share one connection as parallel
// streams. The status line in the response headers shows the
// negotiated version — and the .curl() escape hatch can switch a
// request back to plain HTTP/1.1 when a server misbehaves on h2.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>
#include <string>

int main() {
  asio::io_context io;
  cofetch::Client http(io);
  const std::string url = "https://postman-echo.com/get";

  const auto status_line = [](const cofetch::Response& res) {
    return res.header_data_.substr(0, res.header_data_.find('\r'));
  };

  // Default: these four multiplex as streams over one h2 connection.
  for (int i = 1; i <= 4; ++i) {
    http.async_get(url, [i, status_line](std::error_code ec,
                                         const cofetch::Response& res) {
      std::cout << "stream #" << i << "  "
                << (ec ? ec.message() : status_line(res)) << "\n";
    });
  }

  // Opt out per request: force HTTP/1.1 on a connection of its own.
  http.request(url)
      .curl([](CURL* h) {
        curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
      })
      .get([status_line](std::error_code ec, const cofetch::Response& res) {
        std::cout << "forced 1.1  " << (ec ? ec.message() : status_line(res))
                  << "\n";
      });

  io.run();
}
