// cofetch is async, but a blocking facade is a few lines. Own one
// io_context and Client so connections still pool across calls, then per
// request fire it with use_future and drive the loop until it settles.
// restart() makes the drained loop reusable for the next call.
//
// The trade-off: this serialises requests, giving up the in-flight
// concurrency that is the point of the async API. Reach for it only when a
// plain blocking call is what you actually want.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>
#include <string>
#include <utility>

class SyncClient {
 public:
  SyncClient() : http_(io_) {}

  cofetch::Response get(std::string url) {
    return run(http_.async_get(std::move(url), asio::use_future));
  }
  cofetch::Response post(std::string url, std::string body) {
    return run(http_.async_post(std::move(url), std::move(body),
                                asio::use_future));
  }

 private:
  // get() rethrows transport errors as std::system_error.
  cofetch::Response run(std::future<cofetch::Response> pending) {
    io_.restart();  // reusable after the previous call drained it
    io_.run();      // block this thread until the request settles
    return pending.get();
  }

  asio::io_context io_;    // declared first: http_ is constructed from it
  cofetch::Client http_;   // one client, so the pool survives across calls
};

int main() {
  SyncClient http;

  const cofetch::Response a = http.get("https://postman-echo.com/get");
  std::cout << "get  http " << a.http_code_ << "\n";

  // Second call reuses the pooled connection from the first.
  const cofetch::Response b =
      http.post("https://postman-echo.com/post", R"({"qty": 1})");
  std::cout << "post http " << b.http_code_ << "\n";
}
