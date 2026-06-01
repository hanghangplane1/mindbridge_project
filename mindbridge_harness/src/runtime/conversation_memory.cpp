#include "mindbridge/runtime/conversation_memory.hpp"
#include "mindbridge/runtime/utf8.hpp"

#include <algorithm>
#include <ctime>
#include <sstream>

namespace mindbridge {

void ConversationMemory::append(const std::string& conversation_id,
                                const std::string& role,
                                const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_[sanitize_utf8(conversation_id)].push_back({sanitize_utf8(role), sanitize_utf8(content)});
}

std::string ConversationMemory::recent_text(const std::string& conversation_id, int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = messages_.find(conversation_id);
    if (it == messages_.end() || limit <= 0) {
        return "";
    }
    const auto& values = it->second;
    const size_t start = values.size() > static_cast<size_t>(limit)
                             ? values.size() - static_cast<size_t>(limit)
                             : 0;
    std::ostringstream out;
    for (size_t i = start; i < values.size(); ++i) {
        out << sanitize_utf8(values[i].role) << ": " << sanitize_utf8(values[i].content) << "\n";
    }
    return sanitize_utf8(out.str());
}

nlohmann::json ConversationMemory::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json notes = nlohmann::json::array();
    for (const auto& note : episodic_notes_) {
        notes.push_back({{"text", note.text}, {"tags", note.tags}, {"created_at", note.created_at}});
    }
    nlohmann::json summaries = nlohmann::json::object();
    for (const auto& kv : knowledge_summaries_) {
        summaries[kv.first] = {{"summary", kv.second.summary}, {"freshness", kv.second.freshness}};
    }
    return {
        {"working",
         {{"task_summary", working_.task_summary},
          {"current_intent", working_.current_intent},
          {"risk_level", working_.risk_level},
          {"open_questions", working_.open_questions},
          {"next_action", working_.next_action}}},
        {"recent_contexts", recent_contexts_},
        {"knowledge_summaries", summaries},
        {"mcp_status", mcp_status_},
        {"episodic_notes", notes},
    };
}

void ConversationMemory::set_task_summary(const std::string& summary) {
    std::lock_guard<std::mutex> lock(mutex_);
    working_.task_summary = summary;
}

void ConversationMemory::set_current_intent(const std::string& intent) {
    std::lock_guard<std::mutex> lock(mutex_);
    working_.current_intent = intent;
}

void ConversationMemory::set_risk_level(const std::string& level) {
    std::lock_guard<std::mutex> lock(mutex_);
    working_.risk_level = level;
}

void ConversationMemory::set_next_action(const std::string& action) {
    std::lock_guard<std::mutex> lock(mutex_);
    working_.next_action = action;
}

void ConversationMemory::add_open_question(const std::string& question) {
    std::lock_guard<std::mutex> lock(mutex_);
    working_.open_questions.push_back(question);
    if (working_.open_questions.size() > 8) {
        working_.open_questions.erase(working_.open_questions.begin());
    }
}

void ConversationMemory::add_context_summary(const std::string& summary) {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_contexts_.push_back(summary);
    if (recent_contexts_.size() > 12) {
        recent_contexts_.erase(recent_contexts_.begin());
    }
}

void ConversationMemory::add_knowledge_summary(const std::string& key,
                                               const std::string& summary,
                                               const std::string& freshness) {
    std::lock_guard<std::mutex> lock(mutex_);
    knowledge_summaries_[key] = {summary, freshness};
}

void ConversationMemory::invalidate_knowledge_summary(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    knowledge_summaries_.erase(key);
}

void ConversationMemory::add_mcp_status(const std::string& key, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    mcp_status_[key] = status;
}

void ConversationMemory::set_max_entries(int max) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_entries_ = max;
}

void ConversationMemory::add_note(const std::string& note, const std::vector<std::string>& tags) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::time_t now = std::time(nullptr);
    episodic_notes_.push_back({note, tags, std::to_string(static_cast<long long>(now))});
    if (static_cast<int>(episodic_notes_.size()) > max_entries_) {
        // Convert episodic notes to MemoryEntry format for scoring
        std::vector<MemoryEntry> entries;
        entries.reserve(episodic_notes_.size());
        for (size_t i = 0; i < episodic_notes_.size(); ++i) {
            MemoryEntry me;
            me.id = std::to_string(i);
            me.text = episodic_notes_[i].text;
            me.emotion_tags = episodic_notes_[i].tags;
            me.risk_level = working_.risk_level;
            // Compute age in hours from created_at timestamp
            std::time_t created = static_cast<std::time_t>(std::stoll(episodic_notes_[i].created_at));
            me.age_hours = static_cast<double>(now - created) / 3600.0;
            // No embedding available -- relevance will be handled below
            entries.push_back(std::move(me));
        }
        int count_to_evict = static_cast<int>(episodic_notes_.size()) - max_entries_;
        // Use scorer to find lowest-scoring candidates (RISK entries are protected).
        // Since we have no embeddings, score = recency * importance (relevance treated as 1.0).
        // We compute this manually via select_eviction_candidates with an empty query embedding,
        // then apply a recency*importance fallback when relevance yields 0.
        auto candidates = scorer_.select_eviction_candidates(entries, count_to_evict, {});
        // select_eviction_candidates scores with relevance=0 (empty embeddings), making all
        // scores zero. Re-sort by recency * importance directly for meaningful ordering.
        if (!candidates.empty()) {
            struct IndexScore {
                int index;
                double score;
            };
            std::vector<IndexScore> rescored;
            rescored.reserve(candidates.size());
            for (int idx : candidates) {
                double rec = scorer_.compute_recency(entries[idx].age_hours);
                double imp = scorer_.compute_importance(entries[idx].risk_level, entries[idx].emotion_tags);
                rescored.push_back({idx, rec * imp});
            }
            std::sort(rescored.begin(), rescored.end(),
                      [](const IndexScore& a, const IndexScore& b) { return a.score < b.score; });
            // Remove lowest-scoring entries (evict from back to preserve indices)
            std::vector<int> to_remove;
            to_remove.reserve(rescored.size());
            for (const auto& rs : rescored) {
                to_remove.push_back(rs.index);
            }
            std::sort(to_remove.rbegin(), to_remove.rend());
            for (int idx : to_remove) {
                episodic_notes_.erase(episodic_notes_.begin() + idx);
            }
        }
    }
}

std::vector<ConversationMemory::EpisodicNote> ConversationMemory::relevant_notes(const std::string& query,
                                                                                  int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EpisodicNote> out;
    for (auto it = episodic_notes_.rbegin(); it != episodic_notes_.rend() && static_cast<int>(out.size()) < limit;
         ++it) {
        bool matched = query.empty();
        if (!matched && it->text.find(query) != std::string::npos) {
            matched = true;
        }
        if (!matched) {
            for (const auto& tag : it->tags) {
                if (!tag.empty() && query.find(tag) != std::string::npos) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched) {
            out.push_back(*it);
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace mindbridge
