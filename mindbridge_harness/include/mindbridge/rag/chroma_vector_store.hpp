#pragma once

#include "mindbridge/rag/vector_store_client.hpp"

#include <string>

namespace mindbridge {

/** Chroma v1 HTTP: POST /api/v1/collections/{name}/query */
class ChromaVectorStore : public VectorStoreClient {
public:
    ChromaVectorStore(std::string base_url,
                      std::string collection_name,
                      std::string embedding_model = "nomic-embed-text");

    std::vector<std::string> query(ModelClient& embed_model,
                                     const std::string& text,
                                     int n_results = 3) override;

private:
    std::string base_url_;
    std::string collection_name_;
    std::string embedding_model_;
    std::string collection_uuid_;
};

}  // namespace mindbridge
