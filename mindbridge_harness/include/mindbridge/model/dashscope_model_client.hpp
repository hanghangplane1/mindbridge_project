#pragma once

#include "mindbridge/model/model_client.hpp"

#include <string>

namespace mindbridge {

class DashScopeModelClient : public ModelClient {
public:
    DashScopeModelClient(std::string api_key, std::string model, std::string base_url = "");

    const std::string& model_id() const override { return model_; }
    nlohmann::json last_completion_metadata() const override { return last_completion_metadata_; }

    std::string chat(const std::string& system_prompt, const std::string& user_message) override;
    void chat_stream(const std::string& system_prompt,
                     const std::string& user_message,
                     const std::function<bool(const std::string& chunk)>& on_chunk) override;
    std::vector<double> embedding(const std::string& text, const std::string& embed_model) override;

private:
    std::string api_key_;
    std::string model_;
    std::string base_url_;
    nlohmann::json last_completion_metadata_ = nlohmann::json::object();
};

}  // namespace mindbridge
