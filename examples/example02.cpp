#include <asio.hpp>
#include <iostream>

#include "http/cofetch.h"

using namespace std;

// Sequential requests written linearly with co_await, like JS async/await.
asio::awaitable<void> demo(cofetch::Client& client) {
  const auto time_res = co_await client.async_get(
      "https://fapi.binance.com/fapi/v1/time", asio::use_awaitable);
  cout << "server time: " << time_res.data_ << "\n";

  const auto echo =
      co_await client.async_post("https://postman-echo.com/post",
                                 "prev=" + time_res.data_, asio::use_awaitable);
  cout << "echo ok: " << echo.is_ok() << " (http " << echo.http_code_ << ")\n";
}

int main() {
  asio::io_context io;
  cofetch::Client client(io);
  asio::co_spawn(io, demo(client), asio::detached);
  io.run();
}
