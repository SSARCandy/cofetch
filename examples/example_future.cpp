// std::future style: start the request, drive the loop, collect the
// result. get() rethrows transport errors as std::system_error.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>

int main() {
  asio::io_context io;
  cofetch::Client http(io);

  auto pending =
      http.async_get("https://postman-echo.com/get", asio::use_future);

  io.run();  // the future is ready once the loop drains

  const cofetch::Response res = pending.get();
  std::cout << "http " << res.http_code_ << "\n";
}
