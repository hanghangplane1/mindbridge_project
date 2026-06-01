#include "mindbridge/harness/response_validator.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace mindbridge {

namespace {

const std::vector<std::string> kHarmfulPatterns = {
    "你可以去死", "你活该", "放弃吧", "没救了", "你完了",
    "去死吧", "不配活着", "活该受罪"};

const std::vector<std::string> kCrisisKeywords = {
    "自杀", "自残", "不想活", "结束生命", "伤害自己",
    "活着没意义", "活不下去", "想死", "绝望"};

const std::vector<std::string> kHotlineNumbers = {
    "400-161-9995", "12355", "120", "110", "82951332"};

std::string trim_copy(const std::string& value) {
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

std::string normalized_for_echo_check(const std::string& value) {
    std::string out;
    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            continue;
        }
        switch (ch) {
            case '.':
            case ',':
            case ';':
            case ':':
            case '!':
            case '?':
            case '"':
            case '\'':
            case '(':
            case ')':
                continue;
            default:
                out.push_back(static_cast<char>(ch));
                break;
        }
    }
    return out;
}

std::string extract_latest_user_text(const std::string& query) {
    const std::string marker = "## 当前求助者最新发言";
    const size_t marker_pos = query.find(marker);
    if (marker_pos == std::string::npos) {
        return trim_copy(query);
    }
    size_t start = query.find('\n', marker_pos);
    if (start == std::string::npos) {
        return "";
    }
    ++start;
    size_t end = query.find("\n## ", start);
    if (end == std::string::npos) {
        end = query.size();
    }
    return trim_copy(query.substr(start, end - start));
}

}  // namespace

bool ResponseValidator::contains_harmful_pattern(const std::string& text) {
    for (const auto& pattern : kHarmfulPatterns) {
        if (text.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ResponseValidator::contains_crisis_keyword(const std::string& text) {
    for (const auto& kw : kCrisisKeywords) {
        if (text.find(kw) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ResponseValidator::contains_hotline(const std::string& text) {
    for (const auto& number : kHotlineNumbers) {
        if (text.find(number) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> ResponseValidator::extract_key_terms(const std::string& context) {
    std::vector<std::string> terms;
    // Extract ## headings
    std::regex heading_re(R"(##\s*(.+))");
    auto begin = std::sregex_iterator(context.begin(), context.end(), heading_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string term = (*it)[1].str();
        // Trim whitespace
        term.erase(0, term.find_first_not_of(" \t\n\r"));
        term.erase(term.find_last_not_of(" \t\n\r") + 1);
        if (!term.empty()) {
            terms.push_back(term);
        }
    }
    // Extract **bold** text
    std::regex bold_re(R"(\*\*([^*]+)\*\*)");
    begin = std::sregex_iterator(context.begin(), context.end(), bold_re);
    for (auto it = begin; it != end; ++it) {
        std::string term = (*it)[1].str();
        if (!term.empty()) {
            terms.push_back(term);
        }
    }
    return terms;
}

ValidationResult ResponseValidator::validate(const std::string& query,
                                              const std::string& response,
                                              const std::string& rag_context) const {
    ValidationResult result;

    // Check response length
    if (response.size() < 15) {
        result.is_valid = false;
        result.reason = "response_too_short";
        result.issues.push_back("Response shorter than 15 characters");
        return result;
    }

    // Check harmful patterns
    if (contains_harmful_pattern(response)) {
        result.is_valid = false;
        result.reason = "harmful_pattern";
        result.issues.push_back("Response contains harmful pattern");
        return result;
    }

    const std::string latest_user_text = extract_latest_user_text(query);
    const std::string normalized_latest = normalized_for_echo_check(latest_user_text);
    const std::string normalized_response = normalized_for_echo_check(response);
    if (normalized_latest.size() >= 24 && normalized_response == normalized_latest) {
        result.is_valid = false;
        result.reason = "echoed_user_text";
        result.issues.push_back("Response repeats the latest user text instead of answering it");
        return result;
    }

    // Check crisis hotline requirement
    if (contains_crisis_keyword(query) && !contains_hotline(response)) {
        result.is_valid = false;
        result.reason = "missing_hotline";
        result.issues.push_back("Crisis query but response missing hotline number");
        return result;
    }

    // Check RAG context key term coverage (only if context was provided)
    if (!rag_context.empty()) {
        auto terms = extract_key_terms(rag_context);
        if (!terms.empty()) {
            bool found = false;
            for (const auto& term : terms) {
                if (response.find(term) != std::string::npos) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                result.is_valid = false;
                result.reason = "no_context_reference";
                result.issues.push_back("Response does not reference any key term from RAG context");
                return result;
            }
        }
    }

    return result;
}

}  // namespace mindbridge
