#pragma once

#include "agent_rpc/common/types.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>

namespace agent_rpc {
namespace server {

// 服务端实现类
class AgentCommunicationServiceImpl {
public:
    AgentCommunicationServiceImpl();
    ~AgentCommunicationServiceImpl();
    
    // 设置消息处理器
    void setMessageHandler(common::MessageHandler handler);
    
    // 设置错误处理器
    void setErrorHandler(common::ErrorHandler handler);
    
    // 设置健康检查处理器
    void setHealthCheckHandler(common::HealthCheckHandler handler);
    
    // 注册代理
    void registerAgent(const std::string& agent_id, const common::ServiceEndpoint& endpoint);
    
    // 注销代理
    void unregisterAgent(const std::string& agent_id);
    
    // 获取代理列表
    std::vector<common::ServiceEndpoint> getAgents() const;
    
    // 发送消息给指定代理
    bool sendMessageToAgent(const std::string& agent_id, const std::string& message);
    
    // 广播消息
    int broadcastMessage(const std::string& message, 
                        const std::vector<std::string>& target_agents = {});
    


private:
    // 内部方法
    std::string generateMessageId();
    bool isAgentOnline(const std::string& agent_id);
    void updateAgentHeartbeat(const std::string& agent_id);
    void cleanupOfflineAgents();
    
    // 成员变量
    mutable std::mutex agents_mutex_;
    std::map<std::string, common::ServiceEndpoint> agents_;
    std::map<std::string, common::MessageQueue<std::string>> agent_message_queues_;
    
    common::MessageHandler message_handler_;
    common::ErrorHandler error_handler_;
    common::HealthCheckHandler health_check_handler_;
    
    std::atomic<int> message_id_counter_{0};
    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};
};

// 健康检查服务实现
class HealthServiceImpl {
public:
    HealthServiceImpl();
    ~HealthServiceImpl() = default;
    
    void setHealthCheckHandler(common::HealthCheckHandler handler);
    
    bool checkHealth();

private:
    common::HealthCheckHandler health_check_handler_;
};

} // namespace server
} // namespace agent_rpc
