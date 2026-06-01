#include "agent_rpc/client/rpc_client.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/message_converter.h"
#include "agent_rpc/common/serializer.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/generic/generic_stub.h>
#include <uuid/uuid.h>
#include <sstream>
#include <iomanip>

namespace {
// Helper function to create ByteBuffer from string
grpc::ByteBuffer stringToByteBuffer(const std::string& str) {
    grpc::Slice slice(str);
    return grpc::ByteBuffer(&slice, 1);
}
} // anonymous namespace

namespace agent_rpc {
namespace client {

// RpcClient 实现
RpcClient::RpcClient() 
    : heartbeat_running_(false)
    , connection_retry_count_(0)
    , ai_query_client_(std::make_unique<AIQueryClient>()) {
}

RpcClient::~RpcClient() {
    disconnect();
    stopHeartbeat();
}

bool RpcClient::initialize(const common::RpcConfig& config) {
    config_ = config;
    
    // 初始化序列化器
    common::MessageSerializer::getInstance().initialize(common::SerializerFactory::PROTOBUF_BINARY);
    
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
        
        // Connect AIQueryClient to the same server
        if (ai_query_client_) {
            if (!ai_query_client_->connect(server_address)) {
                LOG_WARN("Failed to connect AIQueryClient, AI queries will not be available");
            }
        }
        
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
        
        // Disconnect AIQueryClient
        if (ai_query_client_) {
            ai_query_client_->disconnect();
        }
        
        channel_.reset();
        stub_.reset();
        connected_ = false;
        
        LOG_INFO("Disconnected from RPC server");
    }
}

bool RpcClient::sendMessage(const std::string& message, 
                           const std::string& target_agent,
                           int timeout_seconds) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return false;
    }
    
    // TODO: Implement using proper gRPC stub
    // 当前主要功能通过 AIQueryClient 实现
    LOG_WARN("sendMessage not fully implemented, use aiQuery for AI queries");
    (void)message;
    (void)target_agent;
    (void)timeout_seconds;
    return false;
}

std::vector<std::string> RpcClient::receiveMessages(const std::string& agent_id,
                                                   int max_messages,
                                                   int timeout_seconds) {
    std::vector<std::string> messages;
    
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return messages;
    }
    
    // TODO: Implement using proper gRPC stub
    LOG_WARN("receiveMessages not fully implemented");
    return messages;
}

int RpcClient::broadcastMessage(const std::string& message,
                               const std::vector<std::string>& target_agents,
                               bool exclude_sender) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return 0;
    }
    
    // TODO: Implement using proper gRPC stub
    LOG_WARN("broadcastMessage not fully implemented");
    return 0;
}

std::vector<common::ServiceEndpoint> RpcClient::getAgents(const std::string& filter,
                                                         int limit,
                                                         int offset) {
    std::vector<common::ServiceEndpoint> agents;
    
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return agents;
    }
    
    // TODO: Implement using proper gRPC stub
    LOG_WARN("getAgents not fully implemented");
    return agents;
}

std::string RpcClient::registerAgent(const common::ServiceEndpoint& agent_info,
                                    int heartbeat_interval) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return "";
    }
    
    // TODO: Implement using proper gRPC stub
    LOG_WARN("registerAgent not fully implemented");
    return "";
}

bool RpcClient::unregisterAgent(const std::string& agent_id,
                               const std::string& reason) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return false;
    }
    
    stopHeartbeat();
    
    // TODO: Implement using proper gRPC stub
    LOG_WARN("unregisterAgent not fully implemented");
    return true;
}

bool RpcClient::sendHeartbeat(const std::string& agent_id,
                             const common::ServiceEndpoint& agent_info) {
    if (!connected_) {
        return false;
    }
    
    // TODO: Implement using proper gRPC stub
    return true;
}

void RpcClient::listenMessages(const std::string& agent_id,
                              common::MessageHandler handler,
                              int max_messages,
                              int timeout_seconds) {
    if (!connected_) {
        LOG_ERROR("Client not connected to server");
        return;
    }
    
    // TODO: Implement using proper gRPC stub
    LOG_WARN("listenMessages not fully implemented");
}

void RpcClient::setMessageHandler(common::MessageHandler handler) {
    message_handler_ = handler;
}

void RpcClient::setErrorHandler(common::ErrorHandler handler) {
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
    
    stub_ = std::make_unique<grpc::TemplatedGenericStub<grpc::ByteBuffer, grpc::ByteBuffer>>(channel_);
    
    if (!stub_) {
        throw std::runtime_error("Failed to create gRPC stub");
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

// ============================================================================
// AI Query Methods (Requirements: 2.1)
// ============================================================================

agent_communication::AIQueryResponse RpcClient::aiQuery(
    const std::string& question,
    const std::string& context_id,
    int timeout_seconds) {
    
    if (!ai_query_client_ || !ai_query_client_->isConnected()) {
        agent_communication::AIQueryResponse response;
        auto* status = response.mutable_status();
        status->set_code(-1);
        status->set_message("AIQueryClient not connected");
        LOG_ERROR("AIQueryClient not connected");
        return response;
    }
    
    return ai_query_client_->query(question, context_id, timeout_seconds);
}

bool RpcClient::aiQueryStream(
    const std::string& question,
    StreamEventCallback callback,
    const std::string& context_id,
    int timeout_seconds) {
    
    if (!ai_query_client_ || !ai_query_client_->isConnected()) {
        LOG_ERROR("AIQueryClient not connected");
        return false;
    }
    
    return ai_query_client_->queryStream(question, callback, context_id, timeout_seconds);
}

} // namespace client
} // namespace agent_rpc
