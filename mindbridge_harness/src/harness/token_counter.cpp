#include "mindbridge/harness/token_counter.hpp"

#include <cstdint>
#include <memory>
#include <string>

// Attempt to include tiktoken-cpp if available at build time.
// Define MINDDBRIDGE_USE_TIKTOKEN in CMake when the library is found.
#if defined(MINDDBRIDGE_USE_TIKTOKEN)
#include <tiktoken/tiktoken.h>
#endif

namespace mindbridge {

// ---------------------------------------------------------------------------
// Fallback token estimator (character-based, CJK-aware)
// ---------------------------------------------------------------------------
namespace {

// Count CJK codepoints and non-CJK codepoints in a UTF-8 string.
// CJK unified ideographs: U+4E00 .. U+9FFF  (common block)
struct CharStats {
    int cjk_chars = 0;
    int non_cjk_chars = 0;
};

CharStats classify_chars(const std::string& text) {
    CharStats stats;
    const unsigned char* data = reinterpret_cast<const unsigned char*>(text.data());
    const size_t len = text.size();
    size_t i = 0;

    while (i < len) {
        uint32_t codepoint = 0;
        unsigned char lead = data[i];
        int bytes = 0;

        if (lead < 0x80) {
            // ASCII (1 byte)
            codepoint = lead;
            bytes = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            // 2-byte sequence
            codepoint = lead & 0x1F;
            bytes = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            // 3-byte sequence
            codepoint = lead & 0x0F;
            bytes = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            // 4-byte sequence
            codepoint = lead & 0x07;
            bytes = 4;
        } else {
            // Invalid lead byte; skip
            ++i;
            ++stats.non_cjk_chars;
            continue;
        }

        // Check that enough continuation bytes are available
        if (i + static_cast<size_t>(bytes) > len) {
            // Truncated sequence; count remainder as non-CJK
            stats.non_cjk_chars += static_cast<int>(len - i);
            break;
        }

        bool valid = true;
        for (int j = 1; j < bytes; ++j) {
            if ((data[i + j] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (data[i + j] & 0x3F);
        }

        if (!valid) {
            ++i;
            ++stats.non_cjk_chars;
            continue;
        }

        // Classify
        if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) {
            ++stats.cjk_chars;
        } else {
            ++stats.non_cjk_chars;
        }

        i += static_cast<size_t>(bytes);
    }

    return stats;
}

int estimate_tokens_fallback(const std::string& text) {
    CharStats cs = classify_chars(text);
    // CJK chars ~ 0.5 token each, non-CJK ~ 0.25 token each, with 1.2x safety
    double raw = cs.cjk_chars * 0.5 + cs.non_cjk_chars * 0.25;
    int estimated = static_cast<int>(raw * 1.2) + 1;  // +1 ensures non-empty text -> >=1
    return estimated;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// PIMPL implementation
// ---------------------------------------------------------------------------
struct TokenCounter::Impl {
    bool using_fallback = true;

#if defined(MINDDBRIDGE_USE_TIKTOKEN)
    std::unique_ptr<tiktoken::tiktoken> encoder;

    Impl() {
        try {
            encoder = std::make_unique<tiktoken::tiktoken>("cl100k_base");
            using_fallback = false;
        } catch (...) {
            encoder.reset();
            using_fallback = true;
        }
    }
#else
    Impl() = default;
#endif

    int count(const std::string& text) const {
#if defined(MINDDBRIDGE_USE_TIKTOKEN)
        if (encoder) {
            auto tokens = encoder->encode(text);
            return static_cast<int>(tokens.size());
        }
#endif
        return estimate_tokens_fallback(text);
    }
};

// ---------------------------------------------------------------------------
// TokenCounter public API
// ---------------------------------------------------------------------------

TokenCounter::TokenCounter() : impl_(new Impl()) {}

TokenCounter::~TokenCounter() { delete impl_; }

TokenCounter::TokenCounter(TokenCounter&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

TokenCounter& TokenCounter::operator=(TokenCounter&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

int TokenCounter::count_tokens(const std::string& text) const {
    if (!impl_) return 0;
    return impl_->count(text);
}

bool TokenCounter::is_using_fallback() const {
    if (!impl_) return true;
    return impl_->using_fallback;
}

}  // namespace mindbridge
