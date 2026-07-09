#pragma once
// cofetch: async HTTP client on top of libcurl's multi interface and ASIO.
//
// One implementation, three calling styles via ASIO completion tokens:
//
//   // coroutine ("await fetch()"):
//   auto res = co_await client.async_get(url, asio::use_awaitable);
//
//   // plain callback (zero-overhead hot path):
//   client.async_get(url, [](std::error_code ec, cofetch::Response res) {});
//
//   // std::future:
//   auto fut = client.async_get(url, asio::use_future);
//
// Drive it with io_context::run(), or io_context::poll() in a busy loop.
// Not thread-safe: run the client and its io_context on one thread.
#include <asio.hpp>
//
#include <curl/curl.h>

#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <system_error>
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

  int pending_requests() const { return running_; }

 private:
  using Handler = asio::any_completion_handler<void(std::error_code, Response)>;

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

  struct SocketState {
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
    } else {
      eh = pool_.back();
      pool_.pop_back();
      curl_easy_reset(eh);
    }

    transfers_.emplace_front(eh, std::move(h));
    const auto it = transfers_.begin();
    it->self = it;
    it->body = std::move(r.body_);

    curl_easy_setopt(eh, CURLOPT_URL, r.url_.c_str());
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &it->buffer);
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(eh, CURLOPT_HEADERDATA, &it->header_buffer);
    curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, write_cb);
    curl_easy_setopt(eh, CURLOPT_PRIVATE, &*it);
    curl_easy_setopt(eh, CURLOPT_TIMEOUT,
                     static_cast<long>(r.timeout_.count()));
    curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(eh, CURLOPT_MAXLIFETIME_CONN, 30L);
    curl_easy_setopt(eh, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    curl_easy_setopt(eh, CURLOPT_OPENSOCKETDATA, this);
    curl_easy_setopt(eh, CURLOPT_CLOSESOCKETFUNCTION, close_socket_cb);
    curl_easy_setopt(eh, CURLOPT_CLOSESOCKETDATA, this);

    if (!r.headers_.empty()) {
      curl_slist* chunk = nullptr;
      for (const auto& header : r.headers_) {
        chunk = curl_slist_append(chunk, header.c_str());
      }
      curl_easy_setopt(eh, CURLOPT_HTTPHEADER, chunk);
      it->headers = chunk;
    }

    switch (r.method_) {
      case Request::Method::GET:
        curl_easy_setopt(eh, CURLOPT_HTTPGET, 1L);
        break;
      case Request::Method::POST:
        curl_easy_setopt(eh, CURLOPT_POST, 1L);
        set_body(*it);
        break;
      case Request::Method::PUT:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "PUT");
        set_body(*it);
        break;
      case Request::Method::DEL:
        curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "DELETE");
        set_body(*it);
        break;
    }

    // curl schedules the kickstart itself through the timer callback.
    curl_multi_add_handle(multi_, eh);
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
                       void*) {
    auto* self = static_cast<Client*>(userp);
    const auto it = self->sockets_.find(fd);
    if (it == self->sockets_.end()) return 0;
    it->second->watch = (what == CURL_POLL_REMOVE) ? 0 : what;
    if (it->second->watch != 0) self->arm(it->second);
    return 0;
  }

  void arm(const std::shared_ptr<SocketState>& state) {
    const curl_socket_t fd = state->socket.native_handle();
    if ((state->watch & CURL_POLL_IN) && !state->read_armed) {
      state->read_armed = true;
      state->socket.async_wait(
          asio::ip::tcp::socket::wait_read,
          [this, w = std::weak_ptr<SocketState>(state), fd](
              std::error_code ec) { on_event(w, fd, CURL_CSELECT_IN, ec); });
    }
    if ((state->watch & CURL_POLL_OUT) && !state->write_armed) {
      state->write_armed = true;
      state->socket.async_wait(
          asio::ip::tcp::socket::wait_write,
          [this, w = std::weak_ptr<SocketState>(state), fd](
              std::error_code ec) { on_event(w, fd, CURL_CSELECT_OUT, ec); });
    }
  }

  void on_event(const std::weak_ptr<SocketState>& weak, curl_socket_t fd,
                int flag, std::error_code ec) {
    // The shared_ptr keeps the state alive across the socket_action call
    // below, which may close this very socket via close_socket_cb.
    const auto state = weak.lock();
    if (!state) return;
    (flag == CURL_CSELECT_IN ? state->read_armed : state->write_armed) =
        false;
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
      return 0;
    }
    self->timer_.expires_after(std::chrono::milliseconds(timeout_ms));
    self->timer_.async_wait([self](std::error_code ec) {
      if (ec) return;  // rescheduled or canceled
      curl_multi_socket_action(self->multi_, CURL_SOCKET_TIMEOUT, 0,
                               &self->running_);
      self->check_completions();
    });
    return 0;
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
      pool_.emplace_back(t->eh);
      transfers_.erase(t->self);

      const std::error_code ec =
          curl_code == CURLE_OK ? std::error_code{} : make_error_code(curl_code);
      auto ex = asio::get_associated_executor(handler, io_.get_executor());
      asio::dispatch(ex, [h = std::move(handler), ec,
                          r = std::move(res)]() mutable {
        std::move(h)(ec, std::move(r));
      });
    }
  }

  asio::io_context& io_;
  CURLM* multi_;
  asio::steady_timer timer_;
  std::map<curl_socket_t, std::shared_ptr<SocketState>> sockets_;
  std::list<Transfer> transfers_;
  std::vector<CURL*> pool_;
  int running_ = 0;
};

}  // namespace cofetch
