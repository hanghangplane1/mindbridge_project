#include "mindbridge/harness/permission_checker.hpp"

namespace mindbridge {

PermissionChecker::PermissionChecker(Mode mode) : mode_(mode) {}

void PermissionChecker::set_denied_tools(std::unordered_set<std::string> names) {
    denied_tools_ = std::move(names);
}

void PermissionChecker::set_allowed_tools(std::unordered_set<std::string> names) {
    allowed_tools_ = std::move(names);
}

PermissionDecision PermissionChecker::evaluate(const std::string& tool_name,
                                                bool is_read_only) const {
    if (denied_tools_.count(tool_name)) {
        return {false, false, tool_name + " is explicitly denied"};
    }
    if (!allowed_tools_.empty()) {
        if (allowed_tools_.count(tool_name)) {
            return {true, false, tool_name + " is explicitly allowed"};
        }
        return {false, false, tool_name + " is not in the allowed tools list"};
    }
    if (mode_ == Mode::FullAuto) {
        return {true, false, "full_auto allows tools"};
    }
    if (is_read_only) {
        return {true, false, "read-only tool allowed"};
    }
    if (mode_ == Mode::Plan) {
        return {false, false, "plan mode blocks mutating tools"};
    }
    // Default: mutating tools need confirmation (harness 侧不弹 UI，标记为需确认且默认不放行)
    return {false, true, "mutating tool requires confirmation in default mode"};
}

}  // namespace mindbridge
