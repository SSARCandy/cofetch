#include <asio.hpp>

#include "gtest/gtest.h"
#include "http/cofetch.h"

using cofetch::Client;
using cofetch::Request;
using cofetch::Response;

// NOTE: only EXPECT_* inside coroutines; ASSERT_* expands to a plain
// `return;`, which does not compile in a coroutine body.
namespace {

TEST(Cofetch, get_coroutine) {
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto res = co_await client.async_get(
            "https://postman-echo.com/get", asio::use_awaitable);
        EXPECT_TRUE(res.is_ok());
        EXPECT_LT(0u, res.data_.length());
        EXPECT_LT(0u, res.header_data_.length());
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Cofetch, transport_error) {
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto [ec, res] =
            co_await client.async_get("https://nonexistent.invalid/",
                                      asio::as_tuple(asio::use_awaitable));
        EXPECT_TRUE(static_cast<bool>(ec));
        EXPECT_TRUE(ec.category() == cofetch::curl_category());
        EXPECT_FALSE(res.is_ok());
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Cofetch, post_with_headers_and_body) {
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto res = co_await client.async_perform(
            Request("https://postman-echo.com/post")
                .method(Request::Method::POST)
                .headers({"x-api-test: hello"})
                .body("b=123"),
            asio::use_awaitable);
        EXPECT_TRUE(res.is_ok());
        EXPECT_NE(std::string::npos, res.data_.find("hello"));
        EXPECT_NE(std::string::npos, res.data_.find("123"));
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Cofetch, sequential_chain) {
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto first = co_await client.async_get(
            "https://postman-echo.com/get?token=42", asio::use_awaitable);
        EXPECT_TRUE(first.is_ok());
        EXPECT_NE(std::string::npos, first.data_.find("42"));

        // The second request depends on the first: linear code, no nesting.
        const auto second =
            co_await client.async_post("https://postman-echo.com/post",
                                       "from_first=42", asio::use_awaitable);
        EXPECT_TRUE(second.is_ok());
        EXPECT_NE(std::string::npos, second.data_.find("from_first"));
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Cofetch, callback_busy_poll) {
  asio::io_context io;
  Client client(io);
  int completed = 0;
  for (int i = 0; i < 5; ++i) {
    client.async_get("https://postman-echo.com/get",
                     [&](std::error_code ec, Response res) {
                       EXPECT_FALSE(ec);
                       EXPECT_TRUE(res.is_ok());
                       ++completed;
                     });
  }
  while (completed < 5) {
    io.poll();
  }
  EXPECT_EQ(5, completed);
}

TEST(Cofetch, use_future) {
  asio::io_context io;
  Client client(io);
  auto fut = client.async_get("https://postman-echo.com/get", asio::use_future);
  io.run();
  const Response res = fut.get();
  EXPECT_TRUE(res.is_ok());
}

}  // namespace
