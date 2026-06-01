#include "mindbridge/model/openai_compatible_model_client.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mindbridge {

namespace {

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

struct StreamState {
    std::function<bool(const std::string&)> callback;
    std::string pending;
    std::string error_payload;
    bool saw_sse_data{false};
    bool emitted_content{false};
};

std::string join_url(const std::string& base, const std::string& path) {
    std::string normalized_base = base;
    while (!normalized_base.empty() && normalized_base.back() == '/') {
        normalized_base.pop_back();
    }
    if (normalized_base.size() >= 3 &&
        normalized_base.compare(normalized_base.size() - 3, 3, "/v1") == 0 &&
        path.rfind("/v1/", 0) == 0) {
        return normalized_base + path.substr(3);
    }
    return normalized_base + path;
}

struct HttpResponse {
    long code{0};
    std::string body;
};

std::string auth_header_for_url(const std::string& url, const std::string& api_key) {
    if (api_key.empty()) {
        return "";
    }
    if (url.find("xiaomimimo.com") != std::string::npos) {
        return "api-key: " + api_key;
    }
    return "Authorization: Bearer " + api_key;
}

HttpResponse post_json(const std::string& url,
                       const std::string& api_key,
                       const std::string& body,
                       long timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    char error_buffer[CURL_ERROR_SIZE] = {0};
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!api_key.empty()) {
        headers = curl_slist_append(headers, auth_header_for_url(url, api_key).c_str());
    }
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
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
        throw std::runtime_error("openai-compatible request failed: " + detail);
    }
    if (code >= 400) {
        throw std::runtime_error("openai-compatible HTTP " + std::to_string(code) + ": " + response);
    }
    return {code, response};
}

bool is_retryable_error(const std::string& message) {
    return message.find("HTTP 500") != std::string::npos ||
           message.find("HTTP 502") != std::string::npos ||
           message.find("HTTP 503") != std::string::npos ||
           message.find("HTTP 504") != std::string::npos ||
           message.find("request failed") != std::string::npos;
}

bool env_truthy(const char* key) {
    const char* value = std::getenv(key);
    if (!value) {
        return false;
    }
    const std::string s(value);
    return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on";
}

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

void apply_generation_options(nlohmann::json& request, const std::string& system_prompt) {
    int fallback = env_int("MINDBRIDGE_CHAT_MAX_TOKENS", 512);
    if (system_prompt.find("阶段性心理状态评估器") != std::string::npos) {
        fallback = env_int("MINDBRIDGE_SESSION_SUMMARY_MAX_TOKENS", 900);
    } else if (system_prompt.find("心理风险评估器") != std::string::npos) {
        fallback = env_int("MINDBRIDGE_EVALUATOR_MAX_TOKENS", 768);
    }
    const int max_tokens = env_int("MINDBRIDGE_MODEL_MAX_TOKENS", fallback);
    if (max_tokens > 0) {
        request["max_completion_tokens"] = max_tokens;
    }
}

bool qwen_thinking_enabled(const std::string& system_prompt) {
    if (system_prompt.find("心理风险评估器") != std::string::npos ||
        system_prompt.find("阶段性心理状态评估器") != std::string::npos) {
        return env_truthy("MINDBRIDGE_QWEN_EVALUATOR_ENABLE_THINKING") ||
               env_truthy("MINDBRIDGE_QWEN_ENABLE_THINKING");
    }
    return env_truthy("MINDBRIDGE_QWEN_CHAT_ENABLE_THINKING");
}

bool is_qwen_request(const std::string& base_url, const std::string& model) {
    return base_url.find("dashscope.aliyuncs.com") != std::string::npos ||
           model.rfind("qwen", 0) == 0;
}

HttpResponse post_json_with_retries(const std::string& url,
                                    const std::string& api_key,
                                    const std::string& body,
                                    long timeout_sec,
                                    int max_attempts = 3) {
    std::string last_error;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            return post_json(url, api_key, body, timeout_sec);
        } catch (const std::exception& e) {
            last_error = e.what();
            if (attempt == max_attempts || !is_retryable_error(last_error)) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(350 * attempt));
        }
    }
    throw std::runtime_error(last_error.empty() ? "openai-compatible request failed" : last_error);
}

size_t stream_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* state = static_cast<StreamState*>(userp);
    state->pending.append(static_cast<char*>(contents), size * nmemb);
    size_t pos = 0;
    while ((pos = state->pending.find('\n')) != std::string::npos) {
        std::string line = state->pending.substr(0, pos);
        state->pending.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data:", 0) != 0) {
            continue;
        }
        std::string payload = line.substr(5);
        while (!payload.empty() && payload.front() == ' ') {
            payload.erase(payload.begin());
        }
        if (payload == "[DONE]") {
            continue;
        }
        state->saw_sse_data = true;
        try {
            auto j = nlohmann::json::parse(payload);
            if (j.contains("error")) {
                state->error_payload = sanitize_utf8(j["error"].dump());
                continue;
            }
            const auto& choices = j["choices"];
            if (!choices.empty()) {
                const std::string chunk = sanitize_utf8(
                    choices[0].value("delta", nlohmann::json::object()).value("content", ""));
                if (!chunk.empty()) {
                    state->emitted_content = true;
                }
                if (!chunk.empty() && state->callback && !state->callback(chunk)) {
                    return 0;
                }
            }
        } catch (...) {
        }
    }
    return size * nmemb;
}

}  // namespace

OpenAICompatibleModelClient::OpenAICompatibleModelClient(std::string base_url,
                                                         std::string api_key,
                                                         std::string model,
                                                         std::string embedding_model)
    : base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)),
      embedding_model_(std::move(embedding_model)) {}

std::string OpenAICompatibleModelClient::chat(const std::string& system_prompt,
                                              const std::string& user_message) {
    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", sanitize_utf8(system_prompt)}});
    }
    messages.push_back({{"role", "user"}, {"content", sanitize_utf8(user_message)}});
    nlohmann::json request{{"model", sanitize_utf8(model_)}, {"messages", messages}, {"stream", false}};
    apply_generation_options(request, system_prompt);
    if (is_qwen_request(base_url_, model_)) {
        request["enable_thinking"] = qwen_thinking_enabled(system_prompt);
    }
    const auto response =
        post_json_with_retries(join_url(base_url_, "/v1/chat/completions"), api_key_, request.dump(), 120L);
    auto j = nlohmann::json::parse(response.body);
    if (!j.contains("choices") || j["choices"].empty()) {
        throw std::runtime_error("invalid chat completion response");
    }
    nlohmann::json usage = j.value("usage", nlohmann::json::object());
    last_completion_metadata_ = {
        {"cache_supported", true},
        {"cache_hit", usage.value("cached_tokens", 0) > 0},
        {"cached_tokens", usage.value("cached_tokens", 0)},
        {"input_tokens", usage.value("prompt_tokens", 0)},
        {"output_tokens", usage.value("completion_tokens", 0)},
    };
    return sanitize_utf8(j["choices"][0]["message"].value("content", ""));
}

void OpenAICompatibleModelClient::chat_stream(
    const std::string& system_prompt,
    const std::string& user_message,
    const std::function<bool(const std::string& chunk)>& on_chunk) {
    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", sanitize_utf8(system_prompt)}});
    }
    messages.push_back({{"role", "user"}, {"content", sanitize_utf8(user_message)}});
    nlohmann::json request{{"model", sanitize_utf8(model_)}, {"messages", messages}, {"stream", true}};
    apply_generation_options(request, system_prompt);
    if (is_qwen_request(base_url_, model_)) {
        request["enable_thinking"] = qwen_thinking_enabled(system_prompt);
    }
    const std::string body = request.dump();
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    StreamState state;
    state.callback = on_chunk;
    char error_buffer[CURL_ERROR_SIZE] = {0};
    const std::string url = join_url(base_url_, "/v1/chat/completions");
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!api_key_.empty()) {
        headers = curl_slist_append(headers, auth_header_for_url(url, api_key_).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
        throw std::runtime_error("openai-compatible stream failed: " + detail);
    }
    if (http_code >= 400) {
        throw std::runtime_error("openai-compatible stream returned HTTP " + std::to_string(http_code) +
                                 (state.pending.empty() ? "" : ": " + sanitize_utf8(state.pending)));
    }
    if (!state.error_payload.empty()) {
        throw std::runtime_error("openai-compatible stream returned provider error: " + state.error_payload);
    }
    if (!state.saw_sse_data && !state.pending.empty()) {
        throw std::runtime_error("openai-compatible stream returned non-SSE response: " +
                                 sanitize_utf8(state.pending));
    }
    last_completion_metadata_ = {
        {"cache_supported", true},
        {"cache_hit", false},
        {"cached_tokens", 0},
    };
}

std::vector<double> OpenAICompatibleModelClient::embedding(const std::string& text,
                                                          const std::string& embed_model) {
    const std::string model = !embed_model.empty() ? embed_model : embedding_model_;
    if (model.empty()) {
        throw std::runtime_error("embedding model is not configured");
    }
    nlohmann::json request{{"model", sanitize_utf8(model)}, {"input", sanitize_utf8(text)}};
    const auto response =
        post_json_with_retries(join_url(base_url_, "/v1/embeddings"), api_key_, request.dump(), 60L);
    auto j = nlohmann::json::parse(response.body);
    if (!j.contains("data") || j["data"].empty() || !j["data"][0].contains("embedding")) {
        throw std::runtime_error("invalid embeddings response");
    }
    std::vector<double> values;
    for (const auto& v : j["data"][0]["embedding"]) {
        values.push_back(v.get<double>());
    }
    last_completion_metadata_ = {
        {"cache_supported", true},
        {"cache_hit", false},
        {"cached_tokens", 0},
    };
    return values;
}

}  // namespace mindbridge
