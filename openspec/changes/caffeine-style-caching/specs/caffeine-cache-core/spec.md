## ADDED Requirements

### Requirement: Generic CaffeineCache template class
The system SHALL provide a generic `CaffeineCache<K,V>` C++ template class in `mcp/include/agent_rpc/mcp/rag/caffeine_cache.h` that implements in-process caching with configurable eviction, expiration, and loading semantics.

#### Scenario: Cache construction with config
- **WHEN** a `CaffeineCache` is constructed with a `CacheConfig` and an optional `Loader` function
- **THEN** the cache SHALL be ready to accept `get()`, `put()`, `invalidate()` operations and SHALL be disabled if `config.enabled == false`

#### Scenario: Disabled cache bypass
- **WHEN** the cache is constructed with `config.enabled = false`
- **THEN** all `get()` calls SHALL return `std::nullopt` and all `put()` calls SHALL be no-ops

### Requirement: LoadingCache semantics
The cache SHALL support automatic loading on miss via a `Loader` function provided at construction time.

#### Scenario: Cache miss triggers loader
- **WHEN** `get(key)` is called and the key is not present in the cache
- **THEN** the cache SHALL invoke the `Loader` function with the key, store the result, and return it

#### Scenario: Loader returns nullopt
- **WHEN** the `Loader` function returns `std::nullopt` for a key
- **THEN** the cache SHALL NOT store an entry and SHALL return `std::nullopt`

#### Scenario: Loader exception
- **WHEN** the `Loader` function throws an exception
- **THEN** the cache SHALL NOT store an entry and SHALL propagate the exception to the caller

### Requirement: LRU-K eviction
The cache SHALL use LRU-K (K=2) eviction strategy that considers the interval between the two most recent accesses to distinguish scan-resistant from frequently-accessed entries.

#### Scenario: Cache full evicts LRU-K victim
- **WHEN** the cache is at `max_size` capacity and a new entry is inserted
- **THEN** the cache SHALL evict the entry with the largest interval between its two most recent accesses (or the entry with only one access that is least recently used)

#### Scenario: Scan resistance
- **WHEN** a burst of `max_size` distinct keys is accessed once each (scan pattern)
- **THEN** subsequent `get()` calls for previously-cached hot keys SHALL still hit, because scan entries have large K-2 intervals and are evicted first

### Requirement: expireAfterWrite
The cache SHALL support a write-based TTL where entries expire a configured duration after they were last written.

#### Scenario: Entry expires after write TTL
- **WHEN** an entry was written 61 seconds ago and `write_ttl_seconds = 60`
- **THEN** `get()` for that key SHALL return `std::nullopt` (expired) and the entry SHALL be removed

#### Scenario: Entry not expired
- **WHEN** an entry was written 30 seconds ago and `write_ttl_seconds = 60`
- **THEN** `get()` for that key SHALL return the cached value

### Requirement: refreshAfterWrite
The cache SHALL support a refresh-based pattern where entries that exceed the refresh duration return the stale value to the caller while triggering an asynchronous background refresh.

#### Scenario: Stale entry returns old value and refreshes
- **WHEN** an entry was written 31 seconds ago and `refresh_seconds = 30`
- **AND** `get()` is called for that key
- **THEN** the cache SHALL return the current (stale) value immediately AND enqueue an async refresh via the Loader

#### Scenario: Refresh updates entry
- **WHEN** an async refresh completes successfully with a new value
- **THEN** the cache entry SHALL be updated with the new value and a new `created_at` timestamp

#### Scenario: Refresh failure preserves stale value
- **WHEN** an async refresh fails (loader throws or returns nullopt)
- **THEN** the existing stale value SHALL be preserved and the next `get()` SHALL trigger another refresh attempt

### Requirement: Thread safety
All public methods of `CaffeineCache` SHALL be safe to call from multiple threads concurrently.

#### Scenario: Concurrent get and put
- **WHEN** thread A calls `get(key1)` and thread B calls `put(key2, value2)` simultaneously
- **THEN** both operations SHALL complete correctly without data races or crashes

#### Scenario: Concurrent refresh and get
- **WHEN** a background refresh is in progress and a caller invokes `get()` for the same key
- **THEN** the caller SHALL receive the current (pre-refresh) value without blocking on the refresh

### Requirement: Cache statistics
The cache SHALL track and expose hit count, miss count, eviction count, refresh count, and current size.

#### Scenario: Stats tracking
- **WHEN** 10 `get()` calls are made: 7 hits and 3 misses
- **THEN** `stats().hits == 7`, `stats().misses == 3`, and `stats().hitRate() == 0.7f`

#### Scenario: Stats reset
- **WHEN** `resetStats()` is called
- **THEN** all counters SHALL reset to zero (size remains unchanged)

### Requirement: Configuration via environment variables
The cache SHALL support configuration via environment variables that override defaults.

#### Scenario: Environment variable override
- **WHEN** `MINDBRIDGE_CACHE_MAX_SIZE=5000` is set
- **THEN** the cache SHALL use `max_size = 5000` instead of the default

#### Scenario: Cache disabled via environment
- **WHEN** `MINDBRIDGE_CACHE_ENABLED=false` is set
- **THEN** the cache SHALL behave as if `config.enabled = false`
