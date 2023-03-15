#pragma once
#include <curl/curl.h>
#include <sys/epoll.h>

#include <functional>
#include <list>
#include <string>
#include <unordered_set>
#include <vector>

using namespace std;

class ResponseInfo {
 public:
  ResponseInfo(const size_t& http_code, const string& data,
               const string& header_data, const void* custom_data)
      : http_code_(http_code),
        data_(data),
        header_data_(header_data),
        custom_data_(custom_data) {}

  bool is_ok() const { return http_code_ >= 200 && http_code_ < 300; }
  const size_t http_code_;
  const string& data_;
  const string& header_data_;
  const void* custom_data_;
};

class RequestInfo {
 public:
  RequestInfo(CURL* eh, const function<void(const ResponseInfo&)>& callback,
              const void* custom_data)
      : eh_(eh),
        header_ptr_(nullptr),
        callback_(callback),
        custom_data_(custom_data) {}
  CURL* eh_;
  curl_slist* header_ptr_;
  string buffer_;
  string header_buffer_;
  const function<void(const ResponseInfo&)> callback_;
  const void* custom_data_;
};

class Http {
 public:
  static constexpr size_t HTTP_POOL = 128;

  Http()
      : fd_epoll_(epoll_create1(EPOLL_CLOEXEC)),
        msgs_left_(-1),
        handle_count_(0),
        nonce_(0) {
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

  template <bool is_need_read = true>
  void poll() {
    if constexpr (!is_need_read) {
      // https://curl.se/mail/lib-2020-10/0036.html
      curl_multi_add_handle(multi_handle_, eh_);
      curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0,
                               &handle_count_);
    }
    const auto ready_cnt = epoll_wait(fd_epoll_, events_, HTTP_POOL, 0);
    if (ready_cnt == 0) {
      curl_multi_socket_action(multi_handle_, CURL_SOCKET_TIMEOUT, 0,
                               &handle_count_);
    } else {
      for (int idx = 0; idx < ready_cnt; ++idx) {
        const auto& s = events_[idx].data.fd;
        curl_multi_socket_action(multi_handle_, s, 0, &handle_count_);
      }
    }
    if constexpr (!is_need_read) return;
    multi_info_read();
  }

  Http& request(const string& url,
                const function<void(const ResponseInfo&)>& fn,
                const void* custom_data = nullptr) {
    if (eh_pool_.empty()) {
      eh_ = curl_easy_init();
    } else {
      eh_ = eh_pool_.back();
      eh_pool_.pop_back();
      curl_easy_reset(eh_);
    }

    request_infos_.emplace_front(eh_, fn, custom_data);
    request_infos_it_ = request_infos_.begin();

    curl_easy_setopt(eh_, CURLOPT_WRITEDATA, &(request_infos_it_->buffer_));
    curl_easy_setopt(eh_, CURLOPT_WRITEFUNCTION, write_fn);
    curl_easy_setopt(eh_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(eh_, CURLOPT_PRIVATE, request_infos_it_);
    curl_easy_setopt(eh_, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(eh_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(eh_, CURLOPT_MAXLIFETIME_CONN, 30L);

    // https://github.com/curl/curl/issues/1625#issuecomment-312456910
    curl_easy_setopt(eh_, CURLOPT_POSTFIELDS, "");

    ++nonce_;
    return *this;
  }

  /**
   * @brief Set headers cache with vector of string.
   *
   * By calling this function, it will replace all headers set previously.
   */
  Http& set_headers(const vector<string>& headers) {
    curl_slist* chunk = nullptr;
    for (const auto& header : headers) {
      chunk = curl_slist_append(chunk, header.c_str());
    }
    curl_easy_setopt(eh_, CURLOPT_HTTPHEADER, chunk);
    request_infos_it_->header_ptr_ = chunk;
    return *this;
  }

  /**
   * @brief Set body.
   */
  Http& set_body(const string& post_fields) {
    curl_easy_setopt(eh_, CURLOPT_POSTFIELDSIZE, post_fields.size());
    curl_easy_setopt(eh_, CURLOPT_POSTFIELDS, post_fields.c_str());
    return *this;
  }

  /**
   * @brief Enable response header data, if not called, then it is invalid to
   * access response header.
   */
  Http& enable_response_headers() {
    curl_easy_setopt(eh_, CURLOPT_HEADERDATA,
                     &(request_infos_it_->header_buffer_));
    curl_easy_setopt(eh_, CURLOPT_HEADERFUNCTION, write_fn);
    return *this;
  }

  /**
   * @brief Sets up a GET request and adds it to the multi-handle.
   */
  void get() {
    curl_easy_setopt(eh_, CURLOPT_HTTPGET, 1L);
    curl_multi_add_handle(multi_handle_, eh_);
    poll<false>();
  }

  /**
   * @brief Sets up a POST request and adds it to the multi-handle.
   */
  void post() {
    curl_easy_setopt(eh_, CURLOPT_POST, 1L);
    curl_multi_add_handle(multi_handle_, eh_);
    poll<false>();
  }

  /**
   * @brief Sets up a DELETE request and adds it to the multi-handle.
   */
  void del() {
    curl_easy_setopt(eh_, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_multi_add_handle(multi_handle_, eh_);
    poll<false>();
  }

  /**
   * @brief Sets up a PUT request and adds it to the multi-handle.
   */
  void put() {
    curl_easy_setopt(eh_, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_multi_add_handle(multi_handle_, eh_);
    poll<false>();
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

  void multi_info_read() {
    while ((msg_ = curl_multi_info_read(multi_handle_, &msgs_left_))) {
      RequestInfoList::iterator it;
      size_t http_code;
      curl_easy_getinfo(msg_->easy_handle, CURLINFO_PRIVATE, &it);
      curl_easy_getinfo(msg_->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);
      const auto& x = *it;
      (x.callback_)({http_code, x.buffer_, x.header_buffer_, x.custom_data_});
      curl_multi_remove_handle(multi_handle_, msg_->easy_handle);
      curl_slist_free_all(x.header_ptr_);
      eh_pool_.emplace_back(x.eh_);
      request_infos_.erase(it);
    }
  }

  const int fd_epoll_;
  epoll_event events_[HTTP_POOL];
  unordered_set<int> fd_set_;

  CURL* eh_;
  CURLM* multi_handle_;
  CURLMsg* msg_;
  int msgs_left_;
  mutable int handle_count_;
  mutable size_t nonce_;
  mutable vector<CURL*> eh_pool_;
  using RequestInfoList = list<RequestInfo>;
  mutable RequestInfoList request_infos_;
  mutable RequestInfoList::iterator request_infos_it_;
};
