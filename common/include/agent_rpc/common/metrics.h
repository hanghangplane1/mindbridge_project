#pragma once

#include "types.h"
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <map>

namespace agent_rpc {
namespace common {

// 指标类型
enum class MetricType {
    COUNTER,    // 计数器
    GAUGE,      // 仪表盘
    HISTOGRAM,  // 直方图
    SUMMARY      // 摘要
};

// 指标值
struct MetricValue {
    double value;
    std::chrono::system_clock::time_point timestamp;
    std::map<std::string, std::string> labels;
};

// 指标接口
class Metric {
public:
    virtual ~Metric() = default;
    virtual MetricType getType() const = 0;
    virtual std::string getName() const = 0;
    virtual std::string getDescription() const = 0;
    virtual std::map<std::string, std::string> getLabels() const = 0;
    virtual std::string toString() const = 0;
};

// 计数器指标
class CounterMetric : public Metric {
public:
    CounterMetric(const std::string& name, 
                  const std::string& description = "",
                  const std::map<std::string, std::string>& labels = {});
    ~CounterMetric() = default;
    
    MetricType getType() const override { return MetricType::COUNTER; }
    std::string getName() const override { return name_; }
    std::string getDescription() const override { return description_; }
    std::map<std::string, std::string> getLabels() const override { return labels_; }
    std::string toString() const override;
    
    void increment(double value = 1.0);
    void set(double value);
    double get() const { return value_.load(); }
    void reset();

private:
    std::string name_;
    std::string description_;
    std::map<std::string, std::string> labels_;
    std::atomic<double> value_{0.0};
};

// 仪表盘指标
class GaugeMetric : public Metric {
public:
    GaugeMetric(const std::string& name, 
                const std::string& description = "",
                const std::map<std::string, std::string>& labels = {});
    ~GaugeMetric() = default;
    
    MetricType getType() const override { return MetricType::GAUGE; }
    std::string getName() const override { return name_; }
    std::string getDescription() const override { return description_; }
    std::map<std::string, std::string> getLabels() const override { return labels_; }
    std::string toString() const override;
    
    void set(double value);
    void increment(double value = 1.0);
    void decrement(double value = 1.0);
    double get() const { return value_.load(); }
    void reset();

private:
    std::string name_;
    std::string description_;
    std::map<std::string, std::string> labels_;
    std::atomic<double> value_{0.0};
};

// 监控指标管理器
class Metrics {
public:
    static Metrics& getInstance();
    
    // 初始化
    void initialize();
    
    // RPC相关指标
    void recordRpcRequest(const std::string& service, const std::string& method, double duration_ms);
    void recordRpcResponse(const std::string& service, const std::string& method, int status_code);
    void recordRpcError(const std::string& service, const std::string& method, const std::string& error_type);
    
    // 连接相关指标
    void recordConnection(const std::string& service, bool success);
    void recordDisconnection(const std::string& service);
    
    // 消息相关指标
    void recordMessageSent(const std::string& message_type, size_t size);
    void recordMessageReceived(const std::string& message_type, size_t size);
    
    // 系统指标
    void recordMemoryUsage(size_t bytes);
    void recordCpuUsage(double percentage);
    
    // 导出指标
    std::string exportPrometheus() const;
    std::string exportJson() const;

private:
    Metrics() = default;
    ~Metrics() = default;
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;
    
    void initializeDefaultMetrics();
    
    std::shared_ptr<CounterMetric> rpc_request_counter_;
    std::shared_ptr<CounterMetric> rpc_error_counter_;
    std::shared_ptr<GaugeMetric> active_connections_gauge_;
    std::shared_ptr<CounterMetric> message_counter_;
    std::shared_ptr<GaugeMetric> memory_usage_gauge_;
    std::shared_ptr<GaugeMetric> cpu_usage_gauge_;
};

} // namespace common
} // namespace agent_rpc
