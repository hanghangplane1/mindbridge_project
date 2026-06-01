#pragma once

#include <string>
#include <vector>

namespace mindbridge {

class PromptPolicy {
public:
    static std::string intent_classifier_prompt();
    static std::string counselor_prompt(const std::string& intent,
                                        const std::vector<std::string>& contexts,
                                        const std::string& history);
    static std::string agentic_reasoning_prompt();
    static std::string crisis_prompt();
    static std::string direct_chat_prompt();
    static std::string generate_answer_prompt(const std::string& emotion_label,
                                              const std::string& risk_level,
                                              const std::string& context,
                                              const std::string& query);
    static std::string evaluator_prompt();
    static std::string session_evaluator_prompt();
};

}  // namespace mindbridge
