#include "mindbridge/multimodal/multimodal_tools.hpp"

#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/multimodal/dashscope_qwen_multimodal_analyzer.hpp"
#include "mindbridge/multimodal/mimo_multimodal_analyzer.hpp"

#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace mindbridge {

namespace {

size_t env_mb(const char* key, size_t fallback_mb) {
    const char* value = std::getenv(key);
    if (!value || std::string(value).empty()) {
        return fallback_mb;
    }
    try {
        return static_cast<size_t>(std::stoul(value));
    } catch (...) {
        return fallback_mb;
    }
}

size_t base64_limit_chars(size_t mb) {
    return mb * 1024 * 1024 * 4 / 3 + 4096;
}

MultimodalInputPart part_from_json(const nlohmann::json& j) {
    MultimodalInputPart part;
    if (!j.is_object()) {
        return part;
    }
    part.mime_type = j.value("mime_type", "");
    part.data_base64 = j.value("data_base64", "");
    part.url = j.value("url", "");
    return part;
}

std::unique_ptr<MultimodalAnalyzer> make_analyzer_from_env() {
    const std::string provider =
        mindbridge::app::env_or("MINDBRIDGE_MULTIMODAL_PROVIDER", "dashscope");
    if (provider.empty() || provider == "disabled" || provider == "none") {
        return nullptr;
    }
    if (provider == "dashscope" || provider == "qwen") {
        const std::string api_key = mindbridge::app::env_or(
            "MINDBRIDGE_DASHSCOPE_API_KEY",
            mindbridge::app::env_or("DASHSCOPE_API_KEY",
                                    mindbridge::app::env_or("MINDBRIDGE_MODEL_API_KEY")));
        if (api_key.empty()) {
            return nullptr;
        }
        const std::string thinking =
            mindbridge::app::env_or("MINDBRIDGE_QWEN_MULTIMODAL_ENABLE_THINKING",
                                    mindbridge::app::env_or("MINDBRIDGE_QWEN_ENABLE_THINKING", "false"));
        return std::make_unique<DashScopeQwenMultimodalAnalyzer>(
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_BASE_URL",
                                    mindbridge::app::env_or(
                                        "MINDBRIDGE_MODEL_BASE_URL",
                                        "https://dashscope.aliyuncs.com/compatible-mode/v1")),
            api_key,
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_MULTIMODAL_MODEL",
                                    mindbridge::app::env_or("MINDBRIDGE_MODEL_NAME", "qwen3.6-plus")),
            thinking == "true" || thinking == "1");
    }
    if (provider != "mimo") {
        return nullptr;
    }
    const std::string api_key = mindbridge::app::env_or(
        "MINDBRIDGE_MIMO_API_KEY",
        mindbridge::app::env_or("MINDBRIDGE_MODEL_API_KEY"));
    if (api_key.empty()) {
        return nullptr;
    }
    return std::make_unique<MimoMultimodalAnalyzer>(
        mindbridge::app::env_or("MINDBRIDGE_MIMO_BASE_URL", "https://api.xiaomimimo.com/v1"),
        api_key,
        mindbridge::app::env_or("MINDBRIDGE_MIMO_MODEL",
                                mindbridge::app::env_or("MINDBRIDGE_MODEL_NAME", "mimo-v2.5")));
}

}  // namespace

AnalyzeMultimodalEmotionTool::AnalyzeMultimodalEmotionTool(
    std::unique_ptr<MultimodalAnalyzer> analyzer)
    : analyzer_(std::move(analyzer)) {}

ToolSchema AnalyzeMultimodalEmotionTool::schema() const {
    return ToolSchema{
        {{"text", ToolSchema::Field::Type::String, false},
         {"image", ToolSchema::Field::Type::Object, false},
         {"audio", ToolSchema::Field::Type::Object, false}},
        ToolRisk::Low,
        12000};
}

std::optional<std::string> AnalyzeMultimodalEmotionTool::validate(
    const nlohmann::json& arguments) const {
    const std::string text = arguments.value("text", "");
    const bool has_image = arguments.contains("image") && arguments["image"].is_object();
    const bool has_audio = arguments.contains("audio") && arguments["audio"].is_object();
    if (text.empty() && !has_image && !has_audio) {
        return "at least one of text/image/audio is required";
    }
    if (has_image) {
        const auto image = part_from_json(arguments["image"]);
        if (image.url.empty() && image.data_base64.empty()) {
            return "image requires url or data_base64";
        }
        if (!image.data_base64.empty() &&
            image.data_base64.size() > base64_limit_chars(env_mb("MINDBRIDGE_MULTIMODAL_MAX_IMAGE_MB", 10))) {
            return "image data_base64 exceeds MINDBRIDGE_MULTIMODAL_MAX_IMAGE_MB";
        }
    }
    if (has_audio) {
        const auto audio = part_from_json(arguments["audio"]);
        if (audio.url.empty() && audio.data_base64.empty()) {
            return "audio requires url or data_base64";
        }
        if (!audio.data_base64.empty() &&
            audio.data_base64.size() > base64_limit_chars(env_mb("MINDBRIDGE_MULTIMODAL_MAX_AUDIO_MB", 50))) {
            return "audio data_base64 exceeds MINDBRIDGE_MULTIMODAL_MAX_AUDIO_MB";
        }
    }
    return std::nullopt;
}

ToolResult AnalyzeMultimodalEmotionTool::execute(const nlohmann::json& arguments, ToolContext&) {
    ToolResult result;
    if (!analyzer_) {
        result.summary = "multimodal analyzer is disabled";
        result.data = {{"disabled", true},
                       {"risk_level", "low"},
                       {"final_emotion", "normal"},
                       {"weighted_score", 0.0},
                       {"evidence_summary", "multimodal API key or provider is not configured."}};
        return result;
    }
    MultimodalRequest request;
    request.text = arguments.value("text", "");
    if (arguments.contains("image") && arguments["image"].is_object()) {
        request.image = part_from_json(arguments["image"]);
    }
    if (arguments.contains("audio") && arguments["audio"].is_object()) {
        request.audio = part_from_json(arguments["audio"]);
    }
    try {
        result.data = analyzer_->analyze(request);
        result.summary = "multimodal emotion analyzed";
    } catch (const std::exception& e) {
        result.ok = false;
        result.error_code = "multimodal_analysis_failed";
        result.summary = "multimodal emotion analysis failed";
        result.data = {{"disabled", false},
                       {"error", e.what()},
                       {"risk_level", "low"},
                       {"final_emotion", "normal"},
                       {"weighted_score", 0.0}};
    }
    return result;
}

void register_multimodal_tools(ToolRegistry& registry) {
    registry.register_tool(std::make_shared<AnalyzeMultimodalEmotionTool>(make_analyzer_from_env()));
}

}  // namespace mindbridge
