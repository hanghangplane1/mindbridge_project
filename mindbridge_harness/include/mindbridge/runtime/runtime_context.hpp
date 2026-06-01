#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace mindbridge {

struct RuntimeContext {
    std::string request_id;
    std::string user_id{"anonymous"};
    std::string conversation_id{"default"};
    std::string intent{"CHAT"};
    std::string risk_level{"low"};
    std::string trace_path;

    nlohmann::json to_json() const {
        return {{"request_id", request_id},
                {"user_id", user_id},
                {"conversation_id", conversation_id},
                {"intent", intent},
                {"risk_level", risk_level},
                {"trace_path", trace_path}};
    }
};

}  // namespace mindbridge
