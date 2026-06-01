#include "mindbridge/net/http_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/rfc2818_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <openssl/err.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mindbridge {
namespace net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
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
    out.tls = out.scheme == "https" || out.scheme == "wss";
    if (out.scheme != "http" && out.scheme != "https" && out.scheme != "ws" && out.scheme != "wss") {
        throw std::runtime_error("unsupported URL scheme: " + out.scheme);
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

std::map<std::string, std::string> headers_to_map(const http::response<http::string_body>& response) {
    std::map<std::string, std::string> out;
    for (const auto& field : response.base()) {
        out.emplace(std::string(field.name_string()), std::string(field.value()));
    }
    return out;
}

void ensure_response_size(const HttpResponse& response, std::size_t limit) {
    if (limit > 0 && response.body.size() > limit) {
        throw std::runtime_error("HTTP response exceeds configured limit");
    }
}

HttpResponse request_plain(const ParsedUrl& url,
                           http::verb method,
                           const std::string& body,
                           const HttpClientOptions& options) {
    asio::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(options.timeout_sec));
    stream.connect(resolver.resolve(url.host, url.port));

    http::request<http::string_body> req{method, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, "MindBridge-Net/1.0");
    if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
    }
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    HttpResponse response{static_cast<int>(res.result_int()), headers_to_map(res), res.body()};
    ensure_response_size(response, options.max_response_bytes);
    return response;
}

HttpResponse request_tls(const ParsedUrl& url,
                         http::verb method,
                         const std::string& body,
                         const HttpClientOptions& options) {
    asio::io_context ioc;
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    if (options.verify_tls) {
        if (!options.ca_file.empty()) {
            ctx.load_verify_file(options.ca_file);
        } else {
            ctx.set_default_verify_paths();
        }
        ctx.set_verify_mode(asio::ssl::verify_peer);
    } else {
        ctx.set_verify_mode(asio::ssl::verify_none);
    }

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
        throw beast::system_error(
            beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
    }
    if (options.verify_tls) {
        stream.set_verify_callback(asio::ssl::rfc2818_verification(url.host));
    }

    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(options.timeout_sec));
    beast::get_lowest_layer(stream).connect(resolver.resolve(url.host, url.port));
    stream.handshake(asio::ssl::stream_base::client);

    http::request<http::string_body> req{method, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, "MindBridge-Net/1.0");
    if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
    }
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    beast::error_code ec;
    stream.shutdown(ec);
    if (ec == asio::error::eof || ec == beast::errc::not_connected) {
        ec = {};
    }
    if (ec) {
        throw beast::system_error(ec);
    }

    HttpResponse response{static_cast<int>(res.result_int()), headers_to_map(res), res.body()};
    ensure_response_size(response, options.max_response_bytes);
    return response;
}

template <typename Stream>
void stream_sse_response(Stream& stream,
                         beast::flat_buffer& buffer,
                         const ParsedUrl& url,
                         const std::function<bool(const std::string&)>& on_event,
                         const HttpClientOptions& options);

HttpResponse request(const std::string& raw_url,
                     http::verb method,
                     const std::string& body,
                     const HttpClientOptions& options) {
    const ParsedUrl url = parse_url(raw_url);
    return url.tls ? request_tls(url, method, body, options)
                   : request_plain(url, method, body, options);
}

bool dispatch_sse_block(const std::string& block,
                        const std::function<bool(const std::string&)>& on_event) {
    std::istringstream in(block);
    std::string line;
    std::string payload;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data:", 0) == 0) {
            std::string data = line.substr(5);
            while (!data.empty() && data.front() == ' ') {
                data.erase(data.begin());
            }
            if (!payload.empty()) {
                payload += "\n";
            }
            payload += data;
        }
    }
    return payload.empty() || !on_event || on_event(payload);
}

template <typename Stream>
void write_sse_request(Stream& stream, const ParsedUrl& url, const std::string& body) {
    http::request<http::string_body> req{http::verb::post, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, "MindBridge-Net/1.0");
    req.set(http::field::content_type, "application/json");
    req.set(http::field::accept, "text/event-stream");
    req.body() = body;
    req.prepare_payload();
    http::write(stream, req);
}

template <typename Stream>
void stream_sse_response(Stream& stream,
                         beast::flat_buffer& buffer,
                         const ParsedUrl& url,
                         const std::function<bool(const std::string&)>& on_event,
                         const HttpClientOptions& options) {
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(options.max_response_bytes);
    http::read_header(stream, buffer, parser);
    if (parser.get().result_int() >= 400) {
        throw std::runtime_error("POST " + url.scheme + "://" + url.host + ":" + url.port + url.target +
                                 " stream returned HTTP " + std::to_string(parser.get().result_int()));
    }

    std::string pending;
    char body_buffer[8192];
    while (!parser.is_done()) {
        parser.get().body().data = body_buffer;
        parser.get().body().size = sizeof(body_buffer);
        beast::error_code ec;
        http::read_some(stream, buffer, parser, ec);
        if (ec == http::error::need_buffer) {
            continue;
        }
        if (ec) {
            throw beast::system_error(ec);
        }
        const std::size_t bytes = sizeof(body_buffer) - parser.get().body().size;
        if (bytes == 0) {
            continue;
        }
        pending.append(body_buffer, bytes);
        std::size_t pos = 0;
        while ((pos = pending.find("\n\n")) != std::string::npos) {
            const std::string block = pending.substr(0, pos);
            pending.erase(0, pos + 2);
            if (!dispatch_sse_block(block, on_event)) {
                return;
            }
        }
    }
    if (!pending.empty()) {
        dispatch_sse_block(pending, on_event);
    }
}

void post_sse_plain(const ParsedUrl& url,
                    const std::string& body,
                    const std::function<bool(const std::string&)>& on_event,
                    const HttpClientOptions& options) {
    asio::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(options.timeout_sec));
    stream.connect(resolver.resolve(url.host, url.port));
    write_sse_request(stream, url, body);
    beast::flat_buffer buffer;
    stream_sse_response(stream, buffer, url, on_event, options);
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}

void post_sse_tls(const ParsedUrl& url,
                  const std::string& body,
                  const std::function<bool(const std::string&)>& on_event,
                  const HttpClientOptions& options) {
    asio::io_context ioc;
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    if (options.verify_tls) {
        if (!options.ca_file.empty()) {
            ctx.load_verify_file(options.ca_file);
        } else {
            ctx.set_default_verify_paths();
        }
        ctx.set_verify_mode(asio::ssl::verify_peer);
    } else {
        ctx.set_verify_mode(asio::ssl::verify_none);
    }
    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
        throw beast::system_error(
            beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
    }
    if (options.verify_tls) {
        stream.set_verify_callback(asio::ssl::rfc2818_verification(url.host));
    }
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(options.timeout_sec));
    beast::get_lowest_layer(stream).connect(resolver.resolve(url.host, url.port));
    stream.handshake(asio::ssl::stream_base::client);
    write_sse_request(stream, url, body);
    beast::flat_buffer buffer;
    stream_sse_response(stream, buffer, url, on_event, options);
    beast::error_code ec;
    stream.shutdown(ec);
}

}  // namespace

HttpResponse post_json(const std::string& url,
                       const std::string& body,
                       const HttpClientOptions& options) {
    auto response = request(url, http::verb::post, body, options);
    if (response.status_code >= 400) {
        throw std::runtime_error("POST " + url + " returned HTTP " +
                                 std::to_string(response.status_code) + ": " + response.body);
    }
    return response;
}

HttpResponse get(const std::string& url, const HttpClientOptions& options) {
    auto response = request(url, http::verb::get, "", options);
    if (response.status_code >= 400) {
        throw std::runtime_error("GET " + url + " returned HTTP " +
                                 std::to_string(response.status_code) + ": " + response.body);
    }
    return response;
}

void post_sse(const std::string& url,
              const std::string& body,
              const std::function<bool(const std::string&)>& on_event,
              const HttpClientOptions& options) {
    const ParsedUrl parsed = parse_url(url);
    parsed.tls ? post_sse_tls(parsed, body, on_event, options)
               : post_sse_plain(parsed, body, on_event, options);
}

}  // namespace net
}  // namespace mindbridge
