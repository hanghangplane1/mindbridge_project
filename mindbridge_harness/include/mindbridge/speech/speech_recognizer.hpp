#pragma once

#include <string>

namespace mindbridge {

struct SpeechRecognitionRequest {
    std::string mime_type;
    std::string data_base64;
    std::string format;
    int sample_rate{16000};
};

struct SpeechRecognitionResult {
    std::string transcript;
    std::string request_id;
    long first_package_delay_ms{0};
    long last_package_delay_ms{0};
};

class SpeechRecognizer {
public:
    virtual ~SpeechRecognizer() = default;
    virtual SpeechRecognitionResult recognize(const SpeechRecognitionRequest& request) = 0;
};

}  // namespace mindbridge
