#pragma once
// cofetch: async HTTP client on top of libcurl's multi interface and ASIO.
//
// One implementation, any ASIO completion token. C++17 and up; the
// co_await interface additionally needs C++20.
//
//   // fluent chain, finished by the HTTP verb (zero-overhead hot path):
//   http.request(url)
//       .headers({"content-type: application/json"})
//       .body(payload)
//       .post([](std::error_code ec, const cofetch::Response& res) {});
//
//   // std::future:
//   auto fut = http.async_get(url, asio::use_future);
//
//   // .then()-style chaining (see test/cofetch_tests.cpp):
//   http.async_get(url, asio::deferred)(asio::deferred(next))(handler);
//
//   // C++20 coroutine:
//   auto res = co_await http.async_get(url, asio::use_awaitable);
//
// Drive it with io_context::run(), or io_context::poll() in a busy loop.
// Requests are cancellable through asio's cancellation slots
// (asio::cancel_after, asio::bind_cancellation_slot, co_spawn).
// Not thread-safe: run the client and its io_context on one thread.
// Define COFETCH_USE_BOOST_ASIO to build on Boost.Asio instead of
// standalone asio. The API is identical; cofetch::error_code follows the
// flavor (std::error_code, or boost::system::error_code) — curl category.
#if defined(COFETCH_USE_BOOST_ASIO)
#include <boost/asio.hpp>
#else
#include <asio.hpp>
#endif
//
#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>   // F_DUPFD_CLOEXEC: watching descriptors curl owns
#include <unistd.h>  // ::close
#endif

namespace cofetch {

#if defined(COFETCH_USE_BOOST_ASIO)
namespace net = boost::asio;
// The error type follows the asio flavor so completion tokens recognise
// it (Boost.Asio only unwraps boost::system::error_code).
using error_code = boost::system::error_code;
using error_category = boost::system::error_category;
#else
namespace net = asio;
using error_code = std::error_code;
using error_category = std::error_category;
#endif

/**
 * @brief std::error_category for libcurl transport errors (CURLcode values).
 */
inline const error_category& curl_category() {
  class category final : public error_category {
   public:
    const char* name() const noexcept override { return "curl"; }
    std::string message(int ev) const override {
      return curl_easy_strerror(static_cast<CURLcode>(ev));
    }
  };
  static category instance;
  return instance;
}

inline error_code make_error_code(CURLcode code) {
  return {static_cast<int>(code), curl_category()};
}

namespace detail {

// HTTP field names are case-insensitive (RFC 9110 §5.1), so the header map
// hashes and compares them without regard to case. ASCII-only folding — field
// names are ASCII tokens — which also sidesteps std::tolower's locale and
// signed-char pitfalls. Both are transparent, so on C++20 the map accepts
// std::string_view lookups without building a temporary std::string.
inline char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
struct CiHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const noexcept {
    size_t h = 0;
    for (char c : s) h = h * 31 + static_cast<unsigned char>(ascii_lower(c));
    return h;
  }
};
struct CiEqual {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
  }
};

}  // namespace detail

class Response {
 public:
  // Case-insensitive field-name -> value map (see headers()).
  using Headers = std::unordered_map<std::string, std::string, detail::CiHash,
                                     detail::CiEqual>;

  Response() = default;
  Response(CURLcode curl_code, long http_code, std::string data,
           std::string header_data)
      : curl_code_(curl_code),
        http_code_(http_code),
        data_(std::move(data)),
        header_data_(std::move(header_data)) {}

  /**
   * @brief True when the transfer succeeded and the HTTP status is 2xx.
   */
  bool is_ok() const {
    return curl_code_ == CURLE_OK && http_code_ >= 200 && http_code_ < 300;
  }

  /**
   * @brief Human readable description of the transport error ("No error" when
   * the transfer itself succeeded).
   */
  const char* error() const { return curl_easy_strerror(curl_code_); }

  /**
   * @brief Parse the response headers into a case-insensitive name->value
   * map. Repeated fields are comma-combined (RFC 9110 §5.3). Parsed on
   * demand and not cached: keep the result if you read it repeatedly, and
   * for a single field prefer header(), which skips building the whole map.
   * Only the final response counts: when redirects are followed, or the
   * server sent a 1xx interim response, header_data_ holds one block per
   * hop and the earlier hops' fields are skipped.
   */
  Headers headers() const {
    Headers out;
    for_each_field([&](std::string_view name, std::string_view value) {
      const auto [it, inserted] = out.try_emplace(std::string(name), value);
      if (!inserted) {
        it->second += ", ";
        it->second += value;
      }
    });
    return out;
  }

  /**
   * @brief Case-insensitive lookup of one field without building the full
   * map; repeats are comma-combined. std::nullopt when the field is absent.
   * As with headers(), only the final response's block is searched.
   */
  std::optional<std::string> header(std::string_view name) const {
    std::optional<std::string> found;
    for_each_field([&](std::string_view field, std::string_view value) {
      if (!detail::CiEqual{}(field, name)) return;
      if (found) {
        *found += ", ";
        *found += value;
      } else {
        found = std::string(value);
      }
    });
    return found;
  }

  CURLcode curl_code_ = CURLE_OK;
  long http_code_ = 0;
  std::string data_;
  std::string header_data_;

 private:
  // Iterate the "name: value" fields of the last response block in
  // header_data_, trimmed, skipping the status line and blank separators.
  // The views point into header_data_, so they are valid only for the
  // lifetime of this Response.
  template <typename F>
  void for_each_field(F&& f) const {
    std::string_view sv(header_data_);
    // Each hop's block opens with a status line. Field names are tokens and
    // cannot contain '/', so a line starting with "HTTP/" is only ever a
    // status line: the last one marks where the final response begins.
    const size_t last_status = sv.rfind("\nHTTP/");
    size_t pos = (last_status == std::string_view::npos) ? 0 : last_status + 1;
    while (pos < sv.size()) {
      const size_t nl = sv.find('\n', pos);
      std::string_view line =
          sv.substr(pos, (nl == std::string_view::npos ? sv.size() : nl) - pos);
      pos = (nl == std::string_view::npos) ? sv.size() : nl + 1;
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos) continue;  // status line or blank
      f(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
  }

  // Strip leading/trailing HTTP optional whitespace (space and htab).
  static std::string_view trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
  }
};

/**
 * @brief Value-type description of a request; pass to Client::async_perform.
 */
class Request {
 public:
  enum class Method { GET, POST, PUT, PATCH, DEL };

  explicit Request(std::string url) : url_(std::move(url)) {}

  Request& method(Method m) {
    method_ = m;
    return *this;
  }
  Request& headers(std::vector<std::string> h) {
    headers_ = std::move(h);
    return *this;
  }
  Request& body(std::string b) {
    body_ = std::move(b);
    return *this;
  }
  /**
   * @brief Whole-transfer timeout (default 5s), millisecond resolution.
   * Note libcurl's stock DNS resolver still rounds the name-resolution
   * phase up to whole seconds.
   */
  Request& timeout(std::chrono::milliseconds t) {
    timeout_ = t;
    return *this;
  }
  /**
   * @brief Follow HTTP 3xx redirects, at most max hops (the transfer fails
   * with CURLE_TOO_MANY_REDIRECTS beyond that). Off by default.
   */
  Request& follow_redirects(long max = 30) {
    max_redirects_ = max;
    return *this;
  }
  /**
   * @brief Escape hatch: fn runs on the underlying easy handle after
   * cofetch's own options, so it can set (or override) any CURLOPT_*.
   * The handle is scrubbed with curl_easy_reset before returning to the
   * reuse pool, so options set here never leak into later requests.
   */
  Request& curl(std::function<void(CURL*)> fn) {
    curl_setup_ = std::move(fn);
    return *this;
  }

  std::string url_;
  Method method_ = Method::GET;
  std::vector<std::string> headers_;
  std::string body_;
  std::chrono::milliseconds timeout_{std::chrono::seconds{5}};
  long max_redirects_ = 0;  // 0: do not follow redirects
  std::function<void(CURL*)> curl_setup_;
};

class Client {
 public:
  /**
   * @brief Construct a client driven by io. max_pooled_connections caps the
   * idle easy handles kept for reuse; raise it for servers that keep many
   * connections hot at once (default 64).
   */
  explicit Client(net::io_context& io,
                  size_t max_pooled_connections = kDefaultMaxPooledHandles)
      : io_(io), timer_(io), max_pooled_handles_(max_pooled_connections) {
    {
      // Reference counted inside libcurl, but only thread-safe from 7.84
      // on: serialise clients constructed on different threads at once
      // (one event loop per core).
      const std::lock_guard<std::mutex> lock(global_mutex());
      curl_global_init(CURL_GLOBAL_ALL);
    }
    multi_ = curl_multi_init();
    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, socket_cb);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, timer_cb);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
  }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  /**
   * @brief Destroy the client. Requests still in flight are dropped and
   * their completion handlers are never invoked; prefer draining the
   * io_context first.
   */
  ~Client() {
    *alive_ = false;
    timer_.cancel();
    // Sever every socket first: pending waits complete with
    // operation_aborted (and find no state), and curl's shutdown attempts
    // below fail at once instead of waiting on peers.
    for (const auto& [fd, state] : sockets_) {
      state->watch = 0;
      error_code ignored;
      state->socket.close(ignored);
    }
    // Transfers still in flight: detach them so their handles and header
    // lists can be freed. The handlers go down with transfers_, uninvoked.
    for (const auto& t : transfers_) {
      curl_multi_remove_handle(multi_, t.eh);
      curl_slist_free_all(t.headers);
      curl_easy_cleanup(t.eh);
    }
    for (const auto& t : idle_) curl_easy_cleanup(t.eh);
    curl_multi_cleanup(multi_);  // closes cached connections (close_socket_cb)
    const std::lock_guard<std::mutex> lock(global_mutex());
    curl_global_cleanup();
  }

  /**
   * @brief Start a transfer described by req. Completion signature is
   * void(std::error_code, Response): the error_code carries the CURLcode
   * (curl_category) for transport failures and is empty otherwise; HTTP
   * error statuses are not transport failures (check Response::is_ok()).
   *
   * If the completion handler has an associated cancellation slot
   * (asio::bind_cancellation_slot, asio::cancel_after, co_spawn's
   * cancellation state, ...), emitting cancellation aborts the transfer
   * and the handler completes with asio::error::operation_aborted.
   */
  template <typename CompletionToken>
  auto async_perform(Request req, CompletionToken&& token) {
    return net::async_initiate<CompletionToken, void(error_code, Response)>(
        Initiation{this}, token, std::move(req));
  }

  template <typename CompletionToken>
  auto async_get(std::string url, CompletionToken&& token) {
    return async_perform(Request(std::move(url)),
                         std::forward<CompletionToken>(token));
  }

  template <typename CompletionToken>
  auto async_post(std::string url, std::string body, CompletionToken&& token) {
    return async_perform(Request(std::move(url))
                             .method(Request::Method::POST)
                             .body(std::move(body)),
                         std::forward<CompletionToken>(token));
  }

  /**
   * @brief Fluent builder bound to this client. Chain setters and finish
   * with the HTTP verb, which starts the transfer:
   *
   *   co_await http.request(url).body("b=1").post(asio::use_awaitable);
   *
   * A builder must not be reused after get()/post()/put()/del().
   */
  class RequestBuilder {
   public:
    RequestBuilder(Client& client, std::string url)
        : client_(client), req_(std::move(url)) {}

    RequestBuilder& headers(std::vector<std::string> h) {
      req_.headers(std::move(h));
      return *this;
    }
    RequestBuilder& body(std::string b) {
      req_.body(std::move(b));
      return *this;
    }
    RequestBuilder& timeout(std::chrono::milliseconds t) {
      req_.timeout(t);
      return *this;
    }
    RequestBuilder& follow_redirects(long max = 30) {
      req_.follow_redirects(max);
      return *this;
    }
    RequestBuilder& curl(std::function<void(CURL*)> fn) {
      req_.curl(std::move(fn));
      return *this;
    }

    template <typename CompletionToken>
    auto get(CompletionToken&& token) {
      return perform(Request::Method::GET,
                     std::forward<CompletionToken>(token));
    }
    template <typename CompletionToken>
    auto post(CompletionToken&& token) {
      return perform(Request::Method::POST,
                     std::forward<CompletionToken>(token));
    }
    template <typename CompletionToken>
    auto put(CompletionToken&& token) {
      return perform(Request::Method::PUT,
                     std::forward<CompletionToken>(token));
    }
    template <typename CompletionToken>
    auto patch(CompletionToken&& token) {
      return perform(Request::Method::PATCH,
                     std::forward<CompletionToken>(token));
    }
    template <typename CompletionToken>
    auto del(CompletionToken&& token) {
      return perform(Request::Method::DEL,
                     std::forward<CompletionToken>(token));
    }

   private:
    template <typename CompletionToken>
    auto perform(Request::Method m, CompletionToken&& token) {
      req_.method(m);
      return client_.async_perform(std::move(req_),
                                   std::forward<CompletionToken>(token));
    }

    Client& client_;
    Request req_;
  };

  /**
   * @brief Start a fluent request chain: request(url).body(...).post(token).
   */
  RequestBuilder request(std::string url) {
    return RequestBuilder(*this, std::move(url));
  }

  int pending_requests() const { return running_; }

 private:
  using Handler = net::any_completion_handler<void(error_code, Response)>;

  // Initiation for async_perform. Exposing the io_context executor lets
  // executor-aware tokens work — asio::cancel_after, for one, builds its
  // timeout timer on Initiation::executor_type.
  struct Initiation {
    Client* self;
    using executor_type = net::io_context::executor_type;
    executor_type get_executor() const noexcept {
      return self->io_.get_executor();
    }
    template <typename H>
    void operator()(H handler, Request r) const {
      self->start(std::move(r), Handler(std::move(handler)));
    }
  };

  // Default cap on idle easy handles kept for reuse; beyond this they are
  // freed so a burst of concurrent requests does not pin memory forever.
  // Overridable per client through the constructor.
  static constexpr size_t kDefaultMaxPooledHandles = 64;

  // Pre-size for the response header block: curl delivers it one line per
  // callback, and growing a std::string from empty takes several
  // reallocations to reach the few hundred bytes a typical block needs.
  static constexpr size_t kHeaderReserve = 1024;

  // One transfer's state. Nodes live in transfers_ while in flight and
  // move (spliced, never reallocated) to idle_ afterwards, together with
  // their configured easy handle, so a request costs neither a node nor a
  // handle allocation once the client is warm.
  struct Transfer {
    explicit Transfer(CURL* e) : eh(e) {}
    CURL* eh;
    curl_slist* headers = nullptr;
    std::string body;
    std::string buffer;
    std::string header_buffer;
    Handler handler;
    std::list<Transfer>::iterator self;
    std::uint64_t id = 0;
    // Set when Request::curl ran on this handle: unknown options must be
    // wiped (curl_easy_reset + configure_handle) before the handle is
    // pooled, or they would leak into whatever request draws it next.
    bool scrub_on_done = false;
  };

  struct SocketState : std::enable_shared_from_this<SocketState> {
    SocketState(net::io_context& io, bool borrowed_fd)
        : socket(io), borrowed(borrowed_fd) {}
    net::ip::tcp::socket socket;
    // The descriptor as curl knows it. For our own sockets that is
    // socket.native_handle(); for borrowed ones socket holds a dup.
    curl_socket_t fd = CURL_SOCKET_BAD;
    const bool borrowed;
    int watch = 0;  // current CURL_POLL_* interest
    bool read_armed = false;
    bool write_armed = false;
  };

  void start(Request r, Handler h) {
    if (idle_.empty()) {
      CURL* const eh = curl_easy_init();
      if (eh == nullptr) return fail(std::move(h), CURLE_FAILED_INIT);
      configure_handle(eh);
      transfers_.emplace_front(eh);
    } else {
      // Idle nodes keep their configured handle: no allocation, no reset.
      transfers_.splice(transfers_.begin(), idle_, idle_.begin());
    }
    const auto it = transfers_.begin();
    Transfer& t = *it;
    CURL* const eh = t.eh;
    t.self = it;
    t.handler = std::move(h);
    t.body = std::move(r.body_);
    t.id = ++next_transfer_id_;

    curl_easy_setopt(eh, CURLOPT_URL, r.url_.c_str());
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(eh, CURLOPT_HEADERDATA, &t.header_buffer);
    curl_easy_setopt(eh, CURLOPT_PRIVATE, &t);
    curl_easy_setopt(eh, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(r.timeout_.count()));
    // Always set: clears the previous transfer's values on pooled handles.
    curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION,
                     r.max_redirects_ != 0 ? 1L : 0L);
    curl_easy_setopt(eh, CURLOPT_MAXREDIRS, r.max_redirects_);

    curl_slist* chunk = nullptr;
    for (const auto& header : r.headers_) {
      chunk = curl_slist_append(chunk, header.c_str());
    }
    // Always set: clears the previous transfer's list on pooled handles.
    curl_easy_setopt(eh, CURLOPT_HTTPHEADER, chunk);
    t.headers = chunk;

    switch (r.method_) {
      case Request::Method::GET:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, nullptr);
        // Drop the previous transfer's body pointer (freed with it) so a
        // Request::curl hook turning this into an upload cannot read it.
        // POSTFIELDS selects POST, so it goes before HTTPGET.
        curl_easy_setopt(eh, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(eh, CURLOPT_HTTPGET, 1L);
        break;
      case Request::Method::POST:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(t);
        break;
      case Request::Method::PUT:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(t);
        break;
      case Request::Method::PATCH:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(t);
        break;
      case Request::Method::DEL:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(t);
        break;
    }

    // The escape hatch runs last so it can override anything above.
    if (r.curl_setup_) {
      t.scrub_on_done = true;
      r.curl_setup_(eh);
    }

    auto slot = net::get_associated_cancellation_slot(t.handler);
    if (slot.is_connected()) {
      // Look the transfer up by id at emit time: the slot outlives the
      // transfer (asio only guarantees clearing on handler destruction),
      // so a late emit must find nothing rather than follow a dangling
      // pointer. alive_ covers emits after ~Client.
      slot.assign([this, alive = alive_, id = t.id](net::cancellation_type_t) {
        if (*alive) cancel(id);
      });
    }

    // curl schedules the kickstart itself through the timer callback.
    const CURLMcode rc = curl_multi_add_handle(multi_, eh);
    if (rc != CURLM_OK) {
      // Out of memory, realistically. Never leave a handler uncalled.
      Handler handler = std::move(t.handler);
      retire(t, /*scrub=*/true);
      fail(std::move(handler),
           rc == CURLM_OUT_OF_MEMORY ? CURLE_OUT_OF_MEMORY : CURLE_FAILED_INIT);
    }
  }

  // Complete h with a transport error. Posted, not invoked: asio forbids
  // running a completion handler from inside the initiating function.
  void fail(Handler h, CURLcode code) {
    net::post(io_, net::bind_allocator(net::recycling_allocator<void>(),
                                       [h = std::move(h), code]() mutable {
                                         std::move(h)(
                                             make_error_code(code),
                                             Response{code, 0, {}, {}});
                                       }));
  }

  // Cooperative cancellation, reached through the completion handler's
  // associated cancellation slot. Any cancellation type aborts: the
  // transfer is torn down and the handler completes with
  // operation_aborted. No-op when the transfer already completed.
  void cancel(std::uint64_t id) {
    for (auto& t : transfers_) {
      if (t.id != id) continue;
      // Also discards any DONE message this handle queued in the multi.
      curl_multi_remove_handle(multi_, t.eh);
      if (running_ > 0) --running_;
      Handler handler = std::move(t.handler);
      // Severed mid-flight: scrub before the handle is reused.
      retire(t, /*scrub=*/true);
      // Unlike normal completions this one is posted, not invoked: we are
      // inside the cancellation emit, and completing here can destroy the
      // very signal being emitted (asio::cancel_after owns its signal in
      // the operation state the completion frees).
      net::post(io_, net::bind_allocator(
                         net::recycling_allocator<void>(),
                         [h = std::move(handler)]() mutable {
                           std::move(h)(
                               error_code(net::error::operation_aborted),
                               Response{CURLE_ABORTED_BY_CALLBACK, 0, {}, {}});
                         }));
      return;
    }
  }

  // Done with t (handler already moved out, handle no longer in the multi):
  // return the node and its easy handle to the idle pool for the next
  // request, or free them when the pool is full. scrub wipes the handle's
  // options first — needed after Request::curl ran on it, or when the
  // transfer was cut short.
  void retire(Transfer& t, bool scrub) {
    curl_slist_free_all(t.headers);
    t.headers = nullptr;
    t.id = 0;
    t.scrub_on_done = false;
    t.buffer.clear();  // moved-from: valid but unspecified until cleared
    t.header_buffer.clear();
    std::string().swap(t.body);  // a large upload must not sit in the pool
    if (scrub) {
      curl_easy_reset(t.eh);
      configure_handle(t.eh);
    }
    if (idle_.size() < max_pooled_handles_) {
      idle_.splice(idle_.begin(), transfers_, t.self);
    } else {
      curl_easy_cleanup(t.eh);
      transfers_.erase(t.self);
    }
  }

  // Request-independent options, set once per easy handle. Everything a
  // transfer can vary must be (re)set in start() — pooled handles are
  // reused without curl_easy_reset.
  void configure_handle(CURL* eh) {
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, body_write_cb);
    curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075000  // 7.80.0
    // Cap connection reuse age; on older libcurl the option is absent and
    // pooled connections simply live longer.
    curl_easy_setopt(eh, CURLOPT_MAXLIFETIME_CONN, 30L);
#endif
    // "" advertises every decoder curl was built with (gzip, br, ...).
    curl_easy_setopt(eh, CURLOPT_ACCEPT_ENCODING, "");
    // Wait for an in-progress connection to the same host and multiplex over
    // it (HTTP/2) instead of racing to open one connection per request.
    curl_easy_setopt(eh, CURLOPT_PIPEWAIT, 1L);
    curl_easy_setopt(eh, CURLOPT_BUFFERSIZE, 512L * 1024L);
    curl_easy_setopt(eh, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    curl_easy_setopt(eh, CURLOPT_OPENSOCKETDATA, this);
    curl_easy_setopt(eh, CURLOPT_CLOSESOCKETFUNCTION, close_socket_cb);
    curl_easy_setopt(eh, CURLOPT_CLOSESOCKETDATA, this);
  }

  void set_body(const Transfer& t) {
    curl_easy_setopt(t.eh, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(t.body.size()));
    curl_easy_setopt(t.eh, CURLOPT_POSTFIELDS, t.body.c_str());
  }

  static size_t header_cb(char* data, size_t n, size_t l, std::string* buf) {
    if (buf->empty() && buf->capacity() < kHeaderReserve) {
      buf->reserve(kHeaderReserve);
    }
    buf->append(data, n * l);
    return n * l;
  }

  static size_t body_write_cb(char* data, size_t n, size_t l, Transfer* t) {
    if (t->buffer.empty()) {
      curl_off_t len = 0;
      if (curl_easy_getinfo(t->eh, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len) ==
              CURLE_OK &&
          len > 0) {
        t->buffer.reserve(static_cast<size_t>(len));
      }
    }
    t->buffer.append(data, n * l);
    return n * l;
  }

  // curl asks us (not the OS directly) for sockets, so every fd it uses is
  // backed by an ASIO object we can async_wait on. Cross-platform, no epoll.
  static curl_socket_t open_socket_cb(void* clientp, curlsocktype purpose,
                                      curl_sockaddr* address) {
    auto* const self = static_cast<Client*>(clientp);
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    net::ip::tcp protocol = net::ip::tcp::v4();
    if (address->family == AF_INET6) {
      protocol = net::ip::tcp::v6();
    } else if (address->family != AF_INET) {
      return CURL_SOCKET_BAD;
    }
    auto state =
        std::make_shared<SocketState>(self->io_, /*borrowed_fd=*/false);
    error_code ec;
    state->socket.open(protocol, ec);
    if (ec) return CURL_SOCKET_BAD;
    const curl_socket_t fd = state->socket.native_handle();
    state->fd = fd;
    self->sockets_[fd] = std::move(state);
    return fd;
  }

  static int close_socket_cb(void* clientp, curl_socket_t fd) {
    auto* const self = static_cast<Client*>(clientp);
    const auto it = self->sockets_.find(fd);
    if (it == self->sockets_.end()) {
      // Opened past open_socket_cb (a Request::curl hook replaced it):
      // still ours to close, as the callback contract says.
      close_native(fd);
      return 0;
    }
    SocketState& state = *it->second;
    state.watch = 0;
    error_code ignored;
    state.socket.close(ignored);
    if (state.borrowed) close_native(fd);  // we only closed our dup
    self->sockets_.erase(it);
    return 0;
  }

  static void close_native(curl_socket_t fd) {
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
  }

  // curl also hands us descriptors it created itself: the threaded
  // resolver's wake-up pipe, c-ares' UDP sockets. Watch a dup of those,
  // so the lifetime of curl's own descriptor stays curl's business (it may
  // close it any time after CURL_POLL_REMOVE, and the number may be
  // reused). Ignoring them keeps things working — curl's timer polls the
  // resolver at 1 ms doubling up to 250 ms — but every hostname lookup is
  // then noticed late, up to twice its real duration.
  SocketState* borrow(curl_socket_t fd) {
#if defined(_WIN32)
    (void)fd;
    return nullptr;  // no dup for SOCKETs; curl's polling covers it
#else
    const int dup_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) return nullptr;
    auto state = std::make_shared<SocketState>(io_, /*borrowed_fd=*/true);
    error_code ec;
    // The protocol is a label here (the descriptor may be a socketpair end
    // or an eventfd); only the reactor registration matters to async_wait.
    state->socket.assign(net::ip::tcp::v4(), dup_fd, ec);
    if (ec) {
      ::close(dup_fd);
      return nullptr;
    }
    state->fd = fd;
    SocketState* const raw = state.get();
    sockets_[fd] = std::move(state);
    return raw;
#endif
  }

  static int socket_cb(CURL*, curl_socket_t fd, int what, void* userp,
                       void* socketp) {
    auto* const self = static_cast<Client*>(userp);
    auto* state = static_cast<SocketState*>(socketp);
    if (state == nullptr) {
      if (what == CURL_POLL_REMOVE) return 0;  // never watched: nothing to do
      // First notification for this descriptor: attach the state so curl
      // hands it back on later calls and we skip the lookup.
      const auto it = self->sockets_.find(fd);
      state =
          (it != self->sockets_.end()) ? it->second.get() : self->borrow(fd);
      if (state == nullptr) return 0;
      curl_multi_assign(self->multi_, fd, state);
    }
    if (what == CURL_POLL_REMOVE) {
      state->watch = 0;
      if (state->borrowed) {
        // curl closes its descriptor right after this; drop our dup now (a
        // later re-add borrows afresh). Our own sockets stay: the
        // connection is merely idle in curl's cache.
        error_code ignored;
        state->socket.close(ignored);
        self->sockets_.erase(fd);
      }
      return 0;
    }
    state->watch = what;
    self->arm(state->shared_from_this());
    return 0;
  }

  void arm(const std::shared_ptr<SocketState>& state) {
    if (!state->socket.is_open()) return;
    if ((state->watch & CURL_POLL_IN) && !state->read_armed) {
      state->read_armed = true;
      state->socket.async_wait(
          net::ip::tcp::socket::wait_read,
          net::bind_allocator(
              net::recycling_allocator<void>(),
              [this, w = std::weak_ptr<SocketState>(state)](error_code ec) {
                // Lock before touching the client: a state outlives
                // ~Client only inside a queued completion like this one,
                // and then the lock fails.
                if (auto s = w.lock())
                  on_event(std::move(s), CURL_CSELECT_IN, ec);
              }));
    }
    if ((state->watch & CURL_POLL_OUT) && !state->write_armed) {
      state->write_armed = true;
      state->socket.async_wait(
          net::ip::tcp::socket::wait_write,
          net::bind_allocator(
              net::recycling_allocator<void>(),
              [this, w = std::weak_ptr<SocketState>(state)](error_code ec) {
                if (auto s = w.lock())
                  on_event(std::move(s), CURL_CSELECT_OUT, ec);
              }));
    }
  }

  void on_event(std::shared_ptr<SocketState> state, int flag, error_code ec) {
    // The shared_ptr keeps the state alive across the socket_action call
    // below, which may close this very socket via close_socket_cb (or drop
    // a borrowed one on CURL_POLL_REMOVE).
    (flag == CURL_CSELECT_IN ? state->read_armed : state->write_armed) = false;
    if (ec == net::error::operation_aborted) return;
    curl_multi_socket_action(multi_, state->fd, ec ? CURL_CSELECT_ERR : flag,
                             &running_);
    check_completions();
    if (state->watch != 0) arm(state);
  }

  static int timer_cb(CURLM*, long timeout_ms, void* userp) {
    auto* const self = static_cast<Client*>(userp);
    if (timeout_ms < 0) {
      self->timer_.cancel();
      self->timer_armed_ = false;
      return 0;
    }
    if (timeout_ms == 0) {
      // "Act as soon as possible" — the common per-transfer kick. A plain
      // post (deduplicated) is much cheaper than rescheduling the timer,
      // and we may not call curl back from inside its own callback.
      if (!self->kick_pending_) {
        self->kick_pending_ = true;
        net::post(self->io_,
                  net::bind_allocator(net::recycling_allocator<void>(),
                                      [self, alive = self->alive_] {
                                        if (!*alive) return;
                                        self->kick_pending_ = false;
                                        self->kick();
                                      }));
      }
      return 0;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    // A pending wait that fires no later than the new deadline is good
    // enough: a kick() finding nothing due is a cheap no-op, while
    // rescheduling reprograms the timer every time.
    if (self->timer_armed_ && self->timer_.expiry() <= deadline) return 0;
    self->timer_.expires_at(deadline);
    self->timer_armed_ = true;
    self->timer_.async_wait(
        net::bind_allocator(net::recycling_allocator<void>(),
                            [self, alive = self->alive_](error_code ec) {
                              if (ec || !*alive) return;
                              self->timer_armed_ = false;
                              self->kick();
                            }));
    return 0;
  }

  void kick() {
    curl_multi_socket_action(multi_, CURL_SOCKET_TIMEOUT, 0, &running_);
    check_completions();
  }

  void check_completions() {
    int msgs_left = 0;
    while (CURLMsg* msg = curl_multi_info_read(multi_, &msgs_left)) {
      if (msg->msg != CURLMSG_DONE) continue;
      Transfer* t = nullptr;
      long http_code = 0;
      // msg must not be dereferenced after curl_multi_remove_handle().
      const CURLcode curl_code = msg->data.result;
      CURL* const eh = msg->easy_handle;
      curl_easy_getinfo(eh, CURLINFO_PRIVATE, &t);
      curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);
      curl_multi_remove_handle(multi_, eh);

      Response res{curl_code, http_code, std::move(t->buffer),
                   std::move(t->header_buffer)};
      Handler handler = std::move(t->handler);
      retire(*t, t->scrub_on_done);

      const error_code ec =
          curl_code == CURLE_OK ? error_code{} : make_error_code(curl_code);
      // Single-threaded by contract: the handler's executor is this
      // io_context, where we already are — invoke without the
      // type-erased dispatch hop.
      std::move(handler)(ec, std::move(res));
    }
  }

  static std::mutex& global_mutex() {
    static std::mutex m;
    return m;
  }

  net::io_context& io_;
  CURLM* multi_;
  net::steady_timer timer_;
  // Keyed by the descriptor curl reports (SocketState::fd).
  std::unordered_map<curl_socket_t, std::shared_ptr<SocketState>> sockets_;
  std::list<Transfer> transfers_;    // in flight
  std::list<Transfer> idle_;         // warm nodes + handles for reuse (LIFO)
  const size_t max_pooled_handles_;  // cap on idle_; set by the constructor
  int running_ = 0;
  std::uint64_t next_transfer_id_ = 0;
  bool kick_pending_ = false;
  bool timer_armed_ = false;
  // Outlives the client inside posted/timed kicks: they bail out when the
  // client is gone instead of touching a destroyed multi handle.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace cofetch
