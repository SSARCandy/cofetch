// HTTP/2 multiplexing. cofetch enables CURLOPT_PIPEWAIT, so concurrent
// requests to the same h2 server share one connection as parallel
// streams instead of racing to open sockets. The status line in the
// response headers shows the negotiated version.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>
#include <string>

int main() {
  asio::io_context io;
  cofetch::Client http(io);

  const auto status_line = [](const cofetch::Response& res) {
    return res.header_data_.substr(0, res.header_data_.find('\r'));
  };

  for (int i = 1; i <= 4; ++i) {
    http.async_get("https://nghttp2.org/httpbin/get",
                   [i, status_line](std::error_code ec, cofetch::Response res) {
                     if (ec) {
                       std::cerr << "#" << i << " " << ec.message() << "\n";
                       return;
                     }
                     std::cout << "#" << i << " " << status_line(res) << "\n";
                   });
  }

  io.run();  // typically prints "HTTP/2 200" four times — one connection
}
