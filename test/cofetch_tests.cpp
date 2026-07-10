#include <cofetch.h>

#include <asio.hpp>
#include <cstdlib>
#include <string>

#include "gtest/gtest.h"

using cofetch::Client;
using cofetch::Request;
using cofetch::Response;

// NOTE: only EXPECT_* inside coroutines; ASSERT_* and GTEST_SKIP() expand to
// a plain `return;`, which does not compile in a coroutine body.
namespace {

std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

// Local.* tests hit test/echo_server.py; build.sh and CI start it and
// export COFETCH_ECHO (e.g. http://127.0.0.1:18089).
std::string echo_base() { return env("COFETCH_ECHO"); }

#define COFETCH_REQUIRE_ECHO()                                          \
  if (echo_base().empty()) {                                            \
    GTEST_SKIP() << "COFETCH_ECHO not set (start test/echo_server.py)"; \
  }

// Live.* tests reach real endpoints; opt in with COFETCH_LIVE_TESTS=1.
#define COFETCH_REQUIRE_LIVE()                                            \
  if (env("COFETCH_LIVE_TESTS") != "1") {                                 \
    GTEST_SKIP() << "set COFETCH_LIVE_TESTS=1 to run live-network tests"; \
  }

TEST(Local, get_coroutine) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto res = co_await client.async_get(echo_base() + "/get",
                                                   asio::use_awaitable);
        EXPECT_TRUE(res.is_ok());
        EXPECT_NE(std::string::npos, res.data_.find("/get"));
        EXPECT_LT(0u, res.header_data_.length());
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Local, transport_error) {
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
        EXPECT_FALSE(ec.message().empty());
        EXPECT_FALSE(res.is_ok());
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Local, http_error_status_is_not_transport_error) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  auto fut = client.async_get(echo_base() + "/status/404", asio::use_future);
  io.run();
  const Response res = fut.get();  // no throw: transport succeeded
  EXPECT_FALSE(res.is_ok());
  EXPECT_EQ(404, res.http_code_);
}

TEST(Local, post_with_headers_and_body) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto res =
            co_await client.async_perform(Request(echo_base() + "/post")
                                              .method(Request::Method::POST)
                                              .headers({"x-api-test: hello"})
                                              .body("b=123"),
                                          asio::use_awaitable);
        EXPECT_TRUE(res.is_ok());
        EXPECT_NE(std::string::npos, res.data_.find("hello"));
        EXPECT_NE(std::string::npos, res.data_.find("b=123"));
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Local, sequential_chain) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto first = co_await client.async_get(
            echo_base() + "/get?token=42", asio::use_awaitable);
        EXPECT_TRUE(first.is_ok());
        EXPECT_NE(std::string::npos, first.data_.find("token=42"));

        // The second request depends on the first: linear code, no nesting.
        const auto second = co_await client.async_post(
            echo_base() + "/post", "from_first=42", asio::use_awaitable);
        EXPECT_TRUE(second.is_ok());
        EXPECT_NE(std::string::npos, second.data_.find("from_first"));
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

// The fluent chain: setters chain off client.request() and the HTTP verb
// fires the transfer.
TEST(Local, fluent_builder_chain) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto res = co_await client.request(echo_base() + "/post")
                             .headers({"x-api-test: fluent"})
                             .body("b=456")
                             .timeout(std::chrono::seconds(5))
                             .post(asio::use_awaitable);
        EXPECT_TRUE(res.is_ok());
        EXPECT_NE(std::string::npos, res.data_.find("fluent"));
        EXPECT_NE(std::string::npos, res.data_.find("b=456"));
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

// The .then()-style chain: asio::deferred packages "run this, feed the
// result to the next request" without coroutines.
TEST(Local, deferred_then_chain) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;

  auto chain = client.async_get(echo_base() + "/get?step=1", asio::deferred)(
      asio::deferred([&](std::error_code ec, Response first) {
        EXPECT_FALSE(ec);
        EXPECT_NE(std::string::npos, first.data_.find("step=1"));
        return client.async_post(echo_base() + "/post", "prev=step1",
                                 asio::deferred);
      }));

  std::move(chain)([&](std::error_code ec, Response second) {
    EXPECT_FALSE(ec);
    EXPECT_NE(std::string::npos, second.data_.find("prev=step1"));
    done = true;
  });
  io.run();
  EXPECT_TRUE(done);
}

TEST(Local, callback_busy_poll) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  int completed = 0;
  for (int i = 0; i < 5; ++i) {
    client.async_get(echo_base() + "/get",
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

TEST(Local, use_future) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  auto fut = client.async_get(echo_base() + "/get", asio::use_future);
  io.run();
  const Response res = fut.get();
  EXPECT_TRUE(res.is_ok());
}

TEST(Local, put_and_del_verbs) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto put = co_await client.request(echo_base() + "/put")
                             .body("v=1")
                             .put(asio::use_awaitable);
        EXPECT_TRUE(put.is_ok());
        EXPECT_NE(std::string::npos, put.data_.find("\"PUT\""));
        EXPECT_NE(std::string::npos, put.data_.find("v=1"));

        const auto del = co_await client.request(echo_base() + "/delete")
                             .del(asio::use_awaitable);
        EXPECT_TRUE(del.is_ok());
        EXPECT_NE(std::string::npos, del.data_.find("\"DELETE\""));
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

TEST(Local, timeout_is_transport_error) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  client.request(echo_base() + "/delay/3")
      .timeout(std::chrono::seconds(1))
      .get([&](std::error_code ec, Response res) {
        EXPECT_EQ(static_cast<int>(CURLE_OPERATION_TIMEDOUT), ec.value());
        EXPECT_TRUE(ec.category() == cofetch::curl_category());
        EXPECT_FALSE(res.is_ok());
        done = true;
      });
  io.run();
  EXPECT_TRUE(done);
}

// 70 concurrent transfers exceed the 64-handle pool cap, exercising both
// handle reuse and the overflow cleanup path.
TEST(Local, concurrent_burst_beyond_pool_cap) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  constexpr int kBurst = 70;
  int completed = 0;
  for (int i = 0; i < kBurst; ++i) {
    client.async_get(echo_base() + "/get",
                     [&](std::error_code ec, Response res) {
                       EXPECT_FALSE(ec);
                       EXPECT_TRUE(res.is_ok());
                       ++completed;
                     });
  }
  io.poll();  // process the kick-start so the transfers are in flight
  EXPECT_LT(0, client.pending_requests());
  io.run();
  EXPECT_EQ(kBurst, completed);
  EXPECT_EQ(0, client.pending_requests());
}

TEST(Local, post_empty_body) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  auto fut = client.async_post(echo_base() + "/post", "", asio::use_future);
  io.run();
  const Response res = fut.get();
  EXPECT_TRUE(res.is_ok());
  EXPECT_NE(std::string::npos, res.data_.find("\"data\": \"\""));
}

// Documented destructor semantics: in-flight requests are dropped and
// their handlers never run — and nothing crashes afterwards.
TEST(Local, inflight_dropped_on_destruction) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  bool invoked = false;
  {
    Client client(io);
    client.async_get(echo_base() + "/get",
                     [&](std::error_code, Response) { invoked = true; });
  }
  io.run();
  EXPECT_FALSE(invoked);
}

TEST(Local, response_defaults_and_error_text) {
  const Response res;
  EXPECT_FALSE(res.is_ok());  // http_code_ 0 is not a 2xx
  EXPECT_STREQ("No error", res.error());
}

TEST(Live, get_over_tls) {
  COFETCH_REQUIRE_LIVE();
  asio::io_context io;
  Client client(io);
  auto fut = client.async_get("https://fapi.binance.com/fapi/v1/time",
                              asio::use_future);
  io.run();
  const Response res = fut.get();
  EXPECT_TRUE(res.is_ok());
  EXPECT_NE(std::string::npos, res.data_.find("serverTime"));
}

TEST(Live, post_echo) {
  COFETCH_REQUIRE_LIVE();
  asio::io_context io;
  Client client(io);
  auto fut = client.async_post("https://postman-echo.com/post", "b=123",
                               asio::use_future);
  io.run();
  const Response res = fut.get();
  EXPECT_TRUE(res.is_ok());
  EXPECT_NE(std::string::npos, res.data_.find("b=123"));
}

}  // namespace
