#pragma once
#include <curl/curl.h>
#include <sys/epoll.h>

#include <functional>
#include <list>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;

class ResponseInfo {
 public:
  ResponseInfo(CURLcode curl_code, long http_code, string data,
               string header_data, const void* custom_data)
      : curl_code_(curl_code),
        http_code_(http_code),
        data_(std::move(data)),
        header_data_(std::move(header_data)),
        custom_data_(custom_data) {}

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

  CURLcode curl_code_;
  long http_code_;
  string data_;
  string header_data_;
  const void* custom_data_;
};

class RequestInfo {
 public:
  RequestInfo(CURL* eh, function<void(const ResponseInfo&)> callback,
              const void* custom_data)
      : eh_(eh),
        header_ptr_(nullptr),
        callback_(std::move(callback)),
        custom_data_(custom_data) {}
  CURL* eh_;
  curl_slist* header_ptr_;
  string buffer_;
  string header_buffer_;
  string body_;
  const function<void(const ResponseInfo&)> callback_;
  const void* custom_data_;
  list<RequestInfo>::iterator self_;
};

class Http {
 public:
  static constexpr size_t HTTP_POOL = 128;

  /**
   * @brief Builder for a single request, returned by Http::request(). Each
   * builder owns independent state, so several can be prepared and
   * interleaved freely. A builder must not be reused after
   * get()/post()/del()/put() has been called.
   */
  class Request {
   public:
    Request(Http& http, list<RequestInfo>::iterator it)
        : http_(http), it_(it) {}

    /**
     * @brief Set headers with vector of string, replacing all headers set
     * previously on this request.
     */
    Request& set_headers(const vector<string>& headers) {
      curl_slist* chunk = nullptr;
      for (const auto& header : headers) {
        chunk = curl_slist_append(chunk, header.c_str());
      }
      curl_easy_setopt(it_->eh_, CURLOPT_HTTPHEADER, chunk);
      it_->header_ptr_ = chunk;
      return *this;
    }

    /**
     * @brief Set body. The request stores its own copy, so the argument does
     * not need to outlive the transfer.
     */
    Request& set_body(string post_fields) {
      it_->body_ = std::move(post_fields);
      curl_easy_setopt(it_->eh_, CURLOPT_POSTFIELDSIZE,
                       static_cast<long>(it_->body_.size()));
      curl_easy_setopt(it_->eh_, CURLOPT_POSTFIELDS, it_->body_.c_str());
      return *this;
    }

    /**
     * @brief Enable response header data, if not called, then it is invalid
     * to access response header.
     */
    Request& enable_response_headers() {
      curl_easy_setopt(it_->eh_, CURLOPT_HEADERDATA, &(it_->header_buffer_));
      curl_easy_setopt(it_->eh_, CURLOPT_HEADERFUNCTION, write_fn);
      return *this;
    }

    /**
     * @brief Submit as a GET request.
     */
    void get() {
      curl_easy_setopt(it_->eh_, CURLOPT_HTTPGET, 1L);
      http_.submit(it_->eh_);
    }

    /**
     * @brief Submit as a POST request.
     */
    void post() {
      curl_easy_setopt(it_->eh_, CURLOPT_POST, 1L);
      http_.submit(it_->eh_);
    }

    /**
     * @brief Submit as a DELETE request.
     */
    void del() {
      curl_easy_setopt(it_->eh_, CURLOPT_CUSTOMREQUEST, "DELETE");
      http_.submit(it_->eh_);
    }

    /**
     * @brief Submit as a PUT request.
     */
    void put() {
      curl_easy_setopt(it_->eh_, CURLOPT_CUSTOMREQUEST, "PUT");
      http_.submit(it_->eh_);
    }

   private:
    Http& http_;
    list<RequestInfo>::iterator it_;
  };

  Http() : fd_epoll_(epoll_create1(EPOLL_CLOEXEC)), handle_count_(0) {
    curl_global_init(CURL_GLOBAL_ALL);

    multi_handle_ = curl_multi_init();

    curl_multi_setopt(multi_handle_, CURLMOPT_PIPELINING, CURLPIPE_NOTHING);
    curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETFUNCTION, socket_fn);
    curl_multi_setopt(multi_handle_, CURLMOPT_SOCKETDATA, this);

    for (size_t i = 0; i < HTTP_POOL; ++i) {
      eh_pool_.emplace_back(curl_easy_init());
    }
  }

  ~Http() {
    curl_multi_cleanup(multi_handle_);
    curl_global_cleanup();
  }

  /**
   * @brief Progress in-flight transfers and deliver completed responses to
   * their callbacks. Never blocks.
   */
  void poll() {
    process_events();
    multi_info_read();
  }

  /**
   * @brief Create a request builder for the given URL. The callback is
   * invoked from poll() once the transfer completes (or fails).
   */
  Request request(const string& url, function<void(const ResponseInfo&)> fn,
                  const void* custom_data = nullptr) {
    CURL* eh = nullptr;
    if (eh_pool_.empty()) {
      eh = curl_easy_init();
    } else {
      eh = eh_pool_.back();
      eh_pool_.pop_back();
      curl_easy_reset(eh);
    }

    request_infos_.emplace_front(eh, std::move(fn), custom_data);
    const auto it = request_infos_.begin();
    it->self_ = it;

    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &(it->buffer_));
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_fn);
    curl_easy_setopt(eh, CURLOPT_URL, url.c_str());
    curl_easy_setopt(eh, CURLOPT_PRIVATE, &*it);
    curl_easy_setopt(eh, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(eh, CURLOPT_MAXLIFETIME_CONN, 30L);

    // https://github.com/curl/curl/issues/1625#issuecomment-312456910
    curl_easy_setopt(eh, CURLOPT_POSTFIELDS, "");

    return Request{*this, it};
  }

  int pending_requests() const { return handle_count_; }

 private:
  static size_t write_fn(char* data, size_t n, size_t l, string* buf) {
    const size_t total_size = n * l;
    buf->append(data, total_size);
    return total_size;
  }

  static size_t socket_fn(CURL* e, curl_socket_t s, int epe, Http* http,
                          void* socketp) {
    if (epe == CURL_POLL_REMOVE) {
      epoll_ctl(http->fd_epoll_, EPOLL_CTL_DEL, s, NULL);
      http->fd_set_.erase(s);
      return 0;
    }
    //((epe & CURL_POLL_IN) ? EPOLLIN : 0) |
    //((epe & CURL_POLL_OUT) ? EPOLLOUT : 0);
    const static uint32_t e_table[] = {
        0,
        EPOLLIN,
        EPOLLOUT,
        EPOLLIN | EPOLLOUT,
    };
    epoll_event ev;
    ev.events = e_table[epe];
    ev.data.fd = s;
    if (http->fd_set_.find(s) == http->fd_set_.end()) {
      epoll_ctl(http->fd_epoll_, EPOLL_CTL_ADD, s, &ev);
      http->fd_set_.emplace(s);
    } else {
      epoll_ctl(http->fd_epoll_, EPOLL_CTL_MOD, s, &ev);
    };
    return 0;
  }

  // Completions are intentionally not read here, so a request submitted from
  // inside a callback cannot re-enter multi_info_read().
  void submit(CURL* eh) {
    curl_multi_add_handle(multi_handle_, eh);
    // https://curl.se/mail/lib-2020-10/0036.html
    curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0,
                             &handle_count_);
    process_events();
  }

  void process_events() {
    const auto ready_cnt = epoll_wait(fd_epoll_, events_, HTTP_POOL, 0);
    if (ready_cnt == 0) {
      curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0,
                               &handle_count_);
    } else {
      for (int idx = 0; idx < ready_cnt; ++idx) {
        curl_multi_socket_action(multi_handle_, events_[idx].data.fd, 0,
                                 &handle_count_);
      }
    }
  }

  void multi_info_read() {
    int msgs_left = 0;
    while (CURLMsg* msg = curl_multi_info_read(multi_handle_, &msgs_left)) {
      RequestInfo* info = nullptr;
      long http_code = 0;
      // msg must not be dereferenced after curl_multi_remove_handle().
      const CURLcode curl_code = msg->data.result;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &info);
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);
      curl_multi_remove_handle(multi_handle_, msg->easy_handle);

      (info->callback_)({curl_code, http_code, std::move(info->buffer_),
                         std::move(info->header_buffer_), info->custom_data_});
      curl_slist_free_all(info->header_ptr_);
      eh_pool_.emplace_back(info->eh_);
      request_infos_.erase(info->self_);
    }
  }

  const int fd_epoll_;
  epoll_event events_[HTTP_POOL];
  unordered_set<int> fd_set_;

  CURLM* multi_handle_;
  int handle_count_;
  vector<CURL*> eh_pool_;
  list<RequestInfo> request_infos_;
};
