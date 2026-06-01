#include "mindbridge/model/model_router.hpp"

#include <stdexcept>

namespace mindbridge {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ModelRouter::ModelRouter() { load_defaults(); }

ModelRouter::ModelRouter(const nlohmann::json& config) {
    if (config.is_null() || config.empty()) {
        load_defaults();
    } else {
        reload(config);
    }
}

// ---------------------------------------------------------------------------
// Default profiles
// ---------------------------------------------------------------------------

void ModelRouter::load_defaults() {
    profiles_.clear();

    profiles_["CHAT"] = ModelProfile{
        /* intent           */ "CHAT",
        /* primary_model_id */ "qwen2-7b",
        /* fallback_model_id*/ "qwen2-72b",
        /* max_tokens       */ 2048,
        /* temperature      */ 0.7,
    };

    profiles_["CONSULT"] = ModelProfile{
        /* intent           */ "CONSULT",
        /* primary_model_id */ "qwen2-72b",
        /* fallback_model_id*/ "deepseek-r1",
        /* max_tokens       */ 4096,
        /* temperature      */ 0.3,
    };

    profiles_["RISK"] = ModelProfile{
        /* intent           */ "RISK",
        /* primary_model_id */ "deepseek-r1",
        /* fallback_model_id*/ "qwen2-72b",
        /* max_tokens       */ 8192,
        /* temperature      */ 0.1,
    };
}

// ---------------------------------------------------------------------------
// JSON hot-reload
// ---------------------------------------------------------------------------

void ModelRouter::reload(const nlohmann::json& config) {
    profiles_.clear();

    if (config.is_array()) {
        for (const auto& entry : config) {
            ModelProfile p;
            p.intent            = entry.value("intent", "");
            p.primary_model_id  = entry.value("primary_model_id", "");
            p.fallback_model_id = entry.value("fallback_model_id", "");
            p.max_tokens        = entry.value("max_tokens", 4096);
            p.temperature       = entry.value("temperature", 0.7);

            if (!p.intent.empty()) {
                profiles_[p.intent] = std::move(p);
            }
        }
    } else if (config.is_object()) {
        // Support {"CHAT": {...}, "CONSULT": {...}, ...} form as well.
        for (auto it = config.begin(); it != config.end(); ++it) {
            const auto& entry = it.value();
            if (!entry.is_object()) {
                continue;
            }
            ModelProfile p;
            p.intent            = it.key();
            p.primary_model_id  = entry.value("primary_model_id", "");
            p.fallback_model_id = entry.value("fallback_model_id", "");
            p.max_tokens        = entry.value("max_tokens", 4096);
            p.temperature       = entry.value("temperature", 0.7);

            if (!p.primary_model_id.empty()) {
                profiles_[p.intent] = std::move(p);
            }
        }
    }

    // Guarantee that the three core intents always have a profile.
    if (profiles_.find("CHAT") == profiles_.end() ||
        profiles_.find("CONSULT") == profiles_.end() ||
        profiles_.find("RISK") == profiles_.end()) {
        load_defaults();
        // Re-apply the user-supplied profiles on top of defaults so that
        // explicit entries win while missing ones fall back to defaults.
        if (config.is_array()) {
            for (const auto& entry : config) {
                ModelProfile p;
                p.intent            = entry.value("intent", "");
                p.primary_model_id  = entry.value("primary_model_id", "");
                p.fallback_model_id = entry.value("fallback_model_id", "");
                p.max_tokens        = entry.value("max_tokens", 4096);
                p.temperature       = entry.value("temperature", 0.7);
                if (!p.intent.empty()) {
                    profiles_[p.intent] = std::move(p);
                }
            }
        } else if (config.is_object()) {
            for (auto it = config.begin(); it != config.end(); ++it) {
                const auto& entry = it.value();
                if (!entry.is_object()) {
                    continue;
                }
                ModelProfile p;
                p.intent            = it.key();
                p.primary_model_id  = entry.value("primary_model_id", "");
                p.fallback_model_id = entry.value("fallback_model_id", "");
                p.max_tokens        = entry.value("max_tokens", 4096);
                p.temperature       = entry.value("temperature", 0.7);
                if (!p.primary_model_id.empty()) {
                    profiles_[p.intent] = std::move(p);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

const ModelProfile& ModelRouter::select(const std::string& intent) const {
    auto it = profiles_.find(intent);
    const ModelProfile* profile = nullptr;
    std::string reason;

    if (it != profiles_.end()) {
        profile = &it->second;
        reason  = "exact_match";
    } else {
        // Fall back to the CONSULT profile for unknown intents.
        auto fb = profiles_.find("CONSULT");
        if (fb != profiles_.end()) {
            profile = &fb->second;
            reason  = "fallback_to_consult";
        } else {
            // Should never happen after construction, but be defensive.
            throw std::runtime_error(
                "ModelRouter: no profile for intent '" + intent +
                "' and no CONSULT fallback available");
        }
    }

    if (trace_cb_) {
        trace_cb_(nlohmann::json{
            {"event",          "model_selected"},
            {"requested_intent", intent},
            {"selected_model", profile->primary_model_id},
            {"fallback_model", profile->fallback_model_id},
            {"max_tokens",    profile->max_tokens},
            {"temperature",   profile->temperature},
            {"reason",        reason},
        });
    }

    return *profile;
}

const std::string& ModelRouter::primary_model(const std::string& intent) const {
    return select(intent).primary_model_id;
}

const std::string& ModelRouter::fallback_model(const std::string& intent) const {
    return select(intent).fallback_model_id;
}

int ModelRouter::max_tokens_for(const std::string& intent) const {
    return select(intent).max_tokens;
}

double ModelRouter::temperature_for(const std::string& intent) const {
    return select(intent).temperature;
}

// ---------------------------------------------------------------------------
// Trace callback
// ---------------------------------------------------------------------------

void ModelRouter::set_trace_callback(std::function<void(const nlohmann::json&)> cb) {
    trace_cb_ = std::move(cb);
}

}  // namespace mindbridge
