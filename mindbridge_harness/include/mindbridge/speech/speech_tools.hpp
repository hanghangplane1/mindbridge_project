#pragma once

#include "mindbridge/harness/tool.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/speech/speech_recognizer.hpp"
#include "mindbridge/speech/speech_synthesizer.hpp"

#include <memory>

namespace mindbridge {

class SynthesizeSpeechTool : public ITool {
public:
    explicit SynthesizeSpeechTool(std::unique_ptr<SpeechSynthesizer> synthesizer);

    std::string name() const override { return "synthesize_speech"; }
    std::string description() const override { return "Synthesize assistant text into speech audio."; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return true; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    std::unique_ptr<SpeechSynthesizer> synthesizer_;
    int failure_count_{0};
    bool disabled_{false};
};

class RecognizeSpeechTool : public ITool {
public:
    explicit RecognizeSpeechTool(std::unique_ptr<SpeechRecognizer> recognizer);

    std::string name() const override { return "recognize_speech"; }
    std::string description() const override { return "Recognize captured user speech into text."; }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return true; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    std::unique_ptr<SpeechRecognizer> recognizer_;
};

void register_speech_tools(ToolRegistry& registry);
std::unique_ptr<SpeechSynthesizer> create_speech_synthesizer_from_env();

}  // namespace mindbridge
