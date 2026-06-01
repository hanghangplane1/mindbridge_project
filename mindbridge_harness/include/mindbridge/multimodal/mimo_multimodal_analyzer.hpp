#pragma once

#include "mindbridge/multimodal/multimodal_analyzer.hpp"

#include <string>

namespace mindbridge {

class MimoMultimodalAnalyzer : public MultimodalAnalyzer {
public:
    MimoMultimodalAnalyzer(std::string base_url, std::string api_key, std::string model);

    nlohmann::json analyze(const MultimodalRequest& request) override;

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_;
};

}  // namespace mindbridge
