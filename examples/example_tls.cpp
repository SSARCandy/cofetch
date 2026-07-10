// TLS works out of the box: https URLs are verified against the system
// CA bundle, nothing to configure. When you do need to reach the TLS
// knobs (private CA, client certificate, local dev server), the .curl()
// escape hatch exposes the raw libcurl handle per request.
#include <cofetch.h>

#include <asio.hpp>
#include <iostream>

int main() {
  asio::io_context io;
  cofetch::Client http(io);

  http.request("https://postman-echo.com/get")
      .curl([](CURL* h) {
        // Examples of per-request TLS tuning (all optional):
        //   curl_easy_setopt(h, CURLOPT_CAINFO, "/etc/ssl/company-ca.pem");
        //   curl_easy_setopt(h, CURLOPT_SSLCERT, "client.pem");
        //   curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);  // dev only!
        (void)h;
      })
      .get([](std::error_code ec, cofetch::Response res) {
        if (ec) {
          std::cerr << "tls/transport error: " << ec.message() << "\n";
          return;
        }
        std::cout << "verified https: " << res.http_code_ << "\n";
      });

  io.run();
}
