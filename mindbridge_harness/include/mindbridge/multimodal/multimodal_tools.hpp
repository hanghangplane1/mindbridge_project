#pragma once

#include "mindbridge/harness/tool.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/multimodal/multimodal_analyzer.hpp"

#include <memory>

namespace mindbridge {

class AnalyzeMultimodalEmotionTool : public ITool {
public:
    explicit AnalyzeMultimodalEmotionTool(std::unique_ptr<MultimodalAnalyzer> analyzer);

    std::string name() const override { return "analyze_multimodal_emotion"; }
    std::string description() const override {
        return "Analyze text/image/audio emotion evidence and return fused risk metadata.";
    }
    ToolSchema schema() const override;
    bool is_read_only(const nlohmann::json&) const override { return true; }
    std::optional<std::string> validate(const nlohmann::json& arguments) const override;
    ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) override;

private:
    std::unique_ptr<MultimodalAnalyzer> analyzer_;
};

void register_multimodal_tools(ToolRegistry& registry);

}  // namespace mindbridge
