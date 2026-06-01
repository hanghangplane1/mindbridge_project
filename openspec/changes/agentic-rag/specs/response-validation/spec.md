## ADDED Requirements

### Requirement: Response length validation
The system SHALL reject responses shorter than 15 characters.

#### Scenario: Response too short
- **WHEN** model generates a response with fewer than 15 characters
- **THEN** validation fails with reason="response_too_short"

### Requirement: Harmful pattern detection
The system SHALL reject responses containing harmful patterns.

#### Scenario: Harmful pattern detected
- **WHEN** response contains any of: "你可以去死", "你活该", "放弃吧", "没救了", "你完了"
- **THEN** validation fails with reason="harmful_pattern"

### Requirement: Crisis hotline requirement
The system SHALL require hotline numbers in responses when the user query contains crisis keywords.

#### Scenario: Crisis query without hotline
- **WHEN** query contains crisis keywords ("自杀", "不想活", "自残") AND response does NOT contain any hotline number ("400-161-9995", "12355", "120", "110")
- **THEN** validation fails with reason="missing_hotline"

### Requirement: RAG context key term coverage
The system SHALL check that responses reference at least one key term from the RAG context when context was provided.

#### Scenario: Context provided but not referenced
- **WHEN** RAG context was used AND response does not contain any key term extracted from the context
- **THEN** validation fails with reason="no_context_reference"

#### Scenario: No context provided
- **WHEN** RAG context was empty (CHAT or RISK intent)
- **THEN** this check is skipped

### Requirement: Validation retry with broader query
The system SHALL retry generation with a broader query when validation fails and RAG context was available.

#### Scenario: Validation fails, retry with rewritten_query
- **WHEN** validation fails AND RAG context was available
- **THEN** the system calls retrieve_knowledge again with the rewritten_query (broader than search_query), regenerates the answer, and returns the new answer without further validation

#### Scenario: Validation fails, no context to retry
- **WHEN** validation fails AND RAG context was empty
- **THEN** the system returns the original answer (no retry possible)
