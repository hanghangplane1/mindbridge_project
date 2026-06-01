#include "agent_rpc/metrics.h"
#include "agent_rpc/logger.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <numeric>

namespace agent_rpc {

// CounterMetric 实现
CounterMetric::CounterMetric(const std::string& name, 
                            const std::string& description,
                            const std::map<std::string, std::string>& labels)
    : name_(name)
    , description_(description)
    , labels_(labels) {
}

std::string CounterMetric::toString() const {
    std::stringstream ss;
    ss << "# HELP " << name_ << " " << description_ << std::endl;
    ss << "# TYPE " << name_ << " counter" << std::endl;
    
    ss << name_;
    if (!labels_.empty()) {
        ss << "{";
        bool first = true;
        for (const auto& pair : labels_) {
            if (!first) ss << ",";
            ss << pair.first << "=\"" << pair.second << "\"";
            first = false;
        }
        ss << "}";
    }
    ss << " " << value_.load() << std::endl;
    
    return ss.str();
}

void CounterMetric::increment(double value) {
    value_.fetch_add(value);
}

void CounterMetric::set(double value) {
    value_.store(value);
}

void CounterMetric::reset() {
    value_.store(0.0);
}

// GaugeMetric 实现
GaugeMetric::GaugeMetric(const std::string& name, 
                        const std::string& description,
                        const std::map<std::string, std::string>& labels)
    : name_(name)
    , description_(description)
    , labels_(labels) {
}

std::string GaugeMetric::toString() const {
    std::stringstream ss;
    ss << "# HELP " << name_ << " " << description_ << std::endl;
    ss << "# TYPE " << name_ << " gauge" << std::endl;
    
    ss << name_;
    if (!labels_.empty()) {
        ss << "{";
        bool first = true;
        for (const auto& pair : labels_) {
            if (!first) ss << ",";
            ss << pair.first << "=\"" << pair.second << "\"";
            first = false;
        }
        ss << "}";
    }
    ss << " " << value_.load() << std::endl;
    
    return ss.str();
}

void GaugeMetric::set(double value) {
    value_.store(value);
}

void GaugeMetric::increment(double value) {
    value_.fetch_add(value);
}

void GaugeMetric::decrement(double value) {
    value_.fetch_sub(value);
}

void GaugeMetric::reset() {
    value_.store(0.0);
}

// HistogramMetric 实现
HistogramMetric::HistogramMetric(const std::string& name, 
                                const std::string& description,
                                const std::vector<double>& buckets,
                                const std::map<std::string, std::string>& labels)
    : name_(name)
    , description_(description)
    , labels_(labels) {
    
    for (double bucket : buckets) {
        HistogramBucket b;
        b.upper_bound = bucket;
        buckets_.push_back(b);
    }
}

std::string HistogramMetric::toString() const {
    std::stringstream ss;
    ss << "# HELP " << name_ << " " << description_ << std::endl;
    ss << "# TYPE " << name_ << " histogram" << std::endl;
    
    // 输出桶计数
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    for (const auto& bucket : buckets_) {
        ss << name_ << "_bucket{le=\"" << bucket.upper_bound << "\"";
        if (!labels_.empty()) {
            for (const auto& pair : labels_) {
                ss << "," << pair.first << "=\"" << pair.second << "\"";
            }
        }
        ss << "} " << bucket.count.load() << std::endl;
    }
    
    // 输出总数和总和
    ss << name_ << "_count";
    if (!labels_.empty()) {
        ss << "{";
        bool first = true;
        for (const auto& pair : labels_) {
            if (!first) ss << ",";
            ss << pair.first << "=\"" << pair.second << "\"";
            first = false;
        }
        ss << "}";
    }
    ss << " " << count_.load() << std::endl;
    
    ss << name_ << "_sum";
    if (!labels_.empty()) {
        ss << "{";
        bool first = true;
        for (const auto& pair : labels_) {
            if (!first) ss << ",";
            ss << pair.first << "=\"" << pair.second << "\"";
            first = false;
        }
        ss << "}";
    }
    ss << " " << sum_.load() << std::endl;
    
    return ss.str();
}

void HistogramMetric::observe(double value) {
    sum_.fetch_add(value);
    count_.fetch_add(1);
    
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    for (auto& bucket : buckets_) {
        if (value <= bucket.upper_bound) {
            bucket.count.fetch_add(1);
        }
    }
}

std::vector<HistogramBucket> HistogramMetric::getBuckets() const {
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    return buckets_;
}

void HistogramMetric::reset() {
    sum_.store(0.0);
    count_.store(0);
    
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    for (auto& bucket : buckets_) {
        bucket.count.store(0);
    }
}

// SummaryMetric 实现
SummaryMetric::SummaryMetric(const std::string& name, 
                            const std::string& description,
                            const std::vector<double>& quantiles,
                            const std::map<std::string, std::string>& labels)
    : name_(name)
    , description_(description)
    , labels_(labels)
    , quantiles_(quantiles) {
}

std::string SummaryMetric::toString() const {
    std::stringstream ss;
    ss << "# HELP " << name_ << " " << description_ << std::endl;
    ss << "# TYPE " << name_ << " summary" << std::endl;
    
    // 输出分位数
    std::lock_guard<std::mutex> lock(values_mutex_);
    for (const auto& pair : quantile_values_) {
        ss << name_ << "{quantile=\"" << pair.first << "\"";
        if (!labels_.empty()) {
            for (const auto& label_pair : labels_) {
                ss << "," << label_pair.first << "=\"" << label_pair.second << "\"";
            }
        }
        ss << "} " << pair.second << std::endl;
    }
    
    // 输出总数和总和
    ss << name_ << "_count";
    if (!labels_.empty()) {
        ss << "{";
        bool first = true;
        for (const auto& pair : labels_) {
            if (!first) ss << ",";
            ss << pair.first << "=\"" << pair.second << "\"";
            first = false;
        }
        ss << "}";
    }
    ss << " " << count_.load() << std::endl;
    
    ss << name_ << "_sum";
    if (!labels_.empty()) {
        ss << "{";
        bool first = true;
        for (const auto& pair : labels_) {
            if (!first) ss << ",";
            ss << pair.first << "=\"" << pair.second << "\"";
            first = false;
        }
        ss << "}";
    }
    ss << " " << sum_.load() << std::endl;
    
    return ss.str();
}

void SummaryMetric::observe(double value) {
    sum_.fetch_add(value);
    count_.fetch_add(1);
    
    std::lock_guard<std::mutex> lock(values_mutex_);
    values_.push_back(value);
    
    // 保持最近1000个值
    if (values_.size() > 1000) {
        values_.erase(values_.begin());
    }
    
    updateQuantiles();
}

std::map<double, double> SummaryMetric::getQuantiles() const {
    std::lock_guard<std::mutex> lock(values_mutex_);
    return quantile_values_;
}

void SummaryMetric::reset() {
    sum_.store(0.0);
    count_.store(0);
    
    std::lock_guard<std::mutex> lock(values_mutex_);
    values_.clear();
    quantile_values_.clear();
}

void SummaryMetric::updateQuantiles() {
    if (values_.empty()) {
        return;
    }
    
    std::vector<double> sorted_values = values_;
    std::sort(sorted_values.begin(), sorted_values.end());
    
    quantile_values_.clear();
    for (double quantile : quantiles_) {
        if (quantile < 0.0 || quantile > 1.0) {
            continue;
        }
        
        size_t index = static_cast<size_t>(quantile * (sorted_values.size() - 1));
        if (index >= sorted_values.size()) {
            index = sorted_values.size() - 1;
        }
        
        quantile_values_[quantile] = sorted_values[index];
    }
}

// MetricsCollector 实现
MetricsCollector& MetricsCollector::getInstance() {
    static MetricsCollector instance;
    return instance;
}

std::shared_ptr<CounterMetric> MetricsCollector::createCounter(const std::string& name, 
                                                              const std::string& description,
                                                              const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::string key = generateMetricKey(name, labels);
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        return std::static_pointer_cast<CounterMetric>(it->second);
    }
    
    auto metric = std::make_shared<CounterMetric>(name, description, labels);
    metrics_[key] = metric;
    
    return metric;
}

std::shared_ptr<GaugeMetric> MetricsCollector::createGauge(const std::string& name, 
                                                          const std::string& description,
                                                          const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::string key = generateMetricKey(name, labels);
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        return std::static_pointer_cast<GaugeMetric>(it->second);
    }
    
    auto metric = std::make_shared<GaugeMetric>(name, description, labels);
    metrics_[key] = metric;
    
    return metric;
}

std::shared_ptr<HistogramMetric> MetricsCollector::createHistogram(const std::string& name, 
                                                                  const std::string& description,
                                                                  const std::vector<double>& buckets,
                                                                  const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::string key = generateMetricKey(name, labels);
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        return std::static_pointer_cast<HistogramMetric>(it->second);
    }
    
    auto metric = std::make_shared<HistogramMetric>(name, description, buckets, labels);
    metrics_[key] = metric;
    
    return metric;
}

std::shared_ptr<SummaryMetric> MetricsCollector::createSummary(const std::string& name, 
                                                              const std::string& description,
                                                              const std::vector<double>& quantiles,
                                                              const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::string key = generateMetricKey(name, labels);
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        return std::static_pointer_cast<SummaryMetric>(it->second);
    }
    
    auto metric = std::make_shared<SummaryMetric>(name, description, quantiles, labels);
    metrics_[key] = metric;
    
    return metric;
}

std::shared_ptr<Metric> MetricsCollector::getMetric(const std::string& name) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    for (const auto& pair : metrics_) {
        if (pair.second->getName() == name) {
            return pair.second;
        }
    }
    
    return nullptr;
}

std::vector<std::shared_ptr<Metric>> MetricsCollector::getAllMetrics() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::vector<std::shared_ptr<Metric>> result;
    for (const auto& pair : metrics_) {
        result.push_back(pair.second);
    }
    
    return result;
}

void MetricsCollector::removeMetric(const std::string& name) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto it = metrics_.begin();
    while (it != metrics_.end()) {
        if (it->second->getName() == name) {
            it = metrics_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string MetricsCollector::exportPrometheus() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::stringstream ss;
    for (const auto& pair : metrics_) {
        ss << pair.second->toString() << std::endl;
    }
    
    return ss.str();
}

std::string MetricsCollector::exportJson() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::stringstream ss;
    ss << "{" << std::endl;
    ss << "  \"metrics\": [" << std::endl;
    
    bool first = true;
    for (const auto& pair : metrics_) {
        if (!first) ss << "," << std::endl;
        ss << "    {" << std::endl;
        ss << "      \"name\": \"" << pair.second->getName() << "\"," << std::endl;
        ss << "      \"type\": \"" << static_cast<int>(pair.second->getType()) << "\"," << std::endl;
        ss << "      \"description\": \"" << pair.second->getDescription() << "\"," << std::endl;
        ss << "      \"labels\": {";
        
        bool label_first = true;
        for (const auto& label_pair : pair.second->getLabels()) {
            if (!label_first) ss << ",";
            ss << "\"" << label_pair.first << "\": \"" << label_pair.second << "\"";
            label_first = false;
        }
        ss << "}" << std::endl;
        ss << "    }";
        first = false;
    }
    
    ss << std::endl << "  ]" << std::endl;
    ss << "}" << std::endl;
    
    return ss.str();
}

std::string MetricsCollector::generateMetricKey(const std::string& name, 
                                               const std::map<std::string, std::string>& labels) const {
    std::string key = name;
    for (const auto& pair : labels) {
        key += "|" + pair.first + "=" + pair.second;
    }
    return key;
}

// Metrics 实现
Metrics& Metrics::getInstance() {
    static Metrics instance;
    return instance;
}

void Metrics::initialize() {
    initializeDefaultMetrics();
    LOG_INFO("Metrics system initialized");
}

void Metrics::recordRpcRequest(const std::string& service, const std::string& method, double duration_ms) {
    if (!rpc_request_counter_) {
        rpc_request_counter_ = MetricsCollector::getInstance().createCounter(
            "rpc_requests_total", "Total number of RPC requests");
    }
    
    rpc_request_counter_->increment();
    
    if (!rpc_duration_histogram_) {
        rpc_duration_histogram_ = MetricsCollector::getInstance().createHistogram(
            "rpc_duration_ms", "RPC request duration in milliseconds",
            {0.1, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0});
    }
    
    rpc_duration_histogram_->observe(duration_ms);
}

void Metrics::recordRpcResponse(const std::string& service, const std::string& method, int status_code) {
    // 可以添加状态码相关的指标
}

void Metrics::recordRpcError(const std::string& service, const std::string& method, const std::string& error_type) {
    if (!rpc_error_counter_) {
        rpc_error_counter_ = MetricsCollector::getInstance().createCounter(
            "rpc_errors_total", "Total number of RPC errors");
    }
    
    rpc_error_counter_->increment();
}

void Metrics::recordConnection(const std::string& service, bool success) {
    if (success) {
        if (!active_connections_gauge_) {
            active_connections_gauge_ = MetricsCollector::getInstance().createGauge(
                "active_connections", "Number of active connections");
        }
        active_connections_gauge_->increment();
    }
}

void Metrics::recordDisconnection(const std::string& service) {
    if (active_connections_gauge_) {
        active_connections_gauge_->decrement();
    }
}

void Metrics::recordMessageSent(const std::string& message_type, size_t size) {
    if (!message_counter_) {
        message_counter_ = MetricsCollector::getInstance().createCounter(
            "messages_sent_total", "Total number of messages sent");
    }
    
    message_counter_->increment();
}

void Metrics::recordMessageReceived(const std::string& message_type, size_t size) {
    if (!message_counter_) {
        message_counter_ = MetricsCollector::getInstance().createCounter(
            "messages_received_total", "Total number of messages received");
    }
    
    message_counter_->increment();
}

void Metrics::recordMemoryUsage(size_t bytes) {
    if (!memory_usage_gauge_) {
        memory_usage_gauge_ = MetricsCollector::getInstance().createGauge(
            "memory_usage_bytes", "Memory usage in bytes");
    }
    
    memory_usage_gauge_->set(bytes);
}

void Metrics::recordCpuUsage(double percentage) {
    if (!cpu_usage_gauge_) {
        cpu_usage_gauge_ = MetricsCollector::getInstance().createGauge(
            "cpu_usage_percent", "CPU usage percentage");
    }
    
    cpu_usage_gauge_->set(percentage);
}

std::shared_ptr<CounterMetric> Metrics::getRpcRequestCounter() {
    return rpc_request_counter_;
}

std::shared_ptr<HistogramMetric> Metrics::getRpcDurationHistogram() {
    return rpc_duration_histogram_;
}

std::shared_ptr<CounterMetric> Metrics::getRpcErrorCounter() {
    return rpc_error_counter_;
}

std::shared_ptr<GaugeMetric> Metrics::getActiveConnectionsGauge() {
    return active_connections_gauge_;
}

std::shared_ptr<CounterMetric> Metrics::getMessageCounter() {
    return message_counter_;
}

std::shared_ptr<GaugeMetric> Metrics::getMemoryUsageGauge() {
    return memory_usage_gauge_;
}

std::shared_ptr<GaugeMetric> Metrics::getCpuUsageGauge() {
    return cpu_usage_gauge_;
}

std::string Metrics::exportPrometheus() const {
    return MetricsCollector::getInstance().exportPrometheus();
}

std::string Metrics::exportJson() const {
    return MetricsCollector::getInstance().exportJson();
}

void Metrics::initializeDefaultMetrics() {
    // 初始化默认指标
    rpc_request_counter_ = MetricsCollector::getInstance().createCounter(
        "rpc_requests_total", "Total number of RPC requests");
    
    rpc_duration_histogram_ = MetricsCollector::getInstance().createHistogram(
        "rpc_duration_ms", "RPC request duration in milliseconds",
        {0.1, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0});
    
    rpc_error_counter_ = MetricsCollector::getInstance().createCounter(
        "rpc_errors_total", "Total number of RPC errors");
    
    active_connections_gauge_ = MetricsCollector::getInstance().createGauge(
        "active_connections", "Number of active connections");
    
    message_counter_ = MetricsCollector::getInstance().createCounter(
        "messages_total", "Total number of messages");
    
    memory_usage_gauge_ = MetricsCollector::getInstance().createGauge(
        "memory_usage_bytes", "Memory usage in bytes");
    
    cpu_usage_gauge_ = MetricsCollector::getInstance().createGauge(
        "cpu_usage_percent", "CPU usage percentage");
}

} // namespace agent_rpc
