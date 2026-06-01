#include "mindbridge/benchmark/benchmark_runner.hpp"

#include <nlohmann/json.hpp>

#include <curl/curl.h>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const char* env = std::getenv("MINDBRIDGE_TASKS_JSON");
    const char* path = (argc > 1) ? argv[1]
                      : (env ? env : "mindbridge_harness/benchmarks/mindbridge_tasks.json");
    try {
        auto summary = mindbridge::BenchmarkRunner::run_file(path);
        nlohmann::json out;
        out["total"] = summary.total;
        out["passed"] = summary.passed;
        out["failed"] = summary.failed;
        out["pass_rate"] = summary.total ? static_cast<double>(summary.passed) / summary.total : 0.0;
        out["failure_categories"] = summary.failure_categories;
        out["details"] = summary.details;
        nlohmann::json report = {
            {"request_success_rate", out["pass_rate"]},
            {"avg_latency_ms", nullptr},
            {"p95_latency_ms", nullptr},
            {"rag_hit_rate", nullptr},
            {"risk_detection_accuracy", nullptr},
            {"mcp_delivery_rate", nullptr},
            {"json_sse_protocol_compliance", nullptr},
            {"deterministic_baseline", "contract tasks may use deterministic_output for harness-only validation"},
            {"model_comparison", "set MINDBRIDGE_MODEL_PROVIDER/MINDBRIDGE_MODEL_NAME and rerun this benchmark"},
        };
        out["quality_report"] = report;
        std::cout << out.dump(2) << std::endl;
        const int rc = summary.failed > 0 ? 1 : 0;
        curl_global_cleanup();
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "benchmark error: " << e.what() << std::endl;
        curl_global_cleanup();
        return 2;
    }
}
