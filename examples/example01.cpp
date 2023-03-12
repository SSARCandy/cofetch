#include <chrono>
#include <iostream>

#include "http/Http.h"
#include "simdjson/singleheader/simdjson.h"

using namespace std;

constexpr size_t REQUESTS = 200;
int main() {
  const auto start = chrono::high_resolution_clock::now();
  const string url = "https://fapi.binance.com/fapi/v1/time";
  Http http;

  size_t completed = 0;
  size_t failed = 0;
  for (auto i = 0; i < REQUESTS; ++i) {
    // clang-format off
    http
      .request(url, [&](const ResponseInfo& res) {
        if (!res.is_ok()) ++failed;
        ++completed;
      })
      .get();
    // clang-format on
  }
  do {
    http.poll();
  } while (http.pending_requests());
  const auto stop = chrono::high_resolution_clock::now();
  const chrono::duration<double> duration = stop - start;
  cout << ">> Completed : " << completed << "\n"
       << ">> Failed    : " << failed << "\n"
       << ">> Requests per second: " << REQUESTS / duration.count()
       << " [#/sec]" << endl;
}
