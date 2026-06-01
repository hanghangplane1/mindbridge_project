## ADDED Requirements

### Requirement: Prompt prefix cache in AgentLoop
`AgentLoop` SHALL maintain a `CaffeineCache` that caches built prompt strings keyed by `PromptPrefix::hash`.

#### Scenario: Cache hit on prompt build
- **WHEN** `AgentLoop` builds a prompt and the `PromptPrefix::hash` matches a cached entry
- **THEN** the cached prompt string SHALL be used directly, skipping `PromptPolicy::build()` and `ContextManager::build()`

#### Scenario: Cache miss triggers full build
- **WHEN** `AgentLoop` builds a prompt and the `PromptPrefix::hash` is NOT in the cache
- **THEN** the full prompt build pipeline SHALL execute and the result SHALL be stored in the cache

### Requirement: Prompt cache invalidation on tool change
The prompt cache SHALL be invalidated when the available tools change (different `tool_signature`).

#### Scenario: Tool list changes
- **WHEN** the `PromptPrefix::tool_signature` differs from the cached entry's tool_signature
- **THEN** the cached prompt SHALL be invalidated and a fresh prompt SHALL be built

### Requirement: Prompt cache TTL
The prompt cache SHALL use `expireAfterWrite` with a configurable TTL (default 10 minutes).

#### Scenario: Prompt expires after TTL
- **WHEN** a cached prompt was built 11 minutes ago and `write_ttl_seconds = 600`
- **THEN** the next prompt build SHALL rebuild the prompt from scratch

### Requirement: Prompt cache configuration
The prompt cache SHALL be configurable via environment variables.

#### Scenario: Prompt cache disabled
- **WHEN** `MINDBRIDGE_PROMPT_CACHE_ENABLED=false` is set
- **THEN** every prompt build SHALL go through the full pipeline

#### Scenario: Prompt cache TTL configured
- **WHEN** `MINDBRIDGE_PROMPT_CACHE_TTL_SECONDS=300` is set
- **THEN** the prompt cache SHALL use `write_ttl_seconds = 300`
