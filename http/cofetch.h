#pragma once
// cofetch: async HTTP client on top of libcurl's multi interface and ASIO.
//
// One implementation, any ASIO completion token. C++17 and up; the
// co_await interface additionally needs C++20.
//
//   // fluent chain, finished by the HTTP verb (zero-overhead hot path):
//   client.request(url)
//       .headers({"content-type: application/json"})
//       .body(payload)
//       .post([](std::error_code ec, cofetch::Response res) {});
//
//   // std::future:
//   auto fut = client.async_get(url, asio::use_future);
//
//   // .then()-style chaining (see test/cofetch_tests.cpp):
//   client.async_get(url, asio::deferred)(asio::deferred(next))(handler);
//
//   // C++20 coroutine:
//   auto res = co_await client.async_get(url, asio::use_awaitable);
//
// Drive it with io_context::run(), or io_context::poll() in a busy loop.
// Not thread-safe: run the client and its io_context on one thread.
#include <asio.hpp>
//
#include <curl/curl.h>

#include <chrono>
#include <list>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cofetch {

/**
 * @brief std::error_category for libcurl transport errors (CURLcode values).
 */
inline const std::error_category& curl_category() {
  class category final : public std::error_category {
   public:
    const char* name() const noexcept override { return "curl"; }
    std::string message(int ev) const override {
      return curl_easy_strerror(static_cast<CURLcode>(ev));
    }
  };
  static category instance;
  return instance;
}

inline std::error_code make_error_code(CURLcode code) {
  return {static_cast<int>(code), curl_category()};
}

class Response {
 public:
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

  CURLcode curl_code_ = CURLE_OK;
  long http_code_ = 0;
  std::string data_;
  std::string header_data_;
};

/**
 * @brief Value-type description of a request; pass to Client::async_perform.
 */
class Request {
 public:
  enum class Method { GET, POST, PUT, DEL };

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
  Request& timeout(std::chrono::seconds t) {
    timeout_ = t;
    return *this;
  }

  std::string url_;
  Method method_ = Method::GET;
  std::vector<std::string> headers_;
  std::string body_;
  std::chrono::seconds timeout_{5};
};

class Client {
 public:
  explicit Client(asio::io_context& io) : io_(io), timer_(io) {
    curl_global_init(CURL_GLOBAL_ALL);
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
    for (auto& [fd, state] : sockets_) {
      std::error_code ignored;
      state->socket.close(ignored);
    }
    curl_multi_cleanup(multi_);
    for (CURL* eh : pool_) {
      curl_easy_cleanup(eh);
    }
    curl_global_cleanup();
  }

  /**
   * @brief Start a transfer described by req. Completion signature is
   * void(std::error_code, Response): the error_code carries the CURLcode
   * (curl_category) for transport failures and is empty otherwise; HTTP
   * error statuses are not transport failures (check Response::is_ok()).
   */
  template <typename CompletionToken>
  auto async_perform(Request req, CompletionToken&& token) {
    return asio::async_initiate<CompletionToken,
                                void(std::error_code, Response)>(
        [this](auto handler, Request r) {
          start(std::move(r), Handler(std::move(handler)));
        },
        token, std::move(req));
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
   *   co_await client.request(url).body("b=1").post(asio::use_awaitable);
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
    RequestBuilder& timeout(std::chrono::seconds t) {
      req_.timeout(t);
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
  using Handler = asio::any_completion_handler<void(std::error_code, Response)>;

  // Idle easy handles kept for reuse; beyond this they are freed so a burst
  // of concurrent requests does not pin memory forever.
  static constexpr size_t kMaxPooledHandles = 64;

  struct Transfer {
    Transfer(CURL* e, Handler h) : eh(e), handler(std::move(h)) {}
    CURL* eh;
    curl_slist* headers = nullptr;
    std::string body;
    std::string buffer;
    std::string header_buffer;
    Handler handler;
    std::list<Transfer>::iterator self;
  };

  struct SocketState : std::enable_shared_from_this<SocketState> {
    explicit SocketState(asio::io_context& io) : socket(io) {}
    asio::ip::tcp::socket socket;
    int watch = 0;  // current CURL_POLL_* interest
    bool read_armed = false;
    bool write_armed = false;
  };

  void start(Request r, Handler h) {
    CURL* eh = nullptr;
    if (pool_.empty()) {
      eh = curl_easy_init();
      configure_handle(eh);
    }  // pooled handles keep their static options; no curl_easy_reset
    else {
      eh = pool_.back();
      pool_.pop_back();
    }

    transfers_.emplace_front(eh, std::move(h));
    const auto it = transfers_.begin();
    it->self = it;
    it->body = std::move(r.body_);

    curl_easy_setopt(eh, CURLOPT_URL, r.url_.c_str());
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &*it);
    curl_easy_setopt(eh, CURLOPT_HEADERDATA, &it->header_buffer);
    curl_easy_setopt(eh, CURLOPT_PRIVATE, &*it);
    curl_easy_setopt(eh, CURLOPT_TIMEOUT,
                     static_cast<long>(r.timeout_.count()));

    curl_slist* chunk = nullptr;
    for (const auto& header : r.headers_) {
      chunk = curl_slist_append(chunk, header.c_str());
    }
    // Always set: clears the previous transfer's list on pooled handles.
    curl_easy_setopt(eh, CURLOPT_HTTPHEADER, chunk);
    it->headers = chunk;

    switch (r.method_) {
      case Request::Method::GET:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(eh, CURLOPT_HTTPGET, 1L);
        break;
      case Request::Method::POST:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(*it);
        break;
      case Request::Method::PUT:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(*it);
        break;
      case Request::Method::DEL:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(*it);
        break;
    }

    // curl schedules the kickstart itself through the timer callback.
    curl_multi_add_handle(multi_, eh);
  }

  // Request-independent options, set once per easy handle. Everything a
  // transfer can vary must be (re)set in start() — pooled handles are
  // reused without curl_easy_reset.
  void configure_handle(CURL* eh) {
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, body_write_cb);
    curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, write_cb);
    curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(eh, CURLOPT_MAXLIFETIME_CONN, 30L);
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

  void set_body(Transfer& t) {
    curl_easy_setopt(t.eh, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(t.body.size()));
    curl_easy_setopt(t.eh, CURLOPT_POSTFIELDS, t.body.c_str());
  }

  static size_t write_cb(char* data, size_t n, size_t l, std::string* buf) {
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
    auto* self = static_cast<Client*>(clientp);
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    asio::ip::tcp protocol = asio::ip::tcp::v4();
    if (address->family == AF_INET6) {
      protocol = asio::ip::tcp::v6();
    } else if (address->family != AF_INET) {
      return CURL_SOCKET_BAD;
    }
    auto state = std::make_shared<SocketState>(self->io_);
    std::error_code ec;
    state->socket.open(protocol, ec);
    if (ec) return CURL_SOCKET_BAD;
    const curl_socket_t fd = state->socket.native_handle();
    self->sockets_[fd] = std::move(state);
    return fd;
  }

  static int close_socket_cb(void* clientp, curl_socket_t fd) {
    auto* self = static_cast<Client*>(clientp);
    const auto it = self->sockets_.find(fd);
    if (it == self->sockets_.end()) return 1;
    it->second->watch = 0;
    std::error_code ignored;
    it->second->socket.close(ignored);
    self->sockets_.erase(it);
    return 0;
  }

  static int socket_cb(CURL*, curl_socket_t fd, int what, void* userp,
                       void* socketp) {
    auto* self = static_cast<Client*>(userp);
    auto* state = static_cast<SocketState*>(socketp);
    if (state == nullptr) {
      // First notification for this socket: attach the state so curl hands
      // it back on later calls and we skip the lookup.
      const auto it = self->sockets_.find(fd);
      if (it == self->sockets_.end()) return 0;
      state = it->second.get();
      curl_multi_assign(self->multi_, fd, state);
    }
    state->watch = (what == CURL_POLL_REMOVE) ? 0 : what;
    if (state->watch != 0) self->arm(state->shared_from_this());
    return 0;
  }

  void arm(const std::shared_ptr<SocketState>& state) {
    const curl_socket_t fd = state->socket.native_handle();
    if ((state->watch & CURL_POLL_IN) && !state->read_armed) {
      state->read_armed = true;
      state->socket.async_wait(
          asio::ip::tcp::socket::wait_read,
          asio::bind_allocator(asio::recycling_allocator<void>(),
                               [this, w = std::weak_ptr<SocketState>(state),
                                fd](std::error_code ec) {
                                 on_event(w, fd, CURL_CSELECT_IN, ec);
                               }));
    }
    if ((state->watch & CURL_POLL_OUT) && !state->write_armed) {
      state->write_armed = true;
      state->socket.async_wait(
          asio::ip::tcp::socket::wait_write,
          asio::bind_allocator(asio::recycling_allocator<void>(),
                               [this, w = std::weak_ptr<SocketState>(state),
                                fd](std::error_code ec) {
                                 on_event(w, fd, CURL_CSELECT_OUT, ec);
                               }));
    }
  }

  void on_event(const std::weak_ptr<SocketState>& weak, curl_socket_t fd,
                int flag, std::error_code ec) {
    // The shared_ptr keeps the state alive across the socket_action call
    // below, which may close this very socket via close_socket_cb.
    const auto state = weak.lock();
    if (!state) return;
    (flag == CURL_CSELECT_IN ? state->read_armed : state->write_armed) = false;
    if (ec == asio::error::operation_aborted) return;
    curl_multi_socket_action(multi_, fd, ec ? CURL_CSELECT_ERR : flag,
                             &running_);
    check_completions();
    if (state->socket.is_open() && state->watch != 0) arm(state);
  }

  static int timer_cb(CURLM*, long timeout_ms, void* userp) {
    auto* self = static_cast<Client*>(userp);
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
        asio::post(self->io_,
                   asio::bind_allocator(asio::recycling_allocator<void>(),
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
        asio::bind_allocator(asio::recycling_allocator<void>(),
                             [self, alive = self->alive_](std::error_code ec) {
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
      CURL* eh = msg->easy_handle;
      curl_easy_getinfo(eh, CURLINFO_PRIVATE, &t);
      curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);
      curl_multi_remove_handle(multi_, eh);

      Response res{curl_code, http_code, std::move(t->buffer),
                   std::move(t->header_buffer)};
      Handler handler = std::move(t->handler);
      curl_slist_free_all(t->headers);
      if (pool_.size() < kMaxPooledHandles) {
        pool_.emplace_back(t->eh);
      } else {
        curl_easy_cleanup(t->eh);
      }
      transfers_.erase(t->self);

      const std::error_code ec = curl_code == CURLE_OK
                                     ? std::error_code{}
                                     : make_error_code(curl_code);
      // Single-threaded by contract: the handler's executor is this
      // io_context, where we already are — invoke without the
      // type-erased dispatch hop.
      std::move(handler)(ec, std::move(res));
    }
  }

  asio::io_context& io_;
  CURLM* multi_;
  asio::steady_timer timer_;
  std::unordered_map<curl_socket_t, std::shared_ptr<SocketState>> sockets_;
  std::list<Transfer> transfers_;
  std::vector<CURL*> pool_;
  int running_ = 0;
  bool kick_pending_ = false;
  bool timer_armed_ = false;
  // Outlives the client inside posted/timed kicks: they bail out when the
  // client is gone instead of touching a destroyed multi handle.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace cofetch
