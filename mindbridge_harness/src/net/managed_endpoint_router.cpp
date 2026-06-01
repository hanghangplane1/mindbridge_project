#include "mindbridge/net/managed_endpoint_router.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace mindbridge {
namespace net {
namespace {

std::string health_url_for(const std::string& base_url) {
    if (!base_url.empty() && base_url.back() == '/') {
        return base_url.substr(0, base_url.size() - 1) + "/api/health";
    }
    return base_url + "/api/health";
}

long elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

}  // namespace

ManagedEndpointRouter::ManagedEndpointRouter(std::string role,
                                             std::string skill,
                                             std::vector<std::string> urls,
                                             EndpointRouterOptions options)
    : role_(std::move(role)), skill_(std::move(skill)), options_(std::move(options)) {
    router_.initialize(agent_rpc::orchestrator::RoutingStrategy::CONSISTENT_HASH);
    endpoints_.reserve(urls.size());
    for (std::size_t i = 0; i < urls.size(); ++i) {
        EndpointState endpoint;
        endpoint.id = role_ + "-" + std::to_string(i + 1);
        endpoint.role = role_;
        endpoint.skill = skill_;
        endpoint.url = urls[i];
        endpoints_.push_back(endpoint);
    }
    rebuild_router_locked();
}

bool ManagedEndpointRouter::circuit_open(const EndpointState& endpoint) {
    return endpoint.circuit_open_until > std::chrono::steady_clock::now();
}

void ManagedEndpointRouter::rebuild_router_locked() {
    std::vector<agent_rpc::orchestrator::AgentInfo> agents;
    for (const auto& endpoint : endpoints_) {
        if (!endpoint.healthy || circuit_open(endpoint)) {
            continue;
        }
        agent_rpc::orchestrator::AgentInfo agent;
        agent.id = endpoint.id;
        agent.name = endpoint.id;
        agent.url = endpoint.url;
        agent.skills = {skill_};
        agent.tags = {role_, skill_};
        agent.is_healthy = true;
        agent.description = "managed " + role_ + " endpoint";
        agent.version = "1.0.0";
        agents.push_back(agent);
    }
    if (agents.empty()) {
        for (const auto& endpoint : endpoints_) {
            agent_rpc::orchestrator::AgentInfo agent;
            agent.id = endpoint.id;
            agent.name = endpoint.id;
            agent.url = endpoint.url;
            agent.skills = {skill_};
            agent.tags = {role_, skill_};
            agent.is_healthy = true;
            agent.description = "managed " + role_ + " endpoint fallback";
            agent.version = "1.0.0";
            agents.push_back(agent);
        }
    }
    router_.updateAgentList(agents);
}

bool ManagedEndpointRouter::probe_endpoint_locked(EndpointState& endpoint) {
    const auto start = std::chrono::steady_clock::now();
    try {
        HttpClientOptions options = options_.client_options;
        options.timeout_sec = options_.health_timeout_sec;
        get(health_url_for(endpoint.url), options);
        endpoint.last_latency_ms = elapsed_ms(start);
        endpoint.last_error.clear();
        endpoint.consecutive_failures = 0;
        ++endpoint.consecutive_successes;
        if (!endpoint.healthy && endpoint.consecutive_successes >= options_.health_recover_threshold) {
            endpoint.healthy = true;
            endpoint.circuit_open_until = {};
        }
        return true;
    } catch (const std::exception& e) {
        endpoint.last_latency_ms = elapsed_ms(start);
        endpoint.last_error = e.what();
        endpoint.consecutive_successes = 0;
        ++endpoint.consecutive_failures;
        if (endpoint.consecutive_failures >= options_.health_fail_threshold) {
            endpoint.healthy = false;
            endpoint.circuit_open_until =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.circuit_open_ms);
        }
        return false;
    }
}

void ManagedEndpointRouter::refresh_health_locked() {
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;
    for (auto& endpoint : endpoints_) {
        const bool was_available = endpoint.healthy && !circuit_open(endpoint);
        const bool due = endpoint.last_probe == std::chrono::steady_clock::time_point{} ||
                         now - endpoint.last_probe >= std::chrono::milliseconds(options_.health_interval_ms) ||
                         circuit_open(endpoint);
        if (!due) {
            continue;
        }
        endpoint.last_probe = now;
        if (circuit_open(endpoint)) {
            continue;
        }
        probe_endpoint_locked(endpoint);
        const bool available = endpoint.healthy && !circuit_open(endpoint);
        changed = changed || was_available != available;
    }
    if (changed) {
        rebuild_router_locked();
    }
}

std::vector<std::string> ManagedEndpointRouter::candidate_urls_locked(const std::string& question,
                                                                      const std::string& routing_key) {
    refresh_health_locked();
    std::vector<std::string> out;
    auto picked = router_.selectAgent(question, {skill_}, routing_key);
    if (picked.has_value()) {
        out.push_back(picked->url);
    }
    for (const auto& endpoint : endpoints_) {
        if (!endpoint.healthy || circuit_open(endpoint)) {
            continue;
        }
        if (std::find(out.begin(), out.end(), endpoint.url) == out.end()) {
            out.push_back(endpoint.url);
        }
    }
    if (out.empty()) {
        for (const auto& endpoint : endpoints_) {
            out.push_back(endpoint.url);
        }
    }
    return out;
}

EndpointState* ManagedEndpointRouter::find_by_url_locked(const std::string& url) {
    for (auto& endpoint : endpoints_) {
        if (endpoint.url == url) {
            return &endpoint;
        }
    }
    return nullptr;
}

std::string ManagedEndpointRouter::select(const std::string& question, const std::string& routing_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto candidates = candidate_urls_locked(question, routing_key);
    return candidates.empty() ? "" : candidates.front();
}

std::string ManagedEndpointRouter::post_json(const std::string& question,
                                             const std::string& routing_key,
                                             const std::string& body,
                                             bool allow_failover,
                                             long timeout_sec) {
    std::vector<std::string> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        candidates = candidate_urls_locked(question, routing_key);
    }
    if (candidates.empty()) {
        throw std::runtime_error("no endpoint configured for role: " + role_);
    }

    std::string last_error;
    const std::size_t max_attempts = allow_failover ? candidates.size() : 1;
    for (std::size_t i = 0; i < max_attempts; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const std::string& url = candidates[i];
        try {
            HttpClientOptions options = options_.client_options;
            options.timeout_sec = timeout_sec;
            auto response = net::post_json(url, body, options);
            mark_success(url, elapsed_ms(start));
            return response.body;
        } catch (const std::exception& e) {
            last_error = e.what();
            mark_failure(url, last_error);
        }
    }
    throw std::runtime_error(last_error.empty() ? "endpoint request failed" : last_error);
}

void ManagedEndpointRouter::post_sse(const std::string& question,
                                     const std::string& routing_key,
                                     const std::string& body,
                                     const std::function<bool(const std::string&)>& on_event,
                                     long timeout_sec) {
    const std::string url = select(question, routing_key);
    if (url.empty()) {
        throw std::runtime_error("no endpoint configured for role: " + role_);
    }
    const auto start = std::chrono::steady_clock::now();
    try {
        HttpClientOptions options = options_.client_options;
        options.timeout_sec = timeout_sec;
        net::post_sse(url, body, on_event, options);
        mark_success(url, elapsed_ms(start));
    } catch (const std::exception& e) {
        mark_failure(url, e.what());
        throw;
    }
}

void ManagedEndpointRouter::mark_success(const std::string& url, long latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* endpoint = find_by_url_locked(url);
    if (!endpoint) {
        return;
    }
    endpoint->last_latency_ms = latency_ms;
    endpoint->last_error.clear();
    endpoint->consecutive_failures = 0;
    endpoint->consecutive_successes++;
    endpoint->healthy = true;
    endpoint->circuit_open_until = {};
    rebuild_router_locked();
}

void ManagedEndpointRouter::mark_failure(const std::string& url, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* endpoint = find_by_url_locked(url);
    if (!endpoint) {
        return;
    }
    endpoint->last_error = error;
    endpoint->consecutive_successes = 0;
    endpoint->consecutive_failures++;
    if (endpoint->consecutive_failures >= options_.health_fail_threshold) {
        endpoint->healthy = false;
        endpoint->circuit_open_until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.circuit_open_ms);
    }
    rebuild_router_locked();
}

std::vector<std::string> ManagedEndpointRouter::urls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(endpoints_.size());
    for (const auto& endpoint : endpoints_) {
        out.push_back(endpoint.url);
    }
    return out;
}

nlohmann::json ManagedEndpointRouter::health_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    refresh_health_locked();
    nlohmann::json endpoints = nlohmann::json::array();
    const auto now = std::chrono::steady_clock::now();
    for (const auto& endpoint : endpoints_) {
        const bool open = circuit_open(endpoint);
        endpoints.push_back({
            {"id", endpoint.id},
            {"url", endpoint.url},
            {"healthy", endpoint.healthy && !open},
            {"circuit_open", open},
            {"failure_count", endpoint.consecutive_failures},
            {"last_latency_ms", endpoint.last_latency_ms},
            {"last_error", endpoint.last_error},
            {"circuit_open_remaining_ms",
             open ? std::chrono::duration_cast<std::chrono::milliseconds>(
                        endpoint.circuit_open_until - now)
                        .count()
                  : 0},
        });
    }
    return {{"role", role_},
            {"skill", skill_},
            {"health_interval_ms", options_.health_interval_ms},
            {"fail_threshold", options_.health_fail_threshold},
            {"circuit_open_ms", options_.circuit_open_ms},
            {"endpoints", endpoints}};
}

}  // namespace net
}  // namespace mindbridge
