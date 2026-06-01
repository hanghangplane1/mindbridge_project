#pragma once

#include "mindbridge/runtime/task_state.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace mindbridge {

class RunStore {
public:
    explicit RunStore(std::string root = ".mindbridge");
    std::string start_run(const std::string& task_id);
    void write_task_state(const TaskState& state);
    void append_trace(const nlohmann::json& event);
    void write_report(const nlohmann::json& report);

    const std::string& run_id() const { return run_id_; }
    std::string run_dir() const;

private:
    std::string root_;
    std::string run_id_;
};

}  // namespace mindbridge
