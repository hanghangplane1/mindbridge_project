#pragma once

#include "mindbridge/speech/speech_synthesizer.hpp"

#include <string>

namespace mindbridge {

class DashScopeCosyVoiceSynthesizer : public SpeechSynthesizer {
public:
    DashScopeCosyVoiceSynthesizer(std::string websocket_url,
                                  std::string api_key,
                                  std::string model,
                                  std::string instruction,
                                  std::string fallback_model = "",
                                  std::string fallback_voice = "");

    SpeechSynthesisResult synthesize(const SpeechSynthesisRequest& request) override;
    bool synthesize_stream(const SpeechSynthesisRequest& request,
                           SpeechSynthesisCallback& callback) override;

private:
    std::string websocket_url_;
    std::string api_key_;
    std::string model_;
    std::string instruction_;
    std::string fallback_model_;
    std::string fallback_voice_;
};

}  // namespace mindbridge
