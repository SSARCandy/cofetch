#include <cofetch.h>

#include <asio.hpp>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.h"

// Env knobs:
//   COFETCH_BENCH_POLL=1   busy-poll the io_context (trading-loop mode)
//   COFETCH_BENCH_LOOPS=N  run N event loops on N threads (one Client
//                          each), splitting the workload — the deployed
//                          shape when one core is not enough.
namespace {

int run_throughput(const std::string& url, int total, int concurrency,
                   bool busy_poll) {
  asio::io_context io(1);
  cofetch::Client client(io);
  int failed = 0;
  int in_flight = 0;
  int launched = 0;
  int done = 0;
  std::function<void()> refill = [&] {
    while (in_flight < concurrency && launched < total) {
      ++in_flight;
      ++launched;
      client.async_get(url, [&](std::error_code ec, cofetch::Response res) {
        --in_flight;
        ++done;
        if (ec || !res.is_ok()) ++failed;
        refill();
      });
    }
  };
  refill();
  if (busy_poll) {
    while (done < total) io.poll();
  } else {
    io.run();
  }
  return failed;
}

}  // namespace

int main(int argc, char** argv) {
  const bench::Args args = bench::parse(argc, argv);
  const bool busy_poll = std::getenv("COFETCH_BENCH_POLL") != nullptr;
  const char* loops_env = std::getenv("COFETCH_BENCH_LOOPS");
  const int loops = loops_env ? std::atoi(loops_env) : 1;
  const std::string url = args.base_url + "/";

  if (args.scenario == "throughput") {
    std::atomic<int> failed{0};
    const bench::Timer timer;
    if (loops > 1) {
      std::vector<std::thread> pool;
      pool.reserve(loops);
      for (int t = 0; t < loops; ++t) {
        pool.emplace_back([&, t] {
          const int share =
              args.total / loops + (t < args.total % loops ? 1 : 0);
          failed +=
              run_throughput(url, share, args.concurrency / loops, busy_poll);
        });
      }
      for (auto& th : pool) th.join();
      bench::report("cofetch-" + std::to_string(loops) + "loops", args,
                    timer.seconds(), failed);
    } else {
      failed = run_throughput(url, args.total, args.concurrency, busy_poll);
      bench::report(busy_poll ? "cofetch-poll" : "cofetch", args,
                    timer.seconds(), failed);
    }
    return failed != 0;
  }

  if (args.scenario == "chain") {
    asio::io_context io(1);
    cofetch::Client client(io);
    int failed = 0;
    const bool callbacks = std::getenv("COFETCH_BENCH_CB") != nullptr;
    const bench::Timer timer;
    if (callbacks) {
      // Same shape as the epoll baseline's chain: callback per request.
      int remaining = args.total;
      std::function<void()> next = [&] {
        client.async_get(url, [&](std::error_code ec, cofetch::Response res) {
          if (ec || !res.is_ok()) ++failed;
          if (--remaining > 0) next();
        });
      };
      next();
      if (busy_poll) {
        while (remaining > 0) io.poll();
      } else {
        io.run();
      }
      bench::report(busy_poll ? "cofetch-cb-poll" : "cofetch-cb", args,
                    timer.seconds(), failed);
      return failed != 0;
    }
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
          for (int i = 0; i < args.total; ++i) {
            const auto res =
                co_await client.async_get(url, asio::use_awaitable);
            if (!res.is_ok()) ++failed;
          }
        },
        asio::detached);
    io.run();
    bench::report("cofetch-coro", args, timer.seconds(), failed);
    return failed != 0;
  }

  std::fprintf(stderr, "unknown scenario %s\n", args.scenario.c_str());
  return 2;
}
