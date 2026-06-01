#pragma once
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mindbridge {

// ---------------------------------------------------------------------------
// TokenBucket  -- simple leaky-bucket rate limiter
// ---------------------------------------------------------------------------
class TokenBucket {
public:
    TokenBucket(double rate_limit = 60.0, int burst_size = 60);

    /// Try to consume one token without blocking.
    /// Returns true if a token was available.
    bool try_consume();

    /// Try to consume one token, blocking up to |timeout_ms| milliseconds.
    /// Returns true if a token was acquired, false on timeout.
    bool wait_and_consume(int timeout_ms = 5000);

private:
    void refill_unl();  // caller must hold mutex_

    double rate_;                                // tokens per second
    int    burst_;                               // max burst size
    double tokens_;                              // current available tokens
    std::chrono::steady_clock::time_point last_refill_;
    std::mutex              mutex_;
    std::condition_variable cv_;
};

// ---------------------------------------------------------------------------
// SlidingWindowBudget  -- 24-hour rolling token budget
// ---------------------------------------------------------------------------
class SlidingWindowBudget {
public:
    /// daily_budget == 0 means unlimited (never exceeded).
    explicit SlidingWindowBudget(int64_t daily_budget = 0);

    /// Record |tokens| consumed at the current time.
    void record_usage(int64_t tokens);

    /// Returns true if the rolling 24h usage exceeds the budget.
    bool is_exceeded() const;

    /// Returns the total tokens consumed in the current 24h window.
    int64_t current_usage() const;

private:
    void cleanup() const;  // remove entries older than 24h; mutable-safe

    int64_t daily_budget_;  // 0 = unlimited
    mutable std::mutex mutex_;

    struct Entry {
        std::chrono::steady_clock::time_point time;
        int64_t tokens;
    };
    mutable std::vector<Entry> window_;  // guarded by mutex_, mutated in const methods via cleanup
};

// ---------------------------------------------------------------------------
// McpServerQuota  -- per-server configuration snapshot
// ---------------------------------------------------------------------------
struct McpServerQuota {
    std::string server_name;
    double   rate_limit  {60.0};
    int      burst_size  {60};
    int64_t  daily_budget {0};   // 0 = unlimited
};

// ---------------------------------------------------------------------------
// McpQuotaManager  -- manages per-server token buckets and daily budgets
// ---------------------------------------------------------------------------
class McpQuotaManager {
public:
    /// Register (or reconfigure) a server with the given limits.
    /// Defaults: 60 tokens/sec, burst of 60, no daily budget.
    void register_server(const std::string& name,
                         double   rate_limit  = 60.0,
                         int      burst       = 60,
                         int64_t  daily       = 0);

    /// Acquire a single token for |name|, waiting up to |timeout_ms|.
    /// If the server is unknown it is lazily registered with defaults.
    /// Returns false on timeout or if the daily budget is exceeded.
    bool try_acquire(const std::string& name, int timeout_ms = 5000);

    /// Record |tokens| consumed against the daily budget of |name|.
    void record_usage(const std::string& name, int64_t tokens);

    /// Check whether the daily budget for |name| has been exceeded.
    bool is_budget_exceeded(const std::string& name) const;

    /// Hot-reload configuration from a JSON object.
    /// Expected format:
    ///   { "servers": { "<name>": { "rate_limit": 60, "burst_size": 60,
    ///                               "daily_budget": 0 }, ... } }
    void reload_config(const nlohmann::json& config);

private:
    /// Ensure bucket + budget exist for |name|; caller must hold mutex_.
    void ensure_server_locked(const std::string& name);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, TokenBucket>        buckets_;
    std::unordered_map<std::string, SlidingWindowBudget> budgets_;
    std::unordered_map<std::string, McpServerQuota>     quotas_;
};

}  // namespace mindbridge
