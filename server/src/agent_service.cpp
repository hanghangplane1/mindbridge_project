#include "agent_rpc/server/agent_service.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/message_converter.h"
#include "agent_rpc/common/serializer.h"
#include <grpcpp/grpcpp.h>
#include <sstream>
#include <iomanip>

namespace agent_rpc {
namespace server {

// AgentCommunicationServiceImpl 实现
AgentCommunicationServiceImpl::AgentCommunicationServiceImpl() 
    : cleanup_running_(false) {
    // 启动清理线程
    cleanup_running_ = true;
    cleanup_thread_ = std::thread([this]() {
        while (cleanup_running_) {
            // 使用较短的睡眠间隔，以便更快响应关闭请求
            for (int i = 0; i < 30 && cleanup_running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (cleanup_running_) {
                cleanupOfflineAgents();
            }
        }
    });
}

AgentCommunicationServiceImpl::~AgentCommunicationServiceImpl() {
    // 停止清理线程
    cleanup_running_ = false;
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
}

void AgentCommunicationServiceImpl::setMessageHandler(common::MessageHandler handler) {
    message_handler_ = handler;
}

void AgentCommunicationServiceImpl::setErrorHandler(common::ErrorHandler handler) {
    error_handler_ = handler;
}

void AgentCommunicationServiceImpl::setHealthCheckHandler(common::HealthCheckHandler handler) {
    health_check_handler_ = handler;
}

void AgentCommunicationServiceImpl::registerAgent(const std::string& agent_id, const common::ServiceEndpoint& endpoint) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_[agent_id] = endpoint;
    // Use emplace to construct MessageQueue in-place (avoids copy/move assignment)
    agent_message_queues_.try_emplace(agent_id);
    LOG_INFO("Agent registered: " + agent_id + " at " + endpoint.host + ":" + std::to_string(endpoint.port));
}

void AgentCommunicationServiceImpl::unregisterAgent(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_.erase(agent_id);
    agent_message_queues_.erase(agent_id);
    LOG_INFO("Agent unregistered: " + agent_id);
}

std::vector<common::ServiceEndpoint> AgentCommunicationServiceImpl::getAgents() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::vector<common::ServiceEndpoint> result;
    for (const auto& pair : agents_) {
        result.push_back(pair.second);
    }
    return result;
}

bool AgentCommunicationServiceImpl::sendMessageToAgent(const std::string& agent_id, const std::string& message) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agent_message_queues_.find(agent_id);
    if (it != agent_message_queues_.end()) {
        it->second.push(message);
        return true;
    }
    return false;
}

int AgentCommunicationServiceImpl::broadcastMessage(const std::string& message, 
                                                   const std::vector<std::string>& target_agents) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    int success_count = 0;
    
    if (target_agents.empty()) {
        // 广播给所有代理
        for (auto& pair : agent_message_queues_) {
            pair.second.push(message);
            success_count++;
        }
    } else {
        // 广播给指定代理
        for (const auto& agent_id : target_agents) {
            auto it = agent_message_queues_.find(agent_id);
            if (it != agent_message_queues_.end()) {
                it->second.push(message);
                success_count++;
            }
        }
    }
    
    return success_count;
}

std::string AgentCommunicationServiceImpl::generateMessageId() {
    return std::to_string(++message_id_counter_);
}

bool AgentCommunicationServiceImpl::isAgentOnline(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    return agents_.find(agent_id) != agents_.end();
}

void AgentCommunicationServiceImpl::updateAgentHeartbeat(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(agent_id);
    if (it != agents_.end()) {
        it->second.last_heartbeat = std::chrono::steady_clock::now();
    }
}

void AgentCommunicationServiceImpl::cleanupOfflineAgents() {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(60); // 60秒超时
    
    auto it = agents_.begin();
    while (it != agents_.end()) {
        if (now - it->second.last_heartbeat > timeout) {
            LOG_WARN("Agent offline, removing: " + it->first);
            agent_message_queues_.erase(it->first);
            it = agents_.erase(it);
        } else {
            ++it;
        }
    }
}

// HealthServiceImpl 实现
HealthServiceImpl::HealthServiceImpl() = default;

void HealthServiceImpl::setHealthCheckHandler(common::HealthCheckHandler handler) {
    health_check_handler_ = handler;
}

bool HealthServiceImpl::checkHealth() {
    if (health_check_handler_) {
        return health_check_handler_();
    }
    return true; // 默认健康
}

} // namespace server
} // namespace agent_rpc
