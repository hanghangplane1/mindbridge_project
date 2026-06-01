// knowledge_ingest/main.cpp -- CLI tool to chunk and upsert documents into ChromaDB.
//
// Reads .md/.txt files from a directory, chunks them, generates embeddings via
// Ollama, and upserts into a ChromaDB collection. Supports incremental updates
// via content-hash tracking in .ingest_state.json.
//
// Dependencies: libcurl, nlohmann/json, OpenSSL (for SHA256).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config {
    std::string input_dir;
    std::string collection   = "mindbridge_knowledge";
    std::string chroma_url   = "http://localhost:8000";
    std::string embedding_model = "text-embedding-v3";
    std::string api_url      = "https://dashscope.aliyuncs.com/compatible-mode/v1/embeddings";
    std::string api_key;
    int         chunk_size   = 500;
    int         chunk_overlap = 50;
    bool        dry_run      = false;
    bool        verbose      = false;
};

// ---------------------------------------------------------------------------
// HTTP helpers (libcurl)
// ---------------------------------------------------------------------------
namespace http {

struct Response {
    long        status = 0;
    std::string body;
};

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

Response post(const std::string& url, const std::string& json_body,
              const std::string& content_type = "application/json") {
    Response resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }

    struct curl_slist* headers = nullptr;
    std::string ct = "Content-Type: " + content_type;
    headers = curl_slist_append(headers, ct.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error(std::string("curl POST failed: ") +
                                 curl_easy_strerror(rc));
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

Response get(const std::string& url) {
    Response resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        curl_easy_cleanup(curl);
        throw std::runtime_error(std::string("curl GET failed: ") +
                                 curl_easy_strerror(rc));
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    curl_easy_cleanup(curl);
    return resp;
}

}  // namespace http

// ---------------------------------------------------------------------------
// SHA-256 (OpenSSL EVP)
// ---------------------------------------------------------------------------
std::string sha256_hex(const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA256 digest failed");
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA256 final failed");
    }
    EVP_MD_CTX_free(ctx);

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash_len * 2);
    for (unsigned int i = 0; i < hash_len; ++i) {
        result.push_back(hex[(hash[i] >> 4) & 0x0F]);
        result.push_back(hex[hash[i] & 0x0F]);
    }
    return result;
}

// ---------------------------------------------------------------------------
// File reading
// ---------------------------------------------------------------------------
struct FileEntry {
    fs::path path;
    std::string content;
    std::string hash;
};

std::vector<FileEntry> read_files(const std::string& input_dir) {
    std::vector<FileEntry> entries;
    for (auto& entry : fs::recursive_directory_iterator(input_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        // Normalize to lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext != ".md" && ext != ".txt") continue;

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) {
            std::cerr << "[WARN] Cannot open: " << entry.path() << "\n";
            continue;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        entries.push_back({entry.path(), content, sha256_hex(content)});
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Chunking
// ---------------------------------------------------------------------------
struct Chunk {
    std::string text;
    int         index;      // chunk ordinal within the file
};

// Split text by a delimiter, returning non-empty segments.
static std::vector<std::string> split_by(const std::string& text,
                                          const std::string& delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < text.size()) {
        size_t pos = text.find(delim, start);
        if (pos == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + delim.size();
    }
    return parts;
}

// Trim leading/trailing whitespace.
static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(start, end - start);
}

// Split a single segment into chunks respecting max_size with overlap.
static std::vector<std::string> chunk_text(const std::string& text,
                                            int max_size, int overlap) {
    std::vector<std::string> result;
    if (text.empty()) return result;

    if (static_cast<int>(text.size()) <= max_size) {
        result.push_back(text);
        return result;
    }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = pos + static_cast<size_t>(max_size);
        if (end >= text.size()) {
            result.push_back(text.substr(pos));
            break;
        }
        // Try to break at a sentence or word boundary.
        size_t break_pos = text.rfind(". ", end);
        if (break_pos == std::string::npos || break_pos <= pos) {
            break_pos = text.rfind('\n', end);
        }
        if (break_pos == std::string::npos || break_pos <= pos) {
            break_pos = text.rfind(' ', end);
        }
        if (break_pos == std::string::npos || break_pos <= pos) {
            break_pos = end;
        } else {
            break_pos += 1;  // Include the break character.
        }
        result.push_back(text.substr(pos, break_pos - pos));
        size_t step = break_pos - pos;
        if (step <= static_cast<size_t>(overlap)) {
            // Avoid infinite loop: advance by at least 1.
            pos += step;
        } else {
            pos += step - static_cast<size_t>(overlap);
        }
    }
    return result;
}

std::vector<Chunk> chunk_document(const std::string& content,
                                   const std::string& source_path,
                                   int max_size, int overlap,
                                   bool verbose) {
    // Step 1: Split by ## headings (markdown section breaks).
    auto sections = split_by(content, "\n## ");

    std::vector<Chunk> chunks;
    int chunk_idx = 0;

    for (auto& section : sections) {
        std::string sec = trim(section);
        if (sec.empty()) continue;

        // Step 2: Within each section, split by double-newline (paragraphs).
        auto paragraphs = split_by(sec, "\n\n");

        // Accumulate small paragraphs together before chunking.
        std::string buffer;
        for (auto& para : paragraphs) {
            para = trim(para);
            if (para.empty()) continue;

            if (!buffer.empty() &&
                buffer.size() + para.size() + 2 > static_cast<size_t>(max_size)) {
                // Flush buffer.
                for (auto& c : chunk_text(buffer, max_size, overlap)) {
                    chunks.push_back({std::move(c), chunk_idx++});
                }
                buffer.clear();
            }
            if (!buffer.empty()) buffer += "\n\n";
            buffer += para;
        }
        if (!buffer.empty()) {
            for (auto& c : chunk_text(buffer, max_size, overlap)) {
                chunks.push_back({std::move(c), chunk_idx++});
            }
        }
    }

    if (verbose) {
        std::cerr << "[INFO] " << source_path << ": "
                  << chunks.size() << " chunk(s)\n";
    }
    return chunks;
}

// ---------------------------------------------------------------------------
// Incremental state (.ingest_state.json)
// ---------------------------------------------------------------------------
using StateMap = std::unordered_map<std::string, std::string>;

static fs::path state_path(const std::string& input_dir) {
    return fs::path(input_dir) / ".ingest_state.json";
}

StateMap load_state(const std::string& input_dir) {
    StateMap state;
    auto p = state_path(input_dir);
    if (!fs::exists(p)) return state;

    std::ifstream ifs(p);
    if (!ifs.is_open()) return state;

    try {
        json j = json::parse(ifs);
        for (auto& [key, val] : j.items()) {
            state[key] = val.get<std::string>();
        }
    } catch (const std::exception& e) {
        std::cerr << "[WARN] Failed to parse state file: " << e.what() << "\n";
    }
    return state;
}

void save_state(const std::string& input_dir, const StateMap& state) {
    json j;
    for (auto& [k, v] : state) {
        j[k] = v;
    }
    auto p = state_path(input_dir);
    std::ofstream ofs(p);
    if (!ofs.is_open()) {
        std::cerr << "[ERROR] Cannot write state file: " << p << "\n";
        return;
    }
    ofs << j.dump(2) << "\n";
}

// ---------------------------------------------------------------------------
// DashScope / OpenAI-compatible embedding API
// ---------------------------------------------------------------------------
std::vector<float> get_embedding(const Config& cfg, const std::string& text) {
    json req;
    req["model"] = cfg.embedding_model;
    req["input"] = text;

    // Build headers with Authorization
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!cfg.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    std::string resp_body;
    curl_easy_setopt(curl, CURLOPT_URL, cfg.api_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    std::string body = req.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http::write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("embedding curl failed: ") + curl_easy_strerror(rc));
    }
    if (status != 200) {
        throw std::runtime_error("embedding API failed (HTTP " + std::to_string(status) + "): " + resp_body);
    }

    json result = json::parse(resp_body);
    // DashScope/OpenAI format: {"data": [{"embedding": [...]}]}
    auto& emb = result["data"][0]["embedding"];
    std::vector<float> vec;
    vec.reserve(emb.size());
    for (auto& v : emb) {
        vec.push_back(v.get<float>());
    }
    return vec;
}

// ---------------------------------------------------------------------------
// ChromaDB operations
// ---------------------------------------------------------------------------
// Returns the collection UUID (needed for ChromaDB 0.4.x upsert endpoint).
std::string ensure_collection(const Config& cfg) {
    json req;
    req["name"]          = cfg.collection;
    req["get_or_create"] = true;

    auto resp = http::post(
        cfg.chroma_url + "/api/v1/collections", req.dump());

    if (resp.status != 200 && resp.status != 201 && resp.status != 409) {
        throw std::runtime_error(
            "Failed to create/get collection (HTTP " +
            std::to_string(resp.status) + "): " + resp.body);
    }

    // Response contains the collection UUID.
    json result = json::parse(resp.body);
    std::string uuid = result.value("id", "");
    if (uuid.empty()) {
        // Fallback: GET collection by name to retrieve UUID.
        auto get_resp = http::get(cfg.chroma_url + "/api/v1/collections/" + cfg.collection);
        if (get_resp.status == 200) {
            json get_result = json::parse(get_resp.body);
            uuid = get_result.value("id", "");
        }
    }
    if (uuid.empty()) {
        throw std::runtime_error("Could not resolve collection UUID for: " + cfg.collection);
    }
    return uuid;
}

void chroma_upsert(const std::string& chroma_url,
                   const std::string& collection_uuid,
                   const std::vector<std::string>& ids,
                   const std::vector<std::string>& documents,
                   const std::vector<std::vector<float>>& embeddings,
                   const std::vector<json>& metadatas) {
    json emb_json = json::array();
    for (auto& e : embeddings) {
        emb_json.push_back(e);
    }

    json meta_json = json::array();
    for (auto& m : metadatas) {
        meta_json.push_back(m);
    }

    json req;
    req["ids"]         = ids;
    req["documents"]   = documents;
    req["embeddings"]  = emb_json;
    req["metadatas"]   = meta_json;

    auto resp = http::post(
        chroma_url + "/api/v1/collections/" + collection_uuid + "/upsert",
        req.dump());

    if (resp.status != 200 && resp.status != 201) {
        throw std::runtime_error(
            "ChromaDB upsert failed (HTTP " + std::to_string(resp.status) +
            "): " + resp.body);
    }
}

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------
Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + arg);
            }
            return argv[++i];
        };

        if (arg == "--input-dir")           cfg.input_dir       = next();
        else if (arg == "--collection")     cfg.collection      = next();
        else if (arg == "--chroma-url")     cfg.chroma_url      = next();
        else if (arg == "--embedding-model") cfg.embedding_model = next();
        else if (arg == "--api-url")        cfg.api_url         = next();
        else if (arg == "--api-key")        cfg.api_key         = next();
        else if (arg == "--chunk-size")     cfg.chunk_size      = std::stoi(next());
        else if (arg == "--chunk-overlap")  cfg.chunk_overlap   = std::stoi(next());
        else if (arg == "--dry-run")        cfg.dry_run         = true;
        else if (arg == "--verbose")        cfg.verbose         = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: knowledge_ingest [OPTIONS]\n"
                "\n"
                "Read .md/.txt files, chunk them, and upsert into ChromaDB.\n"
                "\n"
                "Options:\n"
                "  --input-dir DIR        (required) Directory to scan\n"
                "  --collection NAME      ChromaDB collection (default: mindbridge_knowledge)\n"
                "  --chroma-url URL       ChromaDB endpoint  (default: http://localhost:8000)\n"
                "  --embedding-model M    Embedding model name  (default: text-embedding-v3)\n"
                "  --api-url URL          Embedding API endpoint (default: DashScope)\n"
                "  --api-key KEY          Embedding API key (required for remote API)\n"
                "  --chunk-size N         Max characters per chunk (default: 500)\n"
                "  --chunk-overlap N      Overlap characters     (default: 50)\n"
                "  --dry-run              Report stats without making API calls\n"
                "  --verbose              Print per-file chunk details\n"
                "  -h, --help             Show this help\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (cfg.input_dir.empty()) {
        throw std::runtime_error("--input-dir is required");
    }
    if (!fs::is_directory(cfg.input_dir)) {
        throw std::runtime_error("--input-dir is not a directory: " + cfg.input_dir);
    }
    if (cfg.chunk_size <= 0 || cfg.chunk_overlap < 0 ||
        cfg.chunk_overlap >= cfg.chunk_size) {
        throw std::runtime_error(
            "Invalid chunk-size/chunk-overlap: chunk_size must be > 0 "
            "and chunk_overlap must be in [0, chunk_size)");
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Config cfg;
    try {
        cfg = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    // ---- Read files ----
    std::vector<FileEntry> files;
    try {
        files = read_files(cfg.input_dir);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Reading files: " << e.what() << "\n";
        return 1;
    }

    if (files.empty()) {
        std::cout << "No .md/.txt files found in " << cfg.input_dir << "\n";
        return 0;
    }

    // ---- Load incremental state ----
    StateMap prev_state = load_state(cfg.input_dir);
    StateMap new_state  = prev_state;

    // ---- Chunk all files ----
    struct FileChunk {
        FileEntry            file;
        std::vector<Chunk>   chunks;
    };
    std::vector<FileChunk> file_chunks;
    int total_chunks = 0;
    size_t total_chars = 0;

    for (auto& f : files) {
        // Check incremental: skip if hash unchanged.
        auto it = prev_state.find(f.path.string());
        if (it != prev_state.end() && it->second == f.hash) {
            if (cfg.verbose) {
                std::cerr << "[SKIP] " << f.path << " (unchanged)\n";
            }
            // Preserve in new_state.
            continue;
        }

        auto chunks = chunk_document(f.content, f.path.string(),
                                      cfg.chunk_size, cfg.chunk_overlap,
                                      cfg.verbose);
        if (!chunks.empty()) {
            file_chunks.push_back({std::move(f), std::move(chunks)});
        }
        // Update state for this file.
        new_state[f.path.string()] = f.hash;
    }

    for (auto& fc : file_chunks) {
        total_chunks += static_cast<int>(fc.chunks.size());
        for (auto& c : fc.chunks) {
            total_chars += c.text.size();
        }
    }

    // Also count already-skipped files.
    int skipped = static_cast<int>(files.size()) - static_cast<int>(file_chunks.size());

    // ---- Dry-run report ----
    if (cfg.dry_run) {
        std::cout << "=== Dry Run ===\n"
                  << "  Files scanned:        " << files.size() << "\n"
                  << "  Files skipped (cached): " << skipped << "\n"
                  << "  Files to process:     " << file_chunks.size() << "\n"
                  << "  Total chunks:         " << total_chunks << "\n"
                  << "  Total characters:     " << total_chars << "\n"
                  << "  Est. tokens (~4 chars): ~" << (total_chars / 4) << "\n";
        return 0;
    }

    if (file_chunks.empty()) {
        std::cout << "All files are up to date. Nothing to ingest.\n";
        return 0;
    }

    // ---- Initialize libcurl ----
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // ---- Ensure collection exists ----
    std::string collection_uuid;
    try {
        collection_uuid = ensure_collection(cfg);
        if (cfg.verbose) {
            std::cerr << "[INFO] Collection '" << cfg.collection
                      << "' ready (id=" << collection_uuid << ").\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Collection setup: " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    // ---- Process files in batches ----
    // We upsert one file at a time to keep memory bounded and provide
    // per-file progress.
    int upserted_files = 0;
    int upserted_chunks = 0;

    for (auto& fc : file_chunks) {
        std::vector<std::string>          ids;
        std::vector<std::string>          documents;
        std::vector<std::vector<float>>   embeddings;
        std::vector<json>                 metadatas;

        ids.reserve(fc.chunks.size());
        documents.reserve(fc.chunks.size());
        embeddings.reserve(fc.chunks.size());
        metadatas.reserve(fc.chunks.size());

        for (auto& chunk : fc.chunks) {
            // Deterministic ID: hash of (file_path + chunk_index).
            std::string id_seed = fc.file.path.string() + "::" +
                                  std::to_string(chunk.index);
            std::string id = "doc_" + sha256_hex(id_seed).substr(0, 16);

            // Get embedding from DashScope API.
            std::vector<float> emb;
            try {
                emb = get_embedding(cfg, chunk.text);
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] Embedding failed for "
                          << fc.file.path << " chunk " << chunk.index
                          << ": " << e.what() << "\n";
                throw;
            }

            json meta;
            meta["source"]    = fc.file.path.string();
            meta["chunk_idx"] = chunk.index;
            meta["hash"]      = fc.file.hash;

            ids.push_back(std::move(id));
            documents.push_back(chunk.text);
            embeddings.push_back(std::move(emb));
            metadatas.push_back(std::move(meta));
        }

        // Upsert to ChromaDB.
        try {
            chroma_upsert(cfg.chroma_url, collection_uuid, ids, documents, embeddings, metadatas);
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Upsert failed for " << fc.file.path
                      << ": " << e.what() << "\n";
            curl_global_cleanup();
            return 1;
        }

        upserted_files++;
        upserted_chunks += static_cast<int>(fc.chunks.size());
        std::cout << "[" << upserted_files << "/" << file_chunks.size() << "] "
                  << fc.file.path.filename().string()
                  << " -- " << fc.chunks.size() << " chunk(s) upserted\n";
    }

    // ---- Save state ----
    save_state(cfg.input_dir, new_state);

    curl_global_cleanup();

    std::cout << "\nDone. " << upserted_files << " file(s), "
              << upserted_chunks << " chunk(s) upserted into '"
              << cfg.collection << "'.\n";
    return 0;
}
