#pragma once

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

// 日志级别
enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    FATAL = 5
};

// 日志配置
struct LogConfig {
    LogLevel level = LogLevel::INFO;
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

// 日志格式化器
class LogFormatter {
public:
    LogFormatter(const std::string& format = "[%Y-%m-%d %H:%M:%S.%f] [%l] [%t] [%s:%n] %v");
    ~LogFormatter() = default;
    
    std::string format(const LogEntry& entry);
    void setFormat(const std::string& format);

private:
    std::string format_;
    std::string formatTimestamp(const std::chrono::system_clock::time_point& timestamp);
    std::string formatLevel(LogLevel level);
    std::string formatThreadId(std::thread::id thread_id);
    std::string getColorCode(LogLevel level);
    std::string resetColorCode();
};

// 日志输出器接口
class LogAppender {
public:
    virtual ~LogAppender() = default;
    virtual void append(const LogEntry& entry) = 0;
    virtual void flush() = 0;
};

// 控制台输出器
class ConsoleAppender : public LogAppender {
public:
    ConsoleAppender(bool color_output = true);
    ~ConsoleAppender() = default;
    
    void append(const LogEntry& entry) override;
    void flush() override;

private:
    bool color_output_;
    LogFormatter formatter_;
};

// 文件输出器
class FileAppender : public LogAppender {
public:
    FileAppender(const std::string& filename, 
                 size_t max_file_size = 100 * 1024 * 1024,
                 int max_files = 10);
    ~FileAppender();
    
    void append(const LogEntry& entry) override;
    void flush() override;

private:
    void rotateFile();
    std::string getLogFileName(int index);
    
    std::string base_filename_;
    size_t max_file_size_;
    int max_files_;
    std::ofstream file_stream_;
    std::mutex file_mutex_;
    size_t current_file_size_;
    LogFormatter formatter_;
};

// 异步日志器
class AsyncLogger {
public:
    AsyncLogger();
    ~AsyncLogger();
    
    void start();
    void stop();
    void addAppender(std::shared_ptr<LogAppender> appender);
    void log(const LogEntry& entry);
    void flush();

private:
    void logLoop();
    
    std::atomic<bool> running_{false};
    std::thread log_thread_;
    std::queue<LogEntry> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::vector<std::shared_ptr<LogAppender>> appenders_;
    mutable std::mutex appenders_mutex_;
};

// 主日志器类
class Logger {
public:
    static Logger& getInstance();
    
    // 初始化日志器
    void initialize(const LogConfig& config);
    
    // 设置日志级别
    void setLevel(LogLevel level);
    
    // 添加输出器
    void addAppender(std::shared_ptr<LogAppender> appender);
    
    // 日志方法
    void trace(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "");
    
    void debug(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "");
    
    void info(const std::string& message, 
              const std::string& source_file = "", 
              int line_number = 0,
              const std::string& function_name = "");
    
    void warn(const std::string& message, 
              const std::string& source_file = "", 
              int line_number = 0,
              const std::string& function_name = "");
    
    void error(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "");
    
    void fatal(const std::string& message, 
               const std::string& source_file = "", 
               int line_number = 0,
               const std::string& function_name = "");
    
    // 带字段的日志
    void log(LogLevel level,
             const std::string& message,
             const std::map<std::string, std::string>& fields = {},
             const std::string& source_file = "",
             int line_number = 0,
             const std::string& function_name = "");
    
    // 刷新日志
    void flush();
    
    // 获取配置
    const LogConfig& getConfig() const { return config_; }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    void logInternal(LogLevel level,
                    const std::string& message,
                    const std::map<std::string, std::string>& fields = {},
                    const std::string& source_file = "",
                    int line_number = 0,
                    const std::string& function_name = "");
    
    LogConfig config_;
    std::unique_ptr<AsyncLogger> async_logger_;
    std::vector<std::shared_ptr<LogAppender>> appenders_;
    mutable std::mutex appenders_mutex_;
};

// 便利宏
#define LOG_TRACE(msg) Logger::getInstance().trace(msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_DEBUG(msg) Logger::getInstance().debug(msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_INFO(msg) Logger::getInstance().info(msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_WARN(msg) Logger::getInstance().warn(msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_ERROR(msg) Logger::getInstance().error(msg, __FILE__, __LINE__, __FUNCTION__)
#define LOG_FATAL(msg) Logger::getInstance().fatal(msg, __FILE__, __LINE__, __FUNCTION__)

#define LOG_TRACE_FIELDS(msg, fields) Logger::getInstance().log(LogLevel::TRACE, msg, fields, __FILE__, __LINE__, __FUNCTION__)
#define LOG_DEBUG_FIELDS(msg, fields) Logger::getInstance().log(LogLevel::DEBUG, msg, fields, __FILE__, __LINE__, __FUNCTION__)
#define LOG_INFO_FIELDS(msg, fields) Logger::getInstance().log(LogLevel::INFO, msg, fields, __FILE__, __LINE__, __FUNCTION__)
#define LOG_WARN_FIELDS(msg, fields) Logger::getInstance().log(LogLevel::WARN, msg, fields, __FILE__, __LINE__, __FUNCTION__)
#define LOG_ERROR_FIELDS(msg, fields) Logger::getInstance().log(LogLevel::ERROR, msg, fields, __FILE__, __LINE__, __FUNCTION__)
#define LOG_FATAL_FIELDS(msg, fields) Logger::getInstance().log(LogLevel::FATAL, msg, fields, __FILE__, __LINE__, __FUNCTION__)

} // namespace agent_rpc
