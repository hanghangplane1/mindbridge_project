#include "mindbridge/runtime/prompt_prefix.hpp"

#include <ctime>
#include <sstream>

namespace mindbridge {

namespace {

std::string stable_hash(const std::string& text) {
    const std::size_t value = std::hash<std::string>{}(text);
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

}  // namespace

PromptPrefix build_prompt_prefix(const std::string& system_rules,
                                 const std::vector<std::string>& tools_surface) {
    std::ostringstream text;
    text << system_rules << "\n\nTools:\n";
    std::ostringstream tool_sig;
    for (const auto& tool : tools_surface) {
        text << "- " << tool << "\n";
        tool_sig << tool << "|";
    }
    const std::time_t now = std::time(nullptr);
    PromptPrefix prefix;
    prefix.text = text.str();
    prefix.hash = stable_hash(prefix.text);
    prefix.tool_signature = stable_hash(tool_sig.str());
    prefix.built_at = std::to_string(static_cast<long long>(now));
    return prefix;
}

nlohmann::json prompt_prefix_metadata(const PromptPrefix& prefix) {
    return {
        {"prefix_hash", prefix.hash},
        {"tool_signature", prefix.tool_signature},
        {"prefix_chars", prefix.text.size()},
        {"prefix_built_at", prefix.built_at},
    };
}

}  // namespace mindbridge
