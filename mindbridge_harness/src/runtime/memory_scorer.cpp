#include "mindbridge/runtime/memory_scorer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace mindbridge {

namespace {

const std::unordered_set<std::string> kEmotionKeywords = {
    "distressed", "anxious", "crisis", "depressed",
    "suicidal",   "panic",   "hopeless", "angry", "fearful"
};

bool has_emotion_tag(const std::vector<std::string>& tags) {
    for (const auto& tag : tags) {
        if (kEmotionKeywords.count(tag) > 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

MemoryScorer::MemoryScorer() : config_(Config{}) {}
MemoryScorer::MemoryScorer(Config config) : config_(config) {}

double MemoryScorer::compute_recency(double age_hours) const {
    return std::exp(-config_.lambda * age_hours);
}

double MemoryScorer::cosine_similarity(const std::vector<double>& a,
                                       const std::vector<double>& b) {
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    if (a.size() != b.size()) {
        return 0.0;
    }

    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;

    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    double denominator = std::sqrt(norm_a) * std::sqrt(norm_b);
    if (denominator == 0.0) {
        return 0.0;
    }

    return dot / denominator;
}

double MemoryScorer::compute_importance(const std::string& risk_level,
                                        const std::vector<std::string>& emotion_tags) const {
    if (risk_level == "RISK") {
        return config_.risk_importance;
    }
    if (has_emotion_tag(emotion_tags)) {
        return config_.emotion_importance;
    }
    return config_.normal_importance;
}

ScoredMemory MemoryScorer::score(const MemoryEntry& entry,
                                 const std::vector<double>& query_embedding) const {
    ScoredMemory result;
    result.entry = entry;

    result.recency = compute_recency(entry.age_hours);
    result.relevance = cosine_similarity(query_embedding, entry.embedding);
    result.importance = compute_importance(entry.risk_level, entry.emotion_tags);
    result.score = result.recency * result.relevance * result.importance;

    return result;
}

std::vector<ScoredMemory> MemoryScorer::score_all(
    const std::vector<MemoryEntry>& entries,
    const std::vector<double>& query_embedding) const {

    std::vector<ScoredMemory> results;
    results.reserve(entries.size());

    for (const auto& entry : entries) {
        results.push_back(score(entry, query_embedding));
    }

    return results;
}

std::vector<int> MemoryScorer::select_eviction_candidates(
    const std::vector<MemoryEntry>& entries,
    int count_to_evict,
    const std::vector<double>& query_embedding) const {

    if (count_to_evict <= 0 || entries.empty()) {
        return {};
    }

    // Build index-score pairs, skipping RISK entries
    struct IndexScore {
        int index;
        double score;
    };

    std::vector<IndexScore> candidates;
    candidates.reserve(entries.size());

    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (entries[i].risk_level == "RISK") {
            continue;
        }
        ScoredMemory sm = score(entries[i], query_embedding);
        candidates.push_back({i, sm.score});
    }

    // Sort ascending by score (lowest first -> evict first)
    std::sort(candidates.begin(), candidates.end(),
              [](const IndexScore& a, const IndexScore& b) {
                  return a.score < b.score;
              });

    // Clamp to available candidates
    int actual = std::min(count_to_evict, static_cast<int>(candidates.size()));

    std::vector<int> result;
    result.reserve(actual);
    for (int i = 0; i < actual; ++i) {
        result.push_back(candidates[i].index);
    }

    return result;
}

const MemoryScorer::Config& MemoryScorer::config() const {
    return config_;
}

}  // namespace mindbridge
