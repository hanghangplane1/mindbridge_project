#include "agent_rpc/rpc_client.h"
#include "agent_rpc/logger.h"
#include "agent_rpc/metrics.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <uuid/uuid.h>
#include <sstream>
#include <iomanip>

namespace agent_rpc {

RpcClient::RpcClient() 
    : heartbeat_running_(false)
    , real_time_running_(false)
    , connection_retry_count_(0) {
}

RpcClient::~RpcClient() {
    disconnect();
    stopHeartbeat();
    stopRealTimeCommunication();
}

bool RpcClient::initialize(const RpcConfig& config) {
    config_ = config;
    LOG_INFO("RPC client initialized");
    return true;
}

bool RpcClient::connect(const std::string& server_address) {
    server_address_ = server_address;
    
    try {
        setupChannel();
        connected_ = true;
        last_connection_time_ = std::chrono::steady_clock::now();
        connection_retry_count_ = 0;
        
        LOG_INFO("Connected to RPC server: " + server_address);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to connect to RPC server: " + std::string(e.what()));
        connected_ = false;
        return false;
    }
}

void RpcClient::disconnect() {
    if (connected_) {
        stopHeartbeat();
        stopRealTimeCommunication();
        
        channel_.reset();
        stub_.reset();
        health_stub_.reset();
        connected_ = false;
        
        LOG_INFO("Disconnected from RPC server");
    }
}

bool RpcClient::sendMessage(const agent_communication::Message& message, 
                           const std::string& target_agent,
                           int timeout_seconds) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return false;
    }
    
    try {
        auto metrics = Metrics::getInstance();
        auto start_time = std::chrono::steady_clock::now();
        
        agent_communication::SendMessageRequest request;
        request.mutable_message()->CopyFrom(message);
        request.set_target_agent(target_agent);
        request.set_timeout_seconds(timeout_seconds);
        
        agent_communication::SendMessageResponse response;
        grpc::ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);
        context.set_deadline(deadline);
        
        grpc::Status status = stub_->SendMessage(&context, request, &response);
        
        if (status.ok()) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            metrics->recordRpcRequest("AgentCommunicationService", "SendMessage", duration.count());
            metrics->recordRpcResponse("AgentCommunicationService", "SendMessage", response.status().code());
            
            if (response.status().code() == 0) {
                LOG_INFO("Message sent successfully: " + response.message_id());
                return true;
            } else {
                LOG_WARN("Failed to send message: " + response.status().message());
                return false;
            }
        } else {
            LOG_ERROR("gRPC error: " + status.error_message());
            metrics->recordRpcError("AgentCommunicationService", "SendMessage", "GrpcError");
            return false;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error sending message: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "SendMessage", "Exception");
        return false;
    }
}

std::vector<agent_communication::Message> RpcClient::receiveMessages(const std::string& agent_id,
                                                                    int max_messages,
                                                                    int timeout_seconds) {
    std::vector<agent_communication::Message> messages;
    
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return messages;
    }
    
    try {
        auto metrics = Metrics::getInstance();
        auto start_time = std::chrono::steady_clock::now();
        
        agent_communication::ReceiveMessageRequest request;
        request.set_agent_id(agent_id);
        request.set_max_messages(max_messages);
        request.set_timeout_seconds(timeout_seconds);
        
        agent_communication::ReceiveMessageResponse response;
        grpc::ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);
        context.set_deadline(deadline);
        
        grpc::Status status = stub_->ReceiveMessage(&context, request, &response);
        
        if (status.ok()) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            metrics->recordRpcRequest("AgentCommunicationService", "ReceiveMessage", duration.count());
            metrics->recordRpcResponse("AgentCommunicationService", "ReceiveMessage", response.status().code());
            
            if (response.status().code() == 0) {
                for (const auto& msg : response.messages()) {
                    messages.push_back(msg);
                }
                LOG_INFO("Received " + std::to_string(messages.size()) + " messages for agent: " + agent_id);
            } else {
                LOG_WARN("Failed to receive messages: " + response.status().message());
            }
        } else {
            LOG_ERROR("gRPC error: " + status.error_message());
            metrics->recordRpcError("AgentCommunicationService", "ReceiveMessage", "GrpcError");
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error receiving messages: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "ReceiveMessage", "Exception");
    }
    
    return messages;
}

int RpcClient::broadcastMessage(const agent_communication::Message& message,
                               const std::vector<std::string>& target_agents,
                               bool exclude_sender) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return 0;
    }
    
    try {
        auto metrics = Metrics::getInstance();
        auto start_time = std::chrono::steady_clock::now();
        
        agent_communication::BroadcastMessageRequest request;
        request.mutable_message()->CopyFrom(message);
        request.set_exclude_sender(exclude_sender);
        
        for (const auto& agent : target_agents) {
            request.add_target_agents(agent);
        }
        
        agent_communication::BroadcastMessageResponse response;
        grpc::ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
        context.set_deadline(deadline);
        
        grpc::Status status = stub_->BroadcastMessage(&context, request, &response);
        
        if (status.ok()) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            metrics->recordRpcRequest("AgentCommunicationService", "BroadcastMessage", duration.count());
            metrics->recordRpcResponse("AgentCommunicationService", "BroadcastMessage", response.status().code());
            
            if (response.status().code() == 0) {
                LOG_INFO("Broadcast message to " + std::to_string(response.success_count()) + " agents");
                return response.success_count();
            } else {
                LOG_WARN("Failed to broadcast message: " + response.status().message());
                return 0;
            }
        } else {
            LOG_ERROR("gRPC error: " + status.error_message());
            metrics->recordRpcError("AgentCommunicationService", "BroadcastMessage", "GrpcError");
            return 0;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error broadcasting message: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "BroadcastMessage", "Exception");
        return 0;
    }
}

std::vector<ServiceEndpoint> RpcClient::getAgents(const std::string& filter,
                                                 int limit,
                                                 int offset) {
    std::vector<ServiceEndpoint> agents;
    
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return agents;
    }
    
    try {
        auto metrics = Metrics::getInstance();
        auto start_time = std::chrono::steady_clock::now();
        
        agent_communication::GetAgentsRequest request;
        request.set_filter(filter);
        request.set_limit(limit);
        request.set_offset(offset);
        
        agent_communication::GetAgentsResponse response;
        grpc::ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
        context.set_deadline(deadline);
        
        grpc::Status status = stub_->GetAgents(&context, request, &response);
        
        if (status.ok()) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            metrics->recordRpcRequest("AgentCommunicationService", "GetAgents", duration.count());
            metrics->recordRpcResponse("AgentCommunicationService", "GetAgents", response.status().code());
            
            if (response.status().code() == 0) {
                for (const auto& agent_info : response.agents()) {
                    ServiceEndpoint endpoint;
                    endpoint.host = agent_info.host();
                    endpoint.port = agent_info.port();
                    endpoint.service_name = agent_info.service_name();
                    endpoint.version = agent_info.version();
                    
                    for (const auto& pair : agent_info.metadata()) {
                        endpoint.metadata[pair.first] = pair.second;
                    }
                    
                    agents.push_back(endpoint);
                }
                LOG_INFO("Retrieved " + std::to_string(agents.size()) + " agents");
            } else {
                LOG_WARN("Failed to get agents: " + response.status().message());
            }
        } else {
            LOG_ERROR("gRPC error: " + status.error_message());
            metrics->recordRpcError("AgentCommunicationService", "GetAgents", "GrpcError");
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error getting agents: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "GetAgents", "Exception");
    }
    
    return agents;
}

std::string RpcClient::registerAgent(const ServiceEndpoint& agent_info,
                                    int heartbeat_interval) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return "";
    }
    
    try {
        auto metrics = Metrics::getInstance();
        auto start_time = std::chrono::steady_clock::now();
        
        agent_communication::RegisterAgentRequest request;
        auto* info = request.mutable_agent_info();
        info->set_host(agent_info.host);
        info->set_port(agent_info.port);
        info->set_service_name(agent_info.service_name);
        info->set_version(agent_info.version);
        
        for (const auto& pair : agent_info.metadata) {
            (*info->mutable_metadata())[pair.first] = pair.second;
        }
        
        request.set_heartbeat_interval(heartbeat_interval);
        
        agent_communication::RegisterAgentResponse response;
        grpc::ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
        context.set_deadline(deadline);
        
        grpc::Status status = stub_->RegisterAgent(&context, request, &response);
        
        if (status.ok()) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            metrics->recordRpcRequest("AgentCommunicationService", "RegisterAgent", duration.count());
            metrics->recordRpcResponse("AgentCommunicationService", "RegisterAgent", response.status().code());
            
            if (response.status().code() == 0) {
                current_agent_id_ = response.agent_id();
                current_agent_info_ = agent_info;
                
                // 启动心跳
                startHeartbeat();
                
                LOG_INFO("Agent registered successfully: " + current_agent_id_);
                return current_agent_id_;
            } else {
                LOG_WARN("Failed to register agent: " + response.status().message());
                return "";
            }
        } else {
            LOG_ERROR("gRPC error: " + status.error_message());
            metrics->recordRpcError("AgentCommunicationService", "RegisterAgent", "GrpcError");
            return "";
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error registering agent: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "RegisterAgent", "Exception");
        return "";
    }
}

bool RpcClient::unregisterAgent(const std::string& agent_id,
                               const std::string& reason) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return false;
    }
    
    try {
        // 停止心跳
        stopHeartbeat();
        
        auto metrics = Metrics::getInstance();
        auto start_time = std::chrono::steady_clock::now();
        
        agent_communication::UnregisterAgentRequest request;
        request.set_agent_id(agent_id);
        request.set_reason(reason);
        
        agent_communication::UnregisterAgentResponse response;
        grpc::ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
        context.set_deadline(deadline);
        
        grpc::Status status = stub_->UnregisterAgent(&context, request, &response);
        
        if (status.ok()) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            metrics->recordRpcRequest("AgentCommunicationService", "UnregisterAgent", duration.count());
            metrics->recordRpcResponse("AgentCommunicationService", "UnregisterAgent", response.status().code());
            
            if (response.status().code() == 0) {
                LOG_INFO("Agent unregistered successfully: " + agent_id);
                return true;
            } else {
                LOG_WARN("Failed to unregister agent: " + response.status().message());
                return false;
            }
        } else {
            LOG_ERROR("gRPC error: " + status.error_message());
            metrics->recordRpcError("AgentCommunicationService", "UnregisterAgent", "GrpcError");
            return false;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error unregistering agent: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "UnregisterAgent", "Exception");
        return false;
    }
}

bool RpcClient::sendHeartbeat(const std::string& agent_id,
                             const ServiceEndpoint& agent_info) {
    if (!connected_) {
        return false;
    }
    
    try {
        agent_communication::HeartbeatRequest request;
        request.set_agent_id(agent_id);
        auto* info = request.mutable_agent_info();
        info->set_host(agent_info.host);
        info->set_port(agent_info.port);
        info->set_service_name(agent_info.service_name);
        info->set_version(agent_info.version);
        
        agent_communication::HeartbeatResponse response;
        grpc::ClientContext context;
        
        // 设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(10);
        context.set_deadline(deadline);
        
        grpc::Status status = stub_->Heartbeat(&context, request, &response);
        
        if (status.ok() && response.status().code() == 0) {
            return true;
        } else {
            LOG_WARN("Heartbeat failed: " + (status.ok() ? response.status().message() : status.error_message()));
            return false;
        }
        
    } catch (const std::exception& e) {
        LOG_WARN("Heartbeat error: " + std::string(e.what()));
        return false;
    }
}

void RpcClient::listenMessages(const std::string& agent_id,
                              MessageHandler handler,
                              int max_messages,
                              int timeout_seconds) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return;
    }
    
    try {
        agent_communication::ReceiveMessageRequest request;
        request.set_agent_id(agent_id);
        request.set_max_messages(max_messages);
        request.set_timeout_seconds(timeout_seconds);
        
        grpc::ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);
        context.set_deadline(deadline);
        
        agent_communication::Message message;
        auto reader = stub_->ListenMessages(&context, request);
        
        while (reader->Read(&message)) {
            if (handler) {
                handler(message);
            }
        }
        
        grpc::Status status = reader->Finish();
        if (!status.ok()) {
            LOG_ERROR("ListenMessages stream error: " + status.error_message());
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in listenMessages: " + std::string(e.what()));
    }
}

bool RpcClient::batchSendMessages(const std::vector<agent_communication::SendMessageRequest>& requests) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return false;
    }
    
    try {
        auto metrics = Metrics::getInstance();
        auto start_time = std::chrono::steady_clock::now();
        
        grpc::ClientContext context;
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(60);
        context.set_deadline(deadline);
        
        agent_communication::SendMessageResponse response;
        auto writer = stub_->BatchSendMessages(&context, &response);
        
        for (const auto& request : requests) {
            if (!writer->Write(request)) {
                LOG_ERROR("Failed to write batch request");
                break;
            }
        }
        
        writer->WritesDone();
        grpc::Status status = writer->Finish();
        
        if (status.ok()) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            metrics->recordRpcRequest("AgentCommunicationService", "BatchSendMessages", duration.count());
            metrics->recordRpcResponse("AgentCommunicationService", "BatchSendMessages", response.status().code());
            
            LOG_INFO("Batch send completed: " + response.message_id());
            return response.status().code() == 0;
        } else {
            LOG_ERROR("Batch send error: " + status.error_message());
            metrics->recordRpcError("AgentCommunicationService", "BatchSendMessages", "GrpcError");
            return false;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in batchSendMessages: " + std::string(e.what()));
        auto metrics = Metrics::getInstance();
        metrics->recordRpcError("AgentCommunicationService", "BatchSendMessages", "Exception");
        return false;
    }
}

void RpcClient::startRealTimeCommunication(const std::string& agent_id,
                                          MessageHandler incoming_handler,
                                          std::function<void()> connection_handler) {
    if (real_time_running_) {
        LOG_WARN("Real-time communication already running");
        return;
    }
    
    real_time_running_ = true;
    message_handler_ = incoming_handler;
    
    real_time_thread_ = std::thread([this, agent_id, connection_handler]() {
        realTimeCommunicationLoop();
    });
    
    if (connection_handler) {
        connection_handler();
    }
}

void RpcClient::stopRealTimeCommunication() {
    if (real_time_running_) {
        real_time_running_ = false;
        
        std::lock_guard<std::mutex> lock(real_time_mutex_);
        if (real_time_stream_) {
            real_time_stream_->WritesDone();
        }
        
        if (real_time_thread_.joinable()) {
            real_time_thread_.join();
        }
        
        LOG_INFO("Real-time communication stopped");
    }
}

void RpcClient::setMessageHandler(MessageHandler handler) {
    message_handler_ = handler;
}

void RpcClient::setErrorHandler(ErrorHandler handler) {
    error_handler_ = handler;
}

void RpcClient::setupChannel() {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(config_.max_receive_message_size);
    args.SetMaxSendMessageSize(config_.max_message_size);
    
    if (config_.enable_ssl) {
        setupSslCredentials();
        channel_ = grpc::CreateCustomChannel(server_address_, grpc::SslCredentials(grpc::SslCredentialsOptions()), args);
    } else {
        channel_ = grpc::CreateCustomChannel(server_address_, grpc::InsecureChannelCredentials(), args);
    }
    
    if (!channel_) {
        throw std::runtime_error("Failed to create gRPC channel");
    }
    
    stub_ = agent_communication::AgentCommunicationService::NewStub(channel_);
    health_stub_ = agent_communication::HealthService::NewStub(channel_);
    
    if (!stub_ || !health_stub_) {
        throw std::runtime_error("Failed to create gRPC stubs");
    }
}

void RpcClient::setupSslCredentials() {
    // SSL证书配置逻辑
    // 这里可以根据需要实现SSL证书加载
}

bool RpcClient::reconnect() {
    if (connection_retry_count_ >= MAX_RETRY_COUNT) {
        LOG_ERROR("Max reconnection attempts reached");
        return false;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS * (connection_retry_count_ + 1)));
    
    try {
        setupChannel();
        connected_ = true;
        connection_retry_count_ = 0;
        last_connection_time_ = std::chrono::steady_clock::now();
        
        LOG_INFO("Reconnected to server successfully");
        return true;
    } catch (const std::exception& e) {
        connection_retry_count_++;
        LOG_ERROR("Reconnection failed: " + std::string(e.what()));
        return false;
    }
}

void RpcClient::startHeartbeat() {
    if (heartbeat_running_) {
        return;
    }
    
    heartbeat_running_ = true;
    heartbeat_thread_ = std::thread([this]() {
        heartbeatLoop();
    });
}

void RpcClient::stopHeartbeat() {
    if (heartbeat_running_) {
        heartbeat_running_ = false;
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
    }
}

void RpcClient::heartbeatLoop() {
    while (heartbeat_running_) {
        if (connected_ && !current_agent_id_.empty()) {
            if (!sendHeartbeat(current_agent_id_, current_agent_info_)) {
                LOG_WARN("Heartbeat failed, attempting reconnection");
                connected_ = false;
                if (!reconnect()) {
                    LOG_ERROR("Failed to reconnect, stopping heartbeat");
                    break;
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(config_.heartbeat_interval));
    }
}

void RpcClient::realTimeCommunicationLoop() {
    try {
        grpc::ClientContext context;
        real_time_stream_ = stub_->RealTimeCommunication(&context);
        
        // 发送线程
        std::thread send_thread([this]() {
            agent_communication::Message message;
            while (real_time_running_) {
                // 这里可以实现消息发送逻辑
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        
        // 接收线程
        agent_communication::Message message;
        while (real_time_running_ && real_time_stream_->Read(&message)) {
            if (message_handler_) {
                message_handler_(message);
            }
        }
        
        send_thread.join();
        grpc::Status status = real_time_stream_->Finish();
        
        if (!status.ok()) {
            LOG_ERROR("Real-time communication error: " + status.error_message());
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error in real-time communication: " + std::string(e.what()));
    }
}

} // namespace agent_rpc
