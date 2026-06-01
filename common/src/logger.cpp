#include "agent_rpc/common/logger.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace agent_rpc {
namespace common {

// 简单的Logger实现
class SimpleLogger : public Logger {
public:
    SimpleLogger() : log_level_(LogLevel::Level_INFO), console_output_(true) {}
    
    void trace(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "") override {
        log(LogLevel::Level_TRACE, message, source_file, line_number, function_name);
    }
    
    void debug(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "") override {
        log(LogLevel::Level_DEBUG, message, source_file, line_number, function_name);
    }
    
    void info(const std::string& message, 
              const std::string& source_file = "", 
              int line_number = 0,
              const std::string& function_name = "") override {
        log(LogLevel::Level_INFO, message, source_file, line_number, function_name);
    }
    
    void warn(const std::string& message, 
              const std::string& source_file = "", 
              int line_number = 0,
              const std::string& function_name = "") override {
        log(LogLevel::Level_WARN, message, source_file, line_number, function_name);
    }
    
    void error(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "") override {
        log(LogLevel::Level_ERROR, message, source_file, line_number, function_name);
    }
    
    void fatal(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "") override {
        log(LogLevel::Level_FATAL, message, source_file, line_number, function_name);
    }
    
    void flush() override {
        std::cout.flush();
    }
    
    void setLogLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_level_ = level;
    }
    
    void setLogFile(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_file_ = filename;
        log_to_file_ = !filename.empty();
    }

private:
    void log(LogLevel level, const std::string& message, 
             const std::string& source_file, int line_number, const std::string& function_name) {
        if (level < log_level_) {
            return;
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 获取当前时间
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        // 格式化时间
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        
        // 获取日志级别字符串
        std::string level_str;
        switch (level) {
            case LogLevel::Level_TRACE: level_str = "TRACE"; break;
            case LogLevel::Level_DEBUG: level_str = "DEBUG"; break;
            case LogLevel::Level_INFO:  level_str = "INFO "; break;
            case LogLevel::Level_WARN:  level_str = "WARN "; break;
            case LogLevel::Level_ERROR: level_str = "ERROR"; break;
            case LogLevel::Level_FATAL: level_str = "FATAL"; break;
        }
        
        // 构建日志消息
        std::string log_message = "[" + ss.str() + "] [" + level_str + "] " + message;
        if (!source_file.empty()) {
            log_message += " [" + source_file;
            if (line_number > 0) {
                log_message += ":" + std::to_string(line_number);
            }
            if (!function_name.empty()) {
                log_message += " " + function_name;
            }
            log_message += "]";
        }
        
        // 输出到控制台
        if (console_output_) {
            std::cout << log_message << std::endl;
        }
        
        // 输出到文件
        if (log_to_file_) {
            std::ofstream file(log_file_, std::ios::app);
            if (file.is_open()) {
                file << log_message << std::endl;
                file.close();
            }
        }
    }
    
    LogLevel log_level_;
    bool console_output_;
    std::string log_file_;
    bool log_to_file_ = false;
    std::mutex mutex_;
};

// 全局日志器实例
static SimpleLogger g_logger;

// 便利函数
void logTrace(const std::string& message) {
    g_logger.trace(message);
}

void logDebug(const std::string& message) {
    g_logger.debug(message);
}

void logInfo(const std::string& message) {
    g_logger.info(message);
}

void logWarn(const std::string& message) {
    g_logger.warn(message);
}

void logError(const std::string& message) {
    g_logger.error(message);
}

void logFatal(const std::string& message) {
    g_logger.fatal(message);
}

void setLogLevel(LogLevel level) {
    g_logger.setLogLevel(level);
}

void setLogFile(const std::string& filename) {
    g_logger.setLogFile(filename);
}

} // namespace common
} // namespace agent_rpc