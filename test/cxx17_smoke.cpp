// Compiled as -std=c++17 in CI to guarantee the header stays C++17-clean:
// callbacks, the fluent chain, and deferred composition without coroutines.
// Also compiled with -DCOFETCH_USE_BOOST_ASIO to guard the Boost.Asio build.
#if defined(COFETCH_USE_BOOST_ASIO)
#include <boost/asio.hpp>
namespace anet = boost::asio;
#else
#include <asio.hpp>
namespace anet = asio;
#endif
//
#include <cofetch.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static_assert(__cplusplus < 202002L, "smoke test must compile as C++17");

int main() {
  const char* echo = std::getenv("COFETCH_ECHO");
  if (echo == nullptr) {
    std::puts("cxx17 smoke: compiled (set COFETCH_ECHO to run)");
    return 0;
  }
  const std::string base = echo;

  anet::io_context io;
  cofetch::Client client(io);
  bool ok = false;

  auto chain = client.request(base + "/post")
                   .body("x=1")
                   .post(anet::deferred)(anet::deferred(
                       [&](std::error_code, const cofetch::Response&) {
                         return client.async_get(base + "/get", anet::deferred);
                       }));
  std::move(chain)([&](std::error_code ec, const cofetch::Response& res) {
    ok = !ec && res.is_ok();
  });

  io.run();
  std::printf("cxx17 smoke: %s\n", ok ? "ok" : "failed");
  return ok ? 0 : 1;
}
