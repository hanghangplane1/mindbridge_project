#pragma once

#include <string>

namespace mindbridge {

class TokenCounter {
public:
    TokenCounter();
    ~TokenCounter();

    TokenCounter(const TokenCounter&) = delete;
    TokenCounter& operator=(const TokenCounter&) = delete;
    TokenCounter(TokenCounter&& other) noexcept;
    TokenCounter& operator=(TokenCounter&& other) noexcept;

    int count_tokens(const std::string& text) const;
    bool is_using_fallback() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace mindbridge
