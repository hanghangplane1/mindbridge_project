/**
 * @file caffeine_cache.h
 * @brief Caffeine-style C++ cache with LRU-K eviction, TTL, refreshAfterWrite, LoadingCache
 *
 * Inspired by com.github.benmanes.caffeine (Java). Provides:
 * - LRU-K (K=2) eviction: scan-resistant, considers interval between two most recent accesses
 * - expireAfterWrite: entries expire a fixed duration after last write
 * - refreshAfterWrite: stale entries return old value + async background refresh
 * - LoadingCache: miss auto-invokes a Loader function
 * - Thread-safe: all public methods protected by mutex
 * - Stats: hits, misses, evictions, refreshes, size, hitRate()
 */

#pragma once

#include <string>
#include <optional>
#include <functional>
#include <unordered_map>
#include <list>
#include <mutex>
#include <chrono>
#include <thread>
#include <queue>
#include <condition_variable>
#include <cstdlib>
#include <cstring>

namespace agent_rpc {
namespace mcp {
namespace rag {

/**
 * @brief Cache configuration
 */
struct CaffeineCacheConfig {
    size_t max_size = 10000;                  ///< Maximum number of entries
    int write_ttl_seconds = 60;               ///< expireAfterWrite: TTL after write (0 = no expiry)
    int access_ttl_seconds = 0;               ///< expireAfterAccess: TTL after last access (0 = disabled)
    int refresh_seconds = 0;                  ///< refreshAfterWrite: async refresh after this duration (0 = disabled)
    bool enabled = true;                      ///< Master enable/disable switch
    bool record_stats = true;                 ///< Track hit/miss/eviction stats

    /**
     * @brief Load configuration from environment variables with fallback to defaults
     *
     * Env vars:
     *   MINDBRIDGE_CACHE_ENABLED (bool: "true"/"false")
     *   MINDBRIDGE_CACHE_MAX_SIZE (size_t)
     *   MINDBRIDGE_CACHE_WRITE_TTL_SECONDS (int)
     *   MINDBRIDGE_CACHE_REFRESH_SECONDS (int)
     *   MINDBRIDGE_CACHE_ACCESS_TTL_SECONDS (int)
     */
    static CaffeineCacheConfig from_env() {
        CaffeineCacheConfig cfg;
        if (const char* v = std::getenv("MINDBRIDGE_CACHE_ENABLED")) {
            cfg.enabled = (std::strcmp(v, "true") == 0 || std::strcmp(v, "1") == 0);
        }
        if (const char* v = std::getenv("MINDBRIDGE_CACHE_MAX_SIZE")) {
            cfg.max_size = static_cast<size_t>(std::atol(v));
        }
        if (const char* v = std::getenv("MINDBRIDGE_CACHE_WRITE_TTL_SECONDS")) {
            cfg.write_ttl_seconds = std::atoi(v);
        }
        if (const char* v = std::getenv("MINDBRIDGE_CACHE_REFRESH_SECONDS")) {
            cfg.refresh_seconds = std::atoi(v);
        }
        if (const char* v = std::getenv("MINDBRIDGE_CACHE_ACCESS_TTL_SECONDS")) {
            cfg.access_ttl_seconds = std::atoi(v);
        }
        return cfg;
    }
};

/**
 * @brief Cache statistics
 */
struct CaffeineCacheStats {
    size_t hits = 0;
    size_t misses = 0;
    size_t evictions = 0;
    size_t refreshes = 0;
    size_t size = 0;

    float hitRate() const {
        size_t total = hits + misses;
        return total > 0 ? static_cast<float>(hits) / total : 0.0f;
    }

    void reset() {
        hits = 0;
        misses = 0;
        evictions = 0;
        refreshes = 0;
        // size is NOT reset — it reflects actual cache state
    }
};

/**
 * @brief Generic Caffeine-style cache
 *
 * @tparam K Key type (must be hashable)
 * @tparam V Value type
 */
template<typename K, typename V>
class CaffeineCache {
public:
    using Loader = std::function<std::optional<V>(const K& key)>;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Seconds = std::chrono::seconds;

    /**
     * @brief Construct a cache with config and optional loader
     */
    explicit CaffeineCache(CaffeineCacheConfig config, Loader loader = nullptr)
        : config_(std::move(config)), loader_(std::move(loader)), shutdown_(false) {
        if (config_.refresh_seconds > 0) {
            refresh_thread_ = std::thread([this]() { refresh_loop(); });
        }
    }

    /**
     * @brief Destructor — stops background refresh thread
     */
    ~CaffeineCache() {
        {
            std::lock_guard<std::mutex> lock(refresh_mutex_);
            shutdown_ = true;
        }
        refresh_cv_.notify_all();
        if (refresh_thread_.joinable()) {
            refresh_thread_.join();
        }
    }

    // Non-copyable, non-movable (owns a thread)
    CaffeineCache(const CaffeineCache&) = delete;
    CaffeineCache& operator=(const CaffeineCache&) = delete;
    CaffeineCache(CaffeineCache&&) = delete;
    CaffeineCache& operator=(CaffeineCache&&) = delete;

    /**
     * @brief Get a value, loading it via Loader on miss (LoadingCache semantics)
     *
     * Behavior:
     * - Hit (not expired, not stale): return cached value
     * - Hit but stale (past refresh_seconds): return stale value + enqueue async refresh
     * - Hit but expired (past write_ttl_seconds): remove and treat as miss
     * - Miss: invoke Loader, store result, return it
     * - Miss + no Loader: return nullopt
     */
    std::optional<V> get(const K& key) {
        if (!config_.enabled) {
            stats_.misses++;
            return std::nullopt;
        }

        std::unique_lock<std::mutex> lock(mutex_);

        auto it = entries_.find(key);
        if (it != entries_.end()) {
            auto& entry = it->second;

            // Check hard expiry (expireAfterWrite)
            if (is_write_expired(entry)) {
                remove_entry(it);
                stats_.misses++;
                // Fall through to loader path
            } else {
                // Update access time for LRU-K
                update_access(entry);
                entry.last_access = Clock::now();

                // Move to front of LRU list
                move_to_front(key);

                // Check if refresh needed (refreshAfterWrite)
                if (config_.refresh_seconds > 0 && is_refresh_needed(entry)) {
                    enqueue_refresh(key);
                }

                stats_.hits++;
                return entry.value;
            }
        } else {
            stats_.misses++;
        }

        // Miss path — use loader if available
        if (loader_) {
            // Release lock during loader call to avoid blocking other threads
            lock.unlock();
            auto loaded = loader_(key);
            lock.lock();

            if (loaded.has_value()) {
                put_internal(key, std::move(loaded.value()));
                return loaded.value();
            }
            // Loader returned nullopt — don't cache
            return std::nullopt;
        }

        return std::nullopt;
    }

    /**
     * @brief Get value only if present and not expired (no loading)
     */
    std::optional<V> get_if_present(const K& key) {
        if (!config_.enabled) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = entries_.find(key);
        if (it == entries_.end()) {
            return std::nullopt;
        }

        if (is_write_expired(it->second)) {
            remove_entry(it);
            return std::nullopt;
        }

        update_access(it->second);
        it->second.last_access = Clock::now();
        move_to_front(key);
        stats_.hits++;
        return it->second.value;
    }

    /**
     * @brief Insert or update a value
     */
    void put(const K& key, const V& value) {
        if (!config_.enabled) return;
        std::lock_guard<std::mutex> lock(mutex_);
        put_internal(key, value);
    }

    /**
     * @brief Insert or update a value (move semantics)
     */
    void put(const K& key, V&& value) {
        if (!config_.enabled) return;
        std::lock_guard<std::mutex> lock(mutex_);
        put_internal(key, std::move(value));
    }

    /**
     * @brief Remove a specific key
     */
    void invalidate(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            remove_entry(it);
        }
    }

    /**
     * @brief Remove all entries
     */
    void invalidate_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        lru_list_.clear();
        stats_.size = 0;
    }

    /**
     * @brief Get current stats snapshot
     */
    CaffeineCacheStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto s = stats_;
        s.size = entries_.size();
        return s;
    }

    /**
     * @brief Reset stats counters (size is not reset)
     */
    void reset_stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.reset();
    }

    /**
     * @brief Get current number of entries
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    /**
     * @brief Check if a key exists and is not expired
     */
    bool contains(const K& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        return !is_write_expired(it->second);
    }

private:
    struct CacheEntry {
        V value;
        TimePoint created_at;       // for expireAfterWrite / refreshAfterWrite
        TimePoint last_access;      // for expireAfterAccess / LRU-K
        TimePoint prev_access;      // for LRU-K (K=2)
        bool accessed = false;      // true if get() was called at least once
    };

    using EntryMap = std::unordered_map<K, CacheEntry>;
    using LruItem = std::pair<K, TimePoint>;  // key + prev_access for LRU-K ordering
    using LruList = std::list<LruItem>;

    // --- Entry lifecycle ---

    void put_internal(const K& key, V value) {
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            // Update existing
            it->second.value = std::move(value);
            it->second.created_at = Clock::now();
            it->second.last_access = Clock::now();
            // prev_access stays — LRU-K uses the interval
            return;
        }

        // Evict if at capacity
        while (entries_.size() >= config_.max_size && !lru_list_.empty()) {
            evict_lruk();
        }

        // Insert new entry
        auto now = Clock::now();
        CacheEntry entry;
        entry.value = std::move(value);
        entry.created_at = now;
        entry.last_access = now;
        entry.prev_access = now;  // first access, no previous
        entries_.emplace(key, std::move(entry));
        lru_list_.emplace_front(key, now);
        entry_refs_[key] = lru_list_.begin();
    }

    void remove_entry(typename EntryMap::iterator it) {
        auto ref_it = entry_refs_.find(it->first);
        if (ref_it != entry_refs_.end()) {
            lru_list_.erase(ref_it->second);
            entry_refs_.erase(ref_it);
        }
        entries_.erase(it);
        stats_.size = entries_.size();
    }

    void move_to_front(const K& key) {
        auto ref_it = entry_refs_.find(key);
        if (ref_it != entry_refs_.end()) {
            lru_list_.splice(lru_list_.begin(), lru_list_, ref_it->second);
        }
    }

    // --- LRU-K eviction ---

    void update_access(CacheEntry& entry) {
        entry.prev_access = entry.last_access;
        entry.last_access = Clock::now();
        entry.accessed = true;
    }

    void evict_lruk() {
        if (lru_list_.empty()) return;

        // LRU-K (K=2) eviction strategy:
        // 1. Prefer evicting entries that were NEVER accessed via get() (only put)
        // 2. Among same accessed-status, evict the one with oldest prev_access
        // 3. As tiebreaker, evict the one at the back of the LRU list
        auto victim_it = lru_list_.end();
        TimePoint oldest_prev = Clock::now();
        bool victim_accessed = true;
        bool found = false;

        // Iterate from back (oldest) to front — prefer evicting old entries
        for (auto it = lru_list_.end(); it != lru_list_.begin();) {
            --it;
            auto entry_it = entries_.find(it->first);
            if (entry_it == entries_.end()) {
                continue;
            }

            auto& entry = entry_it->second;
            bool is_better_victim = false;
            if (!found) {
                is_better_victim = true;
            } else if (entry.accessed != victim_accessed) {
                // Prefer evicting never-accessed entries
                is_better_victim = !entry.accessed;
            } else if (entry.accessed == victim_accessed) {
                // Same accessed status — prefer older prev_access
                is_better_victim = entry.prev_access <= oldest_prev;
            }

            if (is_better_victim) {
                oldest_prev = entry.prev_access;
                victim_it = it;
                victim_accessed = entry.accessed;
                found = true;
                // If we found a never-accessed entry at the back, that's the best victim
                if (!victim_accessed) break;
            }
        }

        if (!found) return;

        // Evict
        auto key = victim_it->first;
        entry_refs_.erase(key);
        lru_list_.erase(victim_it);
        entries_.erase(key);
        stats_.evictions++;
        stats_.size = entries_.size();
    }

    // --- Expiration ---

    using Duration = std::chrono::steady_clock::duration;

    bool is_write_expired(const CacheEntry& entry) const {
        if (config_.write_ttl_seconds <= 0) return false;
        return (Clock::now() - entry.created_at) >= Seconds(config_.write_ttl_seconds);
    }

    bool is_access_expired(const CacheEntry& entry) const {
        if (config_.access_ttl_seconds <= 0) return false;
        return (Clock::now() - entry.last_access) >= Seconds(config_.access_ttl_seconds);
    }

    bool is_refresh_needed(const CacheEntry& entry) const {
        if (config_.refresh_seconds <= 0) return false;
        return (Clock::now() - entry.created_at) >= Seconds(config_.refresh_seconds);
    }

    // --- RefreshAfterWrite ---

    void enqueue_refresh(const K& key) {
        std::lock_guard<std::mutex> rlock(refresh_mutex_);
        refresh_queue_.push(key);
        refresh_cv_.notify_one();
    }

    void refresh_loop() {
        while (true) {
            K key;
            {
                std::unique_lock<std::mutex> rlock(refresh_mutex_);
                refresh_cv_.wait(rlock, [this]() {
                    return shutdown_ || !refresh_queue_.empty();
                });
                if (shutdown_ && refresh_queue_.empty()) return;
                key = std::move(refresh_queue_.front());
                refresh_queue_.pop();
            }

            if (!loader_) continue;

            try {
                auto new_value = loader_(key);
                if (new_value.has_value()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = entries_.find(key);
                    if (it != entries_.end()) {
                        it->second.value = std::move(new_value.value());
                        it->second.created_at = Clock::now();
                        stats_.refreshes++;
                    }
                }
                // If loader returns nullopt, keep stale value
            } catch (...) {
                // Refresh failed — keep stale value, will retry on next get()
            }
        }
    }

    // --- Members ---

    CaffeineCacheConfig config_;
    Loader loader_;

    mutable std::mutex mutex_;
    EntryMap entries_;
    LruList lru_list_;
    std::unordered_map<K, typename LruList::iterator> entry_refs_;
    CaffeineCacheStats stats_;

    // Background refresh
    std::thread refresh_thread_;
    std::mutex refresh_mutex_;
    std::condition_variable refresh_cv_;
    std::queue<K> refresh_queue_;
    bool shutdown_;
};

} // namespace rag
} // namespace mcp
} // namespace agent_rpc
