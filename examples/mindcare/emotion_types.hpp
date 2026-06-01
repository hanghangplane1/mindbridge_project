#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mindcare {

struct EmotionResult {
    std::string risk_level = "low";
    std::vector<std::string> emotion_tags;
    double confidence = 0.0;
    std::string suggestion;
};

inline EmotionResult emotion_from_json(const nlohmann::json& j) {
    EmotionResult result;
    if (j.contains("riskLevel") && j["riskLevel"].is_string()) {
        result.risk_level = j["riskLevel"].get<std::string>();
    }
    if (j.contains("emotionTags") && j["emotionTags"].is_array()) {
        for (const auto& item : j["emotionTags"]) {
            if (item.is_string()) {
                result.emotion_tags.push_back(item.get<std::string>());
            }
        }
    }
    if (j.contains("confidence") && j["confidence"].is_number()) {
        result.confidence = j["confidence"].get<double>();
    }
    if (j.contains("suggestion") && j["suggestion"].is_string()) {
        result.suggestion = j["suggestion"].get<std::string>();
    }
    return result;
}

inline nlohmann::json emotion_to_json(const EmotionResult& result) {
    return nlohmann::json{
        {"riskLevel", result.risk_level},
        {"emotionTags", result.emotion_tags},
        {"confidence", result.confidence},
        {"suggestion", result.suggestion},
    };
}

}  // namespace mindcare
