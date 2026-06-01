## ADDED Requirements

### Requirement: L1 read cache for get_record
`DistributedStateStore::get_record()` SHALL check an in-process `CaffeineCache` before querying SQLite/MySQL. On cache miss, the result from the database SHALL be stored in the cache.

#### Scenario: Cache hit on get_record
- **WHEN** `get_record(user_id, conv_id, ns, key)` is called and the record exists in the L1 cache
- **THEN** the cached `StateRecord` SHALL be returned without querying SQLite/MySQL

#### Scenario: Cache miss loads from DB
- **WHEN** `get_record(user_id, conv_id, ns, key)` is called and the record is NOT in the L1 cache
- **THEN** the system SHALL query SQLite/MySQL, store the result in the L1 cache, and return it

#### Scenario: Record not found cached as empty
- **WHEN** `get_record()` queries the database and no record is found
- **THEN** the cache SHALL store a sentinel "not found" entry with a short TTL (e.g., 10s) to prevent repeated DB queries for non-existent keys

### Requirement: L1 read cache for conversation_history
`DistributedStateStore::conversation_history()` SHALL use a `CaffeineCache` with `refreshAfterWrite` semantics to cache conversation history lists.

#### Scenario: Hot conversation returns cached history
- **WHEN** `conversation_history(user_id, conv_id)` is called for an active conversation and the result is in the L1 cache within the refresh window
- **THEN** the cached history vector SHALL be returned without querying SQLite/MySQL

#### Scenario: Stale conversation history triggers background refresh
- **WHEN** `conversation_history()` is called and the cached entry exceeds `refresh_seconds`
- **THEN** the system SHALL return the stale cached value immediately AND enqueue an async DB query to refresh the cache

#### Scenario: New conversation turn invalidates history cache
- **WHEN** `upsert_record()` writes a new record with namespace `conversation`
- **THEN** the `conversation_history` cache for that `(user_id, conversation_id)` SHALL be invalidated

### Requirement: Write-through cache update on upsert
`DistributedStateStore::upsert_record()` SHALL update the L1 cache after a successful database write (write-through pattern).

#### Scenario: Upsert updates L1 cache
- **WHEN** `upsert_record()` successfully writes a record to SQLite/MySQL
- **THEN** the L1 cache entry for that key SHALL be updated with the new record value

#### Scenario: Upsert invalidates conversation history cache
- **WHEN** `upsert_record()` writes a record in the `conversation` namespace
- **THEN** the `conversation_history` cache entry for that conversation SHALL be invalidated

### Requirement: Cache bypass for version lookup
`upsert_record()` SHALL NOT use the cache for its internal `get_record()` call used to determine the next version number. The version lookup SHALL always query the database directly.

#### Scenario: Version lookup bypasses cache
- **WHEN** `upsert_record()` internally calls `get_record()` to get the current version
- **THEN** the call SHALL go directly to SQLite/MySQL, bypassing the L1 cache

### Requirement: Cache configuration for state store
The state store cache SHALL be configurable via environment variables and SHALL support complete disable.

#### Scenario: State store cache disabled
- **WHEN** `MINDBRIDGE_STATE_CACHE_ENABLED=false` is set
- **THEN** `get_record()` and `conversation_history()` SHALL bypass the cache entirely

#### Scenario: State store cache size configured
- **WHEN** `MINDBRIDGE_STATE_CACHE_MAX_SIZE=20000` is set
- **THEN** the state store L1 cache SHALL use `max_size = 20000`
