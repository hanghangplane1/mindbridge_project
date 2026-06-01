#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace mindbridge {

struct MemoryEntry {
    std::string id;
    std::string text;
    double age_hours{0.0};
    std::string risk_level;                      // "low", "medium", "high", "RISK"
    std::vector<std::string> emotion_tags;       // e.g., {"distressed", "anxious"}
    std::vector<double> embedding;               // pre-computed, may be empty
};

struct ScoredMemory {
    MemoryEntry entry;
    double recency{0.0};
    double relevance{0.0};
    double importance{0.0};
    double score{0.0};
};

class MemoryScorer {
public:
    struct Config {
        double lambda{0.05};
        double risk_importance{1.5};
        double emotion_importance{1.3};
        double normal_importance{1.0};
        int max_entries{50};
    };

    MemoryScorer();
    explicit MemoryScorer(Config config);

    double compute_recency(double age_hours) const;

    static double cosine_similarity(const std::vector<double>& a, const std::vector<double>& b);

    double compute_importance(const std::string& risk_level,
                              const std::vector<std::string>& emotion_tags) const;

    ScoredMemory score(const MemoryEntry& entry,
                       const std::vector<double>& query_embedding) const;

    std::vector<ScoredMemory> score_all(const std::vector<MemoryEntry>& entries,
                                        const std::vector<double>& query_embedding) const;

    // Returns indices of entries to evict (lowest scoring, excluding RISK entries)
    std::vector<int> select_eviction_candidates(const std::vector<MemoryEntry>& entries,
                                                int count_to_evict,
                                                const std::vector<double>& query_embedding) const;

    const Config& config() const;

private:
    Config config_;
};

}  // namespace mindbridge
