#pragma once

#include "mindbridge/harness/mcp_quota.hpp"
#include "mindbridge/harness/tool.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mindbridge {

class ToolRegistry {
public:
    void register_tool(std::shared_ptr<ITool> tool);
    bool has(const std::string& name) const;
    bool is_read_only(const std::string& name, const nlohmann::json& arguments) const;
    std::optional<std::string> validate(const std::string& name, const nlohmann::json& arguments) const;
    ToolResult execute(const std::string& name, const nlohmann::json& arguments, ToolContext& ctx);

    std::vector<std::string> tool_names() const;

    void register_mcp_quota(const std::string& server_name, double rate_limit, int burst_size, int64_t daily_budget);
    void reload_mcp_quota_config(const nlohmann::json& config);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ITool>> tools_;
    McpQuotaManager quota_mgr_;
};

}  // namespace mindbridge
