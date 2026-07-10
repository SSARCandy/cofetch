#include <functional>
#include <string>

#include "baseline/http_epoll.h"
#include "bench_util.h"

// The pre-ASIO epoll client this library grew out of (git ae4d136),
// kept as the performance baseline cofetch must not regress from.
// Linux-only. Its handle pool caps usable concurrency at 128.
int main(int argc, char** argv) {
  const bench::Args args = bench::parse(argc, argv);
  const std::string url = args.base_url + "/";

  Http http;
  int failed = 0;

  if (args.scenario == "throughput") {
    int in_flight = 0;
    int launched = 0;
    int done = 0;
    std::function<void()> refill = [&] {
      while (in_flight < args.concurrency && launched < args.total) {
        ++in_flight;
        ++launched;
        http.request(url,
                     [&](const ResponseInfo& res) {
                       --in_flight;
                       ++done;
                       if (!res.is_ok()) ++failed;
                       refill();
                     })
            .get();
      }
    };
    const bench::Timer timer;
    refill();
    while (done < args.total) http.poll();
    bench::report("epoll-baseline", args, timer.seconds(), failed);
    return failed != 0;
  }

  if (args.scenario == "chain") {
    const bench::Timer timer;
    for (int i = 0; i < args.total; ++i) {
      bool done = false;
      http.request(url,
                   [&](const ResponseInfo& res) {
                     if (!res.is_ok()) ++failed;
                     done = true;
                   })
          .get();
      while (!done) http.poll();
    }
    bench::report("epoll-baseline", args, timer.seconds(), failed);
    return failed != 0;
  }

  std::fprintf(stderr, "unknown scenario %s\n", args.scenario.c_str());
  return 2;
}
