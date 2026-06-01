#include "mindbridge/harness/tool_registry.hpp"

#include <stdexcept>
#include <vector>

namespace mindbridge {

namespace {

std::string extract_server_name(const std::string& tool_name) {
    // MCP tools follow the pattern: mcp_<servername>_<toolname>
    const std::string prefix = "mcp_";
    if (tool_name.size() > prefix.size() && tool_name.compare(0, prefix.size(), prefix) == 0) {
        auto pos = tool_name.find('_', prefix.size());
        if (pos != std::string::npos && pos > prefix.size()) {
            return tool_name.substr(prefix.size(), pos - prefix.size());
        }
    }
    return "local";
}

bool matches_type(const nlohmann::json& value, ToolSchema::Field::Type type) {
    switch (type) {
        case ToolSchema::Field::Type::String:
            return value.is_string();
        case ToolSchema::Field::Type::Number:
            return value.is_number();
        case ToolSchema::Field::Type::Boolean:
            return value.is_boolean();
        case ToolSchema::Field::Type::Object:
            return value.is_object();
        case ToolSchema::Field::Type::Array:
            return value.is_array();
        default:
            return false;
    }
}

}  // namespace

void ToolRegistry::register_tool(std::shared_ptr<ITool> tool) {
    if (!tool) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tools_[tool->name()] = std::move(tool);
}

bool ToolRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.count(name) > 0;
}

bool ToolRegistry::is_read_only(const std::string& name, const nlohmann::json& arguments) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return true;
    }
    return it->second->is_read_only(arguments);
}

std::optional<std::string> ToolRegistry::validate(const std::string& name,
                                                  const nlohmann::json& arguments) const {
    std::shared_ptr<ITool> t;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            return "unknown tool: " + name;
        }
        t = it->second;
    }
    if (!arguments.is_object()) {
        return "tool arguments must be an object";
    }
    const ToolSchema schema = t->schema();
    for (const auto& field : schema.fields) {
        if (field.required && !arguments.contains(field.name)) {
            return "missing required argument: " + field.name;
        }
        if (arguments.contains(field.name) &&
            !matches_type(arguments.at(field.name), field.type)) {
            return "invalid type for argument: " + field.name;
        }
    }
    return t->validate(arguments);
}

ToolResult ToolRegistry::execute(const std::string& name,
                                 const nlohmann::json& arguments,
                                 ToolContext& ctx) {
    std::shared_ptr<ITool> t;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            throw std::runtime_error("unknown tool: " + name);
        }
        t = it->second;
    }

    // Rate limiting: check quota before execution
    const std::string server_name = extract_server_name(name);
    if (!quota_mgr_.try_acquire(server_name, 5000)) {
        ToolResult r;
        r.ok = false;
        r.error_code = "rate_limit_exceeded";
        r.summary = "Rate limit exceeded for server: " + server_name;
        return r;
    }

    ToolResult result = t->execute(arguments, ctx);

    // Record usage against daily budget (rough token estimate: chars / 4)
    quota_mgr_.record_usage(server_name, static_cast<int64_t>(result.summary.size() / 4));

    const ToolSchema schema = t->schema();
    const std::string raw = result.data.dump();
    if (raw.size() > schema.max_output_chars) {
        nlohmann::json clipped_data = {
            {"preview", raw.substr(0, schema.max_output_chars)},
            {"truncated_chars", raw.size() - schema.max_output_chars},
        };
        result.data = clipped_data;
        result.clipped = true;
    }
    return result;
}

std::vector<std::string> ToolRegistry::tool_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) {
        out.push_back(kv.first);
    }
    return out;
}

void ToolRegistry::register_mcp_quota(const std::string& server_name,
                                       double rate_limit,
                                       int burst_size,
                                       int64_t daily_budget) {
    quota_mgr_.register_server(server_name, rate_limit, burst_size, daily_budget);
}

void ToolRegistry::reload_mcp_quota_config(const nlohmann::json& config) {
    quota_mgr_.reload_config(config);
}

}  // namespace mindbridge
