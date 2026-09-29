#include <cofetch.h>

#include <asio.hpp>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>

#include <cerrno>
#endif

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
        EXPECT_STREQ("curl", ec.category().name());
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
      asio::deferred([&](std::error_code ec, const Response& first) {
        EXPECT_FALSE(ec);
        EXPECT_NE(std::string::npos, first.data_.find("step=1"));
        return client.async_post(echo_base() + "/post", "prev=step1",
                                 asio::deferred);
      }));

  std::move(chain)([&](std::error_code ec, const Response& second) {
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
                     [&](std::error_code ec, const Response& res) {
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

TEST(Local, put_patch_del_verbs) {
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

        const auto patch = co_await client.request(echo_base() + "/patch")
                               .body("v=2")
                               .patch(asio::use_awaitable);
        EXPECT_TRUE(patch.is_ok());
        EXPECT_NE(std::string::npos, patch.data_.find("\"PATCH\""));
        EXPECT_NE(std::string::npos, patch.data_.find("v=2"));

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
      .get([&](std::error_code ec, const Response& res) {
        EXPECT_EQ(static_cast<int>(CURLE_OPERATION_TIMEDOUT), ec.value());
        EXPECT_TRUE(ec.category() == cofetch::curl_category());
        EXPECT_FALSE(res.is_ok());
        done = true;
      });
  io.run();
  EXPECT_TRUE(done);
}

// A sub-second timeout proves millisecond precision reaches
// CURLOPT_TIMEOUT_MS: 200ms fires well before the server's 2s delay.
TEST(Local, sub_second_timeout) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  client.request(echo_base() + "/delay/2")
      .timeout(std::chrono::milliseconds(200))
      .get([&](std::error_code ec, const Response& res) {
        EXPECT_EQ(static_cast<int>(CURLE_OPERATION_TIMEDOUT), ec.value());
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
                     [&](std::error_code ec, const Response& res) {
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

// A custom pool cap is honoured: a burst well above it still completes,
// exercising handle reuse and overflow cleanup at the configured limit.
TEST(Local, custom_pool_cap_burst) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io, /*max_pooled_connections=*/8);
  constexpr int kBurst = 30;
  int completed = 0;
  for (int i = 0; i < kBurst; ++i) {
    client.async_get(echo_base() + "/get",
                     [&](std::error_code ec, const Response& res) {
                       EXPECT_FALSE(ec);
                       EXPECT_TRUE(res.is_ok());
                       ++completed;
                     });
  }
  io.run();
  EXPECT_EQ(kBurst, completed);
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
                     [&](std::error_code, const Response&) { invoked = true; });
  }
  io.run();
  EXPECT_FALSE(invoked);
}

TEST(Local, redirects_not_followed_by_default) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  auto fut = client.async_get(echo_base() + "/redirect/2", asio::use_future);
  io.run();
  const Response res = fut.get();  // transport ok, 302 passed through
  EXPECT_FALSE(res.is_ok());
  EXPECT_EQ(302, res.http_code_);
}

TEST(Local, follow_redirects_lands_on_target) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  client.request(echo_base() + "/redirect/2")
      .follow_redirects()
      .get([&](std::error_code ec, const Response& res) {
        EXPECT_FALSE(ec);
        EXPECT_TRUE(res.is_ok());
        EXPECT_NE(std::string::npos, res.data_.find("\"/get\""));
        // header_data_ keeps every hop's raw block, but the parsed view is
        // the final response only: no Location left over from the 302s,
        // and Content-Length is the landing page's, not "0, 0, N".
        EXPECT_NE(std::string::npos, res.header_data_.find("302"));
        EXPECT_FALSE(res.header("location").has_value());
        EXPECT_EQ(std::to_string(res.data_.size()),
                  res.header("content-length").value_or("missing"));
        done = true;
      });
  io.run();
  EXPECT_TRUE(done);
}

TEST(Local, too_many_redirects_is_transport_error) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  client.request(echo_base() + "/redirect/3")
      .follow_redirects(1)
      .get([&](std::error_code ec, const Response& res) {
        EXPECT_EQ(static_cast<int>(CURLE_TOO_MANY_REDIRECTS), ec.value());
        EXPECT_TRUE(ec.category() == cofetch::curl_category());
        EXPECT_FALSE(res.is_ok());
        done = true;
      });
  io.run();
  EXPECT_TRUE(done);
}

// The .curl() escape hatch reaches the raw easy handle — and whatever it
// sets is scrubbed before the pooled handle serves the next request.
TEST(Local, curl_escape_hatch_and_pool_scrub) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool hooked = false;
  client.request(echo_base() + "/get")
      .curl([](CURL* h) {
        curl_easy_setopt(h, CURLOPT_USERAGENT, "cofetch-hook/1");
      })
      .get([&](std::error_code ec, const Response& res) {
        EXPECT_FALSE(ec);
        EXPECT_NE(std::string::npos, res.data_.find("cofetch-hook/1"));
        hooked = true;
      });
  io.run();
  EXPECT_TRUE(hooked);

  // Same client, same pooled handle (LIFO): the sticky option must be gone.
  io.restart();
  bool plain = false;
  client.async_get(
      echo_base() + "/get", [&](std::error_code ec, const Response& res) {
        EXPECT_FALSE(ec);
        EXPECT_TRUE(res.is_ok());
        EXPECT_EQ(std::string::npos, res.data_.find("cofetch-hook"));
        plain = true;
      });
  io.run();
  EXPECT_TRUE(plain);
}

TEST(Local, cancellation_slot_aborts_inflight_request) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  asio::cancellation_signal first_sig, second_sig;
  int done = 0;
  const auto on_done = [&](std::error_code ec, const Response& res) {
    EXPECT_EQ(asio::error::operation_aborted, ec);
    EXPECT_FALSE(res.is_ok());
    ++done;
  };
  // Two in flight; the older one is cancelled first, so the lookup has to
  // walk past the newer transfer to find it.
  client.request(echo_base() + "/delay/3")
      .get(asio::bind_cancellation_slot(first_sig.slot(), on_done));
  client.request(echo_base() + "/delay/3")
      .get(asio::bind_cancellation_slot(second_sig.slot(), on_done));
  asio::steady_timer trigger(io, std::chrono::milliseconds(100));
  trigger.async_wait([&](std::error_code) {
    first_sig.emit(asio::cancellation_type::terminal);
    second_sig.emit(asio::cancellation_type::terminal);
  });
  io.run();  // returns long before the 3s delay: the transfers were torn down
  EXPECT_EQ(2, done);
  EXPECT_EQ(0, client.pending_requests());
}

// Cancelling before the io_context ever ran: the transfer was added but not
// kicked off yet, and still completes (posted) with operation_aborted.
TEST(Local, cancel_before_transfer_starts) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  asio::cancellation_signal sig;
  bool done = false;
  client.request(echo_base() + "/get")
      .get(asio::bind_cancellation_slot(
          sig.slot(), [&](std::error_code ec, const Response& res) {
            EXPECT_EQ(asio::error::operation_aborted, ec);
            EXPECT_EQ(CURLE_ABORTED_BY_CALLBACK, res.curl_code_);
            done = true;
          }));
  sig.emit(asio::cancellation_type::terminal);
  EXPECT_FALSE(done);  // never completed inline
  io.run();
  EXPECT_TRUE(done);
}

// cancel_after composes the same way it does for any asio operation.
TEST(Local, cancel_after_token) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  bool done = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        const auto [ec, res] = co_await client.async_get(
            echo_base() + "/delay/3",
            asio::cancel_after(std::chrono::milliseconds(100),
                               asio::as_tuple(asio::use_awaitable)));
        EXPECT_EQ(asio::error::operation_aborted, ec);
        done = true;
      },
      asio::detached);
  io.run();
  EXPECT_TRUE(done);
}

// A late emit — after the request completed, and even after the client is
// gone — must be a harmless no-op, not a dangling-pointer dereference.
TEST(Local, cancel_after_completion_is_noop) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  asio::cancellation_signal sig;
  int completions = 0;
  {
    Client client(io);
    client.async_get(echo_base() + "/get",
                     asio::bind_cancellation_slot(
                         sig.slot(), [&](std::error_code ec, const Response&) {
                           EXPECT_FALSE(ec);
                           ++completions;
                         }));
    io.run();
    EXPECT_EQ(1, completions);
    sig.emit(asio::cancellation_type::terminal);  // transfer already gone
    EXPECT_EQ(1, completions);
  }
  sig.emit(asio::cancellation_type::terminal);  // client already gone
  EXPECT_EQ(1, completions);
}

TEST(Local, response_defaults_and_error_text) {
  const Response res;
  EXPECT_FALSE(res.is_ok());  // http_code_ 0 is not a 2xx
  EXPECT_STREQ("No error", res.error());
}

// Parser unit test (no server): status line skipped, case-insensitive keys,
// repeated fields comma-combined, values trimmed, empty value retained.
TEST(Response, header_parsing) {
  const Response res{CURLE_OK, 200, "",
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Set-Cookie: a=1\r\n"
                     "set-cookie: b=2\r\n"
                     "X-Empty:\r\n"
                     "\r\n"};

  const auto h = res.headers();
  EXPECT_EQ(3u, h.size());  // status line dropped; Set-Cookie folded into one
  EXPECT_EQ("text/html; charset=utf-8", h.at("content-type"));
  EXPECT_EQ("text/html; charset=utf-8", h.at("CONTENT-TYPE"));
  EXPECT_EQ("a=1, b=2", h.at("Set-Cookie"));
  EXPECT_EQ("", h.at("x-empty"));

  EXPECT_EQ("text/html; charset=utf-8", res.header("Content-Type").value());
  EXPECT_EQ("a=1, b=2", res.header("set-cookie").value());
  EXPECT_FALSE(res.header("nonexistent").has_value());
}

// With redirects followed (or a 1xx interim response) curl appends one
// header block per hop; only the last block describes the delivered body.
TEST(Response, header_parsing_uses_final_block_only) {
  const Response res{CURLE_OK, 200, "",
                     "HTTP/1.1 302 Found\r\n"
                     "Location: /next\r\n"
                     "Content-Length: 0\r\n"
                     "Set-Cookie: hop=1\r\n"
                     "\r\n"
                     "HTTP/1.1 100 Continue\r\n"
                     "\r\n"
                     "HTTP/2 200 \r\n"
                     "content-type: application/json\r\n"
                     "content-length: 42\r\n"
                     "\r\n"};

  const auto h = res.headers();
  EXPECT_EQ(2u, h.size());
  EXPECT_EQ("42", h.at("Content-Length"));  // not "0, 42"
  EXPECT_EQ(0u, h.count("location"));
  EXPECT_EQ(0u, h.count("set-cookie"));
  EXPECT_EQ("application/json", res.header("Content-Type").value());
  EXPECT_FALSE(res.header("Location").has_value());

  // A single block, or one with no status line at all, is parsed whole;
  // bare LF line ends, blank lines and a missing final newline are fine.
  const Response single{CURLE_OK, 200, "", "X-One: 1\n\nX-Two: 2"};
  EXPECT_EQ(2u, single.headers().size());
  EXPECT_EQ("1", single.header("x-one").value());
  EXPECT_EQ("2", single.header("x-two").value());
}

// One event loop per core: clients constructed and destroyed on several
// threads at once must not trip over libcurl's global init/cleanup.
TEST(Local, clients_on_parallel_threads) {
  COFETCH_REQUIRE_ECHO();
  constexpr int kThreads = 4;
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      asio::io_context io;
      Client client(io);
      client.async_get(echo_base() + "/get",
                       [&](std::error_code ec, const Response& res) {
                         if (!ec && res.is_ok()) ++ok;
                       });
      io.run();
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(kThreads, ok.load());
}

TEST(Local, response_headers_from_server) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  auto fut = client.async_get(echo_base() + "/get", asio::use_future);
  io.run();
  const Response res = fut.get();
  ASSERT_TRUE(res.is_ok());
  const auto ct = res.header("content-type");  // case-insensitive
  ASSERT_TRUE(ct.has_value());
  EXPECT_NE(std::string::npos, ct->find("application/json"));
}

// A response several times curl's receive buffer arrives over many write
// callbacks and is reassembled in order. The 2 MiB upload also makes curl
// send "Expect: 100-continue", so header_data_ starts with a 1xx block that
// the parsed view must skip.
TEST(Local, large_body_reassembled) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  const std::string payload(2 * 1024 * 1024, 'x');
  auto fut =
      client.async_post(echo_base() + "/post", payload, asio::use_future);
  io.run();
  const Response res = fut.get();
  ASSERT_TRUE(res.is_ok());
  EXPECT_LT(payload.size(), res.data_.size());  // the JSON echo wraps it
  EXPECT_NE(std::string::npos, res.data_.find(payload));
  EXPECT_EQ("application/json", res.header("content-type").value_or(""));
}

// The echo server listens on 127.0.0.1 only, so ::1 is refused, but the
// socket for it is opened as IPv6 first. Any transport error will do.
TEST(Local, ipv6_loopback_socket) {
  COFETCH_REQUIRE_ECHO();
  std::string url = echo_base();
  const auto v4 = url.find("127.0.0.1");
  if (v4 == std::string::npos) GTEST_SKIP() << "echo base is not 127.0.0.1";
  url.replace(v4, 9, "[::1]");
  asio::io_context io;
  Client client(io);
  bool done = false;
  client.request(url + "/get")
      .timeout(std::chrono::seconds(2))
      .get([&](std::error_code ec, const Response& res) {
        EXPECT_TRUE(static_cast<bool>(ec));
        EXPECT_TRUE(ec.category() == cofetch::curl_category());
        EXPECT_FALSE(res.is_ok());
        done = true;
      });
  io.run();
  EXPECT_TRUE(done);
}

// A handle curl refuses to add — here because a Request::curl hook already
// gave it to another multi — must still complete its handler, with an error.
TEST(Local, add_handle_failure_completes_with_error) {
  asio::io_context io;
  CURLM* const other = curl_multi_init();
  bool done = false;
  {
    Client client(io);
    client.request("http://127.0.0.1:1/")
        .curl([&](CURL* h) { curl_multi_add_handle(other, h); })
        .get([&](std::error_code ec, const Response& res) {
          EXPECT_EQ(static_cast<int>(CURLE_FAILED_INIT), ec.value());
          EXPECT_TRUE(ec.category() == cofetch::curl_category());
          EXPECT_EQ(CURLE_FAILED_INIT, res.curl_code_);
          EXPECT_FALSE(res.is_ok());
          done = true;
        });
    EXPECT_FALSE(done);  // posted, never completed inside the initiating call
    io.run();
    EXPECT_TRUE(done);
  }  // ~Client detaches the handle from `other` before freeing it
  curl_multi_cleanup(other);
}

#if !defined(_WIN32)
// A Request::curl hook installing its own CURLOPT_OPENSOCKETFUNCTION: the
// connection socket never passes through cofetch's open callback.
struct RawSocket {
  int opened = 0;
  curl_socket_t last = CURL_SOCKET_BAD;
};
curl_socket_t raw_open_socket(void* p, curlsocktype, curl_sockaddr* a) {
  auto* const raw = static_cast<RawSocket*>(p);
  ++raw->opened;
  raw->last = ::socket(a->family, a->socktype, a->protocol);
  return raw->last;
}

// Descriptors curl obtained elsewhere are still watched (through a dup) and,
// when curl hands them to the close callback, closed on its behalf.
TEST(Local, foreign_socket_is_borrowed_and_closed) {
  COFETCH_REQUIRE_ECHO();
  asio::io_context io;
  Client client(io);
  RawSocket raw;
  bool done = false;
  client.request(echo_base() + "/get")
      .headers({"Connection: close"})  // closed right after the response
      .curl([&](CURL* h) {
        curl_easy_setopt(h, CURLOPT_OPENSOCKETFUNCTION, raw_open_socket);
        curl_easy_setopt(h, CURLOPT_OPENSOCKETDATA, &raw);
      })
      .get([&](std::error_code ec, const Response& res) {
        EXPECT_FALSE(ec);
        EXPECT_TRUE(res.is_ok());
        done = true;
      });
  io.run();
  EXPECT_TRUE(done);
  ASSERT_EQ(1, raw.opened);
  // Closed by cofetch when curl asked: the descriptor is gone, not leaked.
  const int rc = ::fcntl(raw.last, F_GETFD);
  const int err = errno;
  EXPECT_EQ(-1, rc);
  EXPECT_EQ(EBADF, err);
}
#endif

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
