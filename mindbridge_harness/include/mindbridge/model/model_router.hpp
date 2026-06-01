#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>

namespace mindbridge {

/**
 * Intent-based model profile: maps a routing intent (CHAT / CONSULT / RISK)
 * to concrete model identifiers and generation parameters.
 */
struct ModelProfile {
    std::string intent;
    std::string primary_model_id;
    std::string fallback_model_id;
    int         max_tokens{4096};
    double      temperature{0.7};
};

/**
 * Stateless-by-design model selector.
 *
 * ModelRouter does NOT own any ModelClient instances -- it merely resolves
 * an intent string to a ModelProfile so that the caller (Orchestrator,
 * Counselor, etc.) can look up the corresponding ModelClient in its own
 * registry.
 *
 * Thread-safety: all const methods are safe to call concurrently.
 * reload() and set_trace_callback() must be externally synchronised.
 */
class ModelRouter {
public:
    ModelRouter();
    explicit ModelRouter(const nlohmann::json& config);

    /// Return the full profile for the given intent.
    /// Falls back to the CONSULT profile when the intent is unknown.
    const ModelProfile& select(const std::string& intent) const;

    /// Convenience accessors -- all delegate to select().
    const std::string& primary_model(const std::string& intent) const;
    const std::string& fallback_model(const std::string& intent) const;
    int                max_tokens_for(const std::string& intent) const;
    double             temperature_for(const std::string& intent) const;

    /// Hot-reload profiles from a JSON configuration.
    /// Existing profiles are replaced entirely.
    void reload(const nlohmann::json& config);

    /// Install an optional trace callback invoked on every model selection.
    void set_trace_callback(std::function<void(const nlohmann::json&)> cb);

private:
    void load_defaults();

    std::unordered_map<std::string, ModelProfile>           profiles_;
    std::function<void(const nlohmann::json&)>              trace_cb_;
};

}  // namespace mindbridge
