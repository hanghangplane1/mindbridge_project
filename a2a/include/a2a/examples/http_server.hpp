#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <cerrno>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

/**
 * @brief 简单的 HTTP 服务器
 * 用于接收 A2A 协议的 HTTP 请求
 * 支持普通请求和 SSE 流式响应
 */
class HttpServer {
public:
    using RequestHandler = std::function<std::string(const std::string&)>;
    using PrefixRequestHandler = std::function<std::string(const std::string&, const std::string&)>;
    // 流式处理器: 接收请求体和写入回调函数
    using StreamHandler = std::function<void(const std::string&, std::function<bool(const std::string&)>)>;
    
    explicit HttpServer(int port) : port_(port), running_(false), server_fd_(-1) {}
    
    ~HttpServer() {
        stop();
    }
    
    void register_handler(const std::string& path, RequestHandler handler) {
        handlers_[path] = handler;
    }

    void register_prefix_handler(const std::string& path_prefix, PrefixRequestHandler handler) {
        prefix_handlers_[path_prefix] = handler;
    }
    
    /**
     * @brief 注册流式处理器
     * @param path 请求路径
     * @param handler 流式处理函数，接收请求体和写入回调
     */
    void register_stream_handler(const std::string& path, StreamHandler handler) {
        stream_handlers_[path] = handler;
    }

    void start() {
        running_ = true;
        signal(SIGPIPE, SIG_IGN);
        
        // 创建 socket
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            throw std::runtime_error("Failed to create socket");
        }
        server_fd_ = server_fd;
        
        // 设置 socket 选项
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // 绑定地址
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);
        
        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            close(server_fd);
            server_fd_ = -1;
            throw std::runtime_error("Failed to bind to port " + std::to_string(port_));
        }
        
        // 监听
        if (listen(server_fd, 10) < 0) {
            close(server_fd);
            server_fd_ = -1;
            throw std::runtime_error("Failed to listen on port " + std::to_string(port_));
        }
        
        std::cout << "HTTP Server listening on port " << port_ << std::endl;
        
        // 接受连接
        while (running_) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                continue;
            }
            
            // 处理请求（在新线程中）
            std::thread([this, client_fd]() {
                this->handle_client(client_fd);
            }).detach();
        }
        
        if (server_fd_ >= 0) {
            close(server_fd);
            server_fd_ = -1;
        }
    }
    
    void stop() {
        running_ = false;
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    }

private:
    static constexpr size_t kReadChunkSize = 4096;
    static constexpr size_t kMaxRequestSize = 8 * 1024 * 1024;

    static std::string trim(const std::string& value) {
        const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch);
        }).base();
        if (begin >= end) {
            return "";
        }
        return std::string(begin, end);
    }

    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    static std::string json_escape(const std::string& value) {
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
                        out << "\\u00";
                        const char* hex = "0123456789abcdef";
                        out << hex[(ch >> 4) & 0x0F] << hex[ch & 0x0F];
                    } else {
                        out << static_cast<char>(ch);
                    }
                    break;
            }
        }
        return out.str();
    }

    static std::string error_json(const std::string& message) {
        return "{\"error\":\"" + json_escape(message) + "\"}";
    }

    static bool send_all(int fd, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t written = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (written <= 0) {
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                return false;
            }
            sent += static_cast<size_t>(written);
        }
        return true;
    }

    static std::string status_text(int status_code) {
        switch (status_code) {
            case 200:
                return "OK";
            case 400:
                return "Bad Request";
            case 404:
                return "Not Found";
            case 413:
                return "Payload Too Large";
            case 500:
                return "Internal Server Error";
            default:
                return "OK";
        }
    }

    static void send_json_response(int client_fd, int status_code, const std::string& response_body) {
        std::ostringstream response;
        response << "HTTP/1.1 " << status_code << " " << status_text(status_code) << "\r\n";
        response << "Content-Type: application/json\r\n";
        response << "Content-Length: " << response_body.length() << "\r\n";
        response << "Access-Control-Allow-Origin: *\r\n";
        response << "\r\n";
        response << response_body;
        send_all(client_fd, response.str());
    }

    static size_t parse_content_length(const std::string& request) {
        const size_t header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return 0;
        }

        std::istringstream stream(request.substr(0, header_end));
        std::string line;
        std::getline(stream, line);  // request line
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string name = lower(trim(line.substr(0, colon)));
            if (name == "content-length") {
                const std::string value = trim(line.substr(colon + 1));
                return static_cast<size_t>(std::stoul(value));
            }
        }
        return 0;
    }

    static bool read_http_request(int client_fd, std::string& request) {
        char buffer[kReadChunkSize];
        size_t header_end = std::string::npos;
        size_t content_length = 0;

        while (true) {
            ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                return !request.empty();
            }
            request.append(buffer, static_cast<size_t>(bytes_read));
            if (request.size() > kMaxRequestSize) {
                throw std::runtime_error("HTTP request exceeds 8MB limit");
            }

            if (header_end == std::string::npos) {
                header_end = request.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    content_length = parse_content_length(request);
                }
            }

            if (header_end != std::string::npos) {
                const size_t body_start = header_end + 4;
                if (request.size() >= body_start + content_length) {
                    return true;
                }
            }
        }
    }

    void handle_client(int client_fd) {
        std::string request;
        try {
            if (!read_http_request(client_fd, request)) {
                close(client_fd);
                return;
            }
        } catch (const std::exception& e) {
            send_json_response(client_fd, 413, error_json(e.what()));
            close(client_fd);
            return;
        }

        // 解析 HTTP 请求
        std::istringstream request_stream(request);
        std::string method, path, version;
        request_stream >> method >> path >> version;
        
        // 提取请求体
        std::string body;
        size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            body = request.substr(body_pos + 4);
        }
        
        // 检查是否需要流式响应（通过检查请求体中的 method）
        bool is_stream_request = false;
        if (!body.empty()) {
            // 简单检查是否包含 message/stream 方法
            is_stream_request = (body.find("\"message/stream\"") != std::string::npos);
        }
        
        // 优先检查流式处理器
        auto stream_it = stream_handlers_.find(path);
        if (stream_it != stream_handlers_.end() &&
            (is_stream_request || path.find("/stream") != std::string::npos)) {
            handle_stream_request(client_fd, body, stream_it->second);
            return;
        }

        // 查找普通处理器
        std::string response_body;
        int status_code = 200;
        
        auto it = handlers_.find(path);
        if (it != handlers_.end()) {
            try {
                response_body = it->second(body);
            } catch (const std::exception& e) {
                status_code = 500;
                response_body = error_json(e.what());
            }
        } else {
            auto prefix_it = std::find_if(prefix_handlers_.begin(), prefix_handlers_.end(),
                                          [&path](const auto& entry) {
                                              return path.rfind(entry.first, 0) == 0;
                                          });
            if (prefix_it != prefix_handlers_.end()) {
                try {
                    response_body = prefix_it->second(path, body);
                } catch (const std::exception& e) {
                    status_code = 500;
                    response_body = error_json(e.what());
                }
            } else {
                status_code = 404;
                response_body = error_json("Not Found");
            }
        }
        
        send_json_response(client_fd, status_code, response_body);
        close(client_fd);
    }
    
    /**
     * @brief 处理流式请求 (SSE - Server-Sent Events)
     */
    void handle_stream_request(int client_fd, const std::string& body, StreamHandler& handler) {
        // 发送 SSE 响应头
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n";
        header << "Content-Type: text/event-stream\r\n";
        header << "Cache-Control: no-cache\r\n";
        header << "Connection: keep-alive\r\n";
        header << "Access-Control-Allow-Origin: *\r\n";
        header << "\r\n";
        
        std::string header_str = header.str();
        if (!send_all(client_fd, header_str)) {
            close(client_fd);
            return;
        }
        
        // 调用流式处理器，传入写入回调
        try {
            handler(body, [client_fd](const std::string& event_data) -> bool {
                // 格式化为 SSE 事件
                std::string sse_event = "data: " + event_data + "\n\n";
                return send_all(client_fd, sse_event);
            });
        } catch (const std::exception& e) {
            // 发送错误事件
            std::string error_event = "data: " + error_json(e.what()) + "\n\n";
            send_all(client_fd, error_event);
        }
        
        close(client_fd);
    }

    int port_;
    std::atomic<bool> running_;
    std::atomic<int> server_fd_;
    std::map<std::string, RequestHandler> handlers_;
    std::map<std::string, PrefixRequestHandler> prefix_handlers_;
    std::map<std::string, StreamHandler> stream_handlers_;
};
