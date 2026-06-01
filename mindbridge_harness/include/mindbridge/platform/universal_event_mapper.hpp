#pragma once

#include "mindbridge/platform/platform_types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace mindbridge {
namespace platform {

UniversalEvent map_trace_event(const nlohmann::json& trace_row,
                               const std::string& session_id,
                               const std::string& run_id);

std::vector<UniversalEvent> load_session_events(const std::filesystem::path& run_root,
                                                const std::string& session_id,
                                                const std::string& run_id);

nlohmann::json list_run_artifacts(const std::filesystem::path& run_root,
                                  const std::string& run_id);

}  // namespace platform
}  // namespace mindbridge
