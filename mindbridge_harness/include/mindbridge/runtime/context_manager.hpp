#pragma once

#include "mindbridge/runtime/prompt_prefix.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace mindbridge {

struct ContextBuildInput {
    PromptPrefix prefix;
    nlohmann::json memory_json = nlohmann::json::object();
    std::vector<std::string> relevant_memory;
    std::vector<std::string> rag_contexts;
    std::string history;
    std::string current_request;
    size_t max_chars{12000};
};

struct ContextBuildOutput {
    std::string prompt;
    nlohmann::json metadata;
};

class ContextManager {
public:
    static ContextBuildOutput build(const ContextBuildInput& input);
};

}  // namespace mindbridge
