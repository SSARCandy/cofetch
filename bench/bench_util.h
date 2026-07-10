#pragma once
// Shared helpers for the benchmark drivers. Each driver is a standalone
// binary so client libraries never contend inside one process:
//   bench_<name> throughput <base_url> <total> <concurrency>
//   bench_<name> chain <base_url> <count>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bench {

struct Args {
  std::string scenario;
  std::string base_url;
  int total = 0;
  int concurrency = 1;
};

inline Args parse(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s throughput <base_url> <total> <concurrency>\n"
                 "       %s chain <base_url> <count>\n",
                 argv[0], argv[0]);
    std::exit(2);
  }
  Args a;
  a.scenario = argv[1];
  a.base_url = argv[2];
  a.total = std::atoi(argv[3]);
  a.concurrency = argc > 4 ? std::atoi(argv[4]) : 1;
  return a;
}

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}
  double seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start_)
        .count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

// One CSV row on stdout; run_bench.sh assembles the table.
inline void report(const std::string& name, const Args& a, double seconds,
                   int failed) {
  std::printf("%s,%s,%d,%d,%.3f,%.0f,%d\n", name.c_str(), a.scenario.c_str(),
              a.total, a.concurrency, seconds, a.total / seconds, failed);
}

}  // namespace bench
