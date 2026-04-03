#pragma once

#include <string>
#include <fstream>
#include <memory>
#include <mutex>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

class Logger {
public:
    static Logger& getInstance();

    bool initialize(const std::string& log_dir = "logs");

    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void critical(const std::string& message);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string& message);
    static std::string getCurrentTimestamp();
    static std::string logLevelToString(LogLevel level);

    LogLevel current_level_ = LogLevel::INFO;
    bool console_output_ = true;
    bool file_output_ = true;
    std::string log_dir_;

    std::unique_ptr<std::ofstream> main_log_file_;
    std::mutex log_mutex_;

    bool openLogFile();
    void closeLogFile();
};
