# API reference

Every public symbol in `cofetch.h` on one page, each with a minimal snippet.
Everything lives in namespace `cofetch`. The snippets assume:

```cpp
asio::io_context io;
cofetch::Client  http(io);
```

Any completion token works everywhere a `token` appears below — a callback,
`asio::use_future`, `asio::use_awaitable` (C++20), `asio::deferred`, … The
completion signature is always `void(cofetch::error_code, cofetch::Response)`.
For runnable programs see the Examples; for per-parameter detail, the class
list in the sidebar.

---

## cofetch::Client

The one object you create. Owns the curl multi handle and the connection pool,
and runs on the `io_context` you hand it. Not thread-safe: one `Client` and its
`io_context` per thread, and the client must outlive its in-flight requests.

**Construct** — borrows the `io_context`; drive it with `run()`/`poll()`. The
optional second argument caps how many idle connections stay pooled for reuse
(default 64) — raise it for high-concurrency servers.
```cpp
cofetch::Client http(io);       // default: up to 64 idle connections pooled
cofetch::Client busy(io, 256);  // larger reuse pool for many hot connections
```

**`async_get` / `async_post`** — ASIO-style shortcuts for the common cases.
```cpp
http.async_get("https://example.com", token);
http.async_post("https://example.com", R"({"x":1})", token);

auto res = co_await http.async_get(url, asio::use_awaitable);  // C++20
auto fut = http.async_get(url, asio::use_future);              // std::future
```

**`async_perform`** — the general form; takes a fully-built `Request`.
`async_get`/`async_post` are thin wrappers over it.
```cpp
cofetch::Request req("https://example.com");
req.method(cofetch::Request::Method::PUT).body("payload");
http.async_perform(std::move(req), token);
```

**`request`** — start the fluent builder (see below); the verb fires it.
```cpp
http.request("https://example.com").body(R"({"x":1})").post(token);
```

**`pending_requests`** — number of transfers in flight.
```cpp
int n = http.pending_requests();
```

---

## cofetch::Client::RequestBuilder

Returned by `http.request(url)`. Setters return `*this`; the HTTP verb is
terminal — it starts the transfer with your completion token. Don't reuse a
builder after the verb.

**Setters**
```cpp
http.request(url)
    .headers({"content-type: application/json"})  // request headers
    .body("field=value")                          // request body
    .timeout(std::chrono::milliseconds(1500))     // whole-transfer cap, ms (default 5s)
    .follow_redirects()                           // chase 3xx, ≤30 hops (default: off)
    .curl([](CURL* h) { /* raw handle, runs last */ });
```

**Verbs (terminal)**
```cpp
http.request(url).get(token);
http.request(url).body(b).post(token);
http.request(url).body(b).put(token);
http.request(url).body(b).patch(token);
http.request(url).del(token);
```

For any other method, set it through the escape hatch:
```cpp
http.request(url)
    .curl([](CURL* h) { curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "HEAD"); })
    .get(token);
```

---

## cofetch::Request

Value-type description of a request: build one, hand it to `async_perform`.
Same setters as the builder, chainable (`Request&`).

```cpp
cofetch::Request req("https://example.com");
req.method(cofetch::Request::Method::POST)   // GET | POST | PUT | PATCH | DEL
   .headers({"content-type: text/plain"})
   .body("hello")
   .timeout(std::chrono::milliseconds(2500))
   .follow_redirects(10)
   .curl([](CURL* h) { /* per-request libcurl tweaks */ });
```

`Request::Method` — `GET`, `POST`, `PUT`, `PATCH`, `DEL`.

---

## cofetch::Response

Delivered to the completion handler; public fields plus two helpers.

```cpp
void on_done(cofetch::error_code ec, const cofetch::Response& r) {
  r.is_ok();       // true when the transfer succeeded AND status is 2xx
  r.http_code_;    // long     — HTTP status (0 if the transfer failed)
  r.data_;         // string   — response body (already decompressed)
  r.header_data_;  // string   — raw response headers
  r.curl_code_;    // CURLcode — transport result
  r.error();       // const char* — human-readable transport error
}
```

---

## Errors

Transport failures arrive as a `cofetch::error_code` carrying the `CURLcode`
under `cofetch::curl_category()`. HTTP error statuses are **not** transport
errors — check `Response::is_ok()` / `http_code_` for those.

```cpp
if (ec && ec.category() == cofetch::curl_category())
  std::cerr << ec.message();  // == curl_easy_strerror(CURLcode)

auto ec2 = cofetch::make_error_code(CURLE_COULDNT_CONNECT);  // build one yourself
```

`cofetch::error_code` is `std::error_code`, or `boost::system::error_code` when
built with Boost.Asio (below).

---

## Build-time options

| macro | effect |
|---|---|
| `COFETCH_USE_BOOST_ASIO` | build against Boost.Asio instead of standalone asio; `cofetch::error_code` becomes `boost::system::error_code` so Boost completion tokens recognise it |

```cpp
// CMake: -DCOFETCH_USE_BOOST_ASIO=ON, or before the include:
#define COFETCH_USE_BOOST_ASIO
#include <cofetch.h>
```
