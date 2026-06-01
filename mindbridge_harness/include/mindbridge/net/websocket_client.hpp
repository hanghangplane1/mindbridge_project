#pragma once

#include "mindbridge/net/http_client.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace mindbridge {
namespace net {

struct WebSocketClientOptions {
    HttpClientOptions http;
    int ping_interval_ms{5000};
    int reconnect_interval_ms{5000};
    bool auto_reconnect{true};
};

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit WebSocketClient(std::string url, WebSocketClientOptions options = {});
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    void set_message_callback(MessageCallback callback);
    void set_error_callback(ErrorCallback callback);

    void start();
    void stop();
    bool send(const std::string& message);
    bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace net
}  // namespace mindbridge
