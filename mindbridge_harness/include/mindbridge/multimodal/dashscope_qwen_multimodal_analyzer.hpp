#pragma once

#include "mindbridge/multimodal/multimodal_analyzer.hpp"

#include <string>

namespace mindbridge {

class DashScopeQwenMultimodalAnalyzer : public MultimodalAnalyzer {
public:
    DashScopeQwenMultimodalAnalyzer(std::string base_url,
                                    std::string api_key,
                                    std::string model,
                                    bool enable_thinking);

    nlohmann::json analyze(const MultimodalRequest& request) override;

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_;
    bool enable_thinking_{false};
};

}  // namespace mindbridge
