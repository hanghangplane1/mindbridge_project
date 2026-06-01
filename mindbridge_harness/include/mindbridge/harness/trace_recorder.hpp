#pragma once

#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace mindbridge {

/** 追加 JSON 行到 trace 文件（供 benchmark / 排障） */
class TraceRecorder {
public:
    explicit TraceRecorder(std::string path);
    ~TraceRecorder();

    void append(const nlohmann::json& event);
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::mutex mutex_;
    std::ofstream out_;
};

}  // namespace mindbridge
