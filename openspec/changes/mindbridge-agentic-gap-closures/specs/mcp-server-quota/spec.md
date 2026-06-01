## ADDED Requirements

### Requirement: Per-server rate limit declaration
The system SHALL allow each MCP Server to declare a `rate_limit` (calls per second) and `burst_size` in its ToolRegistry configuration.

#### Scenario: MCP Server with rate limit configured
- **WHEN** an MCP Server is registered with rate_limit=10/s, burst_size=20
- **THEN** ToolRegistry stores these limits and enforces them on every tool call to that server

#### Scenario: MCP Server without rate limit
- **WHEN** an MCP Server is registered without rate_limit configuration
- **THEN** ToolRegistry applies a default rate_limit=60/s with no burst

### Requirement: Token bucket rate limiting
The system SHALL use a token bucket algorithm to enforce per-server rate limits.

#### Scenario: Call within rate limit
- **WHEN** a tool call to MCP Server A arrives and the bucket has tokens
- **THEN** the call proceeds immediately and one token is consumed

#### Scenario: Call exceeds rate limit
- **WHEN** a tool call to MCP Server A arrives and the bucket is empty
- **THEN** the system queues the call for up to 5 seconds, then rejects with a rate_limit_exceeded error if still full

#### Scenario: Burst traffic
- **WHEN** 15 calls arrive simultaneously to a server with burst_size=20
- **THEN** all 15 calls proceed immediately (burst capacity allows it)

### Requirement: Per-server daily budget
The system SHALL enforce a `daily_budget` (in tokens or call count) per MCP Server.

#### Scenario: Daily budget configured
- **WHEN** an MCP Server has daily_budget=100000 tokens
- **THEN** ToolRegistry tracks cumulative token usage for that server over a 24-hour sliding window

#### Scenario: Daily budget exceeded
- **WHEN** a server's cumulative usage reaches its daily_budget
- **THEN** ToolRegistry rejects new calls to that server with a budget_exceeded error and logs to trace

#### Scenario: Daily budget not configured
- **WHEN** an MCP Server has no daily_budget set
- **THEN** no daily limit is enforced for that server

### Requirement: Quota state in trace
The system SHALL record quota enforcement events in the trace.

#### Scenario: Rate limit triggered
- **WHEN** a tool call is queued or rejected due to rate limiting
- **THEN** a mcp_rate_limited event is logged with server_name, wait_time_ms, and result (queued/rejected)

#### Scenario: Budget exceeded
- **WHEN** a tool call is rejected due to daily budget
- **THEN** a mcp_budget_exceeded event is logged with server_name and cumulative_usage

### Requirement: Quota configuration hot-reload
The system SHALL support updating MCP Server quota configuration at runtime.

#### Scenario: Quota config updated
- **WHEN** the MCP quota configuration is modified
- **THEN** ToolRegistry applies new limits to subsequent calls without requiring a restart
