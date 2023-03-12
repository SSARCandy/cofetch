#include "gtest/gtest.h"
#include "http/Http.h"
#include "simdjson/singleheader/simdjson.h"

using namespace simdjson;

namespace {

TEST(Http, check_handle_count) {
  const vector<string> urls = {
      "https://postman-echo.com/get",
      "https://stackoverflow.com/",
  };
  Http http;
  for (const auto& url : urls) {
    http.request(url, [&](const ResponseInfo& res) {}).get();
  }

  EXPECT_GT(http.pending_requests(), 0);
}

TEST(Http, get) {
  const vector<string> urls = {
      "https://1.1.1.1",
  };
  Http http;
  for (const auto& url : urls) {
    http.request(
            url,
            [&](const ResponseInfo& res) { EXPECT_LT(0, res.data_.length()); })
        .get();
  }

  do {
    http.poll();
  } while (http.pending_requests());
}

TEST(Http, get_response_header) {
  const vector<string> urls = {
      "https://postman-echo.com/get",
  };
  Http http;
  for (const auto& url : urls) {
    http.request(url,
                 [&](const ResponseInfo& res) {
                   EXPECT_LT(0, res.data_.length());
                   EXPECT_LT(0, res.header_data_.length());
                 })
        .enable_response_headers()
        .get();
  }

  do {
    http.poll();
  } while (http.pending_requests());
}

TEST(Http, get404) {
  Http http;
  http.request("https://www.binance.com/en/1111",
               [&](const ResponseInfo& res) { EXPECT_EQ(404, res.http_code_); })
      .get();

  do {
    http.poll();
  } while (http.pending_requests());
}

TEST(Http, post_with_body) {
  const string url = "https://postman-echo.com/post";
  Http http;

  // clang-format off
  http
    .request(url, [&](const ResponseInfo& res) {
      simdjson::dom::parser parser;
      const auto d = parser.parse(res.data_);
      EXPECT_EQ("123", d["form"]["b"].get_string().value());
    })
    .set_body("b=123")
    .post();
  // clang-format off

  do {
    http.poll();
  } while (http.pending_requests());
}

TEST(Http, post_with_header) {
  const string url = "https://postman-echo.com/post";
  Http http;

  // clang-format off
  http
    .request(url, [&](const ResponseInfo& res) {
      simdjson::dom::parser parser;
      const auto d = parser.parse(res.data_);
      EXPECT_EQ("12345", d["headers"]["x-mbx-apikey"].get_string().value());
    })
    .set_headers({"X-MBX-APIKEY: 12345"})
    .post();
  // clang-format off

  do {
    http.poll();
  } while (http.pending_requests());
}

TEST(Http, post_with_header_and_body) {
  const string url = "https://postman-echo.com/post";
  Http http;

  // clang-format off
  http
    .request(url, [&](const ResponseInfo& res) {
      simdjson::dom::parser parser;
      const auto d = parser.parse(res.data_);
      EXPECT_EQ("123", d["form"]["b"].get_string().value());
      EXPECT_EQ("hello", d["headers"]["header1"].get_string().value());
      EXPECT_EQ("world", d["headers"]["header2"].get_string().value());
    })
    .set_headers({"header1:hello", "header2:world"})
    .set_body("b=123")
    .post();
  // clang-format off

  do {
    http.poll();
  } while (http.pending_requests());
}

TEST(Http, delete) {
  const string url = "https://postman-echo.com/delete";
  Http http;

  // clang-format off
  http
    .request(url, [&](const ResponseInfo& res) {
      simdjson::dom::parser parser;
      const auto d = parser.parse(res.data_);
      EXPECT_EQ("123", d["form"]["b"].get_string().value());
      EXPECT_EQ("hello", d["headers"]["header1"].get_string().value());
      EXPECT_EQ("world", d["headers"]["header2"].get_string().value());
    })
    .set_headers({"header1:hello", "header2:world"})
    .set_body("b=123")
    .del();
  // clang-format off

  do {
    http.poll();
  } while (http.pending_requests());
}

TEST(Http, put) {
  const string url = "https://postman-echo.com/put";
  Http http;

  // clang-format off
  http
    .request(url, [&](const ResponseInfo& res) {
      simdjson::dom::parser parser;
      const auto d = parser.parse(res.data_);
      EXPECT_EQ("123", d["form"]["b"].get_string().value());
    })
    .set_body("b=123")
    .put();
  // clang-format off

  do {
    http.poll();
  } while (http.pending_requests());
}

}  // namespace