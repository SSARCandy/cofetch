// Compiled as -std=c++17 in CI to guarantee the header stays C++17-clean:
// callbacks, the fluent chain, and deferred composition without coroutines.
#include <asio.hpp>
//
#include <cstdio>
#include <cstdlib>
#include <string>

#include "http/cofetch.h"

static_assert(__cplusplus < 202002L, "smoke test must compile as C++17");

int main() {
  const char* echo = std::getenv("COFETCH_ECHO");
  if (echo == nullptr) {
    std::puts("cxx17 smoke: compiled (set COFETCH_ECHO to run)");
    return 0;
  }
  const std::string base = echo;

  asio::io_context io;
  cofetch::Client client(io);
  bool ok = false;

  auto chain = client.request(base + "/post")
                   .body("x=1")
                   .post(asio::deferred)(
                       asio::deferred([&](std::error_code, cofetch::Response) {
                         return client.async_get(base + "/get", asio::deferred);
                       }));
  std::move(chain)([&](std::error_code ec, cofetch::Response res) {
    ok = !ec && res.is_ok();
  });

  io.run();
  std::printf("cxx17 smoke: %s\n", ok ? "ok" : "failed");
  return ok ? 0 : 1;
}
