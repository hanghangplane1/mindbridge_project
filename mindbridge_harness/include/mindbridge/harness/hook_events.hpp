#pragma once

#include <string>

namespace mindbridge {

enum class HookEvent {
    SessionStart,
    SessionEnd,
    UserPromptSubmit,
    PreToolUse,
    PostToolUse,
    Stop,
};

inline std::string hook_event_name(HookEvent e) {
    switch (e) {
        case HookEvent::SessionStart:
            return "session_start";
        case HookEvent::SessionEnd:
            return "session_end";
        case HookEvent::UserPromptSubmit:
            return "user_prompt_submit";
        case HookEvent::PreToolUse:
            return "pre_tool_use";
        case HookEvent::PostToolUse:
            return "post_tool_use";
        case HookEvent::Stop:
            return "stop";
        default:
            return "unknown";
    }
}

}  // namespace mindbridge
