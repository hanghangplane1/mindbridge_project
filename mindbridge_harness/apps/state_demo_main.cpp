#include "mindbridge/harness/permission_checker.hpp"
#include "mindbridge/harness/tool_registry.hpp"
#include "mindbridge/runtime/conversation_memory.hpp"
#include "mindbridge/state/distributed_state_store.hpp"
#include "mindbridge/tools/core_tools.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    namespace mb = mindbridge;
    namespace st = mindbridge::state;

    const fs::path root = fs::path(".mindbridge") / "state_demo";
    fs::create_directories(root);
    fs::remove(root / "master.sqlite");
    fs::remove(root / "follower.sqlite");

    st::DistributedStateStore master((root / "master.sqlite").string(), "master-01");
    st::DistributedStateStore follower((root / "follower.sqlite").string(), "follower-a");
    master.initialize();
    follower.initialize();

    mb::ConversationMemory memory;
    mb::ToolRegistry tools;
    mb::register_core_tools(tools, memory, nullptr, nullptr, &master);
    mb::ToolContext alice_ctx{{{"runtime",
                                {{"user_id", "user-alice"},
                                 {"conversation_id", "chat-001"},
                                 {"risk_level", "low"}}}}};
    mb::ToolContext bob_ctx{{{"runtime",
                              {{"user_id", "user-bob"},
                               {"conversation_id", "consult-900"},
                               {"risk_level", "high"}}}}};

    tools.execute("conversation_memory_write",
                  {{"conversation_id", "chat-001"},
                   {"role", "user"},
                   {"content", "今天天气不错，我只是想聊聊天。"},
                   {"namespace", "chat_memory"}},
                  alice_ctx);
    tools.execute("conversation_memory_write",
                  {{"conversation_id", "chat-001"},
                   {"role", "assistant"},
                   {"content", "当然，我们可以轻松聊聊。"},
                   {"namespace", "chat_memory"}},
                  alice_ctx);
    tools.execute("conversation_memory_write",
                  {{"conversation_id", "consult-900"},
                   {"role", "user"},
                   {"content", "我最近真的撑不住了，需要认真聊聊。"},
                   {"risk_level", "high"},
                   {"namespace", "risk_memory"}},
                  bob_ctx);

    st::ReplicaWorker worker(master, follower, "follower-a");
    const int first_applied = worker.sync_once();
    const auto first_changes = master.fetch_changes_after(0, 1);
    bool duplicate_replay_applied = false;
    if (!first_changes.empty()) {
        duplicate_replay_applied = follower.apply_change(first_changes.front(), "follower-a");
    }
    const int second_applied = worker.sync_once();

    const auto alice_chat = follower.list_records("user-alice", "chat-001", "chat_memory");
    const auto alice_risk = follower.list_records("user-alice", "chat-001", "risk_memory");
    const auto bob_risk = follower.list_records("user-bob", "consult-900", "risk_memory");
    const auto visible_conversations = master.list_conversations("user-alice");
    const auto visible_history = master.conversation_history("user-alice", "chat-001");
    const auto master_status = master.status();
    const auto follower_status = follower.status("follower-a");

    nlohmann::json summary = {
        {"demo", "mindbridge_distributed_state_store_mvp"},
        {"master",
         {{"node_id", master_status.node_id},
          {"record_count", master_status.record_count},
          {"change_count", master_status.change_count}}},
        {"follower",
         {{"node_id", follower_status.node_id},
          {"record_count", follower_status.record_count},
          {"last_applied_change_id", follower_status.progress.last_applied_change_id},
          {"applied_count", follower_status.progress.applied_count},
          {"skipped_count", follower_status.progress.skipped_count}}},
        {"sync",
         {{"first_applied", first_applied},
          {"explicit_duplicate_replay_applied", duplicate_replay_applied},
          {"incremental_duplicate_replay_applied", second_applied}}},
        {"isolation",
         {{"alice_chat_records", alice_chat.size()},
          {"alice_risk_records", alice_risk.size()},
          {"bob_risk_records", bob_risk.size()},
          {"user_namespace_isolated", alice_chat.size() == 2 && alice_risk.empty() && bob_risk.size() == 1}}},
        {"visible_history",
         {{"conversation_count", visible_conversations.size()},
          {"turn_count", visible_history.size()},
          {"source", "distributed_state_store_master"}}},
        {"replication_status", worker.status()}};

    const bool ok = summary["isolation"]["user_namespace_isolated"].get<bool>() &&
                    visible_conversations.size() == 1 &&
                    visible_history.size() == 2;
    std::cout << summary.dump(2) << std::endl;
    return ok ? 0 : 1;
}
