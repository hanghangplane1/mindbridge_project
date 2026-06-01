#pragma once

#include <string>
#include <vector>

namespace mindbridge {

struct SpeechSynthesisRequest {
    std::string text;
    std::string voice;
    std::string format;
};

struct SpeechSynthesisResult {
    std::string mime_type;
    std::string data_base64;
};

class SpeechSynthesisCallback {
public:
    virtual ~SpeechSynthesisCallback() = default;
    virtual void on_open() {}
    virtual bool on_data(const std::vector<unsigned char>&) { return true; }
    virtual void on_complete() {}
    virtual void on_error(const std::string&) {}
    virtual void on_close() {}
};

class SpeechSynthesizer {
public:
    virtual ~SpeechSynthesizer() = default;
    virtual SpeechSynthesisResult synthesize(const SpeechSynthesisRequest& request) = 0;
    virtual bool synthesize_stream(const SpeechSynthesisRequest&, SpeechSynthesisCallback&) {
        return false;
    }
};

}  // namespace mindbridge
