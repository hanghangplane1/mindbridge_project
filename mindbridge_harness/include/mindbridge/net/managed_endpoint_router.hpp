#pragma once

#include "agent_rpc/orchestrator/agent_router.h"
#include "mindbridge/net/http_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace mindbridge {
namespace net {

struct EndpointRouterOptions {
    int health_interval_ms{2000};
    int health_fail_threshold{2};
    int health_recover_threshold{1};
    int circuit_open_ms{10000};
    long health_timeout_sec{2};
    HttpClientOptions client_options;
};

struct EndpointState {
    std::string id;
    std::string role;
    std::string skill;
    std::string url;
    bool healthy{true};
    int consecutive_failures{0};
    int consecutive_successes{0};
    long last_latency_ms{0};
    std::string last_error;
    std::chrono::steady_clock::time_point last_probe{};
    std::chrono::steady_clock::time_point circuit_open_until{};
};

class ManagedEndpointRouter {
public:
    ManagedEndpointRouter(std::string role,
                          std::string skill,
                          std::vector<std::string> urls,
                          EndpointRouterOptions options = {});

    std::string select(const std::string& question, const std::string& routing_key);
    std::string post_json(const std::string& question,
                          const std::string& routing_key,
                          const std::string& body,
                          bool allow_failover = true,
                          long timeout_sec = 120);
    void post_sse(const std::string& question,
                  const std::string& routing_key,
                  const std::string& body,
                  const std::function<bool(const std::string&)>& on_event,
                  long timeout_sec = 120);

    void mark_success(const std::string& url, long latency_ms = 0);
    void mark_failure(const std::string& url, const std::string& error);

    std::vector<std::string> urls() const;
    nlohmann::json health_json();

private:
    void refresh_health_locked();
    void rebuild_router_locked();
    bool probe_endpoint_locked(EndpointState& endpoint);
    std::vector<std::string> candidate_urls_locked(const std::string& question,
                                                   const std::string& routing_key);
    EndpointState* find_by_url_locked(const std::string& url);
    static bool circuit_open(const EndpointState& endpoint);

    std::string role_;
    std::string skill_;
    EndpointRouterOptions options_;
    mutable std::mutex mutex_;
    std::vector<EndpointState> endpoints_;
    agent_rpc::orchestrator::AgentRouter router_;
};

}  // namespace net
}  // namespace mindbridge
