## ADDED Requirements

### Requirement: Model profile configuration
The system SHALL support a configurable model profile table mapping intent types to model selections.

#### Scenario: Profile table loaded from config
- **WHEN** the system starts
- **THEN** ModelRouter loads a JSON/YAML config defining profiles with fields: `intent`, `primary_model`, `fallback_model`, `max_tokens`, `temperature`

#### Scenario: Default profile mapping
- **WHEN** no config file is provided
- **THEN** ModelRouter uses default mapping: CHAT→lightweight model, CONSULT→standard model, RISK→strong model

### Requirement: Intent-based model selection
The system SHALL select a model based on the intent classification from PromptPolicy.

#### Scenario: CHAT intent routes to lightweight model
- **WHEN** PromptPolicy classifies intent as CHAT
- **THEN** ModelRouter selects the lightweight model profile (e.g., qwen2-7b or gpt-4o-mini)

#### Scenario: CONSULT intent routes to standard model
- **WHEN** PromptPolicy classifies intent as CONSULT
- **THEN** ModelRouter selects the standard model profile (e.g., qwen2-72b or gpt-4o)

#### Scenario: RISK intent routes to strong model
- **WHEN** PromptPolicy classifies intent as RISK
- **THEN** ModelRouter selects the strong model profile (e.g., claude-opus or deepseek-r1)

### Requirement: Automatic fallback on failure
The system SHALL automatically fall back to the fallback model when the primary model fails.

#### Scenario: Primary model unavailable
- **WHEN** the primary model returns an error or times out
- **THEN** ModelRouter retries with the fallback model and logs a model_fallback event to trace

#### Scenario: Both models fail
- **WHEN** both primary and fallback models fail
- **THEN** ModelRouter returns the error to AgentLoop with TaskState=FAILED

### Requirement: Model routing transparency
The system SHALL record which model was selected and why in the trace.

#### Scenario: Trace records model selection
- **WHEN** ModelRouter selects a model for a run
- **THEN** the trace run_started event includes `selected_model`, `model_profile`, `selection_reason` (intent_match or fallback)

### Requirement: Runtime profile hot-reload
The system SHALL support reloading model profile configuration without restart.

#### Scenario: Config file updated
- **WHEN** the model profile config file is modified and a SIGHUP signal is received
- **THEN** ModelRouter reloads the profile table and applies to subsequent runs
