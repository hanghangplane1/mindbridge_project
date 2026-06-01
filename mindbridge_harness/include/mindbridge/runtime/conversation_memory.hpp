#pragma once

#include "mindbridge/runtime/memory_scorer.hpp"

#include <nlohmann/json.hpp>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace mindbridge {

class ConversationMemory {
public:
    struct EpisodicNote {
        std::string text;
        std::vector<std::string> tags;
        std::string created_at;
    };

    void append(const std::string& conversation_id,
                const std::string& role,
                const std::string& content);
    std::string recent_text(const std::string& conversation_id, int limit) const;
    std::vector<EpisodicNote> relevant_notes(const std::string& query, int limit) const;

    void set_task_summary(const std::string& summary);
    void set_current_intent(const std::string& intent);
    void set_risk_level(const std::string& level);
    void set_next_action(const std::string& action);
    void add_open_question(const std::string& question);
    void add_context_summary(const std::string& summary);
    void add_knowledge_summary(const std::string& key, const std::string& summary, const std::string& freshness);
    void invalidate_knowledge_summary(const std::string& key);
    void add_mcp_status(const std::string& key, const std::string& status);
    void add_note(const std::string& note, const std::vector<std::string>& tags);
    void set_max_entries(int max);
    nlohmann::json to_json() const;

private:
    struct Message {
        std::string role;
        std::string content;
    };

    struct KnowledgeSummary {
        std::string summary;
        std::string freshness;
    };

    struct Working {
        std::string task_summary;
        std::string current_intent{"CHAT"};
        std::string risk_level{"low"};
        std::vector<std::string> open_questions;
        std::string next_action;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Message>> messages_;
    Working working_;
    std::vector<std::string> recent_contexts_;
    std::unordered_map<std::string, KnowledgeSummary> knowledge_summaries_;
    std::unordered_map<std::string, std::string> mcp_status_;
    std::vector<EpisodicNote> episodic_notes_;
    MemoryScorer scorer_;
    int max_entries_{50};
};

}  // namespace mindbridge
