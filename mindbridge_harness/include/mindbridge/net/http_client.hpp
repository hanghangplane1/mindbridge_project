#pragma once

#include <functional>
#include <map>
#include <string>

namespace mindbridge {
namespace net {

struct HttpClientOptions {
    long timeout_sec{120};
    std::size_t max_response_bytes{16 * 1024 * 1024};
    bool verify_tls{true};
    std::string ca_file;
};

struct HttpResponse {
    int status_code{0};
    std::map<std::string, std::string> headers;
    std::string body;
};

HttpResponse post_json(const std::string& url,
                       const std::string& body,
                       const HttpClientOptions& options = {});

HttpResponse get(const std::string& url, const HttpClientOptions& options = {});

void post_sse(const std::string& url,
              const std::string& body,
              const std::function<bool(const std::string&)>& on_event,
              const HttpClientOptions& options = {});

}  // namespace net
}  // namespace mindbridge
