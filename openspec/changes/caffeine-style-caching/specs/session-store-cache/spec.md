## ADDED Requirements

### Requirement: In-process cache for session load
`SessionStore::load()` SHALL check an in-process `CaffeineCache` before reading from the filesystem. On cache miss, the result from the file SHALL be stored in the cache.

#### Scenario: Cache hit on session load
- **WHEN** `load(session_id)` is called and the session data is in the L1 cache
- **THEN** the cached session data SHALL be returned without reading the JSON file

#### Scenario: Cache miss loads from file
- **WHEN** `load(session_id)` is called and the session data is NOT in the L1 cache
- **THEN** the system SHALL read and parse the JSON file, store the result in the L1 cache, and return it

### Requirement: Write-through on session save
`SessionStore::save()` SHALL update the cache after a successful file write.

#### Scenario: Save updates cache
- **WHEN** `save(session_id, data)` successfully writes the JSON file
- **THEN** the L1 cache entry for that session_id SHALL be updated with the new data

### Requirement: expireAfterAccess for session cache
The session cache SHALL use `expireAfterAccess` semantics where entries that have not been accessed for a configured duration are evicted.

#### Scenario: Inactive session evicted
- **WHEN** a session has not been accessed for longer than `access_ttl_seconds`
- **THEN** the cache entry SHALL be evicted and the next `load()` SHALL read from the file

#### Scenario: Active session stays cached
- **WHEN** a session is accessed repeatedly within `access_ttl_seconds`
- **THEN** the cache entry SHALL remain valid and `load()` SHALL return the cached value

### Requirement: Session cache configuration
The session cache SHALL be configurable via environment variables.

#### Scenario: Session cache disabled
- **WHEN** `MINDBRIDGE_SESSION_CACHE_ENABLED=false` is set
- **THEN** `load()` and `save()` SHALL bypass the cache entirely

#### Scenario: Session cache size configured
- **WHEN** `MINDBRIDGE_SESSION_CACHE_MAX_SIZE=500` is set
- **THEN** the session cache SHALL use `max_size = 500`
