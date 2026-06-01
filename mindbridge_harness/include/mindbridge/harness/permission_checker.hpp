#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace mindbridge {

struct PermissionDecision {
    bool allowed{false};
    bool requires_confirmation{false};
    std::string reason;
};

/**
 * 工具执行前策略检查（简化版，对齐 OpenHarness 思路：模式 + 拒绝名单）。
 */
class PermissionChecker {
public:
    enum class Mode { Default, FullAuto, Plan };

    explicit PermissionChecker(Mode mode = Mode::Default);

    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    void set_denied_tools(std::unordered_set<std::string> names);
    void set_allowed_tools(std::unordered_set<std::string> names);

    /** @param is_read_only 工具声明是否只读 */
    PermissionDecision evaluate(const std::string& tool_name, bool is_read_only) const;

private:
    Mode mode_;
    std::unordered_set<std::string> denied_tools_;
    std::unordered_set<std::string> allowed_tools_;
};

}  // namespace mindbridge
