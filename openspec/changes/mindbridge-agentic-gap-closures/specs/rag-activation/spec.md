## ADDED Requirements

### Requirement: Knowledge ingestion pipeline
The system SHALL provide a command-line tool that reads knowledge documents from a directory, chunks them, generates embeddings, and upserts into ChromaDB.

#### Scenario: Ingest text documents
- **WHEN** the ingestion tool is run with `--input-dir /path/to/docs --collection mindbridge_knowledge`
- **THEN** the tool reads all .txt/.md/.pdf files, splits into chunks of ~500 tokens with 50-token overlap, generates embeddings via ModelClient::embedding(), and upserts into the specified ChromaDB collection

#### Scenario: Incremental update
- **WHEN** the ingestion tool is run on a directory that was previously ingested with some files added/modified
- **THEN** the tool detects changed files (by content hash), re-chunks and re-embeds only changed documents, and upserts the updated chunks

#### Scenario: Dry-run mode
- **WHEN** the ingestion tool is run with `--dry-run`
- **THEN** the tool reports what would be ingested (file count, chunk count, estimated tokens) without actually calling ChromaDB or the embedding API

### Requirement: ChromaDB startup configuration
The system SHALL include ChromaDB in the deployment configuration with proper startup scripts.

#### Scenario: Docker Compose deployment
- **WHEN** the system is deployed via docker-compose
- **THEN** a ChromaDB service is included with persistent volume, exposed on the configured port, and health-checked before counselor starts

#### Scenario: Script-based deployment
- **WHEN** `scripts/start_demo.sh` is executed
- **THEN** the script checks if ChromaDB is reachable at the configured URL, starts it if not running, and exports `MINDBRIDGE_CHROMA_URL` and `MINDBRIDGE_CHROMA_COLLECTION` environment variables

### Requirement: Environment variable configuration
The system SHALL document and validate all RAG-related environment variables.

#### Scenario: Required env vars present
- **WHEN** the counselor starts with `MINDBRIDGE_CHROMA_URL` and `MINDBRIDGE_CHROMA_COLLECTION` set
- **THEN** ChromaVectorStore is initialized with those values and `retrieve_knowledge` tool returns real results

#### Scenario: Missing env vars with clear error
- **WHEN** the counselor starts without `MINDBRIDGE_CHROMA_URL` set
- **THEN** the startup logs a warning: "RAG disabled: MINDBRIDGE_CHROMA_URL not set" (current behavior returns silent empty results, this makes it explicit)

### Requirement: Embedding chain verification
The system SHALL verify the embedding chain is functional at startup.

#### Scenario: Embedding model available
- **WHEN** the counselor starts and ChromaDB is configured
- **THEN** the system performs a test embedding (empty string or "test") and verifies the result is a non-empty vector, logging "RAG embedding chain OK"

#### Scenario: Embedding model unavailable
- **WHEN** the embedding call fails (model not running, API key invalid)
- **THEN** the system logs "RAG disabled: embedding model unavailable" and falls back to disabled mode

### Requirement: RAG status in trace
The system SHALL record RAG status in the run trace.

#### Scenario: RAG active
- **WHEN** a CONSULT/RISK run uses `retrieve_knowledge` successfully
- **THEN** the trace includes `rag_contexts_count` and `rag_retrieval_ms` in the run event

#### Scenario: RAG disabled
- **WHEN** a CONSULT/RISK run attempts `retrieve_knowledge` but RAG is disabled
- **THEN** the trace includes `rag_status: "disabled"` and the run continues without RAG contexts
