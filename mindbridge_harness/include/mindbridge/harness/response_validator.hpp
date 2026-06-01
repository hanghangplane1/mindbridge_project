#pragma once

#include <string>
#include <vector>

namespace mindbridge {

struct ValidationResult {
    bool is_valid{true};
    std::string reason;
    std::vector<std::string> issues;
};

class ResponseValidator {
public:
    ValidationResult validate(const std::string& query,
                              const std::string& response,
                              const std::string& rag_context) const;

private:
    static std::vector<std::string> extract_key_terms(const std::string& context);
    static bool contains_crisis_keyword(const std::string& text);
    static bool contains_hotline(const std::string& text);
    static bool contains_harmful_pattern(const std::string& text);
};

}  // namespace mindbridge
