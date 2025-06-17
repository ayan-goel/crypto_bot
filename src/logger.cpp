#include "logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    closeLogFiles();
}

bool Logger::initialize(const std::string& log_dir) {
    log_dir_ = log_dir;
    return openLogFiles();
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

void Logger::logOrderBook(const std::string& symbol, double best_bid, double best_ask, 
                         double bid_size, double ask_size) {
    std::ostringstream oss;
    oss << symbol << " OrderBook - Bid: " << best_bid << "(" << bid_size << ") "
        << "Ask: " << best_ask << "(" << ask_size << ")";
    
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (orderbook_log_file_ && *orderbook_log_file_) {
        *orderbook_log_file_ << getCurrentTimestamp() << " " << oss.str() << std::endl;
    }
}

void Logger::logTrade(const std::string& order_id, const std::string& symbol, 
                     const std::string& side, double price, double quantity, 
                     double commission, const std::string& status) {
    std::ostringstream oss;
    oss << "Trade - OrderID: " << order_id << " Symbol: " << symbol 
        << " Side: " << side << " Price: " << price << " Qty: " << quantity 
        << " Commission: " << commission << " Status: " << status;
    
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (trades_log_file_ && *trades_log_file_) {
        *trades_log_file_ << getCurrentTimestamp() << " " << oss.str() << std::endl;
    }
}

void Logger::logPnL(double realized_pnl, double unrealized_pnl, double total_pnl, 
                   double position, double avg_price) {
    std::ostringstream oss;
    oss << "PnL - Realized: " << realized_pnl << " Unrealized: " << unrealized_pnl 
        << " Total: " << total_pnl << " Position: " << position << " AvgPrice: " << avg_price;
    
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (pnl_log_file_ && *pnl_log_file_) {
        *pnl_log_file_ << getCurrentTimestamp() << " " << oss.str() << std::endl;
    }
}

void Logger::logHealth(const std::string& component, bool is_healthy, 
                      const std::string& details) {
    std::ostringstream oss;
    oss << "Health - Component: " << component << " Status: " 
        << (is_healthy ? "HEALTHY" : "UNHEALTHY") << " Details: " << details;
    
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (health_log_file_ && *health_log_file_) {
        *health_log_file_ << getCurrentTimestamp() << " " << oss.str() << std::endl;
    }
}

void Logger::setLogLevel(LogLevel level) {
    current_level_ = level;
}

void Logger::setLogLevel(const std::string& level_str) {
    current_level_ = stringToLogLevel(level_str);
}

void Logger::setConsoleOutput(bool enabled) {
    console_output_ = enabled;
}

void Logger::setFileOutput(bool enabled) {
    file_output_ = enabled;
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (main_log_file_) main_log_file_->flush();
    if (orderbook_log_file_) orderbook_log_file_->flush();
    if (trades_log_file_) trades_log_file_->flush();
    if (pnl_log_file_) pnl_log_file_->flush();
    if (health_log_file_) health_log_file_->flush();
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
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::logLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

LogLevel Logger::stringToLogLevel(const std::string& level_str) {
    if (level_str == "DEBUG") return LogLevel::DEBUG;
    if (level_str == "INFO") return LogLevel::INFO;
    if (level_str == "WARNING") return LogLevel::WARNING;
    if (level_str == "ERROR") return LogLevel::ERROR;
    if (level_str == "CRITICAL") return LogLevel::CRITICAL;
    return LogLevel::INFO;  // Default
}

bool Logger::openLogFiles() {
    try {
        main_log_file_ = std::make_unique<std::ofstream>(log_dir_ + "/main.log", std::ios::app);
        orderbook_log_file_ = std::make_unique<std::ofstream>(log_dir_ + "/orderbook.log", std::ios::app);
        trades_log_file_ = std::make_unique<std::ofstream>(log_dir_ + "/trades.log", std::ios::app);
        pnl_log_file_ = std::make_unique<std::ofstream>(log_dir_ + "/pnl.log", std::ios::app);
        health_log_file_ = std::make_unique<std::ofstream>(log_dir_ + "/health.log", std::ios::app);
        
        return main_log_file_->is_open() && orderbook_log_file_->is_open() && 
               trades_log_file_->is_open() && pnl_log_file_->is_open() && 
               health_log_file_->is_open();
    } catch (const std::exception&) {
        return false;
    }
}

void Logger::closeLogFiles() {
    main_log_file_.reset();
    orderbook_log_file_.reset();
    trades_log_file_.reset();
    pnl_log_file_.reset();
    health_log_file_.reset();
} 