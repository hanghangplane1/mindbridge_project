#pragma once

#include "ollama_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

class ChromaClient {
public:
    ChromaClient(std::string base_url,
                 std::string collection_name,
                 std::string embedding_model = "nomic-embed-text")
        : base_url_(std::move(base_url)),
          collection_name_(std::move(collection_name)),
          embedding_model_(std::move(embedding_model)) {}

    std::vector<std::string> query(OllamaClient& ollama,
                                   const std::string& text,
                                   int n_results = 3) const {
        const std::vector<double> emb = ollama.embedding(text, embedding_model_);
        nlohmann::json request{
            {"query_embeddings", nlohmann::json::array({emb})},
            {"n_results", n_results},
            {"include", nlohmann::json::array({"documents", "metadatas"})},
        };

        const std::string url = base_url_ + "/api/v1/collections/" + collection_name_ + "/query";
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

private:
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* out = static_cast<std::string*>(userp);
        out->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    static std::string post_json(const std::string& url, const std::string& body, long timeout_sec) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to init CURL");
        }
        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);

        const CURLcode rc = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
        }
        return response;
    }

    std::string base_url_;
    std::string collection_name_;
    std::string embedding_model_;
};
