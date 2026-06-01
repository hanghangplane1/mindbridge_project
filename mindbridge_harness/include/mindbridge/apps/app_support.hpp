#pragma once

#include "mindbridge/model/ollama_model_client.hpp"
#include "mindbridge/model/openai_compatible_model_client.hpp"
#include "mindbridge/model/dashscope_model_client.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mindbridge {
namespace app {

inline std::string env_or(const char* key, const std::string& fallback = "") {
    const char* value = std::getenv(key);
    return value ? std::string(value) : fallback;
}

inline std::string json_string_or(const nlohmann::json& object,
                                  const char* key,
                                  const std::string& fallback = "") {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
        return fallback;
    }
    return sanitize_utf8(object[key].get<std::string>());
}

inline nlohmann::json json_object_or(const nlohmann::json& object,
                                     const char* key,
                                     const nlohmann::json& fallback = nlohmann::json::object()) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_object()) {
        return fallback;
    }
    return object[key];
}

inline std::unique_ptr<ModelClient> create_model_client_from_env() {
    std::string provider = env_or("MINDBRIDGE_MODEL_PROVIDER", "auto");
    std::string base_url = env_or("MINDBRIDGE_MODEL_BASE_URL");
    std::string api_key = env_or("MINDBRIDGE_MODEL_API_KEY");
    const bool has_dashscope_key =
        !env_or("MINDBRIDGE_DASHSCOPE_API_KEY", env_or("DASHSCOPE_API_KEY")).empty();
    if (api_key.empty()) {
        api_key = env_or("MINDBRIDGE_DASHSCOPE_API_KEY", env_or("DASHSCOPE_API_KEY"));
    }
    if ((api_key.rfind("http://", 0) == 0 || api_key.rfind("https://", 0) == 0) &&
        base_url.empty()) {
        base_url = api_key.find("xiaomimimo.com/anthropic") != std::string::npos
                       ? "https://api.xiaomimimo.com/v1"
                       : api_key;
        api_key.clear();
    }
    if (api_key.rfind("http://", 0) == 0 || api_key.rfind("https://", 0) == 0) {
        api_key = "";
    }
    if (api_key.empty()) {
        api_key = env_or("MINDBRIDGE_MIMO_API_KEY");
    }
    if (provider.empty() || provider == "auto") {
        if (has_dashscope_key && base_url.empty()) {
            provider = "dashscope_native";
        } else {
            provider = (!base_url.empty() || !api_key.empty()) ? "openai_compatible" : "ollama";
        }
    }
    if (provider == "dashscope" || provider == "dashscope_native") {
        std::cerr << "[mindbridge] model_provider=dashscope_native model="
                  << env_or("MINDBRIDGE_MODEL_NAME", "qwen-plus") << std::endl;
        return std::make_unique<DashScopeModelClient>(
            api_key,
            env_or("MINDBRIDGE_MODEL_NAME", "qwen-plus"),
            env_or("MINDBRIDGE_DASHSCOPE_NATIVE_BASE_URL", "https://dashscope.aliyuncs.com"));
    }
    if (provider == "openai_compatible" || provider == "openai") {
        if (base_url.empty()) {
            throw std::runtime_error(
                "MINDBRIDGE_MODEL_BASE_URL is required when using openai_compatible model provider");
        }
        std::cerr << "[mindbridge] model_provider=openai_compatible model="
                  << env_or("MINDBRIDGE_MODEL_NAME", "qwen3.6-plus") << std::endl;
        return std::make_unique<OpenAICompatibleModelClient>(
            base_url,
            api_key,
            env_or("MINDBRIDGE_MODEL_NAME", "qwen3.6-plus"),
            env_or("MINDBRIDGE_EMBEDDING_MODEL"));
    }
    if (env_or("MINDBRIDGE_REQUIRE_REMOTE_MODEL") == "1" ||
        env_or("MINDBRIDGE_REQUIRE_REMOTE_MODEL") == "true") {
        throw std::runtime_error(
            "remote model is required, but no remote API provider was configured; set "
            "MINDBRIDGE_MODEL_PROVIDER=dashscope_native with DASHSCOPE_API_KEY, or "
            "MINDBRIDGE_MODEL_PROVIDER=openai_compatible with MINDBRIDGE_MODEL_BASE_URL and "
            "MINDBRIDGE_MODEL_API_KEY");
    }
    std::cerr << "[mindbridge] model_provider=ollama model="
              << env_or("MINDBRIDGE_MODEL_NAME", "qwen2.5:7b-chat") << std::endl;
    return std::make_unique<OllamaModelClient>(
        env_or("MINDBRIDGE_OLLAMA_URL", "http://localhost:11434"),
        env_or("MINDBRIDGE_MODEL_NAME", "qwen2.5:7b-chat"));
}

inline std::string json_error(const std::string& id, int code, const std::string& message) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id.empty() ? "unknown" : sanitize_utf8(id)},
        {"error", {{"code", code}, {"message", sanitize_utf8(message)}}},
    }.dump();
}

inline std::string extract_text_from_jsonrpc(const nlohmann::json& request) {
    const auto& message = request.at("params").at("message");
    if (!message.contains("parts") || !message["parts"].is_array() || message["parts"].empty()) {
        return "";
    }
    std::string out;
    for (const auto& part : message["parts"]) {
        if (!part.is_object()) {
            continue;
        }
        std::string kind = json_string_or(part, "kind");
        if (kind.empty()) {
            kind = json_string_or(part, "type");
        }
        if (kind == "text" || part.contains("text") || part.contains("content")) {
            std::string text = json_string_or(part, "text");
            if (text.empty()) {
                text = json_string_or(part, "content");
            }
            if (!text.empty()) {
                if (!out.empty()) {
                    out += "\n";
                }
                out += text;
            }
        }
    }
    return out;
}

inline nlohmann::json extract_multimodal_arguments_from_jsonrpc(const nlohmann::json& request) {
    nlohmann::json out = nlohmann::json::object();
    out["text"] = extract_text_from_jsonrpc(request);
    const auto& message = request.at("params").at("message");
    if (!message.contains("parts") || !message["parts"].is_array()) {
        return out;
    }
    for (const auto& part : message["parts"]) {
        if (!part.is_object()) {
            continue;
        }
        std::string kind = json_string_or(part, "kind");
        if (kind.empty()) {
            kind = json_string_or(part, "type");
        }
        if (kind == "image" && !out.contains("image")) {
            out["image"] = {{"mime_type", json_string_or(part, "mime_type", "image/jpeg")},
                            {"data_base64", json_string_or(part, "data_base64")},
                            {"url", json_string_or(part, "url")}};
        } else if (kind == "audio" && !out.contains("audio")) {
            out["audio"] = {{"mime_type", json_string_or(part, "mime_type", "audio/webm")},
                            {"data_base64", json_string_or(part, "data_base64")},
                            {"url", json_string_or(part, "url")}};
        }
    }
    return out;
}

inline bool has_non_text_parts(const nlohmann::json& request) {
    const auto& message = request.at("params").at("message");
    if (!message.contains("parts") || !message["parts"].is_array()) {
        return false;
    }
    for (const auto& part : message["parts"]) {
        if (!part.is_object()) {
            continue;
        }
        std::string kind = json_string_or(part, "kind");
        if (kind.empty()) {
            kind = json_string_or(part, "type");
        }
        if (kind == "image" || kind == "audio" || kind == "video") {
            return true;
        }
    }
    return false;
}

inline std::string multimodal_context_text(const nlohmann::json& multimodal) {
    if (!multimodal.is_object() || multimodal.value("disabled", false)) {
        return "";
    }
    std::string out = "多模态情绪先验：";
    out += "final_emotion=" + multimodal.value("final_emotion", "normal");
    out += ", risk_level=" + multimodal.value("risk_level", "low");
    out += ", weighted_score=" + std::to_string(multimodal.value("weighted_score", 0.0));
    const std::string evidence = multimodal.value("evidence_summary", "");
    if (!evidence.empty()) {
        out += ", evidence=" + evidence;
    }
    const std::string transcript = multimodal.value("transcript", "");
    if (!transcript.empty()) {
        out += ", audio_transcript=" + transcript;
    }
    return out;
}

inline std::string extract_context_id(const nlohmann::json& request) {
    const auto& message = request.at("params").at("message");
    std::string context_id = json_string_or(message, "contextId");
    if (context_id.empty()) {
        context_id = json_string_or(message, "context_id", "default");
    }
    return context_id;
}

inline std::string request_id_from(const nlohmann::json& request) {
    if (!request.contains("id")) {
        return "unknown";
    }
    if (request["id"].is_string()) {
        return request["id"].get<std::string>();
    }
    return request["id"].dump();
}

inline size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

struct SseForwardState {
    std::function<bool(const std::string&)> on_event;
    std::string pending;
};

inline bool dispatch_sse_block(const std::string& block,
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

inline size_t sse_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* state = static_cast<SseForwardState*>(userp);
    state->pending.append(static_cast<char*>(contents), size * nmemb);
    size_t pos = 0;
    while ((pos = state->pending.find("\n\n")) != std::string::npos) {
        const std::string block = state->pending.substr(0, pos);
        state->pending.erase(0, pos + 2);
        if (!dispatch_sse_block(block, state->on_event)) {
            return 0;
        }
    }
    return size * nmemb;
}

inline std::string post_json(const std::string& url,
                             const std::string& body,
                             long timeout_sec = 120L) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    char error_buffer[CURL_ERROR_SIZE] = {0};
    std::string response;
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
        throw std::runtime_error("POST " + url + " failed: " + detail);
    }
    if (http_code >= 400) {
        throw std::runtime_error("POST " + url + " returned HTTP " + std::to_string(http_code) + ": " + response);
    }
    return response;
}

inline void post_sse(const std::string& url,
                     const std::string& body,
                     const std::function<bool(const std::string&)>& on_event,
                     long timeout_sec = 120L) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    char error_buffer[CURL_ERROR_SIZE] = {0};
    SseForwardState state{on_event, ""};
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!state.pending.empty()) {
        dispatch_sse_block(state.pending, on_event);
    }
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
        throw std::runtime_error("POST " + url + " stream failed: " + detail);
    }
    if (http_code >= 400) {
        throw std::runtime_error("POST " + url + " stream returned HTTP " + std::to_string(http_code));
    }
}

}  // namespace app
}  // namespace mindbridge
