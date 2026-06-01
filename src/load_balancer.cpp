#include "agent_rpc/load_balancer.h"
#include "agent_rpc/logger.h"
#include <algorithm>
#include <random>

namespace agent_rpc {

// RoundRobinLoadBalancer 实现
RoundRobinLoadBalancer::RoundRobinLoadBalancer() = default;

ServiceEndpoint RoundRobinLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
    // 过滤健康端点
    std::vector<ServiceEndpoint> healthy_endpoints;
    for (const auto& endpoint : endpoints) {
        if (endpoint.is_healthy) {
            healthy_endpoints.push_back(endpoint);
        }
    }
    
    if (healthy_endpoints.empty()) {
        throw std::runtime_error("No healthy endpoints available");
    }
    
    // 轮询选择
    size_t index = current_index_.fetch_add(1) % healthy_endpoints.size();
    return healthy_endpoints[index];
}

void RoundRobinLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    healthy_endpoints_ = endpoints;
    current_index_ = 0;
}

void RoundRobinLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (auto& endpoint : healthy_endpoints_) {
        if (endpoint.host + ":" + std::to_string(endpoint.port) == endpoint_id) {
            endpoint.is_healthy = healthy;
            break;
        }
    }
}

// RandomLoadBalancer 实现
RandomLoadBalancer::RandomLoadBalancer() : gen_(rd_()) {}

ServiceEndpoint RandomLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
    // 过滤健康端点
    std::vector<ServiceEndpoint> healthy_endpoints;
    for (const auto& endpoint : endpoints) {
        if (endpoint.is_healthy) {
            healthy_endpoints.push_back(endpoint);
        }
    }
    
    if (healthy_endpoints.empty()) {
        throw std::runtime_error("No healthy endpoints available");
    }
    
    // 随机选择
    std::uniform_int_distribution<> dis(0, healthy_endpoints.size() - 1);
    return healthy_endpoints[dis(gen_)];
}

void RandomLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    healthy_endpoints_ = endpoints;
}

void RandomLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (auto& endpoint : healthy_endpoints_) {
        if (endpoint.host + ":" + std::to_string(endpoint.port) == endpoint_id) {
            endpoint.is_healthy = healthy;
            break;
        }
    }
}

// LeastConnectionsLoadBalancer 实现
LeastConnectionsLoadBalancer::LeastConnectionsLoadBalancer() = default;

ServiceEndpoint LeastConnectionsLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
    ServiceEndpoint* best_endpoint = nullptr;
    int min_connections = INT_MAX;
    
    for (const auto& endpoint : endpoints) {
        if (!endpoint.is_healthy) continue;
        
        std::string endpoint_id = endpoint.host + ":" + std::to_string(endpoint.port);
        int connections = connection_counts_[endpoint_id];
        
        if (connections < min_connections) {
            min_connections = connections;
            best_endpoint = const_cast<ServiceEndpoint*>(&endpoint);
        }
    }
    
    if (!best_endpoint) {
        throw std::runtime_error("No healthy endpoints available");
    }
    
    // 增加连接计数
    std::string endpoint_id = best_endpoint->host + ":" + std::to_string(best_endpoint->port);
    connection_counts_[endpoint_id]++;
    
    return *best_endpoint;
}

void LeastConnectionsLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
    // 清理不存在的端点连接计数
    std::set<std::string> current_endpoints;
    for (const auto& endpoint : endpoints) {
        current_endpoints.insert(endpoint.host + ":" + std::to_string(endpoint.port));
    }
    
    auto it = connection_counts_.begin();
    while (it != connection_counts_.end()) {
        if (current_endpoints.find(it->first) == current_endpoints.end()) {
            it = connection_counts_.erase(it);
        } else {
            ++it;
        }
    }
    
    // 更新端点信息
    for (const auto& endpoint : endpoints) {
        endpoints_[endpoint.host + ":" + std::to_string(endpoint.port)] = endpoint;
    }
}

void LeastConnectionsLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto it = endpoints_.find(endpoint_id);
    if (it != endpoints_.end()) {
        it->second.is_healthy = healthy;
    }
}

void LeastConnectionsLoadBalancer::incrementConnections(const std::string& endpoint_id) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    connection_counts_[endpoint_id]++;
}

void LeastConnectionsLoadBalancer::decrementConnections(const std::string& endpoint_id) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto it = connection_counts_.find(endpoint_id);
    if (it != connection_counts_.end() && it->second > 0) {
        it->second--;
    }
}

// WeightedRoundRobinLoadBalancer 实现
WeightedRoundRobinLoadBalancer::WeightedRoundRobinLoadBalancer() = default;

ServiceEndpoint WeightedRoundRobinLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
    if (weighted_endpoints_.empty()) {
        throw std::runtime_error("No weighted endpoints available");
    }
    
    // 找到当前权重最大的端点
    WeightedEndpoint* best_endpoint = nullptr;
    int max_current_weight = 0;
    
    for (auto& endpoint : weighted_endpoints_) {
        if (!endpoint.endpoint.is_healthy) continue;
        
        endpoint.current_weight += endpoint.weight;
        
        if (endpoint.current_weight > max_current_weight) {
            max_current_weight = endpoint.current_weight;
            best_endpoint = &endpoint;
        }
    }
    
    if (!best_endpoint) {
        throw std::runtime_error("No healthy endpoints available");
    }
    
    // 减少选中端点的当前权重
    best_endpoint->current_weight -= best_endpoint->weight;
    
    return best_endpoint->endpoint;
}

void WeightedRoundRobinLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
    weighted_endpoints_.clear();
    for (const auto& endpoint : endpoints) {
        WeightedEndpoint weighted_endpoint;
        weighted_endpoint.endpoint = endpoint;
        weighted_endpoint.weight = 1; // 默认权重为1
        weighted_endpoint.current_weight = 0;
        
        // 从元数据中获取权重
        auto it = endpoint.metadata.find("weight");
        if (it != endpoint.metadata.end()) {
            try {
                weighted_endpoint.weight = std::stoi(it->second);
            } catch (...) {
                weighted_endpoint.weight = 1;
            }
        }
        
        weighted_endpoints_.push_back(weighted_endpoint);
    }
    
    current_index_ = 0;
}

void WeightedRoundRobinLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (auto& endpoint : weighted_endpoints_) {
        if (endpoint.endpoint.host + ":" + std::to_string(endpoint.endpoint.port) == endpoint_id) {
            endpoint.endpoint.is_healthy = healthy;
            break;
        }
    }
}

// ConsistentHashLoadBalancer 实现
ConsistentHashLoadBalancer::ConsistentHashLoadBalancer(int virtual_nodes) : virtual_nodes_(virtual_nodes) {}

ServiceEndpoint ConsistentHashLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    
    std::lock_guard<std::mutex> lock(ring_mutex_);
    
    if (hash_ring_.empty()) {
        throw std::runtime_error("Hash ring is empty");
    }
    
    // 生成随机键进行哈希
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 1000000);
    std::string key = std::to_string(dis(gen));
    
    uint32_t hash_value = hash(key);
    return findEndpoint(hash_value);
}

ServiceEndpoint ConsistentHashLoadBalancer::selectEndpointByKey(const std::string& key, 
                                                               const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    
    std::lock_guard<std::mutex> lock(ring_mutex_);
    
    if (hash_ring_.empty()) {
        throw std::runtime_error("Hash ring is empty");
    }
    
    uint32_t hash_value = hash(key);
    return findEndpoint(hash_value);
}

void ConsistentHashLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    
    endpoints_.clear();
    for (const auto& endpoint : endpoints) {
        endpoints_[endpoint.host + ":" + std::to_string(endpoint.port)] = endpoint;
    }
    
    buildHashRing();
}

void ConsistentHashLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    auto it = endpoints_.find(endpoint_id);
    if (it != endpoints_.end()) {
        it->second.is_healthy = healthy;
        buildHashRing(); // 重建哈希环
    }
}

void ConsistentHashLoadBalancer::buildHashRing() {
    hash_ring_.clear();
    
    for (const auto& pair : endpoints_) {
        if (!pair.second.is_healthy) continue;
        
        // 为每个端点创建虚拟节点
        for (int i = 0; i < virtual_nodes_; ++i) {
            std::string virtual_key = pair.first + "#" + std::to_string(i);
            uint32_t hash_value = hash(virtual_key);
            
            HashNode node;
            node.key = virtual_key;
            node.endpoint = pair.second;
            node.hash = hash_value;
            
            hash_ring_.push_back(node);
        }
    }
    
    // 按哈希值排序
    std::sort(hash_ring_.begin(), hash_ring_.end(), 
              [](const HashNode& a, const HashNode& b) {
                  return a.hash < b.hash;
              });
}

uint32_t ConsistentHashLoadBalancer::hash(const std::string& key) {
    // 简单的哈希函数，实际应用中可以使用更好的哈希算法
    uint32_t hash = 0;
    for (char c : key) {
        hash = hash * 31 + c;
    }
    return hash;
}

ServiceEndpoint ConsistentHashLoadBalancer::findEndpoint(uint32_t hash_value) {
    if (hash_ring_.empty()) {
        throw std::runtime_error("Hash ring is empty");
    }
    
    // 二分查找第一个哈希值大于等于目标值的节点
    auto it = std::lower_bound(hash_ring_.begin(), hash_ring_.end(), hash_value,
                              [](const HashNode& node, uint32_t value) {
                                  return node.hash < value;
                              });
    
    // 如果没找到，则使用第一个节点（环形结构）
    if (it == hash_ring_.end()) {
        it = hash_ring_.begin();
    }
    
    return it->endpoint;
}

// LeastResponseTimeLoadBalancer 实现
LeastResponseTimeLoadBalancer::LeastResponseTimeLoadBalancer() = default;

ServiceEndpoint LeastResponseTimeLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    ServiceEndpoint* best_endpoint = nullptr;
    std::chrono::milliseconds min_response_time = std::chrono::milliseconds::max();
    
    for (const auto& endpoint : endpoints) {
        if (!endpoint.is_healthy) continue;
        
        std::string endpoint_id = endpoint.host + ":" + std::to_string(endpoint.port);
        auto it = endpoint_stats_.find(endpoint_id);
        
        if (it == endpoint_stats_.end()) {
            // 新端点，使用默认响应时间
            if (best_endpoint == nullptr) {
                best_endpoint = const_cast<ServiceEndpoint*>(&endpoint);
            }
        } else {
            auto response_time = calculateAverageResponseTime(endpoint_id);
            if (response_time < min_response_time) {
                min_response_time = response_time;
                best_endpoint = const_cast<ServiceEndpoint*>(&endpoint);
            }
        }
    }
    
    if (!best_endpoint) {
        throw std::runtime_error("No healthy endpoints available");
    }
    
    return *best_endpoint;
}

void LeastResponseTimeLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // 清理不存在的端点统计
    std::set<std::string> current_endpoints;
    for (const auto& endpoint : endpoints) {
        current_endpoints.insert(endpoint.host + ":" + std::to_string(endpoint.port));
    }
    
    auto it = endpoint_stats_.begin();
    while (it != endpoint_stats_.end()) {
        if (current_endpoints.find(it->first) == current_endpoints.end()) {
            it = endpoint_stats_.erase(it);
        } else {
            ++it;
        }
    }
}

void LeastResponseTimeLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto it = endpoint_stats_.find(endpoint_id);
    if (it != endpoint_stats_.end()) {
        it->second.endpoint.is_healthy = healthy;
    }
}

void LeastResponseTimeLoadBalancer::updateResponseTime(const std::string& endpoint_id, 
                                                      std::chrono::milliseconds response_time) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto& stats = endpoint_stats_[endpoint_id];
    stats.request_count++;
    stats.last_update = std::chrono::steady_clock::now();
    
    // 计算移动平均
    if (stats.avg_response_time.count() == 0) {
        stats.avg_response_time = response_time;
    } else {
        stats.avg_response_time = std::chrono::milliseconds(
            (stats.avg_response_time.count() * 0.8) + (response_time.count() * 0.2)
        );
    }
}

std::chrono::milliseconds LeastResponseTimeLoadBalancer::calculateAverageResponseTime(const std::string& endpoint_id) {
    auto it = endpoint_stats_.find(endpoint_id);
    if (it == endpoint_stats_.end()) {
        return std::chrono::milliseconds(1000); // 默认1秒
    }
    
    return it->second.avg_response_time;
}

// LoadBalancerFactory 实现
std::unique_ptr<LoadBalancer> LoadBalancerFactory::createLoadBalancer(LoadBalanceStrategy strategy) {
    switch (strategy) {
        case LoadBalanceStrategy::ROUND_ROBIN:
            return std::make_unique<RoundRobinLoadBalancer>();
        case LoadBalanceStrategy::RANDOM:
            return std::make_unique<RandomLoadBalancer>();
        case LoadBalanceStrategy::LEAST_CONNECTIONS:
            return std::make_unique<LeastConnectionsLoadBalancer>();
        case LoadBalanceStrategy::WEIGHTED_ROUND_ROBIN:
            return std::make_unique<WeightedRoundRobinLoadBalancer>();
        case LoadBalanceStrategy::CONSISTENT_HASH:
            return std::make_unique<ConsistentHashLoadBalancer>();
        case LoadBalanceStrategy::LEAST_RESPONSE_TIME:
            return std::make_unique<LeastResponseTimeLoadBalancer>();
        default:
            return std::make_unique<RoundRobinLoadBalancer>();
    }
}

std::vector<std::string> LoadBalancerFactory::getAvailableStrategies() {
    return {
        "RoundRobin",
        "Random", 
        "LeastConnections",
        "WeightedRoundRobin",
        "ConsistentHash",
        "LeastResponseTime"
    };
}

} // namespace agent_rpc
