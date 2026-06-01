#include "mindbridge/net/websocket_client.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/rfc2818_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/err.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>

namespace mindbridge {
namespace net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
    bool tls{false};
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

ParsedUrl parse_url(const std::string& raw) {
    const auto scheme_pos = raw.find("://");
    if (scheme_pos == std::string::npos) {
        throw std::runtime_error("URL missing scheme: " + raw);
    }
    ParsedUrl out;
    out.scheme = lower(raw.substr(0, scheme_pos));
    out.tls = out.scheme == "wss";
    if (out.scheme != "ws" && out.scheme != "wss") {
        throw std::runtime_error("unsupported WebSocket URL scheme: " + out.scheme);
    }

    const std::string rest = raw.substr(scheme_pos + 3);
    const auto path_pos = rest.find('/');
    const std::string authority = path_pos == std::string::npos ? rest : rest.substr(0, path_pos);
    out.target = path_pos == std::string::npos ? "/" : rest.substr(path_pos);
    if (out.target.empty()) {
        out.target = "/";
    }
    if (!authority.empty() && authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos) {
            throw std::runtime_error("invalid IPv6 URL authority: " + raw);
        }
        out.host = authority.substr(1, end - 1);
        if (end + 1 < authority.size() && authority[end + 1] == ':') {
            out.port = authority.substr(end + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
        } else {
            out.host = authority;
        }
    }
    if (out.host.empty()) {
        throw std::runtime_error("URL missing host: " + raw);
    }
    if (out.port.empty()) {
        out.port = out.tls ? "443" : "80";
    }
    return out;
}

template <typename Ws>
bool write_text(Ws& ws, const std::string& message) {
    beast::error_code ec;
    ws.text(true);
    ws.write(asio::buffer(message), ec);
    return !ec;
}

template <typename Ws>
void safe_close(Ws& ws) {
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
}

websocket::stream_base::timeout websocket_timeout_options(const WebSocketClientOptions& options) {
    websocket::stream_base::timeout timeout;
    timeout.handshake_timeout = std::chrono::seconds(5);
    timeout.idle_timeout = std::chrono::milliseconds(
        std::max(1000, options.ping_interval_ms * 2));
    timeout.keep_alive_pings = false;
    return timeout;
}

}  // namespace

struct WebSocketClient::Impl {
    Impl(std::string input_url, WebSocketClientOptions input_options)
        : url(std::move(input_url)), options(std::move(input_options)) {}

    std::string url;
    WebSocketClientOptions options;
    MessageCallback on_message;
    ErrorCallback on_error;
    std::atomic<bool> running{false};
    std::atomic<bool> is_connected{false};
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::string> outbound;

    void notify_error(const std::string& message) {
        if (on_error) {
            on_error(message);
        }
    }

    bool pop_or_wait(std::string& message, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_until(lock, deadline, [this]() { return !running.load() || !outbound.empty(); });
        if (!running.load()) {
            return false;
        }
        if (outbound.empty()) {
            return false;
        }
        message = std::move(outbound.front());
        outbound.pop();
        return true;
    }

    template <typename Ws>
    void session_loop(Ws& ws) {
        is_connected.store(true);
        auto last_ping = std::chrono::steady_clock::now();
        while (running.load()) {
            std::string message;
            const auto next_ping = last_ping + std::chrono::milliseconds(options.ping_interval_ms);
            bool wrote_message = false;
            if (pop_or_wait(message, next_ping)) {
                if (!write_text(ws, message)) {
                    throw std::runtime_error("WebSocket write failed");
                }
                wrote_message = true;
            }
            if (!running.load()) {
                break;
            }

            if (options.ping_interval_ms > 0 && std::chrono::steady_clock::now() >= next_ping) {
                beast::error_code ec;
                ws.ping(websocket::ping_data("mindbridge"), ec);
                if (ec) {
                    throw beast::system_error(ec);
                }
                last_ping = std::chrono::steady_clock::now();
            }
            if (!wrote_message) {
                continue;
            }

            beast::flat_buffer buffer;
            beast::error_code ec;
            ws.read(buffer, ec);
            if (ec == websocket::error::closed) {
                break;
            }
            if (ec) {
                throw beast::system_error(ec);
            }
            if (on_message) {
                on_message(beast::buffers_to_string(buffer.data()));
            }
        }
    }

    void run_plain(const ParsedUrl& parsed) {
        asio::io_context ioc;
        tcp::resolver resolver(ioc);
        websocket::stream<beast::tcp_stream> ws(ioc);
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(options.http.timeout_sec));
        beast::get_lowest_layer(ws).connect(resolver.resolve(parsed.host, parsed.port));
        ws.set_option(websocket_timeout_options(options));
        ws.handshake(parsed.host + ":" + parsed.port, parsed.target);
        session_loop(ws);
        safe_close(ws);
    }

    void run_tls(const ParsedUrl& parsed) {
        asio::io_context ioc;
        asio::ssl::context ctx(asio::ssl::context::tls_client);
        if (options.http.verify_tls) {
            if (!options.http.ca_file.empty()) {
                ctx.load_verify_file(options.http.ca_file);
            } else {
                ctx.set_default_verify_paths();
            }
            ctx.set_verify_mode(asio::ssl::verify_peer);
        } else {
            ctx.set_verify_mode(asio::ssl::verify_none);
        }

        tcp::resolver resolver(ioc);
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ctx);
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), parsed.host.c_str())) {
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
        }
        if (options.http.verify_tls) {
            ws.next_layer().set_verify_callback(asio::ssl::rfc2818_verification(parsed.host));
        }
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(options.http.timeout_sec));
        beast::get_lowest_layer(ws).connect(resolver.resolve(parsed.host, parsed.port));
        ws.next_layer().handshake(asio::ssl::stream_base::client);
        ws.set_option(websocket_timeout_options(options));
        ws.handshake(parsed.host + ":" + parsed.port, parsed.target);
        session_loop(ws);
        safe_close(ws);
    }

    void run_once() {
        const ParsedUrl parsed = parse_url(url);
        parsed.tls ? run_tls(parsed) : run_plain(parsed);
    }

    void run() {
        while (running.load()) {
            try {
                run_once();
            } catch (const std::exception& e) {
                notify_error(e.what());
            }
            is_connected.store(false);
            if (!running.load() || !options.auto_reconnect) {
                break;
            }
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(options.reconnect_interval_ms),
                        [this]() { return !running.load(); });
        }
    }
};

WebSocketClient::WebSocketClient(std::string url, WebSocketClientOptions options)
    : impl_(new Impl(std::move(url), std::move(options))) {}

WebSocketClient::~WebSocketClient() {
    stop();
}

void WebSocketClient::set_message_callback(MessageCallback callback) {
    impl_->on_message = std::move(callback);
}

void WebSocketClient::set_error_callback(ErrorCallback callback) {
    impl_->on_error = std::move(callback);
}

void WebSocketClient::start() {
    if (impl_->running.exchange(true)) {
        return;
    }
    impl_->worker = std::thread([this]() { impl_->run(); });
}

void WebSocketClient::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

bool WebSocketClient::send(const std::string& message) {
    if (!impl_->running.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->outbound.push(message);
    }
    impl_->cv.notify_all();
    return true;
}

bool WebSocketClient::connected() const {
    return impl_->is_connected.load();
}

}  // namespace net
}  // namespace mindbridge
