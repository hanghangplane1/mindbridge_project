#include "mindbridge/harness/response_validator.hpp"
#include "mindbridge/runtime/prompt_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    mindbridge::ResponseValidator validator;

    const std::string wrapped_query =
        "你是 MindBridge 心理支持系统中的支持者。\n"
        "## 历史对话\n"
        "求助者：最近心情很低落。\n"
        "支持者：可以从规律作息开始。\n\n"
        "## 当前求助者最新发言\n"
        "听起来不错，我也尝试着改变自己的生活方式。谢谢你的帮助和鼓励。\n\n"
        "## 你的回复\n";
    const auto echoed = validator.validate(
        wrapped_query,
        "听起来不错，我也尝试着改变自己的生活方式。谢谢你的帮助和鼓励。",
        "");
    expect(!echoed.is_valid, "validator should reject echoing latest user text");
    expect(echoed.reason == "echoed_user_text", "validator reason should be echoed_user_text");

    const std::string prompt = mindbridge::PromptPolicy::generate_answer_prompt(
        "normal", "low", "睡眠卫生：保持规律作息。", "我最近压力很大。");
    expect(prompt.find("220到360") != std::string::npos,
           "consult prompt should allow enough depth for benchmark-quality support");
    expect(prompt.find("3到4条具体、可执行的下一步") != std::string::npos,
           "consult prompt should ask for a concrete action plan");
    expect(prompt.find("感谢、好转或准备尝试") != std::string::npos,
           "consult prompt should keep follow-up depth when the user expresses progress");
    expect(prompt.find("不要复述用户原话") != std::string::npos,
           "consult prompt should explicitly forbid parroting user text");
    expect(prompt.find("知识库检索结果") != std::string::npos,
           "consult prompt should identify RAG context as knowledge retrieval");

    std::cout << "PASS: response validator prompt test" << std::endl;
    return 0;
}
