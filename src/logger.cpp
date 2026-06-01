#include "agent_rpc/logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <thread>

namespace agent_rpc {

// LogFormatter 实现
LogFormatter::LogFormatter(const std::string& format) : format_(format) {}

std::string LogFormatter::format(const LogEntry& entry) {
    std::string result = format_;
    
    // 替换时间戳
    size_t pos = result.find("%Y-%m-%d %H:%M:%S.%f");
    if (pos != std::string::npos) {
        result.replace(pos, 18, formatTimestamp(entry.timestamp));
    }
    
    // 替换日志级别
    pos = result.find("%l");
    if (pos != std::string::npos) {
        result.replace(pos, 2, formatLevel(entry.level));
    }
    
    // 替换线程ID
    pos = result.find("%t");
    if (pos != std::string::npos) {
        result.replace(pos, 2, formatThreadId(entry.thread_id));
    }
    
    // 替换源文件
    pos = result.find("%s");
    if (pos != std::string::npos) {
        result.replace(pos, 2, entry.source_file);
    }
    
    // 替换行号
    pos = result.find("%n");
    if (pos != std::string::npos) {
        result.replace(pos, 2, std::to_string(entry.line_number));
    }
    
    // 替换函数名
    pos = result.find("%f");
    if (pos != std::string::npos) {
        result.replace(pos, 2, entry.function_name);
    }
    
    // 替换消息
    pos = result.find("%v");
    if (pos != std::string::npos) {
        result.replace(pos, 2, entry.message);
    }
    
    return result;
}

void LogFormatter::setFormat(const std::string& format) {
    format_ = format;
}

std::string LogFormatter::formatTimestamp(const std::chrono::system_clock::time_point& timestamp) {
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    
    return ss.str();
}

std::string LogFormatter::formatLevel(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string LogFormatter::formatThreadId(std::thread::id thread_id) {
    std::stringstream ss;
    ss << thread_id;
    return ss.str();
}

std::string LogFormatter::getColorCode(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "\033[37m"; // 白色
        case LogLevel::DEBUG: return "\033[36m"; // 青色
        case LogLevel::INFO:  return "\033[32m"; // 绿色
        case LogLevel::WARN:  return "\033[33m"; // 黄色
        case LogLevel::ERROR: return "\033[31m"; // 红色
        case LogLevel::FATAL: return "\033[35m"; // 紫色
        default: return "\033[0m";
    }
}

std::string LogFormatter::resetColorCode() {
    return "\033[0m";
}

// ConsoleAppender 实现
ConsoleAppender::ConsoleAppender(bool color_output) : color_output_(color_output) {}

void ConsoleAppender::append(const LogEntry& entry) {
    std::string formatted = formatter_.format(entry);
    
    if (color_output_) {
        std::string color_code = formatter_.getColorCode(entry.level);
        std::string reset_code = formatter_.resetColorCode();
        std::cout << color_code << formatted << reset_code << std::endl;
    } else {
        std::cout << formatted << std::endl;
    }
}

void ConsoleAppender::flush() {
    std::cout.flush();
}

// FileAppender 实现
FileAppender::FileAppender(const std::string& filename, 
                          size_t max_file_size,
                          int max_files)
    : base_filename_(filename)
    , max_file_size_(max_file_size)
    , max_files_(max_files)
    , current_file_size_(0) {
    
    file_stream_.open(filename, std::ios::app);
    if (file_stream_.is_open()) {
        file_stream_.seekp(0, std::ios::end);
        current_file_size_ = file_stream_.tellp();
    }
}

FileAppender::~FileAppender() {
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
}

void FileAppender::append(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    if (!file_stream_.is_open()) {
        return;
    }
    
    std::string formatted = formatter_.format(entry);
    file_stream_ << formatted << std::endl;
    current_file_size_ += formatted.length() + 1;
    
    if (current_file_size_ >= max_file_size_) {
        rotateFile();
    }
}

void FileAppender::flush() {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (file_stream_.is_open()) {
        file_stream_.flush();
    }
}

void FileAppender::rotateFile() {
    file_stream_.close();
    
    // 移动现有文件
    for (int i = max_files_ - 1; i > 0; --i) {
        std::string old_name = getLogFileName(i);
        std::string new_name = getLogFileName(i + 1);
        
        if (std::ifstream(old_name).good()) {
            std::rename(old_name.c_str(), new_name.c_str());
        }
    }
    
    std::string old_name = base_filename_;
    std::string new_name = getLogFileName(1);
    std::rename(old_name.c_str(), new_name.c_str());
    
    // 重新打开文件
    file_stream_.open(base_filename_, std::ios::app);
    current_file_size_ = 0;
}

std::string FileAppender::getLogFileName(int index) {
    if (index == 0) {
        return base_filename_;
    }
    
    size_t dot_pos = base_filename_.find_last_of('.');
    if (dot_pos != std::string::npos) {
        return base_filename_.substr(0, dot_pos) + "." + std::to_string(index) + 
               base_filename_.substr(dot_pos);
    } else {
        return base_filename_ + "." + std::to_string(index);
    }
}

// AsyncLogger 实现
AsyncLogger::AsyncLogger() = default;

AsyncLogger::~AsyncLogger() {
    stop();
}

void AsyncLogger::start() {
    if (running_) {
        return;
    }
    
    running_ = true;
    log_thread_ = std::thread([this]() { logLoop(); });
}

void AsyncLogger::stop() {
    if (running_) {
        running_ = false;
        queue_condition_.notify_all();
        
        if (log_thread_.joinable()) {
            log_thread_.join();
        }
    }
}

void AsyncLogger::addAppender(std::shared_ptr<LogAppender> appender) {
    std::lock_guard<std::mutex> lock(appenders_mutex_);
    appenders_.push_back(appender);
}

void AsyncLogger::log(const LogEntry& entry) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        log_queue_.push(entry);
    }
    queue_condition_.notify_one();
}

void AsyncLogger::flush() {
    std::lock_guard<std::mutex> lock(appenders_mutex_);
    for (auto& appender : appenders_) {
        appender->flush();
    }
}

void AsyncLogger::logLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        
        queue_condition_.wait(lock, [this] { return !log_queue_.empty() || !running_; });
        
        if (!running_) {
            break;
        }
        
        // 处理所有待处理的日志
        while (!log_queue_.empty()) {
            LogEntry entry = log_queue_.front();
            log_queue_.pop();
            lock.unlock();
            
            // 发送到所有appender
            {
                std::lock_guard<std::mutex> appender_lock(appenders_mutex_);
                for (auto& appender : appenders_) {
                    appender->append(entry);
                }
            }
            
            lock.lock();
        }
    }
}

// Logger 实现
Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::initialize(const LogConfig& config) {
    config_ = config;
    
    // 创建异步日志器
    async_logger_ = std::make_unique<AsyncLogger>();
    async_logger_->start();
    
    // 添加控制台输出器
    if (config.console_output) {
        auto console_appender = std::make_shared<ConsoleAppender>(config.color_output);
        addAppender(console_appender);
    }
    
    // 添加文件输出器
    if (config.file_output && !config.log_file.empty()) {
        auto file_appender = std::make_shared<FileAppender>(
            config.log_file, config.max_file_size, config.max_files);
        addAppender(file_appender);
    }
    
    LOG_INFO("Logger initialized with level: " + std::to_string(static_cast<int>(config.level)));
}

void Logger::setLevel(LogLevel level) {
    config_.level = level;
}

void Logger::addAppender(std::shared_ptr<LogAppender> appender) {
    std::lock_guard<std::mutex> lock(appenders_mutex_);
    appenders_.push_back(appender);
    
    if (async_logger_) {
        async_logger_->addAppender(appender);
    }
}

void Logger::trace(const std::string& message, 
                  const std::string& source_file, 
                  int line_number,
                  const std::string& function_name) {
    logInternal(LogLevel::TRACE, message, {}, source_file, line_number, function_name);
}

void Logger::debug(const std::string& message, 
                  const std::string& source_file, 
                  int line_number,
                  const std::string& function_name) {
    logInternal(LogLevel::DEBUG, message, {}, source_file, line_number, function_name);
}

void Logger::info(const std::string& message, 
                 const std::string& source_file, 
                 int line_number,
                 const std::string& function_name) {
    logInternal(LogLevel::INFO, message, {}, source_file, line_number, function_name);
}

void Logger::warn(const std::string& message, 
                 const std::string& source_file, 
                 int line_number,
                 const std::string& function_name) {
    logInternal(LogLevel::WARN, message, {}, source_file, line_number, function_name);
}

void Logger::error(const std::string& message, 
                  const std::string& source_file, 
                  int line_number,
                  const std::string& function_name) {
    logInternal(LogLevel::ERROR, message, {}, source_file, line_number, function_name);
}

void Logger::fatal(const std::string& message, 
                  const std::string& source_file, 
                  int line_number,
                  const std::string& function_name) {
    logInternal(LogLevel::FATAL, message, {}, source_file, line_number, function_name);
}

void Logger::log(LogLevel level,
                const std::string& message,
                const std::map<std::string, std::string>& fields,
                const std::string& source_file,
                int line_number,
                const std::string& function_name) {
    logInternal(level, message, fields, source_file, line_number, function_name);
}

void Logger::flush() {
    if (async_logger_) {
        async_logger_->flush();
    }
    
    std::lock_guard<std::mutex> lock(appenders_mutex_);
    for (auto& appender : appenders_) {
        appender->flush();
    }
}

void Logger::logInternal(LogLevel level,
                        const std::string& message,
                        const std::map<std::string, std::string>& fields,
                        const std::string& source_file,
                        int line_number,
                        const std::string& function_name) {
    if (level < config_.level) {
        return;
    }
    
    LogEntry entry;
    entry.level = level;
    entry.message = message;
    entry.source_file = source_file;
    entry.line_number = line_number;
    entry.function_name = function_name;
    entry.thread_id = std::this_thread::get_id();
    entry.timestamp = std::chrono::system_clock::now();
    entry.fields = fields;
    
    if (async_logger_) {
        async_logger_->log(entry);
    } else {
        // 同步输出
        std::lock_guard<std::mutex> lock(appenders_mutex_);
        for (auto& appender : appenders_) {
            appender->append(entry);
        }
    }
}

} // namespace agent_rpc
