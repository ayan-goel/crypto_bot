#pragma once

#include <string>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

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
    
    // Initialize logger with configuration
    bool initialize(const std::string& log_dir = "logs/");
    
    // Log methods
    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);
    void critical(const std::string& message);
    
    // Specialized logging methods
    void logOrderBook(const std::string& symbol, double best_bid, double best_ask, 
                      double bid_size, double ask_size);
    void logTrade(const std::string& order_id, const std::string& symbol, 
                  const std::string& side, double price, double quantity, 
                  double commission, const std::string& status);
    void logPnL(double realized_pnl, double unrealized_pnl, double total_pnl, 
                double position, double avg_price);
    void logHealth(const std::string& component, bool is_healthy, 
                   const std::string& details = "");
    
    // Set log level
    void setLogLevel(LogLevel level);
    void setLogLevel(const std::string& level_str);
    
    // Enable/disable outputs
    void setConsoleOutput(bool enabled);
    void setFileOutput(bool enabled);
    
    // Flush all buffers
    void flush();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    void log(LogLevel level, const std::string& message);
    std::string getCurrentTimestamp();
    std::string logLevelToString(LogLevel level);
    LogLevel stringToLogLevel(const std::string& level_str);
    
    // Member variables
    LogLevel current_level_ = LogLevel::INFO;
    bool console_output_ = true;
    bool file_output_ = true;
    std::string log_dir_;
    
    // File handles
    std::unique_ptr<std::ofstream> main_log_file_;
    std::unique_ptr<std::ofstream> orderbook_log_file_;
    std::unique_ptr<std::ofstream> trades_log_file_;
    std::unique_ptr<std::ofstream> pnl_log_file_;
    std::unique_ptr<std::ofstream> health_log_file_;
    
    // Thread safety
    std::mutex log_mutex_;
    
    // Helper methods
    bool openLogFiles();
    void closeLogFiles();
}; 