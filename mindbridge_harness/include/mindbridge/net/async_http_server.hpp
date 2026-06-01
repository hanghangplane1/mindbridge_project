#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>

namespace mindbridge {
namespace net {

class AsyncHttpServer {
public:
    struct RequestContext {
        std::string path;
        std::string target;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    using RequestHandler = std::function<std::string(const std::string&)>;
    using PrefixRequestHandler = std::function<std::string(const std::string&, const std::string&)>;
    using StreamHandler = std::function<void(const std::string&, std::function<bool(const std::string&)>)>;
    using WebSocketHandler = std::function<std::string(const std::string&)>;
    using ContextRequestHandler = std::function<std::string(const RequestContext&)>;
    using PrefixContextRequestHandler = std::function<std::string(const RequestContext&)>;
    using ContextStreamHandler = std::function<void(const RequestContext&, std::function<bool(const std::string&)>)>;

    struct Options {
        std::string bind_address{"0.0.0.0"};
        std::size_t io_threads{0};
        std::size_t max_request_bytes{8 * 1024 * 1024};
        int request_timeout_sec{120};
        std::string server_name{"MindBridge-AsyncNet/1.0"};
    };

    explicit AsyncHttpServer(int port);
    AsyncHttpServer(int port, Options options);
    ~AsyncHttpServer();

    AsyncHttpServer(const AsyncHttpServer&) = delete;
    AsyncHttpServer& operator=(const AsyncHttpServer&) = delete;

    void register_handler(const std::string& path, RequestHandler handler);
    void register_context_handler(const std::string& path, ContextRequestHandler handler);
    void register_prefix_handler(const std::string& path_prefix, PrefixRequestHandler handler);
    void register_prefix_context_handler(const std::string& path_prefix, PrefixContextRequestHandler handler);
    void register_stream_handler(const std::string& path, StreamHandler handler);
    void register_context_stream_handler(const std::string& path, ContextStreamHandler handler);
    void register_websocket_handler(const std::string& path, WebSocketHandler handler);

    void start();
    void stop();

    static Options options_from_env();

private:
    class Impl;
    Impl* impl_;
};

}  // namespace net
}  // namespace mindbridge
