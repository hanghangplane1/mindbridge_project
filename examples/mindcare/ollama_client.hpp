#pragma once

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class OllamaClient {
public:
    explicit OllamaClient(std::string base_url = "http://localhost:11434",
                          std::string model = "qwen2.5:7b-chat")
        : base_url_(std::move(base_url)), model_(std::move(model)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~OllamaClient() {
        curl_global_cleanup();
    }

    void set_model(const std::string& model) { model_ = model; }
    const std::string& model() const { return model_; }

    std::string chat(const std::string& system_prompt, const std::string& user_message) const {
        nlohmann::json messages = nlohmann::json::array();
        if (!system_prompt.empty()) {
            messages.push_back({{"role", "system"}, {"content", system_prompt}});
        }
        messages.push_back({{"role", "user"}, {"content", user_message}});
        return chat_with_messages(messages, false);
    }

    std::string chat_with_messages(const nlohmann::json& messages, bool stream = false) const {
        nlohmann::json request{
            {"model", model_},
            {"messages", messages},
            {"stream", stream},
        };
        const std::string raw = post_json(base_url_ + "/api/chat", request.dump(), 60L);
        if (stream) {
            return extract_streamed_content(raw);
        }
        return extract_single_content(raw);
    }

    void chat_stream(const std::string& system_prompt,
                     const std::string& user_message,
                     const std::function<bool(const std::string&)>& on_chunk) const {
        nlohmann::json messages = nlohmann::json::array();
        if (!system_prompt.empty()) {
            messages.push_back({{"role", "system"}, {"content", system_prompt}});
        }
        messages.push_back({{"role", "user"}, {"content", user_message}});
        stream_messages(messages, on_chunk);
    }

    std::vector<double> embedding(const std::string& text,
                                  const std::string& model = "nomic-embed-text") const {
        nlohmann::json request{
            {"model", model},
            {"prompt", text},
        };
        const std::string raw = post_json(base_url_ + "/api/embeddings", request.dump(), 30L);
        nlohmann::json response = nlohmann::json::parse(raw);
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

private:
    struct StreamState {
        std::function<bool(const std::string&)> callback;
        std::string pending;
    };

    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* out = static_cast<std::string*>(userp);
        out->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    static size_t stream_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
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
                    const std::string chunk = j["message"]["content"].get<std::string>();
                    if (!chunk.empty() && state->callback && !state->callback(chunk)) {
                        return 0;
                    }
                }
            } catch (...) {
                // Ignore malformed chunks.
            }
        }
        return size * nmemb;
    }

    std::string post_json(const std::string& url, const std::string& body, long timeout_sec) const {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to init CURL");
        }

        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);

        const CURLcode rc = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
        }
        return response;
    }

    void stream_messages(const nlohmann::json& messages,
                         const std::function<bool(const std::string&)>& on_chunk) const {
        nlohmann::json request{
            {"model", model_},
            {"messages", messages},
            {"stream", true},
        };
        const std::string body = request.dump();

        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to init CURL");
        }

        StreamState state{on_chunk, ""};
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, (base_url_ + "/api/chat").c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

        const CURLcode rc = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
            throw std::runtime_error(std::string("curl stream error: ") + curl_easy_strerror(rc));
        }
    }

    static std::string extract_single_content(const std::string& raw) {
        auto response = nlohmann::json::parse(raw);
        if (response.contains("message") && response["message"].contains("content")) {
            return response["message"]["content"].get<std::string>();
        }
        throw std::runtime_error("Invalid Ollama chat response");
    }

    static std::string extract_streamed_content(const std::string& raw) {
        std::stringstream ss(raw);
        std::string line;
        std::string out;
        while (std::getline(ss, line)) {
            if (line.empty()) {
                continue;
            }
            try {
                auto j = nlohmann::json::parse(line);
                if (j.contains("message") && j["message"].contains("content")) {
                    out += j["message"]["content"].get<std::string>();
                }
            } catch (...) {
                // Ignore malformed chunks.
            }
        }
        return out;
    }

    std::string base_url_;
    std::string model_;
};
