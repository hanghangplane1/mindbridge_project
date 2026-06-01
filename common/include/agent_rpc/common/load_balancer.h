#pragma once

#include "types.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <random>
#include <algorithm>

namespace agent_rpc {
namespace common {

// 负载均衡策略枚举
enum class LoadBalanceStrategy {
    ROUND_ROBIN,        // 轮询
    RANDOM,             // 随机
    LEAST_CONNECTIONS,  // 最少连接
    WEIGHTED_ROUND_ROBIN, // 加权轮询
    CONSISTENT_HASH,    // 一致性哈希
    LEAST_RESPONSE_TIME // 最少响应时间
};

// 负载均衡器接口
class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;
    
    // 选择服务端点
    virtual ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) = 0;
    
    // 更新服务端点列表
    virtual void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) = 0;
    
    // 标记服务端点状态
    virtual void markEndpointStatus(const std::string& endpoint_id, bool healthy) = 0;
    
    // 获取策略名称
    virtual std::string getStrategyName() const = 0;
};

// 轮询负载均衡器
class RoundRobinLoadBalancer : public LoadBalancer {
public:
    RoundRobinLoadBalancer();
    ~RoundRobinLoadBalancer() = default;
    
    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "RoundRobin"; }

private:
    std::atomic<size_t> current_index_{0};
    mutable std::mutex endpoints_mutex_;
    std::vector<ServiceEndpoint> healthy_endpoints_;
};

// 随机负载均衡器
class RandomLoadBalancer : public LoadBalancer {
public:
    RandomLoadBalancer();
    ~RandomLoadBalancer() = default;
    
    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "Random"; }

private:
    mutable std::mutex endpoints_mutex_;
    std::vector<ServiceEndpoint> healthy_endpoints_;
    std::random_device rd_;
    mutable std::mt19937 gen_;
};

// 负载均衡器工厂
class LoadBalancerFactory {
public:
    static std::unique_ptr<LoadBalancer> createLoadBalancer(LoadBalanceStrategy strategy);
    static std::vector<std::string> getAvailableStrategies();
};

} // namespace common
} // namespace agent_rpc
