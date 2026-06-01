#include "mindbridge/speech/speech_tools.hpp"

#include "mindbridge/apps/app_support.hpp"
#include "mindbridge/speech/dashscope_cosyvoice_synthesizer.hpp"
#include "mindbridge/speech/dashscope_realtime_speech_recognizer.hpp"
#include "mindbridge/speech/mimo_speech_synthesizer.hpp"

#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace mindbridge {

namespace {

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool looks_like_dashscope_system_voice(const std::string& voice) {
    return starts_with(voice, "long") || starts_with(voice, "loong");
}

std::string dashscope_tts_model_for_voice(std::string model, const std::string& voice) {
    if (looks_like_dashscope_system_voice(voice) &&
        (model == "cosyvoice-v3.5-plus" || model == "cosyvoice-v3.5-flash")) {
        return "cosyvoice-v3-flash";
    }
    return model;
}

std::string dashscope_voice_from_env() {
    return mindbridge::app::env_or(
        "MINDBRIDGE_DASHSCOPE_TTS_VOICE_ID",
        mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_FALLBACK_VOICE", "longanyang"));
}

}  // namespace

std::unique_ptr<SpeechSynthesizer> create_speech_synthesizer_from_env() {
    const std::string provider = mindbridge::app::env_or("MINDBRIDGE_TTS_PROVIDER", "dashscope");
    if (provider.empty() || provider == "disabled" || provider == "none") {
        return nullptr;
    }
    if (provider == "dashscope" || provider == "cosyvoice") {
        const std::string api_key = mindbridge::app::env_or(
            "MINDBRIDGE_DASHSCOPE_TTS_API_KEY",
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_API_KEY",
                                    mindbridge::app::env_or("DASHSCOPE_API_KEY",
                                                            mindbridge::app::env_or("MINDBRIDGE_MODEL_API_KEY"))));
        if (api_key.empty()) {
            return nullptr;
        }
        const std::string voice_id = dashscope_voice_from_env();
        const std::string requested_model =
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_MODEL",
                                    "cosyvoice-v3-flash");
        const std::string fallback_voice =
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_FALLBACK_VOICE", voice_id);
        return std::make_unique<DashScopeCosyVoiceSynthesizer>(
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_WS_URL",
                                    "wss://dashscope.aliyuncs.com/api-ws/v1/inference"),
            api_key,
            dashscope_tts_model_for_voice(requested_model, voice_id),
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_INSTRUCTION"),
            mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_TTS_FALLBACK_MODEL",
                                    "cosyvoice-v3-flash"),
            fallback_voice);
    }
    if (provider != "mimo") {
        return nullptr;
    }
    const std::string api_key = mindbridge::app::env_or(
        "MINDBRIDGE_MIMO_TTS_API_KEY",
        mindbridge::app::env_or("MINDBRIDGE_MIMO_API_KEY",
                                mindbridge::app::env_or("MINDBRIDGE_MODEL_API_KEY")));
    if (api_key.empty()) {
        return nullptr;
    }
    return std::make_unique<MimoSpeechSynthesizer>(
        mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_BASE_URL",
                                mindbridge::app::env_or("MINDBRIDGE_MIMO_BASE_URL",
                                                        "https://api.xiaomimimo.com/v1")),
        api_key,
        mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_MODEL",
                                "mimo-v2.5-tts"),
        mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_PATH", "/v1/chat/completions"));
}

namespace {

std::unique_ptr<SpeechRecognizer> make_recognizer_from_env() {
    const std::string provider = mindbridge::app::env_or("MINDBRIDGE_ASR_PROVIDER", "dashscope");
    if (provider.empty() || provider == "disabled" || provider == "none") {
        return nullptr;
    }
    if (provider != "dashscope" && provider != "fun_asr") {
        return nullptr;
    }
    const std::string api_key = mindbridge::app::env_or(
        "MINDBRIDGE_DASHSCOPE_ASR_API_KEY",
        mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_API_KEY",
                                mindbridge::app::env_or("DASHSCOPE_API_KEY")));
    if (api_key.empty()) {
        return nullptr;
    }
    return std::make_unique<DashScopeRealtimeSpeechRecognizer>(
        mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_ASR_WS_URL",
                                "wss://dashscope.aliyuncs.com/api-ws/v1/inference"),
        api_key,
        mindbridge::app::env_or("MINDBRIDGE_DASHSCOPE_ASR_MODEL", "fun-asr-realtime"));
}

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

}  // namespace

SynthesizeSpeechTool::SynthesizeSpeechTool(std::unique_ptr<SpeechSynthesizer> synthesizer)
    : synthesizer_(std::move(synthesizer)) {}

ToolSchema SynthesizeSpeechTool::schema() const {
    return ToolSchema{
        {{"text", ToolSchema::Field::Type::String, true},
         {"voice", ToolSchema::Field::Type::String, false},
         {"format", ToolSchema::Field::Type::String, false}},
        ToolRisk::Low,
        8000000};
}

std::optional<std::string> SynthesizeSpeechTool::validate(const nlohmann::json& arguments) const {
    const std::string text = arguments.value("text", "");
    if (text.empty()) {
        return "text cannot be empty";
    }
    if (text.size() > 2000) {
        return "text exceeds 2000 chars";
    }
    return std::nullopt;
}

ToolResult SynthesizeSpeechTool::execute(const nlohmann::json& arguments, ToolContext&) {
    ToolResult result;
    if (disabled_) {
        result.summary = "tts synthesizer is auto-disabled";
        result.data = {{"disabled", true},
                       {"reason", "TTS was disabled after repeated failures"}};
        return result;
    }
    if (!synthesizer_) {
        result.summary = "tts synthesizer is disabled";
        result.data = {{"disabled", true},
                       {"reason", "MiMo TTS API key or provider is not configured"}};
        return result;
    }
    try {
        const SpeechSynthesisResult audio = synthesizer_->synthesize(
            {arguments.value("text", ""),
             arguments.value("voice",
                             mindbridge::app::env_or("MINDBRIDGE_TTS_PROVIDER", "dashscope") == "mimo"
                                 ? mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_VOICE", "mimo_default")
                                 : dashscope_voice_from_env()),
             arguments.value("format",
                             mindbridge::app::env_or(
                                 "MINDBRIDGE_DASHSCOPE_TTS_FORMAT",
                                 mindbridge::app::env_or("MINDBRIDGE_MIMO_TTS_FORMAT", "mp3")))});
        result.summary = "speech synthesized";
        result.data = {{"disabled", false},
                       {"mime_type", audio.mime_type},
                       {"data_base64", audio.data_base64}};
        failure_count_ = 0;
    } catch (const std::exception& e) {
        ++failure_count_;
        if (failure_count_ >= static_cast<int>(env_mb("MINDBRIDGE_TTS_AUTO_DISABLE_THRESHOLD", 5))) {
            disabled_ = true;
        }
        result.ok = false;
        result.error_code = "speech_synthesis_failed";
        result.summary = "speech synthesis failed";
        result.data = {{"disabled", false}, {"error", e.what()}};
    }
    return result;
}

RecognizeSpeechTool::RecognizeSpeechTool(std::unique_ptr<SpeechRecognizer> recognizer)
    : recognizer_(std::move(recognizer)) {}

ToolSchema RecognizeSpeechTool::schema() const {
    return ToolSchema{
        {{"audio", ToolSchema::Field::Type::Object, true},
         {"format", ToolSchema::Field::Type::String, false},
         {"sample_rate", ToolSchema::Field::Type::Number, false}},
        ToolRisk::Low,
        8000};
}

std::optional<std::string> RecognizeSpeechTool::validate(const nlohmann::json& arguments) const {
    if (!arguments.contains("audio") || !arguments["audio"].is_object()) {
        return "audio object is required";
    }
    const auto& audio = arguments["audio"];
    const std::string data = audio.value("data_base64", "");
    if (data.empty()) {
        return "audio.data_base64 is required";
    }
    if (data.size() > base64_limit_chars(env_mb("MINDBRIDGE_ASR_MAX_AUDIO_MB", 25))) {
        return "audio data_base64 exceeds MINDBRIDGE_ASR_MAX_AUDIO_MB";
    }
    const int sample_rate = arguments.value("sample_rate", 16000);
    if (sample_rate <= 0 || sample_rate > 48000) {
        return "sample_rate must be between 1 and 48000";
    }
    return std::nullopt;
}

ToolResult RecognizeSpeechTool::execute(const nlohmann::json& arguments, ToolContext&) {
    ToolResult result;
    if (!recognizer_) {
        result.summary = "speech recognizer is disabled";
        result.data = {{"disabled", true},
                       {"reason", "DashScope ASR API key or provider is not configured"},
                       {"transcript", ""}};
        return result;
    }
    try {
        const auto& audio = arguments["audio"];
        const SpeechRecognitionResult recognized = recognizer_->recognize(
            {audio.value("mime_type", "audio/wav"),
             audio.value("data_base64", ""),
             arguments.value("format", "wav"),
             arguments.value("sample_rate", 16000)});
        result.summary = "speech recognized";
        result.data = {{"disabled", false},
                       {"transcript", recognized.transcript},
                       {"request_id", recognized.request_id},
                       {"first_package_delay_ms", recognized.first_package_delay_ms},
                       {"last_package_delay_ms", recognized.last_package_delay_ms}};
    } catch (const std::exception& e) {
        result.ok = false;
        result.error_code = "speech_recognition_failed";
        result.summary = "speech recognition failed";
        result.data = {{"disabled", false}, {"error", e.what()}, {"transcript", ""}};
    }
    return result;
}

void register_speech_tools(ToolRegistry& registry) {
    registry.register_tool(std::make_shared<SynthesizeSpeechTool>(create_speech_synthesizer_from_env()));
    registry.register_tool(std::make_shared<RecognizeSpeechTool>(make_recognizer_from_env()));
}

}  // namespace mindbridge
