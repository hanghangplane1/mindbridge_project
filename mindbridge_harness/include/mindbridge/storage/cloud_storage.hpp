#pragma once

#include "mindbridge/state/distributed_state_store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mindbridge {
namespace storage {

struct StorageIdentity {
    std::string user_id{"anonymous"};
    std::string conversation_id{"default"};
    std::string run_id;
};

struct FileObject {
    std::string md5;
    std::string file_id;
    std::string url;
    std::string filename;
    std::string type;
    std::int64_t size{0};
    std::int64_t ref_count{0};
    std::int64_t created_at{0};
};

struct ChunkSession {
    std::string upload_id;
    std::string md5;
    std::string filename;
    std::string type;
    std::int64_t size{0};
    int chunk_count{0};
    std::vector<int> uploaded;
};

class CloudStorageService {
public:
    CloudStorageService(std::string root_dir,
                        std::string public_base_url,
                        state::DistributedStateStore* state_store);
    ~CloudStorageService();

    void initialize();

    nlohmann::json instant_upload(const StorageIdentity& identity,
                                  const std::string& md5,
                                  const std::string& filename,
                                  const std::string& type);
    nlohmann::json upload_base64(const StorageIdentity& identity,
                                 const std::string& filename,
                                 const std::string& md5,
                                 const std::string& type,
                                 const std::string& data_base64);
    nlohmann::json init_chunk_upload(const StorageIdentity& identity,
                                     const std::string& filename,
                                     const std::string& md5,
                                     const std::string& type,
                                     std::int64_t size,
                                     int chunk_count);
    nlohmann::json upload_chunk(const std::string& upload_id,
                                const std::string& md5,
                                int index,
                                const std::string& bytes);
    nlohmann::json merge_chunks(const StorageIdentity& identity,
                                const std::string& upload_id,
                                const std::string& md5,
                                const std::string& filename,
                                const std::string& type);
    nlohmann::json list_files(const StorageIdentity& identity, int limit, int offset) const;
    nlohmann::json download_file(const StorageIdentity& identity, const std::string& md5) const;
    nlohmann::json status() const;
    int sync_pending_artifacts(int limit = 100);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<CloudStorageService> create_cloud_storage_from_env(
    state::DistributedStateStore* state_store);

}  // namespace storage
}  // namespace mindbridge
