#pragma once

#include "mindbridge/speech/speech_recognizer.hpp"

#include <string>

namespace mindbridge {

class DashScopeRealtimeSpeechRecognizer : public SpeechRecognizer {
public:
    DashScopeRealtimeSpeechRecognizer(std::string websocket_url,
                                      std::string api_key,
                                      std::string model);

    SpeechRecognitionResult recognize(const SpeechRecognitionRequest& request) override;

private:
    std::string websocket_url_;
    std::string api_key_;
    std::string model_;
};

}  // namespace mindbridge
