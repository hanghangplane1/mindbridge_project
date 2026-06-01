#pragma once

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <condition_variable>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "agent_service.pb.h"
#include "agent_service.grpc.pb.h"
#include "common.pb.h"

namespace agent_rpc {

// 前向声明
class RpcServer;
class RpcClient;
class ServiceRegistry;
class LoadBalancer;
class CircuitBreaker;
class Logger;
class Metrics;

// 配置结构
struct RpcConfig {
    std::string server_address = "0.0.0.0:50051";
    int max_message_size = 4 * 1024 * 1024;  // 4MB
    int max_receive_message_size = 4 * 1024 * 1024;  // 4MB
    int timeout_seconds = 30;
    int max_retry_attempts = 3;
    int heartbeat_interval = 30;
    bool enable_ssl = false;
    std::string ssl_cert_path;
    std::string ssl_key_path;
    std::string log_level = "INFO";
    std::string registry_address = "localhost:8500";
};

// 服务信息
struct ServiceEndpoint {
    std::string host;
    int port;
    std::string service_name;
    std::string version;
    std::map<std::string, std::string> metadata;
    bool is_healthy = true;
    std::chrono::steady_clock::time_point last_heartbeat;
};

// 消息队列
template<typename T>
class MessageQueue {
public:
    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
        condition_.notify_one();
    }
    
    bool try_pop(T& item, std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (condition_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            item = queue_.front();
            queue_.pop();
            return true;
        }
        return false;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
    std::condition_variable condition_;
};

// 回调函数类型定义
using MessageHandler = std::function<void(const agent_communication::Message&)>;
using ErrorHandler = std::function<void(const std::string&, int)>;
using HealthCheckHandler = std::function<bool()>;

// RPC框架主类
class RpcFramework {
public:
    static RpcFramework& getInstance();
    
    // 初始化框架
    bool initialize(const RpcConfig& config);
    
    // 启动服务
    bool startServer();
    
    // 停止服务
    void stopServer();
    
    // 获取服务器实例
    std::shared_ptr<RpcServer> getServer();
    
    // 获取客户端实例
    std::shared_ptr<RpcClient> getClient();
    
    // 获取服务注册中心
    std::shared_ptr<ServiceRegistry> getRegistry();
    
    // 获取负载均衡器
    std::shared_ptr<LoadBalancer> getLoadBalancer();
    
    // 获取日志器
    std::shared_ptr<Logger> getLogger();
    
    // 获取监控指标
    std::shared_ptr<Metrics> getMetrics();
    
    // 配置
    const RpcConfig& getConfig() const { return config_; }
    
    // 是否运行中
    bool isRunning() const { return running_; }

private:
    RpcFramework() = default;
    ~RpcFramework() = default;
    RpcFramework(const RpcFramework&) = delete;
    RpcFramework& operator=(const RpcFramework&) = delete;
    
    RpcConfig config_;
    std::atomic<bool> running_{false};
    
    std::shared_ptr<RpcServer> server_;
    std::shared_ptr<RpcClient> client_;
    std::shared_ptr<ServiceRegistry> registry_;
    std::shared_ptr<LoadBalancer> load_balancer_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<Metrics> metrics_;
};

} // namespace agent_rpc
