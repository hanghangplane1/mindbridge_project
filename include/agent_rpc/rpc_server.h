#pragma once

#include "rpc_framework.h"
#include "proto/agent_service.grpc.pb.h"

namespace agent_rpc {

// 服务端实现类
class AgentCommunicationServiceImpl final : public agent_communication::AgentCommunicationService::Service {
public:
    AgentCommunicationServiceImpl();
    ~AgentCommunicationServiceImpl() = default;
    
    // 设置消息处理器
    void setMessageHandler(MessageHandler handler);
    
    // 设置错误处理器
    void setErrorHandler(ErrorHandler handler);
    
    // 设置健康检查处理器
    void setHealthCheckHandler(HealthCheckHandler handler);
    
    // 注册代理
    void registerAgent(const std::string& agent_id, const ServiceEndpoint& endpoint);
    
    // 注销代理
    void unregisterAgent(const std::string& agent_id);
    
    // 获取代理列表
    std::vector<ServiceEndpoint> getAgents() const;
    
    // 发送消息给指定代理
    bool sendMessageToAgent(const std::string& agent_id, const agent_communication::Message& message);
    
    // 广播消息
    int broadcastMessage(const agent_communication::Message& message, 
                        const std::vector<std::string>& target_agents = {});

    // gRPC服务方法实现
    grpc::Status SendMessage(grpc::ServerContext* context,
                           const agent_communication::SendMessageRequest* request,
                           agent_communication::SendMessageResponse* response) override;
    
    grpc::Status ReceiveMessage(grpc::ServerContext* context,
                              const agent_communication::ReceiveMessageRequest* request,
                              agent_communication::ReceiveMessageResponse* response) override;
    
    grpc::Status BroadcastMessage(grpc::ServerContext* context,
                                const agent_communication::BroadcastMessageRequest* request,
                                agent_communication::BroadcastMessageResponse* response) override;
    
    grpc::Status GetAgents(grpc::ServerContext* context,
                         const agent_communication::GetAgentsRequest* request,
                         agent_communication::GetAgentsResponse* response) override;
    
    grpc::Status RegisterAgent(grpc::ServerContext* context,
                             const agent_communication::RegisterAgentRequest* request,
                             agent_communication::RegisterAgentResponse* response) override;
    
    grpc::Status UnregisterAgent(grpc::ServerContext* context,
                                const agent_communication::UnregisterAgentRequest* request,
                                agent_communication::UnregisterAgentResponse* response) override;
    
    grpc::Status Heartbeat(grpc::ServerContext* context,
                         const agent_communication::HeartbeatRequest* request,
                         agent_communication::HeartbeatResponse* response) override;
    
    grpc::Status ListenMessages(grpc::ServerContext* context,
                              const agent_communication::ReceiveMessageRequest* request,
                              grpc::ServerWriter<agent_communication::Message>* writer) override;
    
    grpc::Status BatchSendMessages(grpc::ServerContext* context,
                                 grpc::ServerReader<agent_communication::SendMessageRequest>* reader,
                                 agent_communication::SendMessageResponse* response) override;
    
    grpc::Status RealTimeCommunication(grpc::ServerContext* context,
                                     grpc::ServerReaderWriter<agent_communication::Message,
                                                             agent_communication::Message>* stream) override;

private:
    // 内部方法
    std::string generateMessageId();
    bool isAgentOnline(const std::string& agent_id);
    void updateAgentHeartbeat(const std::string& agent_id);
    void cleanupOfflineAgents();
    
    // 成员变量
    mutable std::mutex agents_mutex_;
    std::map<std::string, ServiceEndpoint> agents_;
    std::map<std::string, MessageQueue<agent_communication::Message>> agent_message_queues_;
    
    MessageHandler message_handler_;
    ErrorHandler error_handler_;
    HealthCheckHandler health_check_handler_;
    
    std::atomic<int> message_id_counter_{0};
    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};
};

// 健康检查服务实现
class HealthServiceImpl final : public agent_communication::HealthService::Service {
public:
    HealthServiceImpl();
    ~HealthServiceImpl() = default;
    
    void setHealthCheckHandler(HealthCheckHandler handler);
    
    grpc::Status Check(grpc::ServerContext* context,
                      const agent_communication::common::HealthCheckRequest* request,
                      agent_communication::common::HealthCheckResponse* response) override;
    
    grpc::Status Watch(grpc::ServerContext* context,
                      const agent_communication::common::HealthCheckRequest* request,
                      grpc::ServerWriter<agent_communication::common::HealthCheckResponse>* writer) override;

private:
    HealthCheckHandler health_check_handler_;
};

// RPC服务器类
class RpcServer {
public:
    RpcServer();
    ~RpcServer();
    
    // 初始化服务器
    bool initialize(const RpcConfig& config);
    
    // 启动服务器
    bool start();
    
    // 停止服务器
    void stop();
    
    // 等待服务器结束
    void wait();
    
    // 获取服务实现
    std::shared_ptr<AgentCommunicationServiceImpl> getService();
    
    // 获取健康检查服务
    std::shared_ptr<HealthServiceImpl> getHealthService();
    
    // 是否运行中
    bool isRunning() const { return running_; }
    
    // 获取服务器地址
    std::string getAddress() const { return address_; }

private:
    void setupServer();
    void setupSslCredentials();
    
    RpcConfig config_;
    std::string address_;
    std::atomic<bool> running_{false};
    
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<AgentCommunicationServiceImpl> service_impl_;
    std::shared_ptr<HealthServiceImpl> health_service_impl_;
    
    std::shared_ptr<grpc::ServerCredentials> server_credentials_;
    std::vector<std::unique_ptr<grpc::ServerBuilder>> builders_;
};

} // namespace agent_rpc
