#include "mindbridge/speech/mimo_speech_synthesizer.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <stdexcept>
#include <utility>

namespace mindbridge {

namespace {

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    const std::string line(buffer, size * nitems);
    auto* content_type = static_cast<std::string*>(userdata);
    const std::string prefix = "content-type:";
    if (line.size() >= prefix.size()) {
        std::string lower = line.substr(0, prefix.size());
        for (char& ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lower == prefix) {
            std::string value = line.substr(prefix.size());
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                value.erase(value.begin());
            }
            while (!value.empty() &&
                   (value.back() == '\r' || value.back() == '\n' ||
                    std::isspace(static_cast<unsigned char>(value.back())))) {
                value.pop_back();
            }
            *content_type = value;
        }
    }
    return size * nitems;
}

std::string join_url(const std::string& base, const std::string& path) {
    std::string normalized = base;
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    if (path.empty()) {
        return normalized;
    }
    if (normalized.size() >= 3 &&
        normalized.compare(normalized.size() - 3, 3, "/v1") == 0 &&
        path.rfind("/v1/", 0) == 0) {
        return normalized + path.substr(3);
    }
    return normalized + (path.front() == '/' ? path : "/" + path);
}

}  // namespace

MimoSpeechSynthesizer::MimoSpeechSynthesizer(std::string base_url,
                                             std::string api_key,
                                             std::string model,
                                             std::string path)
    : base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)),
      path_(std::move(path)) {}

SpeechSynthesisResult MimoSpeechSynthesizer::synthesize(const SpeechSynthesisRequest& request) {
    nlohmann::json body = {
        {"model", model_},
        {"messages",
         nlohmann::json::array({
             {{"role", "user"},
              {"content",
               "请用温暖、自然、稳定的心理支持语气朗读，不要添加原文之外的内容。"}},
             {{"role", "assistant"}, {"content", request.text}},
         })},
        {"audio", {{"format", request.format}, {"voice", request.voice}}},
    };

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    char error_buffer[CURL_ERROR_SIZE] = {0};
    std::string response;
    std::string content_type;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("api-key: " + api_key_).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    const std::string url = join_url(base_url_, path_);
    const std::string payload = body.dump();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &content_type);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
        throw std::runtime_error("mimo tts request failed: " + detail);
    }
    if (code >= 400) {
        throw std::runtime_error("mimo tts HTTP " + std::to_string(code) + ": " + response.substr(0, 800));
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response);
    } catch (const std::exception& e) {
        throw std::runtime_error("mimo tts returned non-JSON response: " + std::string(e.what()));
    }
    if (!parsed.contains("choices") || parsed["choices"].empty()) {
        throw std::runtime_error("invalid MiMo TTS chat completion response");
    }
    const auto& message = parsed["choices"][0]["message"];
    if (!message.contains("audio") || !message["audio"].is_object()) {
        throw std::runtime_error("MiMo TTS response did not contain message.audio");
    }
    const std::string audio_base64 = message["audio"].value("data", "");
    if (audio_base64.empty()) {
        throw std::runtime_error("MiMo TTS response contained empty audio data");
    }
    std::string mime = "audio/wav";
    if (request.format == "mp3" || request.format == "mpeg") {
        mime = "audio/mpeg";
    } else if (request.format == "pcm16") {
        mime = "audio/pcm";
    }
    return {mime, audio_base64};
}

}  // namespace mindbridge
