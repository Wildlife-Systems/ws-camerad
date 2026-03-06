#pragma once

#include <string>
#include <iostream>
#include <sstream>
#include <mutex>
#include <chrono>
#include <iomanip>

namespace camera_daemon {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    FATAL = 4
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) {
        level_ = level;
    }

    void set_output(std::ostream& os) {
        output_ = &os;
    }

    template<typename... Args>
    void log(LogLevel level, const char* file, int line, Args&&... args) {
        if (level < level_) return;

        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count() % 1000;

        *output_ << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
                 << "." << std::setfill('0') << std::setw(3) << ms
                 << " [" << level_str(level) << "] "
                 << extract_filename(file) << ":" << line << " - ";
        
        (((*output_) << std::forward<Args>(args)), ...);
        *output_ << std::endl;
    }

private:
    Logger() : level_(LogLevel::INFO), output_(&std::cout) {}

    const char* level_str(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default: return "?????";
        }
    }

    std::string extract_filename(const char* path) {
        std::string s(path);
        size_t pos = s.find_last_of("/\\");
        return (pos != std::string::npos) ? s.substr(pos + 1) : s;
    }

    LogLevel level_;
    std::ostream* output_;
    std::mutex mutex_;
};

} // namespace camera_daemon

#define LOG_DEBUG(...) camera_daemon::Logger::instance().log(camera_daemon::LogLevel::DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  camera_daemon::Logger::instance().log(camera_daemon::LogLevel::INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  camera_daemon::Logger::instance().log(camera_daemon::LogLevel::WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) camera_daemon::Logger::instance().log(camera_daemon::LogLevel::ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) camera_daemon::Logger::instance().log(camera_daemon::LogLevel::FATAL, __FILE__, __LINE__, __VA_ARGS__)
