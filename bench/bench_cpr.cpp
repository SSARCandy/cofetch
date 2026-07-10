#include <cpr/cpr.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.h"

// cpr's async story is thread-based; represent it honestly with a pool of
// <concurrency> threads, each reusing one keep-alive Session.
int main(int argc, char** argv) {
  const bench::Args args = bench::parse(argc, argv);
  const std::string url = args.base_url + "/";
  std::atomic<int> failed{0};

  if (args.scenario == "throughput") {
    std::atomic<int> next{0};
    const bench::Timer timer;
    std::vector<std::thread> pool;
    pool.reserve(args.concurrency);
    for (int t = 0; t < args.concurrency; ++t) {
      pool.emplace_back([&] {
        cpr::Session session;
        session.SetUrl(cpr::Url{url});
        while (next.fetch_add(1) < args.total) {
          const cpr::Response res = session.Get();
          if (res.status_code != 200) ++failed;
        }
      });
    }
    for (auto& th : pool) th.join();
    bench::report("cpr-threads", args, timer.seconds(), failed);
    return failed != 0;
  }

  if (args.scenario == "chain") {
    const bench::Timer timer;
    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    for (int i = 0; i < args.total; ++i) {
      const cpr::Response res = session.Get();
      if (res.status_code != 200) ++failed;
    }
    bench::report("cpr", args, timer.seconds(), failed);
    return failed != 0;
  }

  std::fprintf(stderr, "unknown scenario %s\n", args.scenario.c_str());
  return 2;
}
