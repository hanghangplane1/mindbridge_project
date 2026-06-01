#include "mindbridge/speech/dashscope_realtime_speech_recognizer.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mindbridge {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

struct WsUrl {
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
};

WsUrl parse_wss_url(const std::string& url) {
    const std::string prefix = "wss://";
    if (url.rfind(prefix, 0) != 0) {
        throw std::runtime_error("DashScope websocket URL must start with wss://");
    }
    std::string rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (target.empty()) {
        target = "/";
    }
    std::string host = authority;
    std::string port = "443";
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty()) {
        throw std::runtime_error("DashScope websocket URL is missing host");
    }
    return {host, port, target};
}

std::vector<unsigned char> decode_base64(const std::string& input) {
    int values[256];
    for (int& v : values) {
        v = -1;
    }
    const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < chars.size(); ++i) {
        values[static_cast<unsigned char>(chars[i])] = static_cast<int>(i);
    }

    std::vector<unsigned char> out;
    int val = 0;
    int bits = -8;
    for (unsigned char ch : input) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        if (values[ch] == -1) {
            throw std::runtime_error("invalid base64 audio data");
        }
        val = (val << 6) + values[ch];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

std::string random_task_id() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 32; ++i) {
        id.push_back(kHex[dist(gen)]);
    }
    return id;
}

std::string json_text_from_buffer(const beast::flat_buffer& buffer) {
    return beast::buffers_to_string(buffer.data());
}

std::string sentence_text_from_event(const nlohmann::json& event) {
    const auto output = event.value("payload", nlohmann::json::object())
                            .value("output", nlohmann::json::object());
    const auto sentence = output.value("sentence", nlohmann::json::object());
    std::string text = sentence.value("text", "");
    if (!text.empty()) {
        return text;
    }
    if (sentence.contains("words") && sentence["words"].is_array()) {
        std::ostringstream out;
        for (const auto& word : sentence["words"]) {
            out << word.value("text", "");
            out << word.value("punctuation", "");
        }
        return out.str();
    }
    return "";
}

size_t env_size(const char* key, size_t fallback) {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback;
    }
    try {
        return static_cast<size_t>(std::stoul(value));
    } catch (...) {
        return fallback;
    }
}

long env_long(const char* key, long fallback) {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback;
    }
    try {
        return std::stol(value);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

DashScopeRealtimeSpeechRecognizer::DashScopeRealtimeSpeechRecognizer(std::string websocket_url,
                                                                     std::string api_key,
                                                                     std::string model)
    : websocket_url_(std::move(websocket_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)) {}

SpeechRecognitionResult DashScopeRealtimeSpeechRecognizer::recognize(
    const SpeechRecognitionRequest& request) {
    if (api_key_.empty()) {
        throw std::runtime_error("DASHSCOPE_API_KEY or MINDBRIDGE_MODEL_API_KEY is required for ASR");
    }
    const std::vector<unsigned char> audio = decode_base64(request.data_base64);
    if (audio.empty()) {
        throw std::runtime_error("ASR audio payload is empty");
    }

    const WsUrl url = parse_wss_url(websocket_url_);
    asio::io_context ioc;
    asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
    tcp::resolver resolver(ioc);
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ssl_ctx);

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), url.host.c_str())) {
        throw std::runtime_error("failed to set DashScope TLS SNI host");
    }
    auto const results = resolver.resolve(url.host, url.port);
    beast::get_lowest_layer(ws).connect(results);
    ws.next_layer().handshake(asio::ssl::stream_base::client);
    ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& req) {
        req.set(http::field::authorization, "Bearer " + api_key_);
        req.set(http::field::user_agent, "MindBridge/1.0");
    }));
    http::response<http::string_body> handshake_response;
    try {
        ws.handshake(handshake_response, url.host, url.target);
    } catch (const std::exception& e) {
        const auto status = static_cast<unsigned>(handshake_response.result_int());
        const std::string body = handshake_response.body().substr(0, 300);
        throw std::runtime_error(
            "DashScope ASR websocket handshake failed. Set a DashScope ASR-capable key in "
            "MINDBRIDGE_DASHSCOPE_ASR_API_KEY, MINDBRIDGE_DASHSCOPE_API_KEY, or DASHSCOPE_API_KEY; "
            "verify MINDBRIDGE_DASHSCOPE_ASR_WS_URL and MINDBRIDGE_DASHSCOPE_ASR_MODEL; "
            "or set MINDBRIDGE_ASR_PROVIDER=disabled. HTTP status=" + std::to_string(status) +
            " body=" + body + " Upstream error: " +
            std::string(e.what()));
    }

    const std::string task_id = random_task_id();
    nlohmann::json run_task = {
        {"header", {{"action", "run-task"}, {"task_id", task_id}, {"streaming", "duplex"}}},
        {"payload",
         {{"task_group", "audio"},
          {"task", "asr"},
          {"function", "recognition"},
          {"model", model_},
          {"parameters",
           {{"format", request.format.empty() ? "wav" : request.format},
            {"sample_rate", request.sample_rate <= 0 ? 16000 : request.sample_rate}}},
          {"input", nlohmann::json::object()}}},
    };
    ws.text(true);
    ws.write(asio::buffer(run_task.dump()));

    bool started = false;
    auto started_at = std::chrono::steady_clock::now();
    for (int i = 0; i < 20 && !started; ++i) {
        beast::flat_buffer buffer;
        ws.read(buffer);
        const auto event = nlohmann::json::parse(json_text_from_buffer(buffer));
        const std::string name = event.value("header", nlohmann::json::object()).value("event", "");
        if (name == "task-started") {
            started = true;
        } else if (name == "task-failed") {
            const auto header = event.value("header", nlohmann::json::object());
            throw std::runtime_error("DashScope ASR task failed: " +
                                     header.value("error_message", header.value("error_code", "")));
        }
    }
    if (!started) {
        throw std::runtime_error("DashScope ASR did not return task-started");
    }

    ws.binary(true);
    const size_t chunk_size = std::max<size_t>(1, env_size("MINDBRIDGE_ASR_CHUNK_BYTES", 6400));
    const long chunk_delay_ms = std::max<long>(0, env_long("MINDBRIDGE_ASR_CHUNK_DELAY_MS", 100));
    for (size_t offset = 0; offset < audio.size(); offset += chunk_size) {
        const size_t n = std::min(chunk_size, audio.size() - offset);
        ws.write(asio::buffer(audio.data() + offset, n));
        if (chunk_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk_delay_ms));
        }
    }

    nlohmann::json finish_task = {
        {"header", {{"action", "finish-task"}, {"task_id", task_id}, {"streaming", "duplex"}}},
        {"payload", {{"input", nlohmann::json::object()}}},
    };
    ws.text(true);
    ws.write(asio::buffer(finish_task.dump()));

    SpeechRecognitionResult result;
    result.request_id = task_id;
    std::string transcript;
    bool finished = false;
    long first_delay = 0;
    long last_delay = 0;
    while (!finished) {
        beast::flat_buffer buffer;
        ws.read(buffer);
        if (!ws.got_text()) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at).count();
        auto event = nlohmann::json::parse(json_text_from_buffer(buffer));
        const auto header = event.value("header", nlohmann::json::object());
        const std::string name = header.value("event", "");
        if (name == "result-generated") {
            const std::string text = sentence_text_from_event(event);
            if (!text.empty()) {
                transcript = text;
                if (first_delay == 0) {
                    first_delay = elapsed;
                }
                last_delay = elapsed;
            }
        } else if (name == "task-finished") {
            result.request_id = header.value("task_id", task_id);
            finished = true;
        } else if (name == "task-failed") {
            throw std::runtime_error("DashScope ASR task failed: " +
                                     header.value("error_message", header.value("error_code", "")));
        }
    }

    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
    result.transcript = transcript;
    result.first_package_delay_ms = first_delay;
    result.last_package_delay_ms = last_delay;
    return result;
}

}  // namespace mindbridge
