#include "agent_rpc/circuit_breaker.h"
#include "agent_rpc/logger.h"
#include <algorithm>
#include <cmath>

namespace agent_rpc {

// CircuitBreaker 实现
CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& config) 
    : config_(config)
    , state_(CircuitState::CLOSED)
    , last_state_change_(std::chrono::steady_clock::now()) {
}

template<typename Func>
auto CircuitBreaker::execute(Func&& func) -> decltype(func()) {
    if (!isRequestAllowed()) {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.rejected_requests++;
        }
        throw std::runtime_error("Circuit breaker is open");
    }
    
    try {
        auto result = func();
        recordSuccess();
        return result;
    } catch (...) {
        recordFailure();
        throw;
    }
}

void CircuitBreaker::recordSuccess() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.total_requests++;
    stats_.successful_requests++;
    stats_.last_success_time = std::chrono::steady_clock::now();
    
    updateFailureRate();
    
    // 如果当前是半开状态且成功次数达到阈值，则切换到关闭状态
    if (state_ == CircuitState::HALF_OPEN && 
        stats_.successful_requests >= config_.success_threshold) {
        transitionToClosed();
    }
}

void CircuitBreaker::recordFailure() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.total_requests++;
    stats_.failed_requests++;
    stats_.last_failure_time = std::chrono::steady_clock::now();
    
    updateFailureRate();
    
    // 如果失败次数达到阈值，切换到开启状态
    if (stats_.failed_requests >= config_.failure_threshold) {
        transitionToOpen();
    }
}

bool CircuitBreaker::isRequestAllowed() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    switch (state_) {
        case CircuitState::CLOSED:
            return true;
            
        case CircuitState::OPEN:
            // 检查是否可以尝试重置
            if (shouldAttemptReset()) {
                transitionToHalfOpen();
                return true;
            }
            return false;
            
        case CircuitState::HALF_OPEN:
            // 半开状态允许少量请求通过
            return stats_.total_requests < config_.success_threshold;
            
        default:
            return false;
    }
}

CircuitBreakerStats CircuitBreaker::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void CircuitBreaker::reset() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_ = CircuitBreakerStats{};
    state_ = CircuitState::CLOSED;
    last_state_change_ = std::chrono::steady_clock::now();
    
    LOG_INFO("Circuit breaker reset");
}

void CircuitBreaker::updateConfig(const CircuitBreakerConfig& config) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    config_ = config;
}

void CircuitBreaker::transitionToOpen() {
    if (state_ != CircuitState::OPEN) {
        state_ = CircuitState::OPEN;
        last_state_change_ = std::chrono::steady_clock::now();
        LOG_WARN("Circuit breaker opened due to failures");
    }
}

void CircuitBreaker::transitionToHalfOpen() {
    if (state_ != CircuitState::HALF_OPEN) {
        state_ = CircuitState::HALF_OPEN;
        last_state_change_ = std::chrono::steady_clock::now();
        
        // 重置成功计数
        stats_.successful_requests = 0;
        stats_.total_requests = 0;
        
        LOG_INFO("Circuit breaker half-opened for testing");
    }
}

void CircuitBreaker::transitionToClosed() {
    if (state_ != CircuitState::CLOSED) {
        state_ = CircuitState::CLOSED;
        last_state_change_ = std::chrono::steady_clock::now();
        
        // 重置统计信息
        stats_ = CircuitBreakerStats{};
        
        LOG_INFO("Circuit breaker closed - service recovered");
    }
}

void CircuitBreaker::updateFailureRate() {
    if (stats_.total_requests >= config_.min_request_count) {
        stats_.current_failure_rate = static_cast<double>(stats_.failed_requests) / stats_.total_requests;
        
        // 如果失败率超过阈值，切换到开启状态
        if (stats_.current_failure_rate >= config_.failure_rate_threshold) {
            transitionToOpen();
        }
    }
}

bool CircuitBreaker::shouldAttemptReset() {
    auto now = std::chrono::steady_clock::now();
    auto time_since_open = now - last_state_change_;
    
    return time_since_open >= config_.timeout;
}

// CircuitBreakerManager 实现
CircuitBreakerManager& CircuitBreakerManager::getInstance() {
    static CircuitBreakerManager instance;
    return instance;
}

std::shared_ptr<CircuitBreaker> CircuitBreakerManager::getCircuitBreaker(const std::string& service_name) {
    std::lock_guard<std::mutex> lock(circuit_breakers_mutex_);
    
    auto it = circuit_breakers_.find(service_name);
    if (it != circuit_breakers_.end()) {
        return it->second;
    }
    
    // 创建新的熔断器
    auto circuit_breaker = std::make_shared<CircuitBreaker>();
    circuit_breakers_[service_name] = circuit_breaker;
    
    LOG_INFO("Created circuit breaker for service: " + service_name);
    return circuit_breaker;
}

void CircuitBreakerManager::removeCircuitBreaker(const std::string& service_name) {
    std::lock_guard<std::mutex> lock(circuit_breakers_mutex_);
    
    auto it = circuit_breakers_.find(service_name);
    if (it != circuit_breakers_.end()) {
        circuit_breakers_.erase(it);
        LOG_INFO("Removed circuit breaker for service: " + service_name);
    }
}

std::map<std::string, CircuitState> CircuitBreakerManager::getAllStates() const {
    std::lock_guard<std::mutex> lock(circuit_breakers_mutex_);
    
    std::map<std::string, CircuitState> states;
    for (const auto& pair : circuit_breakers_) {
        states[pair.first] = pair.second->getState();
    }
    
    return states;
}

void CircuitBreakerManager::resetAll() {
    std::lock_guard<std::mutex> lock(circuit_breakers_mutex_);
    
    for (auto& pair : circuit_breakers_) {
        pair.second->reset();
    }
    
    LOG_INFO("Reset all circuit breakers");
}

void CircuitBreakerManager::updateConfig(const std::string& service_name, const CircuitBreakerConfig& config) {
    std::lock_guard<std::mutex> lock(circuit_breakers_mutex_);
    
    auto it = circuit_breakers_.find(service_name);
    if (it != circuit_breakers_.end()) {
        it->second->updateConfig(config);
        LOG_INFO("Updated circuit breaker config for service: " + service_name);
    }
}

} // namespace agent_rpc
