#include "mindbridge/harness/mcp_quota.hpp"

#include <algorithm>
#include <cassert>

namespace mindbridge {

// ===========================================================================
// TokenBucket
// ===========================================================================

TokenBucket::TokenBucket(double rate_limit, int burst_size)
    : rate_(rate_limit > 0.0 ? rate_limit : 1.0)
    , burst_(burst_size > 0 ? burst_size : 1)
    , tokens_(static_cast<double>(burst_))
    , last_refill_(std::chrono::steady_clock::now())
{
}

void TokenBucket::refill_unl()
{
    auto now = std::chrono::steady_clock::now();
    double elapsed =
        std::chrono::duration<double>(now - last_refill_).count();
    if (elapsed > 0.0) {
        tokens_ = std::min(static_cast<double>(burst_),
                           tokens_ + elapsed * rate_);
        last_refill_ = now;
    }
}

bool TokenBucket::try_consume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    refill_unl();
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        cv_.notify_all();
        return true;
    }
    return false;
}

bool TokenBucket::wait_and_consume(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

    while (true) {
        refill_unl();
        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            cv_.notify_all();
            return true;
        }

        // Compute how long until the next token arrives.
        double deficit = 1.0 - tokens_;
        auto wait_duration =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(deficit / rate_));

        auto wake_time = std::min(deadline,
                                  std::chrono::steady_clock::now() + wait_duration);

        if (cv_.wait_until(lock, wake_time) == std::cv_status::timeout) {
            // Give one last chance after a spurious / real timeout.
            refill_unl();
            if (tokens_ >= 1.0) {
                tokens_ -= 1.0;
                cv_.notify_all();
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            // Otherwise loop (spurious wakeup or partial refill).
        }
    }
}

// ===========================================================================
// SlidingWindowBudget
// ===========================================================================

SlidingWindowBudget::SlidingWindowBudget(int64_t daily_budget)
    : daily_budget_(daily_budget)
{
}

void SlidingWindowBudget::record_usage(int64_t tokens)
{
    std::lock_guard<std::mutex> lock(mutex_);
    window_.push_back({std::chrono::steady_clock::now(), tokens});
    cleanup();
}

bool SlidingWindowBudget::is_exceeded() const
{
    if (daily_budget_ <= 0) return false;  // unlimited
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup();
    int64_t total = 0;
    for (const auto& e : window_) total += e.tokens;
    return total >= daily_budget_;
}

int64_t SlidingWindowBudget::current_usage() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup();
    int64_t total = 0;
    for (const auto& e : window_) total += e.tokens;
    return total;
}

void SlidingWindowBudget::cleanup() const
{
    constexpr auto kWindow = std::chrono::hours(24);
    auto cutoff = std::chrono::steady_clock::now() - kWindow;
    // Erase entries older than 24h (front of the vector is oldest).
    auto it = std::find_if(window_.begin(), window_.end(),
                           [&cutoff](const Entry& e) { return e.time >= cutoff; });
    if (it != window_.begin()) {
        window_.erase(window_.begin(), it);
    }
}

// ===========================================================================
// McpQuotaManager
// ===========================================================================

void McpQuotaManager::ensure_server_locked(const std::string& name)
{
    if (quotas_.find(name) == quotas_.end()) {
        McpServerQuota q;
        q.server_name = name;
        quotas_.emplace(name, q);
        buckets_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(name),
                         std::forward_as_tuple(q.rate_limit, q.burst_size));
        budgets_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(name),
                         std::forward_as_tuple(q.daily_budget));
    }
}

void McpQuotaManager::register_server(const std::string& name,
                                      double  rate_limit,
                                      int     burst,
                                      int64_t daily)
{
    std::lock_guard<std::mutex> lock(mutex_);

    McpServerQuota q;
    q.server_name  = name;
    q.rate_limit   = rate_limit;
    q.burst_size   = burst;
    q.daily_budget = daily;

    quotas_[name] = q;

    // Rebuild the bucket and budget with the new parameters.
    buckets_.erase(name);
    buckets_.emplace(std::piecewise_construct,
                     std::forward_as_tuple(name),
                     std::forward_as_tuple(rate_limit, burst));

    budgets_.erase(name);
    budgets_.emplace(std::piecewise_construct,
                     std::forward_as_tuple(name),
                     std::forward_as_tuple(daily));
}

bool McpQuotaManager::try_acquire(const std::string& name, int timeout_ms)
{
    // Fast path: check daily budget first (cheap read-only lock).
    if (is_budget_exceeded(name)) {
        return false;
    }

    TokenBucket* bucket = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_server_locked(name);
        bucket = &buckets_.at(name);
    }

    return bucket->wait_and_consume(timeout_ms);
}

void McpQuotaManager::record_usage(const std::string& name, int64_t tokens)
{
    SlidingWindowBudget* budget = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_server_locked(name);
        budget = &budgets_.at(name);
    }
    budget->record_usage(tokens);
}

bool McpQuotaManager::is_budget_exceeded(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = budgets_.find(name);
    if (it == budgets_.end()) return false;  // unknown server => not exceeded
    return it->second.is_exceeded();
}

void McpQuotaManager::reload_config(const nlohmann::json& config)
{
    auto servers_it = config.find("servers");
    if (servers_it == config.end() || !servers_it->is_object()) {
        return;
    }

    for (auto& [server_name, server_cfg] : servers_it->items()) {
        if (!server_cfg.is_object()) continue;

        double   rate  = 60.0;
        int      burst = 60;
        int64_t  daily = 0;

        auto rit = server_cfg.find("rate_limit");
        if (rit != server_cfg.end() && rit->is_number()) {
            rate = rit->get<double>();
        }
        auto bit = server_cfg.find("burst_size");
        if (bit != server_cfg.end() && bit->is_number()) {
            burst = bit->get<int>();
        }
        auto dit = server_cfg.find("daily_budget");
        if (dit != server_cfg.end() && dit->is_number()) {
            daily = dit->get<int64_t>();
        }

        register_server(server_name, rate, burst, daily);
    }
}

}  // namespace mindbridge
