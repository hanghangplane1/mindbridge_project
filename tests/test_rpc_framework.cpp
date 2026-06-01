#include "agent_rpc/rpc_framework.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <vector>

using namespace agent_rpc;

class RpcFrameworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 配置测试环境
        config_.server_address = "127.0.0.1:50052";
        config_.log_level = "ERROR";  // 减少测试时的日志输出
        config_.timeout_seconds = 5;
        config_.heartbeat_interval = 10;
        
        // 初始化框架
        framework_ = &RpcFramework::getInstance();
        ASSERT_TRUE(framework_->initialize(config_));
    }
    
    void TearDown() override {
        if (framework_->isRunning()) {
            framework_->stopServer();
        }
    }
    
    RpcConfig config_;
    RpcFramework* framework_;
};

// 测试框架初始化
TEST_F(RpcFrameworkTest, TestInitialization) {
    EXPECT_TRUE(framework_->isRunning() == false);
    EXPECT_NE(framework_->getServer(), nullptr);
    EXPECT_NE(framework_->getClient(), nullptr);
    EXPECT_NE(framework_->getRegistry(), nullptr);
    EXPECT_NE(framework_->getLoadBalancer(), nullptr);
    EXPECT_NE(framework_->getLogger(), nullptr);
    EXPECT_NE(framework_->getMetrics(), nullptr);
}

// 测试服务器启动和停止
TEST_F(RpcFrameworkTest, TestServerStartStop) {
    // 启动服务器
    EXPECT_TRUE(framework_->startServer());
    EXPECT_TRUE(framework_->isRunning());
    
    // 停止服务器
    framework_->stopServer();
    EXPECT_FALSE(framework_->isRunning());
}

// 测试客户端连接
TEST_F(RpcFrameworkTest, TestClientConnection) {
    // 启动服务器
    ASSERT_TRUE(framework_->startServer());
    
    auto client = framework_->getClient();
    ASSERT_NE(client, nullptr);
    
    // 连接到服务器
    EXPECT_TRUE(client->connect(config_.server_address));
    EXPECT_TRUE(client->isConnected());
    
    // 断开连接
    client->disconnect();
    EXPECT_FALSE(client->isConnected());
}

// 测试消息发送和接收
TEST_F(RpcFrameworkTest, TestMessageSendReceive) {
    // 启动服务器
    ASSERT_TRUE(framework_->startServer());
    
    auto client = framework_->getClient();
    ASSERT_NE(client, nullptr);
    
    // 连接到服务器
    ASSERT_TRUE(client->connect(config_.server_address));
    
    // 注册代理
    ServiceEndpoint agent_info;
    agent_info.host = "127.0.0.1";
    agent_info.port = 8080;
    agent_info.service_name = "test_agent";
    agent_info.version = "1.0.0";
    
    std::string agent_id = client->registerAgent(agent_info);
    EXPECT_FALSE(agent_id.empty());
    
    // 创建测试消息
    agent_communication::Message message;
    message.set_id("test_msg_1");
    message.set_type("test");
    message.set_content("Hello, RPC!");
    message.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // 发送消息
    EXPECT_TRUE(client->sendMessage(message, agent_id));
    
    // 接收消息
    auto received_messages = client->receiveMessages(agent_id, 10, 5);
    EXPECT_GE(received_messages.size(), 0);
    
    // 注销代理
    EXPECT_TRUE(client->unregisterAgent(agent_id));
    
    // 断开连接
    client->disconnect();
}

// 测试广播消息
TEST_F(RpcFrameworkTest, TestBroadcastMessage) {
    // 启动服务器
    ASSERT_TRUE(framework_->startServer());
    
    auto client = framework_->getClient();
    ASSERT_NE(client, nullptr);
    
    // 连接到服务器
    ASSERT_TRUE(client->connect(config_.server_address));
    
    // 注册代理
    ServiceEndpoint agent_info;
    agent_info.host = "127.0.0.1";
    agent_info.port = 8080;
    agent_info.service_name = "test_agent";
    agent_info.version = "1.0.0";
    
    std::string agent_id = client->registerAgent(agent_info);
    EXPECT_FALSE(agent_id.empty());
    
    // 创建测试消息
    agent_communication::Message message;
    message.set_id("broadcast_msg_1");
    message.set_type("broadcast");
    message.set_content("Broadcast message");
    message.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    
    // 广播消息
    int success_count = client->broadcastMessage(message);
    EXPECT_GE(success_count, 0);
    
    // 注销代理
    EXPECT_TRUE(client->unregisterAgent(agent_id));
    
    // 断开连接
    client->disconnect();
}

// 测试获取代理列表
TEST_F(RpcFrameworkTest, TestGetAgents) {
    // 启动服务器
    ASSERT_TRUE(framework_->startServer());
    
    auto client = framework_->getClient();
    ASSERT_NE(client, nullptr);
    
    // 连接到服务器
    ASSERT_TRUE(client->connect(config_.server_address));
    
    // 注册代理
    ServiceEndpoint agent_info;
    agent_info.host = "127.0.0.1";
    agent_info.port = 8080;
    agent_info.service_name = "test_agent";
    agent_info.version = "1.0.0";
    
    std::string agent_id = client->registerAgent(agent_info);
    EXPECT_FALSE(agent_id.empty());
    
    // 获取代理列表
    auto agents = client->getAgents();
    EXPECT_GE(agents.size(), 1);
    
    // 查找注册的代理
    bool found = false;
    for (const auto& agent : agents) {
        if (agent.service_name == "test_agent") {
            found = true;
            EXPECT_EQ(agent.host, "127.0.0.1");
            EXPECT_EQ(agent.port, 8080);
            EXPECT_EQ(agent.version, "1.0.0");
            break;
        }
    }
    EXPECT_TRUE(found);
    
    // 注销代理
    EXPECT_TRUE(client->unregisterAgent(agent_id));
    
    // 断开连接
    client->disconnect();
}

// 测试心跳
TEST_F(RpcFrameworkTest, TestHeartbeat) {
    // 启动服务器
    ASSERT_TRUE(framework_->startServer());
    
    auto client = framework_->getClient();
    ASSERT_NE(client, nullptr);
    
    // 连接到服务器
    ASSERT_TRUE(client->connect(config_.server_address));
    
    // 注册代理
    ServiceEndpoint agent_info;
    agent_info.host = "127.0.0.1";
    agent_info.port = 8080;
    agent_info.service_name = "test_agent";
    agent_info.version = "1.0.0";
    
    std::string agent_id = client->registerAgent(agent_info);
    EXPECT_FALSE(agent_id.empty());
    
    // 发送心跳
    EXPECT_TRUE(client->sendHeartbeat(agent_id, agent_info));
    
    // 注销代理
    EXPECT_TRUE(client->unregisterAgent(agent_id));
    
    // 断开连接
    client->disconnect();
}

// 测试负载均衡器
TEST_F(RpcFrameworkTest, TestLoadBalancer) {
    auto load_balancer = framework_->getLoadBalancer();
    ASSERT_NE(load_balancer, nullptr);
    
    // 创建测试端点
    std::vector<ServiceEndpoint> endpoints;
    for (int i = 0; i < 3; ++i) {
        ServiceEndpoint endpoint;
        endpoint.host = "127.0.0.1";
        endpoint.port = 8080 + i;
        endpoint.service_name = "test_service";
        endpoint.version = "1.0.0";
        endpoint.is_healthy = true;
        endpoints.push_back(endpoint);
    }
    
    // 更新端点
    load_balancer->updateEndpoints(endpoints);
    
    // 测试选择端点
    for (int i = 0; i < 10; ++i) {
        ServiceEndpoint selected = load_balancer->selectEndpoint(endpoints);
        EXPECT_TRUE(selected.is_healthy);
        EXPECT_EQ(selected.service_name, "test_service");
    }
}

// 测试熔断器
TEST_F(RpcFrameworkTest, TestCircuitBreaker) {
    auto& manager = CircuitBreakerManager::getInstance();
    
    // 创建熔断器
    auto circuit_breaker = manager.getCircuitBreaker("test_service");
    ASSERT_NE(circuit_breaker, nullptr);
    
    // 初始状态应该是关闭的
    EXPECT_EQ(circuit_breaker->getState(), CircuitState::CLOSED);
    
    // 记录一些失败
    for (int i = 0; i < 10; ++i) {
        circuit_breaker->recordFailure();
    }
    
    // 检查状态
    auto stats = circuit_breaker->getStats();
    EXPECT_EQ(stats.failed_requests, 10);
    
    // 重置熔断器
    circuit_breaker->reset();
    EXPECT_EQ(circuit_breaker->getState(), CircuitState::CLOSED);
}

// 测试日志系统
TEST_F(RpcFrameworkTest, TestLogger) {
    auto logger = framework_->getLogger();
    ASSERT_NE(logger, nullptr);
    
    // 测试不同级别的日志
    LOG_TRACE("This is a trace message");
    LOG_DEBUG("This is a debug message");
    LOG_INFO("This is an info message");
    LOG_WARN("This is a warning message");
    LOG_ERROR("This is an error message");
    LOG_FATAL("This is a fatal message");
    
    // 测试带字段的日志
    std::map<std::string, std::string> fields;
    fields["key1"] = "value1";
    fields["key2"] = "value2";
    LOG_INFO_FIELDS("This is a message with fields", fields);
}

// 测试监控指标
TEST_F(RpcFrameworkTest, TestMetrics) {
    auto metrics = framework_->getMetrics();
    ASSERT_NE(metrics, nullptr);
    
    // 记录一些指标
    metrics->recordRpcRequest("test_service", "test_method", 100.5);
    metrics->recordRpcResponse("test_service", "test_method", 0);
    metrics->recordRpcError("test_service", "test_method", "timeout");
    metrics->recordConnection("test_service", true);
    metrics->recordMessageSent("test", 1024);
    metrics->recordMemoryUsage(1024 * 1024);
    metrics->recordCpuUsage(50.0);
    
    // 导出指标
    std::string prometheus_output = metrics->exportPrometheus();
    EXPECT_FALSE(prometheus_output.empty());
    
    std::string json_output = metrics->exportJson();
    EXPECT_FALSE(json_output.empty());
}

// 主函数
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
