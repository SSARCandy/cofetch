// Same client, different reactors.
//
//   example03 run      asio owns the thread: epoll on Linux, kqueue on
//                      macOS, IOCP on Windows.
//   example03 poll     you own the thread; asio only runs what is ready
//                      (the trading busy-loop mode).
//   example03 foreign  your app already has an epoll loop: keep it in
//                      charge and service cofetch each tick (Linux).
//
// The same source also builds as example03_uring (when liburing is
// present): -DASIO_HAS_IO_URING -DASIO_DISABLE_EPOLL swaps asio's Linux
// reactor from epoll to io_uring — cofetch code does not change.
#include <asio.hpp>
#include <iostream>
#include <string>

#include "http/cofetch.h"

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

constexpr int kRequests = 5;
constexpr const char* kUrl = "https://postman-echo.com/get";

void launch_requests(cofetch::Client& client, int& completed) {
  for (int i = 0; i < kRequests; ++i) {
    client.async_get(kUrl, [&, i](std::error_code ec, cofetch::Response res) {
      ++completed;
      cout << "  #" << i << " -> "
           << (ec ? ec.message() : to_string(res.http_code_)) << "\n";
    });
  }
}

}  // namespace

int main(int argc, char** argv) {
  const string mode = argc > 1 ? argv[1] : "run";
  asio::io_context io;
  cofetch::Client client(io);
  int completed = 0;
  launch_requests(client, completed);

  if (mode == "run") {
    // Blocking reactor: sleeps in the kernel until sockets are ready,
    // returns when no work is left.
    io.run();
  } else if (mode == "poll") {
    // Busy-poll: never blocks; interleave your own hot-loop work.
    while (completed < kRequests) {
      io.poll();
      // ... per-iteration application work goes here ...
    }
#ifdef __linux__
  } else if (mode == "foreign") {
    // A pre-existing epoll reactor stays in charge; cofetch rides along
    // with one non-blocking io.poll() per tick.
    const int ep = epoll_create1(EPOLL_CLOEXEC);
    const int tick = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    const itimerspec every_ms{{0, 1000000}, {0, 1000000}};
    timerfd_settime(tick, 0, &every_ms, nullptr);
    epoll_event want{};
    want.events = EPOLLIN;
    want.data.fd = tick;
    epoll_ctl(ep, EPOLL_CTL_ADD, tick, &want);

    while (completed < kRequests) {
      epoll_event ev;
      const int n = epoll_wait(ep, &ev, 1, 10);
      if (n > 0 && ev.data.fd == tick) {
        uint64_t expirations = 0;
        [[maybe_unused]] const auto r =
            read(tick, &expirations, sizeof(expirations));
        // ... the rest of your reactor's event handling goes here ...
      }
      io.poll();
    }
    close(tick);
    close(ep);
#endif
  } else {
    cerr << "usage: " << argv[0] << " [run|poll|foreign]\n";
    return 2;
  }

  cout << "done: " << completed << "/" << kRequests << " (" << mode
       << " mode)\n";
  return completed == kRequests ? 0 : 1;
}
