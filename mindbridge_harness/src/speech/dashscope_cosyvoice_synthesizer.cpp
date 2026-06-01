#include "mindbridge/speech/dashscope_cosyvoice_synthesizer.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <cctype>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>
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
    const std::string rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string target = slash == std::string::npos ? "/" : rest.substr(slash);
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
    return {host, port, target.empty() ? "/" : target};
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

std::string encode_base64(const std::vector<unsigned char>& bytes) {
    static constexpr char kChars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    int val = 0;
    int bits = -6;
    for (unsigned char ch : bytes) {
        val = (val << 8) + ch;
        bits += 8;
        while (bits >= 0) {
            out.push_back(kChars[(val >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        out.push_back(kChars[((val << 8) >> (bits + 8)) & 0x3f]);
    }
    while (out.size() % 4) {
        out.push_back('=');
    }
    return out;
}

std::string mime_for_format(const std::string& format) {
    std::string lower = format;
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lower == "mp3" || lower == "mpeg") {
        return "audio/mpeg";
    }
    if (lower == "pcm" || lower == "pcm16") {
        return "audio/pcm";
    }
    return "audio/wav";
}

}  // namespace

DashScopeCosyVoiceSynthesizer::DashScopeCosyVoiceSynthesizer(std::string websocket_url,
                                                             std::string api_key,
                                                             std::string model,
                                                             std::string instruction,
                                                             std::string fallback_model,
                                                             std::string fallback_voice)
    : websocket_url_(std::move(websocket_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)),
      instruction_(std::move(instruction)),
      fallback_model_(std::move(fallback_model)),
      fallback_voice_(std::move(fallback_voice)) {}

namespace {

SpeechSynthesisResult synthesize_once(const std::string& websocket_url,
                                      const std::string& api_key,
                                      const std::string& model,
                                      const std::string& instruction,
                                      const SpeechSynthesisRequest& request,
                                      SpeechSynthesisCallback* callback = nullptr) {
    if (api_key.empty()) {
        throw std::runtime_error("DASHSCOPE_API_KEY or MINDBRIDGE_MODEL_API_KEY is required for TTS");
    }
    if (request.voice.empty()) {
        throw std::runtime_error("MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID is required for CosyVoice synthesis");
    }

    const WsUrl url = parse_wss_url(websocket_url);
    asio::io_context ioc;
    asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
    tcp::resolver resolver(ioc);
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ssl_ctx);
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), url.host.c_str())) {
        throw std::runtime_error("failed to set DashScope TLS SNI host");
    }
    auto const results = resolver.resolve(url.host, url.port);
    beast::get_lowest_layer(ws).connect(results);
    ws.next_layer().handshake(asio::ssl::stream_base::client);
    ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& req) {
        req.set(http::field::authorization, "Bearer " + api_key);
        req.set(http::field::user_agent, "MindBridge/1.0");
    }));
    ws.handshake(url.host, url.target);

    const std::string task_id = random_task_id();
    const std::string format = request.format.empty() ? "mp3" : request.format;
    nlohmann::json run_task = {
        {"header", {{"action", "run-task"}, {"task_id", task_id}, {"streaming", "duplex"}}},
        {"payload",
         {{"task_group", "audio"},
          {"task", "tts"},
          {"function", "SpeechSynthesizer"},
          {"model", model},
          {"parameters",
           {{"text_type", "PlainText"},
            {"voice", request.voice},
            {"format", format},
            {"sample_rate", 22050},
            {"enable_ssml", false}}},
          {"input", nlohmann::json::object()}}},
    };
    if (!instruction.empty()) {
        run_task["payload"]["parameters"]["instruction"] = instruction;
    }

    ws.text(true);
    ws.write(asio::buffer(run_task.dump()));
    bool started = false;
    for (int i = 0; i < 20 && !started; ++i) {
        beast::flat_buffer buffer;
        ws.read(buffer);
        if (!ws.got_text()) {
            continue;
        }
        const auto event = nlohmann::json::parse(json_text_from_buffer(buffer));
        const auto header = event.value("header", nlohmann::json::object());
        const std::string name = header.value("event", "");
        if (name == "task-started") {
            started = true;
        } else if (name == "task-failed") {
            throw std::runtime_error("DashScope TTS task failed: " +
                                     header.value("error_message", header.value("error_code", "")));
        }
    }
    if (!started) {
        throw std::runtime_error("DashScope TTS did not return task-started");
    }
    if (callback) {
        callback->on_open();
    }

    nlohmann::json continue_task = {
        {"header", {{"action", "continue-task"}, {"task_id", task_id}, {"streaming", "duplex"}}},
        {"payload", {{"input", {{"text", request.text}}}}},
    };
    nlohmann::json finish_task = {
        {"header", {{"action", "finish-task"}, {"task_id", task_id}, {"streaming", "duplex"}}},
        {"payload", {{"input", nlohmann::json::object()}}},
    };
    ws.text(true);
    ws.write(asio::buffer(continue_task.dump()));
    ws.write(asio::buffer(finish_task.dump()));

    std::vector<unsigned char> audio;
    bool finished = false;
    while (!finished) {
        beast::flat_buffer buffer;
        ws.read(buffer);
        if (ws.got_text()) {
            const auto event = nlohmann::json::parse(json_text_from_buffer(buffer));
            const auto header = event.value("header", nlohmann::json::object());
            const std::string name = header.value("event", "");
            if (name == "task-finished") {
                finished = true;
            } else if (name == "task-failed") {
                throw std::runtime_error("DashScope TTS task failed: " +
                                         header.value("error_message", header.value("error_code", "")));
            }
        } else {
            const auto data = buffer.data();
            std::vector<unsigned char> chunk;
            for (auto it = asio::buffers_begin(data); it != asio::buffers_end(data); ++it) {
                chunk.push_back(static_cast<unsigned char>(*it));
            }
            audio.insert(audio.end(), chunk.begin(), chunk.end());
            if (callback && !chunk.empty() && !callback->on_data(chunk)) {
                finished = true;
            }
        }
    }

    if (callback) {
        callback->on_complete();
    }
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
    if (audio.empty()) {
        throw std::runtime_error("DashScope TTS returned empty audio");
    }
    return {mime_for_format(format), encode_base64(audio)};
}

}  // namespace

SpeechSynthesisResult DashScopeCosyVoiceSynthesizer::synthesize(
    const SpeechSynthesisRequest& request) {
    try {
        return synthesize_once(websocket_url_, api_key_, model_, instruction_, request);
    } catch (const std::exception& e) {
        const bool can_fallback = !fallback_model_.empty() && !fallback_voice_.empty() &&
                                  (fallback_model_ != model_ || fallback_voice_ != request.voice);
        if (!can_fallback) {
            throw;
        }
        SpeechSynthesisRequest fallback_request = request;
        fallback_request.voice = fallback_voice_;
        try {
            return synthesize_once(websocket_url_, api_key_, fallback_model_, instruction_, fallback_request);
        } catch (const std::exception& fallback_error) {
            throw std::runtime_error(std::string(e.what()) +
                                     "; fallback TTS also failed: " +
                                     fallback_error.what());
        }
    }
}

bool DashScopeCosyVoiceSynthesizer::synthesize_stream(
    const SpeechSynthesisRequest& request,
    SpeechSynthesisCallback& callback) {
    try {
        synthesize_once(websocket_url_, api_key_, model_, instruction_, request, &callback);
        callback.on_close();
        return true;
    } catch (const std::exception& e) {
        const bool can_fallback = !fallback_model_.empty() && !fallback_voice_.empty() &&
                                  (fallback_model_ != model_ || fallback_voice_ != request.voice);
        if (can_fallback) {
            SpeechSynthesisRequest fallback_request = request;
            fallback_request.voice = fallback_voice_;
            try {
                synthesize_once(websocket_url_, api_key_, fallback_model_, instruction_, fallback_request, &callback);
                callback.on_close();
                return true;
            } catch (const std::exception& fallback_error) {
                callback.on_error(std::string(e.what()) +
                                  "; fallback TTS also failed: " +
                                  fallback_error.what());
                callback.on_close();
                return false;
            }
        }
        callback.on_error(e.what());
        callback.on_close();
        return false;
    }
}

}  // namespace mindbridge
