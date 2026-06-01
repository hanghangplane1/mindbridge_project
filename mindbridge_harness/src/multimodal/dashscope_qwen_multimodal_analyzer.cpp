#include "mindbridge/multimodal/dashscope_qwen_multimodal_analyzer.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
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

std::string join_url(const std::string& base, const std::string& path) {
    std::string normalized = base;
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    if (normalized.size() >= 3 &&
        normalized.compare(normalized.size() - 3, 3, "/v1") == 0 &&
        path.rfind("/v1/", 0) == 0) {
        return normalized + path.substr(3);
    }
    return normalized + path;
}

std::string post_dashscope_json(const std::string& url,
                                const std::string& api_key,
                                const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    char error_buffer[CURL_ERROR_SIZE] = {0};
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        const std::string detail = error_buffer[0] ? error_buffer : curl_easy_strerror(rc);
        throw std::runtime_error("DashScope multimodal request failed: " + detail);
    }
    if (code >= 400) {
        throw std::runtime_error("DashScope multimodal HTTP " + std::to_string(code) + ": " +
                                 response.substr(0, 800));
    }
    return response;
}

std::string data_url(const MultimodalInputPart& part, const std::string& fallback_mime) {
    if (!part.url.empty()) {
        return part.url;
    }
    const std::string mime = part.mime_type.empty() ? fallback_mime : part.mime_type;
    return "data:" + mime + ";base64," + part.data_base64;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalize_label(const std::string& raw) {
    const std::string value = lower_copy(raw);
    if (value == "normal" || value == "正常" || value == "low") {
        return "normal";
    }
    if (value == "anxious" || value == "anxiety" || value == "焦虑") {
        return "anxious";
    }
    if (value == "low_mood" || value == "depressed" || value == "sad" || value == "低落") {
        return "low_mood";
    }
    if (value == "high_risk" || value == "risk" || value == "高风险") {
        return "high_risk";
    }
    return "normal";
}

double score_for_label(const std::string& label) {
    const std::string normalized = normalize_label(label);
    if (normalized == "anxious") {
        return 2.0;
    }
    if (normalized == "low_mood") {
        return 3.0;
    }
    if (normalized == "high_risk") {
        return 4.0;
    }
    return 0.0;
}

std::string label_for_score(double score) {
    if (score >= 3.5) {
        return "high_risk";
    }
    if (score >= 2.5) {
        return "low_mood";
    }
    if (score >= 1.0) {
        return "anxious";
    }
    return "normal";
}

std::string risk_for_score(double score) {
    if (score >= 2.5) {
        return "high";
    }
    if (score >= 1.0) {
        return "medium";
    }
    return "low";
}

bool contains_high_risk_language(const std::string& text) {
    static const char* keywords[] = {
        "自杀", "自残", "不想活", "结束生命", "伤害自己", "伤害别人", "活着没意义",
        "suicide", "kill myself", "self harm", "hurt myself", "hurt others"};
    const std::string lower = lower_copy(text);
    for (const char* keyword : keywords) {
        if (lower.find(lower_copy(keyword)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

nlohmann::json parse_json_content(const std::string& raw) {
    const auto begin = raw.find('{');
    const auto end = raw.rfind('}');
    if (begin != std::string::npos && end != std::string::npos && begin <= end) {
        return nlohmann::json::parse(raw.substr(begin, end - begin + 1));
    }
    return nlohmann::json::parse(raw);
}

std::string first_choice_content(const std::string& body) {
    const auto j = nlohmann::json::parse(body);
    if (!j.contains("choices") || j["choices"].empty()) {
        throw std::runtime_error("invalid DashScope chat completion response");
    }
    const auto& message = j["choices"][0]["message"];
    const std::string content = message.value("content", "");
    if (!content.empty()) {
        return content;
    }
    return message.value("reasoning_content", "");
}

nlohmann::json emotion_obj(const nlohmann::json& model_json,
                           const char* key,
                           const std::string& fallback_label,
                           bool present) {
    nlohmann::json source = model_json.value(key, nlohmann::json::object());
    std::string label = fallback_label;
    double confidence = present ? 0.5 : 0.0;
    std::string summary = present ? "" : "not provided";
    if (source.is_object()) {
        label = normalize_label(source.value("label", source.value("emotion", fallback_label)));
        confidence = source.value("confidence", confidence);
        summary = source.value("summary", source.value("reason", summary));
    } else if (source.is_string()) {
        label = normalize_label(source.get<std::string>());
    }
    return {{"label", label},
            {"score", score_for_label(label)},
            {"confidence", confidence},
            {"summary", summary},
            {"present", present}};
}

}  // namespace

DashScopeQwenMultimodalAnalyzer::DashScopeQwenMultimodalAnalyzer(std::string base_url,
                                                                 std::string api_key,
                                                                 std::string model,
                                                                 bool enable_thinking)
    : base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)),
      enable_thinking_(enable_thinking) {}

nlohmann::json DashScopeQwenMultimodalAnalyzer::analyze(const MultimodalRequest& request) {
    nlohmann::json content = nlohmann::json::array();
    std::ostringstream instruction;
    instruction
        << "请分析用户的心理情绪状态。只输出严格 JSON，不要输出 markdown。\n"
        << "情绪标签只能使用 normal, anxious, low_mood, high_risk。\n"
        << "风险等级只能使用 low, medium, high。\n"
        << "分别给出 text_emotion, visual_emotion, final_emotion, risk_level, "
           "weighted_score, evidence_summary。\n"
        << "如果图片不存在，visual_emotion 标签设为 normal、present=false。\n"
        << "用户语音识别/文字输入："
        << (request.text.empty() ? "(empty)" : request.text);
    content.push_back({{"type", "text"}, {"text", instruction.str()}});
    if (request.image.has_value() && request.image->present()) {
        content.push_back({{"type", "image_url"},
                           {"image_url", {{"url", data_url(*request.image, "image/jpeg")}}}});
    }

    nlohmann::json body = {
        {"model", model_},
        {"messages",
         nlohmann::json::array({
             {{"role", "system"},
              {"content",
               "You are a multimodal emotion analysis component for a mental-health "
               "support system. Return compact strict JSON only."}},
             {{"role", "user"}, {"content", content}},
         })},
        {"max_completion_tokens", 1024},
    };
    if (enable_thinking_) {
        body["enable_thinking"] = true;
    }

    const std::string raw = post_dashscope_json(join_url(base_url_, "/v1/chat/completions"),
                                                api_key_,
                                                body.dump());
    const std::string model_content = first_choice_content(raw);
    nlohmann::json parsed = nlohmann::json::object();
    try {
        parsed = parse_json_content(model_content);
    } catch (...) {
        parsed = {{"parse_error", true}, {"raw_preview", model_content.substr(0, 800)}};
    }

    const bool has_image = request.image.has_value() && request.image->present();
    const bool has_text = !request.text.empty();
    nlohmann::json text = emotion_obj(parsed, "text_emotion", "normal", has_text);
    nlohmann::json visual = emotion_obj(parsed, "visual_emotion", "normal", has_image);
    nlohmann::json audio = {{"label", "normal"},
                            {"score", 0.0},
                            {"confidence", 0.0},
                            {"summary", "ASR transcript is analyzed as text"},
                            {"present", false}};

    double weighted = 0.0;
    if (has_image) {
        weighted = text.value("score", 0.0) * 0.45 + visual.value("score", 0.0) * 0.55;
    } else {
        weighted = text.value("score", 0.0);
    }

    std::string risk = parsed.value("risk_level", risk_for_score(weighted));
    if (risk != "low" && risk != "medium" && risk != "high") {
        risk = risk_for_score(weighted);
    }
    std::string final_label = normalize_label(parsed.value("final_emotion", label_for_score(weighted)));
    std::string evidence = parsed.value("evidence_summary", parsed.value("summary", ""));
    if (contains_high_risk_language(request.text) ||
        normalize_label(text.value("label", "normal")) == "high_risk" ||
        normalize_label(visual.value("label", "normal")) == "high_risk") {
        risk = "high";
        final_label = "high_risk";
        weighted = std::max(weighted, 4.0);
    }

    return {{"provider", "dashscope"},
            {"model", model_},
            {"disabled", false},
            {"text_emotion", text},
            {"visual_emotion", visual},
            {"audio_emotion", audio},
            {"final_emotion", final_label},
            {"risk_level", risk},
            {"weighted_score", weighted},
            {"evidence_summary", evidence},
            {"transcript", ""},
            {"raw_model_preview", model_content.substr(0, 800)},
            {"modalities", {{"text", has_text}, {"image", has_image}, {"audio", false}}}};
}

}  // namespace mindbridge
