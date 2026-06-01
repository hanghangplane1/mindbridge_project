#include "mindbridge/net/async_http_server.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#endif

namespace mindbridge {
namespace net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

int env_int(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::size_t env_size(const char* key, std::size_t fallback) {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback;
    }
    try {
        return static_cast<std::size_t>(std::stoull(value));
    } catch (...) {
        return fallback;
    }
}

std::string env_string(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value && *value ? std::string(value) : fallback;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out << "\\u00" << kHex[(ch >> 4) & 0x0f] << kHex[ch & 0x0f];
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::string error_json(const std::string& message) {
    return "{\"error\":\"" + json_escape(message) + "\"}";
}

std::string path_only(const std::string& target) {
    const auto query = target.find('?');
    return query == std::string::npos ? target : target.substr(0, query);
}

bool is_stream_request(const std::string& path, const std::string& body) {
    return path.find("/stream") != std::string::npos ||
           body.find("\"message/stream\"") != std::string::npos;
}

}  // namespace

class AsyncHttpServer::Impl {
public:
    explicit Impl(int port, Options options)
        : port_(port),
          options_(std::move(options)),
          ioc_(static_cast<int>(std::max<std::size_t>(1, thread_count()))),
          acceptor_(ioc_) {}

    void register_handler(const std::string& path, RequestHandler handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers_[path] = std::move(handler);
    }

    void register_context_handler(const std::string& path, ContextRequestHandler handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        context_handlers_[path] = std::move(handler);
    }

    void register_prefix_handler(const std::string& path_prefix, PrefixRequestHandler handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        prefix_handlers_[path_prefix] = std::move(handler);
    }

    void register_prefix_context_handler(const std::string& path_prefix, PrefixContextRequestHandler handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        prefix_context_handlers_[path_prefix] = std::move(handler);
    }

    void register_stream_handler(const std::string& path, StreamHandler handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        stream_handlers_[path] = std::move(handler);
    }

    void register_context_stream_handler(const std::string& path, ContextStreamHandler handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        context_stream_handlers_[path] = std::move(handler);
    }

    void register_websocket_handler(const std::string& path, WebSocketHandler handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        websocket_handlers_[path] = std::move(handler);
    }

    void start() {
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        tcp::endpoint endpoint(asio::ip::make_address(options_.bind_address),
                               static_cast<unsigned short>(port_));
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("open acceptor failed: " + ec.message());
        }
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            throw std::runtime_error("set reuse_address failed: " + ec.message());
        }
        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("bind failed on port " + std::to_string(port_) + ": " + ec.message());
        }
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("listen failed on port " + std::to_string(port_) + ": " + ec.message());
        }

        running_.store(true);
        do_accept();

        std::cout << "Async HTTP Server listening on " << options_.bind_address << ":" << port_
                  << " io_threads=" << thread_count() << std::endl;
        std::vector<std::thread> workers;
        const std::size_t threads = thread_count();
        workers.reserve(threads > 0 ? threads - 1 : 0);
        for (std::size_t i = 1; i < threads; ++i) {
            workers.emplace_back([this]() { ioc_.run(); });
        }
        ioc_.run();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void stop() {
        running_.store(false);
        beast::error_code ec;
        acceptor_.close(ec);
        ioc_.stop();
    }

    std::size_t thread_count() const {
        if (options_.io_threads != 0) {
            return options_.io_threads;
        }
        const unsigned int hw = std::thread::hardware_concurrency();
        return std::max<std::size_t>(2, hw == 0 ? 2 : hw);
    }

private:
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(tcp::socket&& socket, Impl& server)
            : stream_(std::move(socket)),
              strand_(asio::make_strand(stream_.get_executor())),
              server_(server) {}

        void run() {
            asio::dispatch(strand_, beast::bind_front_handler(&Session::do_read, shared_from_this()));
        }

    private:
        class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
        public:
            WebSocketSession(beast::tcp_stream&& stream,
                             http::request<http::string_body>&& request,
                             WebSocketHandler handler,
                             std::string server_name)
                : ws_(std::move(stream)),
                  request_(std::move(request)),
                  handler_(std::move(handler)),
                  server_name_(std::move(server_name)) {}

            void run() {
                ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
                ws_.set_option(websocket::stream_base::decorator([this](websocket::response_type& response) {
                    response.set(http::field::server, server_name_);
                }));
                ws_.async_accept(
                    request_,
                    beast::bind_front_handler(&WebSocketSession::on_accept, shared_from_this()));
            }

        private:
            void on_accept(beast::error_code ec) {
                if (ec) {
                    return;
                }
                do_read();
            }

            void do_read() {
                buffer_.consume(buffer_.size());
                ws_.async_read(buffer_,
                               beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this()));
            }

            void on_read(beast::error_code ec, std::size_t) {
                if (ec == websocket::error::closed) {
                    return;
                }
                if (ec) {
                    close();
                    return;
                }
                std::string response;
                try {
                    response = handler_(beast::buffers_to_string(buffer_.data()));
                } catch (const std::exception& e) {
                    response = error_json(e.what());
                }
                auto out = std::make_shared<std::string>(std::move(response));
                ws_.text(true);
                ws_.async_write(asio::buffer(*out),
                                [self = shared_from_this(), out](beast::error_code write_ec, std::size_t) {
                                    if (write_ec) {
                                        self->close();
                                        return;
                                    }
                                    self->do_read();
                                });
            }

            void close() {
                beast::error_code ec;
                ws_.close(websocket::close_code::normal, ec);
            }

            websocket::stream<beast::tcp_stream> ws_;
            http::request<http::string_body> request_;
            beast::flat_buffer buffer_;
            WebSocketHandler handler_;
            std::string server_name_;
        };

        void do_read() {
            parser_ = std::make_unique<http::request_parser<http::string_body>>();
            parser_->body_limit(server_.options_.max_request_bytes);
            stream_.expires_after(std::chrono::seconds(server_.options_.request_timeout_sec));
            http::async_read(stream_,
                             buffer_,
                             *parser_,
                             asio::bind_executor(
                                 strand_,
                                 beast::bind_front_handler(&Session::on_read, shared_from_this())));
        }

        void on_read(beast::error_code ec, std::size_t) {
            if (ec == http::error::body_limit) {
                write_json(413, error_json("HTTP request exceeds configured limit"));
                return;
            }
            if (ec == http::error::end_of_stream) {
                close();
                return;
            }
            if (ec) {
                write_json(400, error_json(ec.message()));
                return;
            }

            http::request<http::string_body> request = parser_->release();
            const std::string target = std::string(request.target());
            const std::string path = path_only(target);
            const std::string body = request.body();
            RequestContext context;
            context.path = path;
            context.target = target;
            context.body = body;
            for (const auto& field : request.base()) {
                context.headers.emplace(std::string(field.name_string()), std::string(field.value()));
            }

            if (websocket::is_upgrade(request)) {
                WebSocketHandler websocket_handler;
                {
                    std::lock_guard<std::mutex> lock(server_.handlers_mutex_);
                    auto ws_it = server_.websocket_handlers_.find(path);
                    if (ws_it != server_.websocket_handlers_.end()) {
                        websocket_handler = ws_it->second;
                    }
                }
                if (!websocket_handler) {
                    write_json(404, error_json("WebSocket endpoint not found"));
                    return;
                }
                std::make_shared<WebSocketSession>(
                    std::move(stream_),
                    std::move(request),
                    std::move(websocket_handler),
                    server_.options_.server_name)
                    ->run();
                return;
            }

            StreamHandler stream_handler;
            ContextStreamHandler context_stream_handler;
            RequestHandler handler;
            ContextRequestHandler context_handler;
            PrefixRequestHandler prefix_handler;
            PrefixContextRequestHandler prefix_context_handler;
            std::string prefix_path;
            {
                std::lock_guard<std::mutex> lock(server_.handlers_mutex_);
                auto context_stream_it = server_.context_stream_handlers_.find(path);
                auto stream_it = server_.stream_handlers_.find(path);
                if (context_stream_it != server_.context_stream_handlers_.end() && is_stream_request(path, body)) {
                    context_stream_handler = context_stream_it->second;
                } else if (stream_it != server_.stream_handlers_.end() && is_stream_request(path, body)) {
                    stream_handler = stream_it->second;
                } else {
                    auto context_it = server_.context_handlers_.find(path);
                    auto it = server_.handlers_.find(path);
                    if (context_it != server_.context_handlers_.end()) {
                        context_handler = context_it->second;
                    } else if (it != server_.handlers_.end()) {
                        handler = it->second;
                    } else {
                        auto prefix_context_it = std::find_if(
                            server_.prefix_context_handlers_.begin(),
                            server_.prefix_context_handlers_.end(),
                            [&path](const auto& entry) { return path.rfind(entry.first, 0) == 0; });
                        auto prefix_it = std::find_if(
                            server_.prefix_handlers_.begin(),
                            server_.prefix_handlers_.end(),
                            [&path](const auto& entry) { return path.rfind(entry.first, 0) == 0; });
                        if (prefix_context_it != server_.prefix_context_handlers_.end()) {
                            prefix_context_handler = prefix_context_it->second;
                        } else if (prefix_it != server_.prefix_handlers_.end()) {
                            prefix_path = path;
                            prefix_handler = prefix_it->second;
                        }
                    }
                }
            }

            if (context_stream_handler) {
                start_stream(context, std::move(context_stream_handler));
                return;
            }
            if (stream_handler) {
                start_stream(body, std::move(stream_handler));
                return;
            }
            if (context_handler) {
                try {
                    write_json(200, context_handler(context));
                } catch (const std::exception& e) {
                    write_json(500, error_json(e.what()));
                }
                return;
            }
            if (handler) {
                try {
                    write_json(200, handler(body));
                } catch (const std::exception& e) {
                    write_json(500, error_json(e.what()));
                }
                return;
            }
            if (prefix_context_handler) {
                try {
                    write_json(200, prefix_context_handler(context));
                } catch (const std::exception& e) {
                    write_json(500, error_json(e.what()));
                }
                return;
            }
            if (prefix_handler) {
                try {
                    write_json(200, prefix_handler(prefix_path, body));
                } catch (const std::exception& e) {
                    write_json(500, error_json(e.what()));
                }
                return;
            }
            write_json(404, error_json("Not Found"));
        }

        void write_json(int status, const std::string& body) {
            auto response = std::make_shared<http::response<http::string_body>>(
                static_cast<http::status>(status), 11);
            response->set(http::field::server, server_.options_.server_name);
            response->set(http::field::content_type, "application/json");
            response->set(http::field::access_control_allow_origin, "*");
            response->keep_alive(false);
            response->body() = body;
            response->prepare_payload();
            stream_.expires_after(std::chrono::seconds(server_.options_.request_timeout_sec));
            http::async_write(stream_,
                              *response,
                              asio::bind_executor(
                                  strand_,
                                  [self = shared_from_this(), response](beast::error_code, std::size_t) {
                                      self->close();
                                  }));
        }

        void start_stream(const std::string& body, StreamHandler handler) {
            std::ostringstream header;
            header << "HTTP/1.1 200 OK\r\n";
            header << "Server: " << server_.options_.server_name << "\r\n";
            header << "Content-Type: text/event-stream\r\n";
            header << "Cache-Control: no-cache\r\n";
            header << "Connection: keep-alive\r\n";
            header << "Access-Control-Allow-Origin: *\r\n";
            header << "\r\n";
            enqueue_raw(header.str(), false);

            asio::post(strand_, [self = shared_from_this(), body, handler = std::move(handler)]() mutable {
                try {
                    handler(body, [self](const std::string& payload) {
                        return self->send_sse_event(payload);
                    });
                } catch (const std::exception& e) {
                    self->send_sse_event(error_json(e.what()));
                }
                self->finish_stream();
            });
        }

        void start_stream(const RequestContext& context, ContextStreamHandler handler) {
            std::ostringstream header;
            header << "HTTP/1.1 200 OK\r\n";
            header << "Server: " << server_.options_.server_name << "\r\n";
            header << "Content-Type: text/event-stream\r\n";
            header << "Cache-Control: no-cache\r\n";
            header << "Connection: keep-alive\r\n";
            header << "Access-Control-Allow-Origin: *\r\n";
            header << "\r\n";
            enqueue_raw(header.str(), false);

            asio::post(strand_, [self = shared_from_this(), context, handler = std::move(handler)]() mutable {
                try {
                    handler(context, [self](const std::string& payload) {
                        return self->send_sse_event(payload);
                    });
                } catch (const std::exception& e) {
                    self->send_sse_event(error_json(e.what()));
                }
                self->finish_stream();
            });
        }

        bool send_sse_event(const std::string& payload) {
            if (!open_.load()) {
                return false;
            }
            enqueue_raw("data: " + payload + "\n\n", false);
            return open_.load();
        }

        void finish_stream() {
            asio::post(strand_, [self = shared_from_this()]() {
                self->close_after_writes_ = true;
                if (!self->write_in_progress_ && self->write_queue_.empty()) {
                    self->close();
                }
            });
        }

        void enqueue_raw(std::string data, bool close_after) {
            auto chunk = std::make_shared<std::string>(std::move(data));
            asio::post(strand_, [self = shared_from_this(), chunk, close_after]() {
                self->write_queue_.push_back(chunk);
                self->close_after_writes_ = self->close_after_writes_ || close_after;
                if (!self->write_in_progress_) {
                    self->do_write_raw();
                }
            });
        }

        void do_write_raw() {
            if (write_queue_.empty()) {
                if (close_after_writes_) {
                    close();
                }
                return;
            }
            write_in_progress_ = true;
            auto chunk = write_queue_.front();
            stream_.expires_never();
            asio::async_write(stream_.socket(),
                              asio::buffer(*chunk),
                              asio::bind_executor(
                                  strand_,
                                  [self = shared_from_this(), chunk](beast::error_code ec, std::size_t) {
                                      self->write_in_progress_ = false;
                                      if (ec) {
                                          self->open_.store(false);
                                          self->close();
                                          return;
                                      }
                                      if (!self->write_queue_.empty()) {
                                          self->write_queue_.pop_front();
                                      }
                                      self->do_write_raw();
                                  }));
        }

        void close() {
            open_.store(false);
            beast::error_code ec;
            stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
            stream_.socket().close(ec);
        }

        beast::tcp_stream stream_;
        asio::strand<beast::tcp_stream::executor_type> strand_;
        beast::flat_buffer buffer_;
        std::unique_ptr<http::request_parser<http::string_body>> parser_;
        Impl& server_;
        std::deque<std::shared_ptr<std::string>> write_queue_;
        bool write_in_progress_{false};
        bool close_after_writes_{false};
        std::atomic<bool> open_{true};
    };

    void do_accept() {
        acceptor_.async_accept(
            asio::make_strand(ioc_),
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), *this)->run();
                }
                if (running_.load()) {
                    do_accept();
                }
            });
    }

    int port_;
    Options options_;
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};
    std::mutex handlers_mutex_;
    std::map<std::string, RequestHandler> handlers_;
    std::map<std::string, ContextRequestHandler> context_handlers_;
    std::map<std::string, PrefixRequestHandler> prefix_handlers_;
    std::map<std::string, PrefixContextRequestHandler> prefix_context_handlers_;
    std::map<std::string, StreamHandler> stream_handlers_;
    std::map<std::string, ContextStreamHandler> context_stream_handlers_;
    std::map<std::string, WebSocketHandler> websocket_handlers_;
};

AsyncHttpServer::AsyncHttpServer(int port) : AsyncHttpServer(port, Options{}) {}

AsyncHttpServer::AsyncHttpServer(int port, Options options)
    : impl_(new Impl(port, std::move(options))) {}

AsyncHttpServer::~AsyncHttpServer() {
    stop();
    delete impl_;
}

void AsyncHttpServer::register_handler(const std::string& path, RequestHandler handler) {
    impl_->register_handler(path, std::move(handler));
}

void AsyncHttpServer::register_context_handler(const std::string& path, ContextRequestHandler handler) {
    impl_->register_context_handler(path, std::move(handler));
}

void AsyncHttpServer::register_prefix_handler(const std::string& path_prefix, PrefixRequestHandler handler) {
    impl_->register_prefix_handler(path_prefix, std::move(handler));
}

void AsyncHttpServer::register_prefix_context_handler(const std::string& path_prefix, PrefixContextRequestHandler handler) {
    impl_->register_prefix_context_handler(path_prefix, std::move(handler));
}

void AsyncHttpServer::register_stream_handler(const std::string& path, StreamHandler handler) {
    impl_->register_stream_handler(path, std::move(handler));
}

void AsyncHttpServer::register_context_stream_handler(const std::string& path, ContextStreamHandler handler) {
    impl_->register_context_stream_handler(path, std::move(handler));
}

void AsyncHttpServer::register_websocket_handler(const std::string& path, WebSocketHandler handler) {
    impl_->register_websocket_handler(path, std::move(handler));
}

void AsyncHttpServer::start() {
    impl_->start();
}

void AsyncHttpServer::stop() {
    if (impl_) {
        impl_->stop();
    }
}

AsyncHttpServer::Options AsyncHttpServer::options_from_env() {
    Options options;
    options.bind_address = env_string("MINDBRIDGE_NET_BIND_ADDRESS", options.bind_address);
    options.io_threads = env_size("MINDBRIDGE_NET_IO_THREADS", options.io_threads);
    options.max_request_bytes = env_size("MINDBRIDGE_NET_MAX_REQUEST_BYTES", options.max_request_bytes);
    options.request_timeout_sec = env_int("MINDBRIDGE_NET_REQUEST_TIMEOUT_SEC", options.request_timeout_sec);
    options.server_name = env_string("MINDBRIDGE_NET_SERVER_NAME", options.server_name);
    return options;
}

}  // namespace net
}  // namespace mindbridge
