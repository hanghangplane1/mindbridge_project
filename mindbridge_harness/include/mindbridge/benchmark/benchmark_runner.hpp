#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace mindbridge {

struct BenchmarkSummary {
    int total{0};
    int passed{0};
    int failed{0};
    nlohmann::json failure_categories = nlohmann::json::object();
    nlohmann::json details = nlohmann::json::array();
};

/** 读取 mindbridge_tasks.json 并执行 http / hardness 类任务 */
class BenchmarkRunner {
public:
    static BenchmarkSummary run_file(const std::string& path);
};

}  // namespace mindbridge
