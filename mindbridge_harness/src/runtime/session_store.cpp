#include "mindbridge/runtime/session_store.hpp"

#include <fstream>
#include <stdexcept>

#include <filesystem>
#include <cstdlib>

namespace mindbridge {

SessionStore::SessionStore(std::string root) : root_(std::move(root)) {
    // Initialize session cache with expireAfterAccess
    agent_rpc::mcp::rag::CaffeineCacheConfig cfg;
    if (const char* v = std::getenv("MINDBRIDGE_SESSION_CACHE_ENABLED")) {
        cfg.enabled = (std::strcmp(v, "true") == 0 || std::strcmp(v, "1") == 0);
    }
    if (const char* v = std::getenv("MINDBRIDGE_SESSION_CACHE_MAX_SIZE")) {
        cfg.max_size = static_cast<size_t>(std::atol(v));
    } else {
        cfg.max_size = 500;
    }
    cfg.write_ttl_seconds = 0;      // no write TTL — sessions persist until access-expired
    cfg.access_ttl_seconds = 300;   // 5 min inactivity eviction
    cache_ = std::make_unique<SessionCache>(cfg);
}

std::string SessionStore::save(const std::string& session_id,
                               const std::string& conversation_id,
                               const nlohmann::json& history,
                               const ConversationMemory& memory) {
    const std::filesystem::path dir = std::filesystem::path(root_) / "sessions";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / (session_id + ".json");
    nlohmann::json payload = {
        {"id", session_id},
        {"conversation_id", conversation_id},
        {"history", history},
        {"memory", memory.to_json()},
    };
    std::ofstream out(file, std::ios::trunc);
    out << payload.dump(2) << "\n";
    // Write-through: update cache after successful file write
    if (cache_) {
        cache_->put(session_id, payload);
    }
    return file.string();
}

nlohmann::json SessionStore::load(const std::string& session_id) const {
    // L1 cache lookup
    if (cache_) {
        auto cached = cache_->get(session_id);
        if (cached.has_value()) {
            return cached.value();
        }
    }
    const std::filesystem::path file = std::filesystem::path(root_) / "sessions" / (session_id + ".json");
    std::ifstream in(file);
    if (!in) {
        throw std::runtime_error("session not found: " + session_id);
    }
    nlohmann::json payload;
    in >> payload;
    // Fill L1 cache
    if (cache_) {
        cache_->put(session_id, payload);
    }
    return payload;
}

}  // namespace mindbridge
