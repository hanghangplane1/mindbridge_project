#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mindbridge {

struct ToolContext {
    nlohmann::json metadata;
};

enum class ToolRisk { Low, High };

struct ToolSchema {
    struct Field {
        std::string name;
        enum class Type { String, Number, Boolean, Object, Array } type{Type::String};
        bool required{false};
    };

    std::vector<Field> fields;
    ToolRisk risk{ToolRisk::Low};
    size_t max_output_chars{4000};
};

struct ToolResult {
    bool ok{true};
    std::string error_code;
    std::string summary;
    nlohmann::json data = nlohmann::json::object();
    bool clipped{false};

    nlohmann::json to_json(bool flatten_data = true) const {
        nlohmann::json out = {
            {"ok", ok},
            {"error_code", error_code},
            {"summary", summary},
            {"data", data},
            {"clipped", clipped},
        };
        if (flatten_data && data.is_object()) {
            for (const auto& item : data.items()) {
                out[item.key()] = item.value();
            }
        }
        return out;
    }
};

/** 单个可执行工具（可由 MCP、纯逻辑等实现） */
class ITool {
public:
    virtual ~ITool() = default;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual ToolSchema schema() const = 0;
    virtual bool is_read_only(const nlohmann::json& arguments) const {
        (void)arguments;
        return schema().risk == ToolRisk::Low;
    }
    virtual std::optional<std::string> validate(const nlohmann::json& arguments) const = 0;
    virtual ToolResult execute(const nlohmann::json& arguments, ToolContext& ctx) = 0;
};

}  // namespace mindbridge
