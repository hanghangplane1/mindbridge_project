#pragma once

#include <nlohmann/json.hpp>
#include "mindbridge/cache/caffeine_cache.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mindbridge {
namespace state {

struct StateRecord {
    std::string user_id{"anonymous"};
    std::string conversation_id{"default"};
    std::string namespace_name{"chat_memory"};
    std::string key;
    std::int64_t version{0};
    std::string risk_level{"low"};
    nlohmann::json payload = nlohmann::json::object();
    std::int64_t updated_at{0};
};

struct StateChange {
    std::int64_t change_id{0};
    std::string user_id{"anonymous"};
    std::string conversation_id{"default"};
    std::string namespace_name{"chat_memory"};
    std::string key;
    std::string operation{"upsert"};
    std::int64_t version{0};
    std::string risk_level{"low"};
    nlohmann::json payload = nlohmann::json::object();
    std::string source_node;
    std::vector<std::string> target_nodes;
    std::int64_t created_at{0};
};

struct ReplicaProgress {
    std::string node_id;
    std::int64_t last_applied_change_id{0};
    std::int64_t applied_count{0};
    std::int64_t skipped_count{0};
    std::int64_t updated_at{0};
};

struct StoreStatus {
    std::string node_id;
    std::int64_t record_count{0};
    std::int64_t change_count{0};
    ReplicaProgress progress;
};

class DistributedStateStore {
public:
    DistributedStateStore(std::string db_path, std::string node_id);
    ~DistributedStateStore();

    const std::string& node_id() const { return node_id_; }

    void initialize();
    std::int64_t upsert_record(StateRecord record,
                               const std::vector<std::string>& target_nodes = {});
    std::int64_t append_conversation_turn(const std::string& user_id,
                                          const std::string& conversation_id,
                                          const std::string& namespace_name,
                                          const std::string& role,
                                          const std::string& content,
                                          const std::string& risk_level);

    std::vector<StateChange> fetch_changes_after(std::int64_t change_id, int limit) const;
    bool apply_change(const StateChange& change, const std::string& replica_node_id);
    ReplicaProgress progress_for(const std::string& replica_node_id) const;
    StoreStatus status(const std::string& progress_node_id = "") const;

    std::optional<StateRecord> get_record(const std::string& user_id,
                                          const std::string& conversation_id,
                                          const std::string& namespace_name,
                                          const std::string& key) const;
    std::vector<StateRecord> list_records(const std::string& user_id,
                                          const std::string& conversation_id,
                                          const std::string& namespace_name) const;
    nlohmann::json list_conversations(const std::string& user_id) const;
    nlohmann::json conversation_history(const std::string& user_id,
                                        const std::string& conversation_id) const;

    using ChangeObserver = std::function<void(const StateChange&, bool applied)>;
    void add_change_observer(ChangeObserver cb);

private:
    void notify_observers(const StateChange& change, bool applied);
    void invalidate_record_cache(const std::string& user_id,
                                 const std::string& conversation_id,
                                 const std::string& namespace_name,
                                 const std::string& key);
    void invalidate_history_cache(const std::string& user_id,
                                  const std::string& conversation_id);
    static std::string make_record_cache_key(const std::string& user_id,
                                              const std::string& conversation_id,
                                              const std::string& namespace_name,
                                              const std::string& key);
    static std::string make_history_cache_key(const std::string& user_id,
                                               const std::string& conversation_id);

    struct Impl;

    std::string db_path_;
    std::string node_id_;
    Impl* impl_{nullptr};
    std::vector<ChangeObserver> change_observers_;
    mutable std::mutex observer_mutex_;

    // L1 caches (Caffeine-style)
    using RecordCache = agent_rpc::mcp::rag::CaffeineCache<std::string, StateRecord>;
    using HistoryCache = agent_rpc::mcp::rag::CaffeineCache<std::string, nlohmann::json>;
    mutable std::unique_ptr<RecordCache> record_cache_;
    mutable std::unique_ptr<HistoryCache> history_cache_;
};

class ReplicaWorker {
public:
    ReplicaWorker(DistributedStateStore& source,
                  DistributedStateStore& replica,
                  std::string replica_node_id);

    int sync_once(int limit = 1000);
    nlohmann::json status() const;

private:
    DistributedStateStore& source_;
    DistributedStateStore& replica_;
    std::string replica_node_id_;
};

class StateReplicationStatusTool;

}  // namespace state
}  // namespace mindbridge
