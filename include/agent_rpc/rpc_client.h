#pragma once

#include "rpc_framework.h"
#include "proto/agent_service.grpc.pb.h"

namespace agent_rpc {

// RPC客户端类
class RpcClient {
public:
    RpcClient();
    ~RpcClient();
    
    // 初始化客户端
    bool initialize(const RpcConfig& config);
    
    // 连接到服务器
    bool connect(const std::string& server_address);
    
    // 断开连接
    void disconnect();
    
    // 发送消息
    bool sendMessage(const agent_communication::Message& message, 
                    const std::string& target_agent,
                    int timeout_seconds = 30);
    
    // 接收消息
    std::vector<agent_communication::Message> receiveMessages(const std::string& agent_id,
                                                             int max_messages = 10,
                                                             int timeout_seconds = 30);
    
    // 广播消息
    int broadcastMessage(const agent_communication::Message& message,
                        const std::vector<std::string>& target_agents = {},
                        bool exclude_sender = true);
    
    // 获取代理列表
    std::vector<ServiceEndpoint> getAgents(const std::string& filter = "",
                                          int limit = 100,
                                          int offset = 0);
    
    // 注册代理
    std::string registerAgent(const ServiceEndpoint& agent_info,
                             int heartbeat_interval = 30);
    
    // 注销代理
    bool unregisterAgent(const std::string& agent_id,
                        const std::string& reason = "");
    
    // 发送心跳
    bool sendHeartbeat(const std::string& agent_id,
                      const ServiceEndpoint& agent_info);
    
    // 监听消息（流式）
    void listenMessages(const std::string& agent_id,
                       MessageHandler handler,
                       int max_messages = 10,
                       int timeout_seconds = 30);
    
    // 批量发送消息（流式）
    bool batchSendMessages(const std::vector<agent_communication::SendMessageRequest>& requests);
    
    // 实时通信（双向流式）
    void startRealTimeCommunication(const std::string& agent_id,
                                   MessageHandler incoming_handler,
                                   std::function<void()> connection_handler = nullptr);
    
    // 停止实时通信
    void stopRealTimeCommunication();
    
    // 设置消息处理器
    void setMessageHandler(MessageHandler handler);
    
    // 设置错误处理器
    void setErrorHandler(ErrorHandler handler);
    
    // 是否连接
    bool isConnected() const { return connected_; }
    
    // 获取连接地址
    std::string getServerAddress() const { return server_address_; }

private:
    // 内部方法
    void setupChannel();
    void setupSslCredentials();
    bool reconnect();
    void startHeartbeat();
    void stopHeartbeat();
    void heartbeatLoop();
    void realTimeCommunicationLoop();
    
    // 成员变量
    RpcConfig config_;
    std::string server_address_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> real_time_running_{false};
    
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<agent_communication::AgentCommunicationService::Stub> stub_;
    std::unique_ptr<agent_communication::HealthService::Stub> health_stub_;
    
    MessageHandler message_handler_;
    ErrorHandler error_handler_;
    
    // 心跳相关
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    std::string current_agent_id_;
    ServiceEndpoint current_agent_info_;
    
    // 实时通信相关
    std::thread real_time_thread_;
    std::unique_ptr<grpc::ClientReaderWriter<agent_communication::Message, 
                                           agent_communication::Message>> real_time_stream_;
    std::mutex real_time_mutex_;
    
    // 连接管理
    mutable std::mutex connection_mutex_;
    std::chrono::steady_clock::time_point last_connection_time_;
    int connection_retry_count_ = 0;
    static constexpr int MAX_RETRY_COUNT = 5;
    static constexpr int RETRY_DELAY_MS = 1000;
};

} // namespace agent_rpc
