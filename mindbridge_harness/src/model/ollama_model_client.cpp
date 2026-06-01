#include "mindbridge/model/ollama_model_client.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <stdexcept>

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
};

size_t stream_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* state = static_cast<StreamState*>(userp);
    state->pending.append(static_cast<char*>(contents), size * nmemb);
    size_t pos = 0;
    while ((pos = state->pending.find('\n')) != std::string::npos) {
        std::string line = state->pending.substr(0, pos);
        state->pending.erase(0, pos + 1);
        if (line.empty()) {
            continue;
        }
        try {
            auto j = nlohmann::json::parse(line);
            if (j.contains("message") && j["message"].contains("content")) {
                const std::string chunk = sanitize_utf8(j["message"]["content"].get<std::string>());
                if (!chunk.empty() && state->callback && !state->callback(chunk)) {
                    return 0;
                }
            }
        } catch (...) {
        }
    }
    return size * nmemb;
}

std::string post_json(const std::string& url, const std::string& body, long timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
    }
    return response;
}

std::string extract_single_content(const std::string& raw) {
    auto response = nlohmann::json::parse(raw);
    if (response.contains("message") && response["message"].contains("content")) {
        return sanitize_utf8(response["message"]["content"].get<std::string>());
    }
    throw std::runtime_error("Invalid Ollama chat response");
}

}  // namespace

OllamaModelClient::OllamaModelClient(std::string base_url, std::string model)
    : base_url_(std::move(base_url)), model_(std::move(model)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

std::string OllamaModelClient::chat(const std::string& system_prompt, const std::string& user_message) {
    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", sanitize_utf8(system_prompt)}});
    }
    messages.push_back({{"role", "user"}, {"content", sanitize_utf8(user_message)}});
    nlohmann::json request{
        {"model", sanitize_utf8(model_)},
        {"messages", messages},
        {"stream", false},
    };
    return extract_single_content(post_json(base_url_ + "/api/chat", request.dump(), 120L));
}

void OllamaModelClient::chat_stream(const std::string& system_prompt,
                                    const std::string& user_message,
                                    const std::function<bool(const std::string& chunk)>& on_chunk) {
    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", sanitize_utf8(system_prompt)}});
    }
    messages.push_back({{"role", "user"}, {"content", sanitize_utf8(user_message)}});
    nlohmann::json request{
        {"model", sanitize_utf8(model_)},
        {"messages", messages},
        {"stream", true},
    };
    const std::string body = request.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    StreamState state{on_chunk, ""};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, (base_url_ + "/api/chat").c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        throw std::runtime_error(std::string("curl stream: ") + curl_easy_strerror(rc));
    }
}

std::vector<double> OllamaModelClient::embedding(const std::string& text, const std::string& embed_model) {
    nlohmann::json request{
        {"model", sanitize_utf8(embed_model)},
        {"prompt", sanitize_utf8(text)},
    };
    const std::string raw = post_json(base_url_ + "/api/embeddings", request.dump(), 60L);
    auto response = nlohmann::json::parse(raw);
    if (!response.contains("embedding") || !response["embedding"].is_array()) {
        throw std::runtime_error("Invalid Ollama embeddings response");
    }
    std::vector<double> values;
    values.reserve(response["embedding"].size());
    for (const auto& v : response["embedding"]) {
        values.push_back(v.get<double>());
    }
    return values;
}

}  // namespace mindbridge
