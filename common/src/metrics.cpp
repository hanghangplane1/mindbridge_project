/**
 * @file metrics.cpp
 * @brief Metrics implementation
 */

#include "agent_rpc/common/metrics.h"
#include <sstream>

namespace agent_rpc {
namespace common {

// CounterMetric implementation
CounterMetric::CounterMetric(const std::string& name, 
                             const std::string& description,
                             const std::map<std::string, std::string>& labels)
    : name_(name), description_(description), labels_(labels) {}

std::string CounterMetric::toString() const {
    std::ostringstream oss;
    oss << name_ << " " << value_.load();
    return oss.str();
}

void CounterMetric::increment(double value) {
    double current = value_.load();
    while (!value_.compare_exchange_weak(current, current + value)) {}
}

void CounterMetric::set(double value) {
    value_.store(value);
}

void CounterMetric::reset() {
    value_.store(0.0);
}

// GaugeMetric implementation
GaugeMetric::GaugeMetric(const std::string& name, 
                         const std::string& description,
                         const std::map<std::string, std::string>& labels)
    : name_(name), description_(description), labels_(labels) {}

std::string GaugeMetric::toString() const {
    std::ostringstream oss;
    oss << name_ << " " << value_.load();
    return oss.str();
}

void GaugeMetric::set(double value) {
    value_.store(value);
}

void GaugeMetric::increment(double value) {
    double current = value_.load();
    while (!value_.compare_exchange_weak(current, current + value)) {}
}

void GaugeMetric::decrement(double value) {
    double current = value_.load();
    while (!value_.compare_exchange_weak(current, current - value)) {}
}

void GaugeMetric::reset() {
    value_.store(0.0);
}

// Metrics implementation
Metrics& Metrics::getInstance() {
    static Metrics instance;
    return instance;
}

void Metrics::initialize() {
    initializeDefaultMetrics();
}

void Metrics::initializeDefaultMetrics() {
    rpc_request_counter_ = std::make_shared<CounterMetric>("rpc_requests_total", "Total RPC requests");
    rpc_error_counter_ = std::make_shared<CounterMetric>("rpc_errors_total", "Total RPC errors");
    active_connections_gauge_ = std::make_shared<GaugeMetric>("active_connections", "Active connections");
    message_counter_ = std::make_shared<CounterMetric>("messages_total", "Total messages");
    memory_usage_gauge_ = std::make_shared<GaugeMetric>("memory_usage_bytes", "Memory usage in bytes");
    cpu_usage_gauge_ = std::make_shared<GaugeMetric>("cpu_usage_percent", "CPU usage percentage");
}

void Metrics::recordRpcRequest(const std::string& service, const std::string& method, double duration_ms) {
    (void)service;
    (void)method;
    (void)duration_ms;
    if (rpc_request_counter_) {
        rpc_request_counter_->increment();
    }
}

void Metrics::recordRpcResponse(const std::string& service, const std::string& method, int status_code) {
    (void)service;
    (void)method;
    (void)status_code;
}

void Metrics::recordRpcError(const std::string& service, const std::string& method, const std::string& error_type) {
    (void)service;
    (void)method;
    (void)error_type;
    if (rpc_error_counter_) {
        rpc_error_counter_->increment();
    }
}

void Metrics::recordConnection(const std::string& service, bool success) {
    (void)service;
    if (success && active_connections_gauge_) {
        active_connections_gauge_->increment();
    }
}

void Metrics::recordDisconnection(const std::string& service) {
    (void)service;
    if (active_connections_gauge_) {
        active_connections_gauge_->decrement();
    }
}

void Metrics::recordMessageSent(const std::string& message_type, size_t size) {
    (void)message_type;
    (void)size;
    if (message_counter_) {
        message_counter_->increment();
    }
}

void Metrics::recordMessageReceived(const std::string& message_type, size_t size) {
    (void)message_type;
    (void)size;
    if (message_counter_) {
        message_counter_->increment();
    }
}

void Metrics::recordMemoryUsage(size_t bytes) {
    if (memory_usage_gauge_) {
        memory_usage_gauge_->set(static_cast<double>(bytes));
    }
}

void Metrics::recordCpuUsage(double percentage) {
    if (cpu_usage_gauge_) {
        cpu_usage_gauge_->set(percentage);
    }
}

std::string Metrics::exportPrometheus() const {
    std::ostringstream oss;
    if (rpc_request_counter_) oss << rpc_request_counter_->toString() << "\n";
    if (rpc_error_counter_) oss << rpc_error_counter_->toString() << "\n";
    if (active_connections_gauge_) oss << active_connections_gauge_->toString() << "\n";
    if (message_counter_) oss << message_counter_->toString() << "\n";
    if (memory_usage_gauge_) oss << memory_usage_gauge_->toString() << "\n";
    if (cpu_usage_gauge_) oss << cpu_usage_gauge_->toString() << "\n";
    return oss.str();
}

std::string Metrics::exportJson() const {
    std::ostringstream oss;
    oss << "{";
    if (rpc_request_counter_) oss << "\"rpc_requests\":" << rpc_request_counter_->get() << ",";
    if (rpc_error_counter_) oss << "\"rpc_errors\":" << rpc_error_counter_->get() << ",";
    if (active_connections_gauge_) oss << "\"active_connections\":" << active_connections_gauge_->get();
    oss << "}";
    return oss.str();
}

} // namespace common
} // namespace agent_rpc
