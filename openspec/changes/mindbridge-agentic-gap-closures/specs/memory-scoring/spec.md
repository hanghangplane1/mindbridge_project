## ADDED Requirements

### Requirement: Three-factor scoring model
The system SHALL compute a memory score as `score = recency(t) × relevance(q, m) × importance(meta)` for each memory entry.

#### Scenario: Score computation for a recent, relevant, high-risk memory
- **WHEN** a memory is 1 hour old (recency ≈ 0.95), semantically similar to current query (relevance ≈ 0.9), and has risk_level=RISK (importance = 1.5)
- **THEN** the computed score is approximately 1.28 (0.95 × 0.9 × 1.5)

#### Scenario: Score computation for an old, irrelevant, normal memory
- **WHEN** a memory is 48 hours old (recency ≈ 0.09), semantically dissimilar (relevance ≈ 0.2), and has risk_level=NORMAL (importance = 1.0)
- **THEN** the computed score is approximately 0.018 (0.09 × 0.2 × 1.0)

### Requirement: Recency decay function
The system SHALL compute recency as `exp(-λ × age_hours)` with configurable λ (default 0.05, half-life ≈ 14 hours).

#### Scenario: Recent memory high recency
- **WHEN** a memory was created 2 hours ago
- **THEN** recency ≈ 0.90

#### Scenario: Old memory low recency
- **WHEN** a memory was created 72 hours ago
- **THEN** recency ≈ 0.03

### Requirement: Relevance via embedding similarity
The system SHALL compute relevance as cosine similarity between the current query embedding and the memory embedding, reusing the existing embedding interface.

#### Scenario: Embedding cache available
- **WHEN** a memory entry already has a cached embedding
- **THEN** the system reuses the cached embedding without recomputation

#### Scenario: New memory needs embedding
- **WHEN** a memory entry has no cached embedding
- **THEN** the system computes embedding via ModelClient::embedding() and caches it

### Requirement: Importance from metadata
The system SHALL assign importance multiplier based on memory metadata.

#### Scenario: High-risk memory
- **WHEN** a memory's associated risk_level is RISK
- **THEN** importance = 1.5

#### Scenario: Emotionally tagged memory
- **WHEN** a memory has an emotion tag (e.g., "distressed", "anxious", "crisis")
- **THEN** importance = 1.3

#### Scenario: Normal memory
- **WHEN** a memory has risk_level=NORMAL and no emotion tags
- **THEN** importance = 1.0

### Requirement: Score-based eviction
The system SHALL evict the lowest-scoring memories when the memory count exceeds the configured maximum (default 50 entries).

#### Scenario: Memory count exceeds limit
- **WHEN** ConversationMemory has 52 entries and max_entries=50
- **THEN** the system computes scores for all entries, removes the 2 lowest-scoring entries, and logs a memory_evicted event to trace with evicted entry scores

#### Scenario: Eviction preserves high-risk memories
- **WHEN** a memory with risk_level=RISK exists and would be evicted by score
- **THEN** the system skips that memory and evicts the next lowest-scoring non-RISK entry instead

### Requirement: Score recalculation trigger
The system SHALL recalculate scores when new context arrives (query change) or periodically (configurable interval, default 30 minutes).

#### Scenario: New query triggers recalculation
- **WHEN** a new user message arrives in a conversation
- **THEN** relevance scores are recomputed for all memories in that conversation

#### Scenario: Periodic recalculation
- **WHEN** 30 minutes have passed since last recalculation
- **THEN** recency scores are updated for all memories (relevance unchanged unless query changed)

### Requirement: Scoring parameters configurable
The system SHALL expose λ, importance weights, max_entries, and recalculation_interval as configuration parameters.

#### Scenario: Custom λ configured
- **WHEN** config sets memory_scoring.lambda=0.02
- **THEN** the system uses the custom decay rate (half-life ≈ 35 hours)
