// Redirects are opt-in. By default a 3xx comes back to you as-is;
// follow_redirects() makes curl chase the Location chain and hands you
// the final response.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>

int main() {
  asio::io_context io;
  cofetch::Client http(io);
  const std::string bounce = "https://nghttp2.org/httpbin/redirect/2";

  // Default: see the redirect itself.
  http.async_get(bounce, [](std::error_code, cofetch::Response res) {
    std::cout << "without follow: http " << res.http_code_ << "\n";
  });

  // Opt in: land on the final page (at most 30 hops by default).
  http.request(bounce).follow_redirects().get(
      [](std::error_code, cofetch::Response res) {
        std::cout << "with follow:    http " << res.http_code_ << "\n";
      });

  io.run();
}
