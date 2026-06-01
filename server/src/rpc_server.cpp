#include "agent_rpc/server/rpc_server.h"
#include "agent_rpc/server/agent_service.h"
#include "agent_rpc/server/ai_query_service.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/message_converter.h"
#include "agent_rpc/common/serializer.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <fstream>

namespace agent_rpc {
namespace server {

// RpcServer 实现
RpcServer::RpcServer() {
    // 设置默认MCP服务器路径 (预留，待实现MCP client)
    mcp_server_path_ = "";
    mcp_server_args_ = {};
}

RpcServer::~RpcServer() {
    stop();
    
    // 显式清理成员变量，确保正确的析构顺序
    service_impl_.reset();
    health_service_impl_.reset();
    ai_query_service_impl_.reset();
    server_.reset();
    server_credentials_.reset();
    builders_.clear();
}

bool RpcServer::initialize(const common::RpcConfig& config) {
    config_ = config;
    address_ = config.server_address;
    
    // 创建服务实现
    service_impl_ = std::make_shared<AgentCommunicationServiceImpl>();
    health_service_impl_ = std::make_shared<HealthServiceImpl>();
    ai_query_service_impl_ = std::make_shared<AIQueryServiceImpl>();
    
    // 初始化序列化器
    common::MessageSerializer::getInstance().initialize(common::SerializerFactory::PROTOBUF_BINARY);
    
    // 初始化AI查询服务
    if (!ai_query_service_impl_->initialize(config_, a2a_config_)) {
        LOG_WARN("Failed to initialize AI Query Service, continuing without it");
    }
    
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
        // 在后台线程中启动服务器
        server_thread_ = std::thread([this]() {
            server_->Wait();
        });
        
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
    
    running_ = false;
    
    if (server_) {
        // 设置一个截止时间，避免无限等待
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        server_->Shutdown(deadline);
    }
    
    // 等待服务器线程结束
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    // 在 gRPC 服务器完全停止后，关闭服务实现
    if (ai_query_service_impl_) {
        ai_query_service_impl_->shutdown();
    }
    
    LOG_INFO("RPC server stopped");
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

std::shared_ptr<AIQueryServiceImpl> RpcServer::getAIQueryService() {
    return ai_query_service_impl_;
}

void RpcServer::setA2AConfig(const a2a_adapter::A2AConfig& config) {
    a2a_config_ = config;
}

void RpcServer::setMCPServerPath(const std::string& path) {
    mcp_server_path_ = path;
}

void RpcServer::setMCPServerArgs(const std::vector<std::string>& args) {
    mcp_server_args_ = args;
}

void RpcServer::setupServer() {
    grpc::ServerBuilder builder;
    
    // 设置服务器地址和端口
    builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
    
    // 设置最大消息大小
    builder.SetMaxReceiveMessageSize(config_.max_receive_message_size);
    builder.SetMaxSendMessageSize(config_.max_message_size);
    
    // 注册AI查询服务 (这是唯一的gRPC服务实现)
    if (ai_query_service_impl_ && ai_query_service_impl_->isAvailable()) {
        builder.RegisterService(ai_query_service_impl_.get());
        LOG_INFO("AI Query Service registered");
    }
    
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
        
        // 读取证书文件
        std::ifstream key_file(config_.ssl_key_path);
        std::ifstream cert_file(config_.ssl_cert_path);
        
        if (key_file.is_open() && cert_file.is_open()) {
            std::string key_content((std::istreambuf_iterator<char>(key_file)),
                                  std::istreambuf_iterator<char>());
            std::string cert_content((std::istreambuf_iterator<char>(cert_file)),
                                  std::istreambuf_iterator<char>());
            
            pkcp.private_key = key_content;
            pkcp.cert_chain = cert_content;
            ssl_opts.pem_key_cert_pairs.push_back(pkcp);
            
            server_credentials_ = grpc::SslServerCredentials(ssl_opts);
            LOG_INFO("SSL credentials configured");
        } else {
            LOG_ERROR("Failed to read SSL certificate files");
            server_credentials_ = grpc::InsecureServerCredentials();
        }
    } else {
        server_credentials_ = grpc::InsecureServerCredentials();
    }
}

} // namespace server
} // namespace agent_rpc
