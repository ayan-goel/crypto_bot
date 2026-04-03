#include "core/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <sys/stat.h>

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    closeLogFile();
}

bool Logger::initialize(const std::string& log_dir) {
    log_dir_ = log_dir;
    struct stat st{};
    if (stat(log_dir_.c_str(), &st) != 0) {
        mkdir(log_dir_.c_str(), 0755);
    }
    return openLogFile();
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warning(const std::string& message) {
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void Logger::critical(const std::string& message) {
    log(LogLevel::CRITICAL, message);
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < current_level_) {
        return;
    }

    std::string timestamp = getCurrentTimestamp();
    std::string level_str = logLevelToString(level);
    std::string formatted_message = timestamp + " [" + level_str + "] " + message;

    std::lock_guard<std::mutex> lock(log_mutex_);

    if (console_output_) {
        std::cout << formatted_message << std::endl;
    }

    if (file_output_ && main_log_file_ && *main_log_file_) {
        *main_log_file_ << formatted_message << std::endl;
    }
}

std::string Logger::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_val = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_buf{};
    localtime_r(&time_t_val, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::logLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARNING:  return "WARNING";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
        default:                 return "UNKNOWN";
    }
}

bool Logger::openLogFile() {
    try {
        main_log_file_ = std::make_unique<std::ofstream>(log_dir_ + "/main.log", std::ios::app);
        return main_log_file_->is_open();
    } catch (const std::exception& e) {
        std::cerr << "Failed to open log file: " << e.what() << std::endl;
        return false;
    }
}

void Logger::closeLogFile() {
    main_log_file_.reset();
}
