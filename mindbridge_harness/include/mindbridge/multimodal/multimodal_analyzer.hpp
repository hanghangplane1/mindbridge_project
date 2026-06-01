#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace mindbridge {

struct MultimodalInputPart {
    std::string mime_type;
    std::string data_base64;
    std::string url;

    bool present() const {
        return !data_base64.empty() || !url.empty();
    }
};

struct MultimodalRequest {
    std::string text;
    std::optional<MultimodalInputPart> image;
    std::optional<MultimodalInputPart> audio;
};

class MultimodalAnalyzer {
public:
    virtual ~MultimodalAnalyzer() = default;
    virtual nlohmann::json analyze(const MultimodalRequest& request) = 0;
};

}  // namespace mindbridge
