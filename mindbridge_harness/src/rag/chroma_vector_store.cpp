#include "mindbridge/rag/chroma_vector_store.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <stdexcept>

namespace mindbridge {

namespace {

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string get_json(const std::string& url, long timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl init failed");
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
    }
    return response;
}

std::string post_json(const std::string& url, const std::string& body, long timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl init failed");
    }
    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
    }
    return response;
}

std::string get_collection_uuid(const std::string& base_url, const std::string& collection_name) {
    const std::string url = base_url + "/api/v1/collections?name=" + collection_name;
    const std::string raw = get_json(url, 10L);
    auto response = nlohmann::json::parse(raw);
    if (response.is_array() && !response.empty() && response[0].contains("id")) {
        return response[0]["id"].get<std::string>();
    }
    throw std::runtime_error("collection not found: " + collection_name);
}

}  // namespace

ChromaVectorStore::ChromaVectorStore(std::string base_url,
                                     std::string collection_name,
                                     std::string embedding_model)
    : base_url_(std::move(base_url)),
      collection_name_(std::move(collection_name)),
      embedding_model_(std::move(embedding_model)),
      collection_uuid_(get_collection_uuid(base_url_, collection_name_)) {}

std::vector<std::string> ChromaVectorStore::query(ModelClient& embed_model,
                                                  const std::string& text,
                                                  int n_results) {
    const std::vector<double> emb = embed_model.embedding(text, embedding_model_);
    nlohmann::json request{
        {"query_embeddings", nlohmann::json::array({emb})},
        {"n_results", n_results},
        {"include", nlohmann::json::array({"documents", "metadatas"})},
    };
    const std::string url =
        base_url_ + "/api/v1/collections/" + collection_uuid_ + "/query";
    const std::string raw = post_json(url, request.dump(), 30L);
    auto response = nlohmann::json::parse(raw);
    std::vector<std::string> docs;
    if (response.contains("documents") && response["documents"].is_array() &&
        !response["documents"].empty() && response["documents"][0].is_array()) {
        for (const auto& d : response["documents"][0]) {
            if (d.is_string()) {
                docs.push_back(d.get<std::string>());
            }
        }
    }
    return docs;
}

}  // namespace mindbridge
