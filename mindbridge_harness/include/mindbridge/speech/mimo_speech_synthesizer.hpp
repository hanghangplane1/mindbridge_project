#pragma once

#include "mindbridge/speech/speech_synthesizer.hpp"

#include <string>

namespace mindbridge {

class MimoSpeechSynthesizer : public SpeechSynthesizer {
public:
    MimoSpeechSynthesizer(std::string base_url,
                          std::string api_key,
                          std::string model,
                          std::string path);

    SpeechSynthesisResult synthesize(const SpeechSynthesisRequest& request) override;

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_;
    std::string path_;
};

}  // namespace mindbridge
