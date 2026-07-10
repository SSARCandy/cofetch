#include <cofetch.h>

#include <asio.hpp>
#include <chrono>
#include <iostream>

using namespace std;

// Throughput demo: many concurrent requests, driven by a busy-polled event
// loop (hot-loop style). See example02 for the coroutine API.
constexpr size_t REQUESTS = 200;

int main() {
  const auto start = chrono::high_resolution_clock::now();
  asio::io_context io;
  cofetch::Client http(io);

  size_t completed = 0;
  size_t failed = 0;
  for (size_t i = 0; i < REQUESTS; ++i) {
    http.async_get("https://fapi.binance.com/fapi/v1/time",
                   [&](std::error_code ec, cofetch::Response res) {
                     if (ec || !res.is_ok()) ++failed;
                     ++completed;
                   });
  }
  while (completed < REQUESTS) {
    io.poll();
  }
  const auto stop = chrono::high_resolution_clock::now();
  const chrono::duration<double> duration = stop - start;
  cout << ">> Completed : " << completed << "\n"
       << ">> Failed    : " << failed << "\n"
       << ">> Requests per second: " << REQUESTS / duration.count()
       << " [#/sec]" << endl;
}
