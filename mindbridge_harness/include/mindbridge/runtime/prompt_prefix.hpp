#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace mindbridge {

struct PromptPrefix {
    std::string text;
    std::string hash;
    std::string tool_signature;
    std::string built_at;
};

PromptPrefix build_prompt_prefix(const std::string& system_rules,
                                 const std::vector<std::string>& tools_surface);
nlohmann::json prompt_prefix_metadata(const PromptPrefix& prefix);

}  // namespace mindbridge
