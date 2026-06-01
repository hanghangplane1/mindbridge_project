#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mindbridge {

/** 与 mindcare evaluator JSON 对齐的风险与情绪结果 */
struct EmotionResult {
    std::string risk_level{"low"};
    std::vector<std::string> emotion_tags;
    double confidence{0.0};
    std::string suggestion;

    nlohmann::json to_json() const {
        return {{"riskLevel", risk_level},
                {"emotionTags", emotion_tags},
                {"confidence", confidence},
                {"suggestion", suggestion}};
    }

    static EmotionResult from_json(const nlohmann::json& j) {
        EmotionResult r;
        if (j.contains("riskLevel")) {
            r.risk_level = j["riskLevel"].get<std::string>();
        }
        if (j.contains("emotionTags") && j["emotionTags"].is_array()) {
            for (const auto& t : j["emotionTags"]) {
                if (t.is_string()) {
                    r.emotion_tags.push_back(t.get<std::string>());
                }
            }
        }
        if (j.contains("confidence")) {
            r.confidence = j["confidence"].get<double>();
        }
        if (j.contains("suggestion")) {
            r.suggestion = j["suggestion"].get<std::string>();
        }
        return r;
    }
};

}  // namespace mindbridge
