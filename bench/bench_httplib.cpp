#include <httplib.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.h"

// cpp-httplib is synchronous; give it the same thread-pool treatment as
// cpr, each thread reusing one keep-alive Client.
int main(int argc, char** argv) {
  const bench::Args args = bench::parse(argc, argv);
  std::atomic<int> failed{0};

  if (args.scenario == "throughput") {
    std::atomic<int> next{0};
    const bench::Timer timer;
    std::vector<std::thread> pool;
    pool.reserve(args.concurrency);
    for (int t = 0; t < args.concurrency; ++t) {
      pool.emplace_back([&] {
        httplib::Client client(args.base_url);
        client.set_keep_alive(true);
        while (next.fetch_add(1) < args.total) {
          const auto res = client.Get("/");
          if (!res || res->status != 200) ++failed;
        }
      });
    }
    for (auto& th : pool) th.join();
    bench::report("cpp-httplib-threads", args, timer.seconds(), failed);
    return failed != 0;
  }

  if (args.scenario == "chain") {
    const bench::Timer timer;
    httplib::Client client(args.base_url);
    client.set_keep_alive(true);
    for (int i = 0; i < args.total; ++i) {
      const auto res = client.Get("/");
      if (!res || res->status != 200) ++failed;
    }
    bench::report("cpp-httplib", args, timer.seconds(), failed);
    return failed != 0;
  }

  std::fprintf(stderr, "unknown scenario %s\n", args.scenario.c_str());
  return 2;
}
