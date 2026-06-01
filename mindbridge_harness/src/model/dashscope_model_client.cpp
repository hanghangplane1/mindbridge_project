#include "mindbridge/model/dashscope_model_client.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mindbridge {
namespace {

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

int env_int(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (!value || !*value) {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

struct HttpResponse {
    long code{0};
    std::string body;
};

HttpResponse post_json(const std::string& url,
                       const std::string& api_key,
                       const std::string& body,
                       long timeout_sec) {
    if (api_key.empty()) {
        throw std::runtime_error("DashScope API key is required");
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    char error_buffer[CURL_ERROR_SIZE] = {0};
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth.c_str());

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
        throw std::runtime_error("dashscope request failed: " + detail);
    }
    if (code >= 400) {
        throw std::runtime_error("dashscope HTTP " + std::to_string(code) + ": " +
                                 sanitize_utf8(response));
    }
    return {code, response};
}

nlohmann::json message(const std::string& role, const std::string& content) {
    return nlohmann::json{{"role", role}, {"content", sanitize_utf8(content)}};
}

bool uses_multimodal_generation_endpoint(const std::string& model) {
    if (model == "qwen3.6-max-preview") {
        return false;
    }
    return model.rfind("qwen3.6-", 0) == 0 || model.rfind("qwen3.5-", 0) == 0 ||
           model.find("-vl") != std::string::npos;
}

nlohmann::json native_message(const std::string& role,
                              const std::string& content,
                              bool multimodal_endpoint) {
    if (!multimodal_endpoint) {
        return message(role, content);
    }
    return nlohmann::json{{"role", role},
                          {"content", nlohmann::json::array({{{"text", sanitize_utf8(content)}}})}};
}

std::string parse_dashscope_text(const nlohmann::json& payload) {
    const auto output = payload.value("output", nlohmann::json::object());
    if (output.contains("text") && output["text"].is_string()) {
        return sanitize_utf8(output["text"].get<std::string>());
    }
    const auto choices = output.value("choices", nlohmann::json::array());
    if (!choices.empty()) {
        const auto msg = choices[0].value("message", nlohmann::json::object());
        if (msg.contains("content") && msg["content"].is_string()) {
            return sanitize_utf8(msg["content"].get<std::string>());
        }
        if (msg.contains("content") && msg["content"].is_array()) {
            std::ostringstream out;
            for (const auto& item : msg["content"]) {
                if (item.is_string()) {
                    out << item.get<std::string>();
                } else if (item.is_object() && item.contains("text")) {
                    out << item.value("text", "");
                }
            }
            return sanitize_utf8(out.str());
        }
    }
    if (payload.contains("message") && payload["message"].is_string()) {
        throw std::runtime_error("dashscope provider error: " +
                                 sanitize_utf8(payload["message"].get<std::string>()));
    }
    throw std::runtime_error("invalid DashScope generation response");
}

}  // namespace

DashScopeModelClient::DashScopeModelClient(std::string api_key,
                                           std::string model,
                                           std::string base_url)
    : api_key_(std::move(api_key)),
      model_(std::move(model)),
      base_url_(trim_trailing_slash(
          base_url.empty() ? "https://dashscope.aliyuncs.com" : std::move(base_url))) {}

std::string DashScopeModelClient::chat(const std::string& system_prompt,
                                       const std::string& user_message) {
    const bool multimodal_endpoint = uses_multimodal_generation_endpoint(model_);
    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty()) {
        messages.push_back(native_message("system", system_prompt, multimodal_endpoint));
    }
    messages.push_back(native_message("user", user_message, multimodal_endpoint));

    nlohmann::json request = {
        {"model", sanitize_utf8(model_)},
        {"input", {{"messages", messages}}},
        {"parameters",
         {{"result_format", "message"},
          {"max_tokens", env_int("MINDBRIDGE_MODEL_MAX_TOKENS",
                                 env_int("MINDBRIDGE_CHAT_MAX_TOKENS", 512))}}},
    };

    const std::string endpoint = multimodal_endpoint ? "multimodal-generation" : "text-generation";
    const auto response = post_json(
        base_url_ + "/api/v1/services/aigc/" + endpoint + "/generation",
        api_key_,
        request.dump(),
        120L);
    const auto parsed = nlohmann::json::parse(response.body);
    last_completion_metadata_ = {
        {"provider", "dashscope"},
        {"endpoint", endpoint},
        {"usage", parsed.value("usage", nlohmann::json::object())},
    };
    return parse_dashscope_text(parsed);
}

void DashScopeModelClient::chat_stream(
    const std::string& system_prompt,
    const std::string& user_message,
    const std::function<bool(const std::string& chunk)>& on_chunk) {
    const std::string text = chat(system_prompt, user_message);
    if (!text.empty() && on_chunk) {
        on_chunk(text);
    }
}

std::vector<double> DashScopeModelClient::embedding(const std::string&, const std::string&) {
    throw std::runtime_error("DashScope native embedding endpoint is not configured for this client");
}

}  // namespace mindbridge
