#pragma once

#include "mindbridge/model/model_client.hpp"

#include <string>

namespace mindbridge {

/** Ollama HTTP API（/api/chat, /api/embeddings），与 examples/mindcare/ollama_client 行为一致 */
class OllamaModelClient : public ModelClient {
public:
    explicit OllamaModelClient(std::string base_url = "http://localhost:11434",
                               std::string model = "qwen2.5:7b-chat");

    void set_model(std::string model) { model_ = std::move(model); }
    const std::string& model_id() const override { return model_; }
    nlohmann::json last_completion_metadata() const override { return nlohmann::json::object(); }

    std::string chat(const std::string& system_prompt, const std::string& user_message) override;
    void chat_stream(const std::string& system_prompt,
                     const std::string& user_message,
                     const std::function<bool(const std::string& chunk)>& on_chunk) override;
    std::vector<double> embedding(const std::string& text, const std::string& embed_model) override;

private:
    std::string base_url_;
    std::string model_;
};

}  // namespace mindbridge
