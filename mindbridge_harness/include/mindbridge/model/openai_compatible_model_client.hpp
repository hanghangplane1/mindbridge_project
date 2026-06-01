#pragma once

#include "mindbridge/model/model_client.hpp"

#include <string>

namespace mindbridge {

class OpenAICompatibleModelClient : public ModelClient {
public:
    OpenAICompatibleModelClient(std::string base_url,
                                std::string api_key,
                                std::string model,
                                std::string embedding_model = "");

    const std::string& model_id() const override { return model_; }
    bool supports_prompt_cache() const override { return true; }
    nlohmann::json last_completion_metadata() const override { return last_completion_metadata_; }
    std::string chat(const std::string& system_prompt, const std::string& user_message) override;
    void chat_stream(const std::string& system_prompt,
                     const std::string& user_message,
                     const std::function<bool(const std::string& chunk)>& on_chunk) override;
    std::vector<double> embedding(const std::string& text, const std::string& embed_model) override;

private:
    std::string base_url_;
    std::string api_key_;
    std::string model_;
    std::string embedding_model_;
    nlohmann::json last_completion_metadata_ = nlohmann::json::object();
};

}  // namespace mindbridge
