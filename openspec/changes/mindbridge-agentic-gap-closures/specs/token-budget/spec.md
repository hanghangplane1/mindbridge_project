## ADDED Requirements

### Requirement: Token budget initialization
The system SHALL initialize a `TokenBudgetController` for each agent run with configurable `total_budget` (default 8000 tokens), `warning_threshold` (0.6), `critical_threshold` (0.8), and `hard_limit` (0.95).

#### Scenario: Default budget creation
- **WHEN** a new agent run starts without explicit budget configuration
- **THEN** the system creates a TokenBudgetController with total_budget=8000, warning=4800, critical=6400, hard_limit=7600

#### Scenario: Custom budget creation
- **WHEN** RunInput specifies `token_budget_limit=16000`
- **THEN** the system creates a TokenBudgetController with total_budget=16000 and proportional thresholds

### Requirement: Token consumption tracking
The system SHALL accumulate prompt tokens and completion tokens after each model call and tool invocation within a run.

#### Scenario: After model generation
- **WHEN** AgentLoop completes a `generate()` call that returns 500 prompt tokens and 200 completion tokens
- **THEN** TokenBudgetController records consumed=700 and updates remaining budget

#### Scenario: After tool invocation
- **WHEN** AgentLoop completes a tool call that produces 300 tokens of output
- **THEN** TokenBudgetController records consumed=300 as tool output tokens

### Requirement: Budget level enforcement
The system SHALL enforce four budget levels with corresponding actions.

#### Scenario: Green level (consumed < 60%)
- **WHEN** consumed tokens are below warning_threshold
- **THEN** the system proceeds normally with no restrictions

#### Scenario: Yellow level — CHAT intent (60% <= consumed < 80%)
- **WHEN** consumed tokens reach warning_threshold AND current intent is CHAT
- **THEN** the system triggers full context compression (truncate oldest history, summarize RAG contexts, reduce max_tokens) and logs a budget_warning event to trace

#### Scenario: Yellow level — CONSULT intent (60% <= consumed < 80%)
- **WHEN** consumed tokens reach warning_threshold AND current intent is CONSULT
- **THEN** the system truncates oldest history only (preserves RAG contexts for compliance queries) and logs a budget_warning event to trace

#### Scenario: Yellow level — RISK intent (60% <= consumed < 80%)
- **WHEN** consumed tokens reach warning_threshold AND current intent is RISK
- **THEN** the system does NOT compress any context, logs a budget_warning event to trace, and continues with full context intact

#### Scenario: Red level (80% <= consumed < 95%)
- **WHEN** consumed tokens reach critical_threshold
- **THEN** the system allows only essential tool calls (risk_level != LOW), skips optional tools, and logs a budget_critical event to trace

#### Scenario: Circuit breaker (consumed >= 95%)
- **WHEN** consumed tokens reach hard_limit
- **THEN** the system terminates the run with TaskState=FAILED (budget_exceeded), returns partial answer if available, and logs a budget_exceeded event to trace

### Requirement: Token counting method
The system SHALL use tiktoken-cpp (cl100k_base encoding) for token counting with a fallback to character-based estimation (1 token ≈ 2 CJK characters or 4 ASCII characters).

#### Scenario: tiktoken-cpp available
- **WHEN** the tiktoken-cpp library is linked and initialized
- **THEN** the system uses BPE token counting for all text

#### Scenario: tiktoken-cpp unavailable fallback
- **WHEN** tiktoken-cpp initialization fails
- **THEN** the system falls back to character-based estimation with a 1.2x safety multiplier

### Requirement: Budget state exposure
The system SHALL expose budget state through RunResult so callers can inspect consumption.

#### Scenario: RunResult includes budget metadata
- **WHEN** an agent run completes (success or failure)
- **THEN** RunResult contains `token_budget_used`, `token_budget_limit`, `budget_level_reached` fields
