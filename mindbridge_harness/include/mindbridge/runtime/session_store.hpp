#pragma once

#include "mindbridge/runtime/conversation_memory.hpp"
#include "mindbridge/cache/caffeine_cache.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace mindbridge {

class SessionStore {
public:
    explicit SessionStore(std::string root = ".mindbridge");
    std::string save(const std::string& session_id,
                     const std::string& conversation_id,
                     const nlohmann::json& history,
                     const ConversationMemory& memory);
    nlohmann::json load(const std::string& session_id) const;

private:
    std::string root_;
    using SessionCache = agent_rpc::mcp::rag::CaffeineCache<std::string, nlohmann::json>;
    mutable std::unique_ptr<SessionCache> cache_;
};

}  // namespace mindbridge
