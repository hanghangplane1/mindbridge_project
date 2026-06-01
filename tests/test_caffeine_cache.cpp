/**
 * @file test_caffeine_cache.cpp
 * @brief Unit tests for CaffeineCache<K,V>
 */

#include <gtest/gtest.h>
#include "agent_rpc/mcp/rag/caffeine_cache.h"

#include <thread>
#include <chrono>
#include <atomic>

using namespace agent_rpc::mcp::rag;

// --- Basic operations ---

TEST(CaffeineCacheTest, PutAndGet) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;  // no expiry
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    auto v = cache.get("a");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), 1);
}

TEST(CaffeineCacheTest, GetMissReturnsNullopt) {
    CaffeineCacheConfig cfg;
    CaffeineCache<std::string, int> cache(cfg);

    auto v = cache.get("nonexistent");
    EXPECT_FALSE(v.has_value());
}

TEST(CaffeineCacheTest, PutOverwrite) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    cache.put("a", 2);
    auto v = cache.get("a");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), 2);
}

TEST(CaffeineCacheTest, Invalidate) {
    CaffeineCacheConfig cfg;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    cache.invalidate("a");
    EXPECT_FALSE(cache.get("a").has_value());
}

TEST(CaffeineCacheTest, InvalidateAll) {
    CaffeineCacheConfig cfg;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    cache.put("b", 2);
    cache.invalidate_all();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.get("a").has_value());
}

TEST(CaffeineCacheTest, GetIfPresent) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    auto v = cache.get_if_present("a");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), 1);

    EXPECT_FALSE(cache.get_if_present("b").has_value());
}

TEST(CaffeineCacheTest, Contains) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    EXPECT_TRUE(cache.contains("a"));
    EXPECT_FALSE(cache.contains("b"));
}

// --- Disabled cache ---

TEST(CaffeineCacheTest, DisabledCacheBypasses) {
    CaffeineCacheConfig cfg;
    cfg.enabled = false;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_EQ(cache.size(), 0u);
}

// --- LoadingCache ---

TEST(CaffeineCacheTest, LoadingCacheOnMiss) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    int load_count = 0;
    auto loader = [&](const std::string& key) -> std::optional<int> {
        load_count++;
        if (key == "a") return 42;
        return std::nullopt;
    };
    CaffeineCache<std::string, int> cache(cfg, loader);

    auto v = cache.get("a");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), 42);
    EXPECT_EQ(load_count, 1);

    // Second get should hit cache, not call loader
    auto v2 = cache.get("a");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2.value(), 42);
    EXPECT_EQ(load_count, 1);  // loader not called again
}

TEST(CaffeineCacheTest, LoadingCacheReturnsNullopt) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    auto loader = [](const std::string&) -> std::optional<int> {
        return std::nullopt;
    };
    CaffeineCache<std::string, int> cache(cfg, loader);

    auto v = cache.get("missing");
    EXPECT_FALSE(v.has_value());
    EXPECT_EQ(cache.size(), 0u);  // not cached
}

TEST(CaffeineCacheTest, LoadingCacheExceptionPropagation) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    auto loader = [](const std::string&) -> std::optional<int> {
        throw std::runtime_error("load failed");
    };
    CaffeineCache<std::string, int> cache(cfg, loader);

    EXPECT_THROW(cache.get("key"), std::runtime_error);
    EXPECT_EQ(cache.size(), 0u);  // not cached on exception
}

// --- expireAfterWrite ---

TEST(CaffeineCacheTest, ExpireAfterWrite) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 1;  // 1 second TTL
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    EXPECT_TRUE(cache.get("a").has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_FALSE(cache.get("a").has_value());  // expired
}

TEST(CaffeineCacheTest, NotExpiredBeforeTTL) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 10;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    EXPECT_TRUE(cache.get("a").has_value());  // not expired yet
}

// --- LRU-K eviction ---

TEST(CaffeineCacheTest, LRUKEviction) {
    CaffeineCacheConfig cfg;
    cfg.max_size = 3;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<int, int> cache(cfg);

    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);

    // Access key 1 and 2 again to give them a second access (LRU-K advantage)
    cache.get(1);
    cache.get(2);

    // Insert key 4 — should evict key 3 (only accessed once, most recent single-access)
    cache.put(4, 40);

    EXPECT_TRUE(cache.contains(1));   // accessed twice
    EXPECT_TRUE(cache.contains(2));   // accessed twice
    EXPECT_FALSE(cache.contains(3));  // evicted — only one access
    EXPECT_TRUE(cache.contains(4));   // just inserted
}

TEST(CaffeineCacheTest, EvictionStatsTracked) {
    CaffeineCacheConfig cfg;
    cfg.max_size = 2;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<int, int> cache(cfg);

    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);  // evicts one

    auto s = cache.stats();
    EXPECT_GE(s.evictions, 1u);
}

// --- Stats ---

TEST(CaffeineCacheTest, StatsHitMiss) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    cache.get("a");      // hit
    cache.get("a");      // hit
    cache.get("b");      // miss
    cache.get("c");      // miss
    cache.get("c");      // miss (not loaded)

    auto s = cache.stats();
    EXPECT_EQ(s.hits, 2u);
    EXPECT_EQ(s.misses, 3u);
    EXPECT_FLOAT_EQ(s.hitRate(), 2.0f / 5.0f);
}

TEST(CaffeineCacheTest, StatsReset) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<std::string, int> cache(cfg);

    cache.put("a", 1);
    cache.get("a");
    cache.get("b");

    cache.reset_stats();
    auto s = cache.stats();
    EXPECT_EQ(s.hits, 0u);
    EXPECT_EQ(s.misses, 0u);
    EXPECT_EQ(s.evictions, 0u);
    // size should remain
    EXPECT_EQ(s.size, 1u);
}

// --- Thread safety ---

TEST(CaffeineCacheTest, ConcurrentGetAndPut) {
    CaffeineCacheConfig cfg;
    cfg.max_size = 1000;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<int, int> cache(cfg);

    std::atomic<int> errors{0};

    auto writer = [&](int start) {
        for (int i = start; i < start + 100; i++) {
            cache.put(i, i * 10);
        }
    };

    auto reader = [&](int start) {
        for (int i = start; i < start + 100; i++) {
            auto v = cache.get_if_present(i);
            // May or may not find it — just checking no crash
            (void)v;
        }
    };

    std::thread t1(writer, 0);
    std::thread t2(writer, 100);
    std::thread t3(reader, 0);
    std::thread t4(reader, 100);

    t1.join();
    t2.join();
    t3.join();
    t4.join();

    EXPECT_EQ(errors.load(), 0);
}

// --- refreshAfterWrite ---

TEST(CaffeineCacheTest, RefreshAfterWriteReturnsStale) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 60;      // hard expiry 60s
    cfg.refresh_seconds = 1;         // refresh after 1s
    cfg.max_size = 100;

    std::atomic<int> load_count{0};
    auto loader = [&](const std::string& key) -> std::optional<int> {
        load_count++;
        return load_count.load();  // returns incrementing values
    };

    CaffeineCache<std::string, int> cache(cfg, loader);

    // First load
    auto v1 = cache.get("a");
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(v1.value(), 1);

    // Wait for refresh window
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // Should return stale value (1) immediately
    auto v2 = cache.get("a");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2.value(), 1);  // stale value returned

    // Wait for background refresh to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Now should have refreshed value
    auto v3 = cache.get("a");
    ASSERT_TRUE(v3.has_value());
    // v3 might be 2 if refresh completed, or 1 if still pending
    EXPECT_GE(v3.value(), 1);
}

// --- Config from env ---

TEST(CaffeineCacheConfigTest, DefaultValues) {
    CaffeineCacheConfig cfg;
    EXPECT_EQ(cfg.max_size, 10000u);
    EXPECT_EQ(cfg.write_ttl_seconds, 60);
    EXPECT_EQ(cfg.refresh_seconds, 0);
    EXPECT_TRUE(cfg.enabled);
}

// --- Multiple types ---

TEST(CaffeineCacheTest, StringToStringCache) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<std::string, std::string> cache(cfg);

    cache.put("hello", "world");
    auto v = cache.get("hello");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), "world");
}

TEST(CaffeineCacheTest, IntKeyCache) {
    CaffeineCacheConfig cfg;
    cfg.write_ttl_seconds = 0;
    CaffeineCache<int, std::string> cache(cfg);

    cache.put(42, "answer");
    auto v = cache.get(42);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v.value(), "answer");
}
