#pragma once

#include <string>
#include <map>
#include <atomic>
#include <mutex>
#include <chrono>
#include <vector>
#include <memory>
#include <thread>
#include <queue>
#include <condition_variable>

namespace agent_rpc {

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

// 直方图桶
struct HistogramBucket {
    double upper_bound;
    std::atomic<uint64_t> count{0};
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

// 直方图指标
class HistogramMetric : public Metric {
public:
    HistogramMetric(const std::string& name, 
                    const std::string& description = "",
                    const std::vector<double>& buckets = {0.005, 0.01, 0.025, 0.05, 0.075, 0.1, 0.25, 0.5, 0.75, 1.0, 2.5, 5.0, 7.5, 10.0, INFINITY},
                    const std::map<std::string, std::string>& labels = {});
    ~HistogramMetric() = default;
    
    MetricType getType() const override { return MetricType::HISTOGRAM; }
    std::string getName() const override { return name_; }
    std::string getDescription() const override { return description_; }
    std::map<std::string, std::string> getLabels() const override { return labels_; }
    std::string toString() const override;
    
    void observe(double value);
    double getSum() const { return sum_.load(); }
    uint64_t getCount() const { return count_.load(); }
    std::vector<HistogramBucket> getBuckets() const;
    void reset();

private:
    std::string name_;
    std::string description_;
    std::map<std::string, std::string> labels_;
    std::vector<HistogramBucket> buckets_;
    std::atomic<double> sum_{0.0};
    std::atomic<uint64_t> count_{0};
    mutable std::mutex buckets_mutex_;
};

// 摘要指标
class SummaryMetric : public Metric {
public:
    SummaryMetric(const std::string& name, 
                  const std::string& description = "",
                  const std::vector<double>& quantiles = {0.5, 0.9, 0.95, 0.99},
                  const std::map<std::string, std::string>& labels = {});
    ~SummaryMetric() = default;
    
    MetricType getType() const override { return MetricType::SUMMARY; }
    std::string getName() const override { return name_; }
    std::string getDescription() const override { return description_; }
    std::map<std::string, std::string> getLabels() const override { return labels_; }
    std::string toString() const override;
    
    void observe(double value);
    double getSum() const { return sum_.load(); }
    uint64_t getCount() const { return count_.load(); }
    std::map<double, double> getQuantiles() const;
    void reset();

private:
    void updateQuantiles();
    
    std::string name_;
    std::string description_;
    std::map<std::string, std::string> labels_;
    std::vector<double> quantiles_;
    std::vector<double> values_;
    std::map<double, double> quantile_values_;
    std::atomic<double> sum_{0.0};
    std::atomic<uint64_t> count_{0};
    mutable std::mutex values_mutex_;
};

// 指标收集器
class MetricsCollector {
public:
    static MetricsCollector& getInstance();
    
    // 创建指标
    std::shared_ptr<CounterMetric> createCounter(const std::string& name, 
                                                 const std::string& description = "",
                                                 const std::map<std::string, std::string>& labels = {});
    
    std::shared_ptr<GaugeMetric> createGauge(const std::string& name, 
                                             const std::string& description = "",
                                             const std::map<std::string, std::string>& labels = {});
    
    std::shared_ptr<HistogramMetric> createHistogram(const std::string& name, 
                                                     const std::string& description = "",
                                                     const std::vector<double>& buckets = {},
                                                     const std::map<std::string, std::string>& labels = {});
    
    std::shared_ptr<SummaryMetric> createSummary(const std::string& name, 
                                                 const std::string& description = "",
                                                 const std::vector<double>& quantiles = {},
                                                 const std::map<std::string, std::string>& labels = {});
    
    // 获取指标
    std::shared_ptr<Metric> getMetric(const std::string& name);
    std::vector<std::shared_ptr<Metric>> getAllMetrics();
    
    // 移除指标
    void removeMetric(const std::string& name);
    
    // 导出为Prometheus格式
    std::string exportPrometheus() const;
    
    // 导出为JSON格式
    std::string exportJson() const;

private:
    MetricsCollector() = default;
    ~MetricsCollector() = default;
    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;
    
    std::string generateMetricKey(const std::string& name, 
                                 const std::map<std::string, std::string>& labels) const;
    
    mutable std::mutex metrics_mutex_;
    std::map<std::string, std::shared_ptr<Metric>> metrics_;
};

// 指标管理器
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
    
    // 获取指标
    std::shared_ptr<CounterMetric> getRpcRequestCounter();
    std::shared_ptr<HistogramMetric> getRpcDurationHistogram();
    std::shared_ptr<CounterMetric> getRpcErrorCounter();
    std::shared_ptr<GaugeMetric> getActiveConnectionsGauge();
    std::shared_ptr<CounterMetric> getMessageCounter();
    std::shared_ptr<GaugeMetric> getMemoryUsageGauge();
    std::shared_ptr<GaugeMetric> getCpuUsageGauge();
    
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
    std::shared_ptr<HistogramMetric> rpc_duration_histogram_;
    std::shared_ptr<CounterMetric> rpc_error_counter_;
    std::shared_ptr<GaugeMetric> active_connections_gauge_;
    std::shared_ptr<CounterMetric> message_counter_;
    std::shared_ptr<GaugeMetric> memory_usage_gauge_;
    std::shared_ptr<GaugeMetric> cpu_usage_gauge_;
};

} // namespace agent_rpc
