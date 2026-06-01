## ADDED Requirements

### Requirement: Agentic reasoning single-call prompt
The system SHALL provide a prompt that combines intent classification, query rewriting, and retrieval decision into a single LLM call, replacing the current separate assess_risk tool call.

#### Scenario: CONSULT intent with retrieval
- **WHEN** user sends "我最近压力好大，晚上翻来覆去睡不着"
- **THEN** the LLM returns JSON with intent="CONSULT", rewritten_query="压力 失眠 焦虑", action="RETRIEVE", search_query="失眠 压力 放松技巧"

#### Scenario: CHAT intent without retrieval
- **WHEN** user sends "今天天气真好"
- **THEN** the LLM returns JSON with intent="CHAT", action="ANSWER", search_query=""

#### Scenario: RISK intent
- **WHEN** user sends "活着没意思"
- **THEN** the LLM returns JSON with intent="RISK", action="ANSWER"

### Requirement: Fallback on JSON parse failure
The system SHALL fall back to heuristic intent classification when the LLM response cannot be parsed as JSON.

#### Scenario: LLM returns non-JSON
- **WHEN** the LLM returns plain text instead of JSON
- **THEN** the system uses RiskPolicy::heuristic_intent() for intent, uses the original user message as search_query, and sets action="RETRIEVE"

### Requirement: Search query used for RAG retrieval
The system SHALL use the search_query from agentic reasoning output for retrieve_knowledge, instead of the original user message.

#### Scenario: Rewritten query improves retrieval
- **WHEN** agentic_reasoning outputs search_query="失眠 焦虑 放松技巧" for user input "翻来覆去睡不着"
- **THEN** retrieve_knowledge is called with query="失眠 焦虑 放松技巧"

### Requirement: Intent-aware RAG triggering
The system SHALL invoke retrieve_knowledge when intent is CONSULT or RISK and action is RETRIEVE. CHAT skips RAG.

#### Scenario: CHAT skips RAG
- **WHEN** intent is CHAT
- **THEN** retrieve_knowledge is NOT called, RAG contexts are empty

#### Scenario: CONSULT uses RAG
- **WHEN** intent is CONSULT and action is RETRIEVE
- **THEN** retrieve_knowledge is called with search_query

#### Scenario: RISK uses RAG
- **WHEN** intent is RISK
- **THEN** retrieve_knowledge is called with search_query to retrieve crisis intervention knowledge (crisis_intervention.md contains crisis response protocols, do/don't guidelines, referral criteria)
