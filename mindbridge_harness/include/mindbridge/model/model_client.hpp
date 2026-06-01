#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mindbridge {

/**
 * 模型访问抽象，便于在 Ollama / vLLM OpenAI-compatible 等后端之间切换。
 */
class ModelClient {
public:
    virtual ~ModelClient() = default;

    virtual std::string chat(const std::string& system_prompt, const std::string& user_message) = 0;

    virtual void chat_stream(const std::string& system_prompt,
                             const std::string& user_message,
                             const std::function<bool(const std::string& chunk)>& on_chunk) = 0;

    virtual std::vector<double> embedding(const std::string& text,
                                            const std::string& embed_model = "nomic-embed-text") = 0;

    virtual const std::string& model_id() const = 0;
    virtual bool supports_prompt_cache() const { return false; }
    virtual nlohmann::json last_completion_metadata() const { return nlohmann::json::object(); }
};

}  // namespace mindbridge
