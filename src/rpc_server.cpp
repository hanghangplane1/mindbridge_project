#include "agent_rpc/rpc_server.h"
#include "agent_rpc/logger.h"
#include "agent_rpc/metrics.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <uuid/uuid.h>
#include <sstream>
#include <iomanip>

namespace agent_rpc {

// AgentCommunicationServiceImpl 实现
AgentCommunicationServiceImpl::AgentCommunicationServiceImpl() 
    : cleanup_running_(false) {
    // 启动清理线程
    cleanup_running_ = true;
    cleanup_thread_ = std::thread([this]() {
        while (cleanup_running_) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            cleanupOfflineAgents();
        }
    });
}

void AgentCommunicationServiceImpl::setMessageHandler(MessageHandler handler) {
    message_handler_ = handler;
}

void AgentCommunicationServiceImpl::setErrorHandler(ErrorHandler handler) {
    error_handler_ = handler;
}

void AgentCommunicationServiceImpl::setHealthCheckHandler(HealthCheckHandler handler) {
    health_check_handler_ = handler;
}

void AgentCommunicationServiceImpl::registerAgent(const std::string& agent_id, const ServiceEndpoint& endpoint) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_[agent_id] = endpoint;
    agent_message_queues_[agent_id] = MessageQueue<agent_communication::Message>();
    LOG_INFO("Agent registered: " + agent_id + " at " + endpoint.host + ":" + std::to_string(endpoint.port));
}

void AgentCommunicationServiceImpl::unregisterAgent(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_.erase(agent_id);
    agent_message_queues_.erase(agent_id);
    LOG_INFO("Agent unregistered: " + agent_id);
}

std::vector<ServiceEndpoint> AgentCommunicationServiceImpl::getAgents() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::vector<ServiceEndpoint> result;
    for (const auto& pair : agents_) {
        result.push_back(pair.second);
    }
    return result;
}

bool AgentCommunicationServiceImpl::sendMessageToAgent(const std::string& agent_id, const agent_communication::Message& message) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agent_message_queues_.find(agent_id);
    if (it != agent_message_queues_.end()) {
        it->second.push(message);
        return true;
    }
    return false;
}

int AgentCommunicationServiceImpl::broadcastMessage(const agent_communication::Message& message, 
                                                   const std::vector<std::string>& target_agents) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    int success_count = 0;
    
    if (target_agents.empty()) {
        // 广播给所有代理
        for (const auto& pair : agent_message_queues_) {
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

// gRPC 服务方法实现
grpc::Status AgentCommunicationServiceImpl::SendMessage(grpc::ServerContext* context,
                                                       const agent_communication::SendMessageRequest* request,
                                                       agent_communication::SendMessageResponse* response) {
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        // 记录指标
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "SendMessage", 0);
        
        // 生成消息ID
        std::string message_id = generateMessageId();
        
        // 创建消息
        agent_communication::Message message = request->message();
        message.set_id(message_id);
        message.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        // 发送消息
        bool success = sendMessageToAgent(request->target_agent(), message);
        
        if (success) {
            response->mutable_status()->set_code(0);
            response->mutable_status()->set_message("Success");
            response->set_message_id(message_id);
            response->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            // 调用消息处理器
            if (message_handler_) {
                message_handler_(message);
            }
            
            LOG_INFO("Message sent successfully: " + message_id + " to " + request->target_agent());
        } else {
            response->mutable_status()->set_code(1);
            response->mutable_status()->set_message("Target agent not found or offline");
            LOG_WARN("Failed to send message to agent: " + request->target_agent());
        }
        
        // 记录响应时间
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        metrics->recordRpcRequest("AgentCommunicationService", "SendMessage", duration.count());
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in SendMessage: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "SendMessage", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::ReceiveMessage(grpc::ServerContext* context,
                                                         const agent_communication::ReceiveMessageRequest* request,
                                                         agent_communication::ReceiveMessageResponse* response) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "ReceiveMessage", 0);
        
        std::vector<agent_communication::Message> messages;
        {
            std::lock_guard<std::mutex> lock(agents_mutex_);
            auto it = agent_message_queues_.find(request->agent_id());
            if (it != agent_message_queues_.end()) {
                int count = 0;
                agent_communication::Message msg;
                while (count < request->max_messages() && 
                       it->second.try_pop(msg, std::chrono::milliseconds(request->timeout_seconds() * 1000))) {
                    messages.push_back(msg);
                    count++;
                }
            }
        }
        
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Success");
        
        for (const auto& msg : messages) {
            *response->add_messages() = msg;
        }
        
        LOG_INFO("Received " + std::to_string(messages.size()) + " messages for agent: " + request->agent_id());
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in ReceiveMessage: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "ReceiveMessage", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::BroadcastMessage(grpc::ServerContext* context,
                                                           const agent_communication::BroadcastMessageRequest* request,
                                                           agent_communication::BroadcastMessageResponse* response) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "BroadcastMessage", 0);
        
        agent_communication::Message message = request->message();
        message.set_id(generateMessageId());
        message.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        std::vector<std::string> target_agents;
        if (request->exclude_sender()) {
            // 排除发送者逻辑
            for (const auto& agent_id : request->target_agents()) {
                target_agents.push_back(agent_id);
            }
        } else {
            target_agents = {request->target_agents().begin(), request->target_agents().end()};
        }
        
        int success_count = broadcastMessage(message, target_agents);
        int failure_count = target_agents.size() - success_count;
        
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Success");
        response->set_success_count(success_count);
        response->set_failure_count(failure_count);
        
        LOG_INFO("Broadcast message to " + std::to_string(success_count) + " agents");
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in BroadcastMessage: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "BroadcastMessage", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::GetAgents(grpc::ServerContext* context,
                                                    const agent_communication::GetAgentsRequest* request,
                                                    agent_communication::GetAgentsResponse* response) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "GetAgents", 0);
        
        std::vector<ServiceEndpoint> agents = getAgents();
        
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Success");
        response->set_total_count(agents.size());
        
        int count = 0;
        int offset = request->offset();
        int limit = request->limit() > 0 ? request->limit() : 100;
        
        for (const auto& agent : agents) {
            if (count >= offset + limit) break;
            if (count >= offset) {
                auto* agent_info = response->add_agents();
                agent_info->set_service_name(agent.service_name);
                agent_info->set_version(agent.version);
                agent_info->set_host(agent.host);
                agent_info->set_port(agent.port);
                
                for (const auto& pair : agent.metadata) {
                    (*agent_info->mutable_metadata())[pair.first] = pair.second;
                }
            }
            count++;
        }
        
        LOG_INFO("Returned " + std::to_string(response->agents_size()) + " agents");
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in GetAgents: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "GetAgents", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::RegisterAgent(grpc::ServerContext* context,
                                                        const agent_communication::RegisterAgentRequest* request,
                                                        agent_communication::RegisterAgentResponse* response) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "RegisterAgent", 0);
        
        // 生成代理ID
        std::string agent_id = generateMessageId();
        
        // 创建服务端点
        ServiceEndpoint endpoint;
        endpoint.host = request->agent_info().host();
        endpoint.port = request->agent_info().port();
        endpoint.service_name = request->agent_info().service_name();
        endpoint.version = request->agent_info().version();
        endpoint.last_heartbeat = std::chrono::steady_clock::now();
        
        // 复制元数据
        for (const auto& pair : request->agent_info().metadata()) {
            endpoint.metadata[pair.first] = pair.second;
        }
        
        // 注册代理
        registerAgent(agent_id, endpoint);
        
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Success");
        response->set_agent_id(agent_id);
        response->set_registration_time(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        LOG_INFO("Agent registered: " + agent_id);
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in RegisterAgent: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "RegisterAgent", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::UnregisterAgent(grpc::ServerContext* context,
                                                          const agent_communication::UnregisterAgentRequest* request,
                                                          agent_communication::UnregisterAgentResponse* response) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "UnregisterAgent", 0);
        
        unregisterAgent(request->agent_id());
        
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Success");
        response->set_unregistration_time(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        LOG_INFO("Agent unregistered: " + request->agent_id());
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in UnregisterAgent: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "UnregisterAgent", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::Heartbeat(grpc::ServerContext* context,
                                                    const agent_communication::HeartbeatRequest* request,
                                                    agent_communication::HeartbeatResponse* response) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "Heartbeat", 0);
        
        updateAgentHeartbeat(request->agent_id());
        
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Success");
        response->set_server_time(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in Heartbeat: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "Heartbeat", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::ListenMessages(grpc::ServerContext* context,
                                                         const agent_communication::ReceiveMessageRequest* request,
                                                         grpc::ServerWriter<agent_communication::Message>* writer) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "ListenMessages", 0);
        
        std::lock_guard<std::mutex> lock(agents_mutex_);
        auto it = agent_message_queues_.find(request->agent_id());
        if (it == agent_message_queues_.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Agent not found");
        }
        
        // 流式发送消息
        agent_communication::Message msg;
        while (context->IsCancelled() == false) {
            if (it->second.try_pop(msg, std::chrono::milliseconds(1000))) {
                if (!writer->Write(msg)) {
                    break;
                }
            }
        }
        
        LOG_INFO("ListenMessages stream ended for agent: " + request->agent_id());
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in ListenMessages: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "ListenMessages", "InternalError");
        
        return grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error");
    }
}

grpc::Status AgentCommunicationServiceImpl::BatchSendMessages(grpc::ServerContext* context,
                                                           grpc::ServerReader<agent_communication::SendMessageRequest>* reader,
                                                           agent_communication::SendMessageResponse* response) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "BatchSendMessages", 0);
        
        agent_communication::SendMessageRequest request;
        int success_count = 0;
        int total_count = 0;
        
        while (reader->Read(&request)) {
            total_count++;
            
            agent_communication::Message message = request.message();
            message.set_id(generateMessageId());
            message.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            if (sendMessageToAgent(request.target_agent(), message)) {
                success_count++;
            }
        }
        
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Success");
        response->set_message_id("batch_" + std::to_string(total_count));
        response->set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        LOG_INFO("Batch send completed: " + std::to_string(success_count) + "/" + std::to_string(total_count) + " messages sent");
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in BatchSendMessages: " + std::string(e.what()));
        response->mutable_status()->set_code(2);
        response->mutable_status()->set_message("Internal server error");
        
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "BatchSendMessages", "InternalError");
        
        return grpc::Status::OK;
    }
}

grpc::Status AgentCommunicationServiceImpl::RealTimeCommunication(grpc::ServerContext* context,
                                                                grpc::ServerReaderWriter<agent_communication::Message,
                                                                                       agent_communication::Message>* stream) {
    try {
        auto metrics = Metrics::getInstance();
        metrics->recordRpcRequest("AgentCommunicationService", "RealTimeCommunication", 0);
        
        agent_communication::Message message;
        
        // 读取客户端消息并转发
        while (stream->Read(&message)) {
            // 设置服务器时间戳
            message.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            // 广播给其他代理
            broadcastMessage(message, {});
            
            // 回写消息确认
            if (!stream->Write(message)) {
                break;
            }
        }
        
        LOG_INFO("RealTimeCommunication stream ended");
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in RealTimeCommunication: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "RealTimeCommunication", "InternalError");
        
        return grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error");
    }
}

// HealthServiceImpl 实现
HealthServiceImpl::HealthServiceImpl() = default;

void HealthServiceImpl::setHealthCheckHandler(HealthCheckHandler handler) {
    health_check_handler_ = handler;
}

grpc::Status HealthServiceImpl::Check(grpc::ServerContext* context,
                                    const agent_communication::common::HealthCheckRequest* request,
                                    agent_communication::common::HealthCheckResponse* response) {
    try {
        bool is_healthy = true;
        if (health_check_handler_) {
            is_healthy = health_check_handler_();
        }
        
        response->set_status(is_healthy ? 
                           agent_communication::common::HealthCheckResponse::SERVING :
                           agent_communication::common::HealthCheckResponse::NOT_SERVING);
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in health check: " + std::string(e.what()));
        response->set_status(agent_communication::common::HealthCheckResponse::NOT_SERVING);
        return grpc::Status::OK;
    }
}

grpc::Status HealthServiceImpl::Watch(grpc::ServerContext* context,
                                    const agent_communication::common::HealthCheckRequest* request,
                                    grpc::ServerWriter<agent_communication::common::HealthCheckResponse>* writer) {
    try {
        while (context->IsCancelled() == false) {
            agent_communication::common::HealthCheckResponse response;
            
            bool is_healthy = true;
            if (health_check_handler_) {
                is_healthy = health_check_handler_();
            }
            
            response.set_status(is_healthy ? 
                              agent_communication::common::HealthCheckResponse::SERVING :
                              agent_communication::common::HealthCheckResponse::NOT_SERVING);
            
            if (!writer->Write(response)) {
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        
        return grpc::Status::OK;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in health watch: " + std::string(e.what()));
        return grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error");
    }
}

// RpcServer 实现
RpcServer::RpcServer() = default;

RpcServer::~RpcServer() {
    stop();
}

bool RpcServer::initialize(const RpcConfig& config) {
    config_ = config;
    address_ = config.server_address;
    
    // 创建服务实现
    service_impl_ = std::make_shared<AgentCommunicationServiceImpl>();
    health_service_impl_ = std::make_shared<HealthServiceImpl>();
    
    setupServer();
    
    LOG_INFO("RPC server initialized on " + address_);
    return true;
}

bool RpcServer::start() {
    if (running_) {
        LOG_WARN("RPC server is already running");
        return true;
    }
    
    if (!server_) {
        LOG_ERROR("RPC server not initialized");
        return false;
    }
    
    try {
        server_->Wait();
        running_ = true;
        LOG_INFO("RPC server started successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to start RPC server: " + std::string(e.what()));
        return false;
    }
}

void RpcServer::stop() {
    if (!running_) {
        return;
    }
    
    if (server_) {
        server_->Shutdown();
        running_ = false;
        LOG_INFO("RPC server stopped");
    }
}

void RpcServer::wait() {
    if (server_) {
        server_->Wait();
    }
}

std::shared_ptr<AgentCommunicationServiceImpl> RpcServer::getService() {
    return service_impl_;
}

std::shared_ptr<HealthServiceImpl> RpcServer::getHealthService() {
    return health_service_impl_;
}

void RpcServer::setupServer() {
    grpc::ServerBuilder builder;
    
    // 设置服务器地址和端口
    builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
    
    // 设置最大消息大小
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(config_.max_receive_message_size);
    args.SetMaxSendMessageSize(config_.max_message_size);
    builder.SetChannelArguments(args);
    
    // 注册服务
    builder.RegisterService(service_impl_.get());
    builder.RegisterService(health_service_impl_.get());
    
    // 启用健康检查服务
    grpc::EnableDefaultHealthCheckService(true);
    
    // 启用服务器反射
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, true);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 10000);
    
    // 构建服务器
    server_ = builder.BuildAndStart();
    
    if (!server_) {
        throw std::runtime_error("Failed to build gRPC server");
    }
}

void RpcServer::setupSslCredentials() {
    if (config_.enable_ssl && !config_.ssl_cert_path.empty() && !config_.ssl_key_path.empty()) {
        grpc::SslServerCredentialsOptions ssl_opts;
        grpc::SslServerCredentialsOptions::PemKeyCertPair pkcp;
        pkcp.private_key = readFile(config_.ssl_key_path);
        pkcp.cert_chain = readFile(config_.ssl_cert_path);
        ssl_opts.pem_key_cert_pairs.push_back(pkcp);
        
        server_credentials_ = grpc::SslServerCredentials(ssl_opts);
    } else {
        server_credentials_ = grpc::InsecureServerCredentials();
    }
}

} // namespace agent_rpc
