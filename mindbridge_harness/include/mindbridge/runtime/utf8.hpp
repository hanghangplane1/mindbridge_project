#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mindbridge {

inline std::string sanitize_utf8(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c <= 0x7f) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        size_t need = 0;
        uint32_t codepoint = 0;
        if ((c & 0xe0) == 0xc0) {
            need = 1;
            codepoint = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            need = 2;
            codepoint = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            need = 3;
            codepoint = c & 0x07;
        } else {
            out += "\xef\xbf\xbd";
            ++i;
            continue;
        }

        if (i + need >= input.size()) {
            out += "\xef\xbf\xbd";
            ++i;
            continue;
        }

        bool ok = true;
        for (size_t j = 1; j <= need; ++j) {
            const unsigned char cc = static_cast<unsigned char>(input[i + j]);
            if ((cc & 0xc0) != 0x80) {
                ok = false;
                break;
            }
            codepoint = (codepoint << 6) | (cc & 0x3f);
        }

        if (!ok ||
            (need == 1 && codepoint < 0x80) ||
            (need == 2 && codepoint < 0x800) ||
            (need == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            out += "\xef\xbf\xbd";
            ++i;
            continue;
        }

        out.append(input, i, need + 1);
        i += need + 1;
    }
    return out;
}

inline size_t utf8_codepoint_bytes(const std::string& input, size_t offset) {
    if (offset >= input.size()) {
        return 0;
    }
    const unsigned char c = static_cast<unsigned char>(input[offset]);
    if (c <= 0x7f) {
        return 1;
    }
    if ((c & 0xe0) == 0xc0) {
        return 2;
    }
    if ((c & 0xf0) == 0xe0) {
        return 3;
    }
    if ((c & 0xf8) == 0xf0) {
        return 4;
    }
    return 1;
}

inline std::string truncate_utf8(const std::string& input,
                                 size_t max_bytes,
                                 const std::string& suffix = "\n...[truncated]") {
    std::string clean = sanitize_utf8(input);
    if (clean.size() <= max_bytes) {
        return clean;
    }
    if (max_bytes == 0) {
        return "";
    }
    if (max_bytes <= suffix.size()) {
        return suffix.substr(0, max_bytes);
    }

    const size_t content_limit = max_bytes - suffix.size();
    size_t end = 0;
    while (end < clean.size()) {
        const size_t bytes = utf8_codepoint_bytes(clean, end);
        if (bytes == 0 || end + bytes > content_limit) {
            break;
        }
        end += bytes;
    }
    return clean.substr(0, end) + suffix;
}

}  // namespace mindbridge
