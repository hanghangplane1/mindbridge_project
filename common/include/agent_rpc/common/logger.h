#pragma once

#include "types.h"
#include <string>
#include <memory>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <map>

namespace agent_rpc {
namespace common {

// 日志级别
// 注意: 使用 Level_ 前缀避免与系统宏冲突 (如 DEBUG, ERROR)
enum class LogLevel {
    Level_TRACE = 0,
    Level_DEBUG = 1,
    Level_INFO = 2,
    Level_WARN = 3,
    Level_ERROR = 4,
    Level_FATAL = 5
};

// 日志配置
struct LogConfig {
    LogLevel level = LogLevel::Level_INFO;
    std::string log_file = "";
    bool console_output = true;
    bool file_output = false;
    bool async_logging = true;
    size_t max_file_size = 100 * 1024 * 1024; // 100MB
    int max_files = 10;
    std::string log_format = "[%Y-%m-%d %H:%M:%S.%f] [%l] [%t] [%s:%n] %v";
    bool color_output = true;
};

// 日志条目
struct LogEntry {
    LogLevel level;
    std::string message;
    std::string source_file;
    int line_number;
    std::string function_name;
    std::thread::id thread_id;
    std::chrono::system_clock::time_point timestamp;
    std::map<std::string, std::string> fields;
};

// 日志器接口
class Logger {
public:
    virtual ~Logger() = default;
    
    virtual void trace(const std::string& message, 
                      const std::string& source_file = "", 
                      int line_number = 0,
                      const std::string& function_name = "") = 0;
    
    virtual void debug(const std::string& message, 
                      const std::string& source_file = "", 
                      int line_number = 0,
                      const std::string& function_name = "") = 0;
    
    virtual void info(const std::string& message, 
                     const std::string& source_file = "", 
                     int line_number = 0,
                     const std::string& function_name = "") = 0;
    
    virtual void warn(const std::string& message, 
                     const std::string& source_file = "", 
                     int line_number = 0,
                     const std::string& function_name = "") = 0;
    
    virtual void error(const std::string& message, 
                      const std::string& source_file = "", 
                      int line_number = 0,
                      const std::string& function_name = "") = 0;
    
    virtual void fatal(const std::string& message, 
                      const std::string& source_file = "", 
                      int line_number = 0,
                      const std::string& function_name = "") = 0;
    
    virtual void flush() = 0;
};

// 全局日志函数声明
void logTrace(const std::string& message);
void logDebug(const std::string& message);
void logInfo(const std::string& message);
void logWarn(const std::string& message);
void logError(const std::string& message);
void logFatal(const std::string& message);

// 便利宏 - 直接调用全局函数
#define LOG_TRACE(msg) agent_rpc::common::logTrace(msg)
#define LOG_DEBUG(msg) agent_rpc::common::logDebug(msg)
#define LOG_INFO(msg) agent_rpc::common::logInfo(msg)
#define LOG_WARN(msg) agent_rpc::common::logWarn(msg)
#define LOG_ERROR(msg) agent_rpc::common::logError(msg)
#define LOG_FATAL(msg) agent_rpc::common::logFatal(msg)

} // namespace common
} // namespace agent_rpc
