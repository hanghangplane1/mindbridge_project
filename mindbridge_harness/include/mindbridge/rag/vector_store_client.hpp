#pragma once

#include "mindbridge/model/model_client.hpp"

#include <string>
#include <vector>

namespace mindbridge {

/** 向量检索抽象（Chroma / 其他实现可插拔） */
class VectorStoreClient {
public:
    virtual ~VectorStoreClient() = default;
    virtual std::vector<std::string> query(ModelClient& embed_model,
                                           const std::string& text,
                                           int n_results = 3) = 0;
};

}  // namespace mindbridge
