## ADDED Requirements

### Requirement: Loop detection
The system SHALL detect when the same tool is called with identical arguments consecutively more than a configurable threshold (default 3).

#### Scenario: Consecutive identical tool calls detected
- **WHEN** AgentLoop invokes tool "search_knowledge" with the same arguments 3 times in a row
- **THEN** TrajectoryAnalyzer flags this as a loop, logs a trajectory_loop_detected event with tool_name, args_hash, and count, and AgentLoop breaks the loop by forcing a different action

#### Scenario: No loop detected
- **WHEN** tool calls alternate between different tools or different arguments
- **THEN** no loop detection event is generated

### Requirement: Step efficiency scoring
The system SHALL compute an efficiency score as `min_tool_calls / actual_tool_calls` where min_tool_calls is estimated from the task type.

#### Scenario: Efficient execution
- **WHEN** a CHAT intent completes in 1 tool call (minimum estimated at 1)
- **THEN** efficiency_score = 1.0

#### Scenario: Inefficient execution
- **WHEN** a CONSULT intent completes in 8 tool calls (minimum estimated at 3)
- **THEN** efficiency_score = 0.375

#### Scenario: Efficiency score in trace
- **WHEN** a run finishes
- **THEN** the run_finished event includes `trajectory_efficiency_score` and `tool_call_count`

### Requirement: Anomaly pattern detection
The system SHALL flag anomalous patterns in the tool call trajectory.

#### Scenario: Frequent failures detected
- **WHEN** more than 50% of tool calls in a run result in errors
- **THEN** TrajectoryAnalyzer logs a trajectory_high_failure_rate event with failure_count, total_count, and failed_tools list

#### Scenario: Unnecessary tool call detected
- **WHEN** a tool is called but its output is never referenced in subsequent model generations
- **THEN** TrajectoryAnalyzer logs a trajectory_unused_tool_output event with tool_name

#### Scenario: Excessive tool steps
- **WHEN** actual tool calls exceed max_tool_steps by more than 50%
- **THEN** TrajectoryAnalyzer logs a trajectory_excessive_steps event

### Requirement: Analysis results in trace
The system SHALL write all trajectory analysis results as fields in the run_finished trace event.

#### Scenario: Complete trajectory analysis
- **WHEN** a run finishes
- **THEN** the run_finished event includes: `trajectory_loop_detected` (bool), `trajectory_efficiency_score` (float), `trajectory_anomalies` (list of anomaly types), `trajectory_tool_call_count` (int)

### Requirement: Analysis configuration
The system SHALL allow configuring loop_threshold, efficiency_estimates per intent, and anomaly thresholds.

#### Scenario: Custom loop threshold
- **WHEN** config sets trajectory.loop_threshold=5
- **THEN** the system only flags loops after 5 consecutive identical calls

#### Scenario: Custom efficiency estimates
- **WHEN** config sets trajectory.efficiency_estimates.RISK=5
- **THEN** the system uses 5 as the minimum expected tool calls for RISK intent efficiency calculation
