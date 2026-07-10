// Compression is transparent: cofetch advertises every encoding curl
// was built with (gzip, brotli, ...), and curl decodes the response
// before it reaches you. This endpoint replies gzip-compressed — the
// body still arrives as plain JSON.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>

int main() {
  asio::io_context io;
  cofetch::Client http(io);

  http.async_get("https://postman-echo.com/gzip",
                 [](std::error_code ec, cofetch::Response res) {
                   if (ec) {
                     std::cerr << ec.message() << "\n";
                     return;
                   }
                   std::cout << res.data_ << "\n";  // already decompressed
                 });

  io.run();
}
