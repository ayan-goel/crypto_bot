#include "risk_manager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>

RiskManager::RiskManager() {
    session_start_time_ = std::chrono::system_clock::now();
    daily_reset_time_ = std::chrono::system_clock::now();
}

RiskManager::~RiskManager() {
    shutdown();
}

bool RiskManager::initialize(const std::string& config_file) {
    config_file_ = config_file;
    loadConfiguration();
    
    // Initialize daily reset time to start of today
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&tt);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    daily_reset_time_ = std::chrono::system_clock::from_time_t(std::mktime(&tm));
    
    recordRiskEvent(RiskEventType::SYSTEM_HEALTH_CRITICAL, RiskLevel::INFO, 
                   "Risk Manager initialized successfully");
    
    std::cout << "🛡️  Risk Manager initialized with limits:" << std::endl;
    std::cout << "   • Daily Loss Limit: $" << std::abs(max_daily_loss_limit_) << std::endl;
    std::cout << "   • Drawdown Limit: $" << std::abs(max_drawdown_limit_) << std::endl;
    std::cout << "   • Order Rate Limit: " << max_orders_per_second_ << "/sec" << std::endl;
    
    return true;
}

void RiskManager::shutdown() {
    stopRiskMonitoring();
    
    recordRiskEvent(RiskEventType::SYSTEM_HEALTH_CRITICAL, RiskLevel::INFO, 
                   "Risk Manager shutting down");
    
    // Generate final risk report
    generateRiskReport("logs/final_risk_report.log");
    
    std::cout << "🛡️  Risk Manager shutdown complete" << std::endl;
}

bool RiskManager::canPlaceOrder(const std::string& symbol, const std::string& side, 
                               double price, double quantity, std::string& rejection_reason) {
    rejection_reason.clear();
    
    // Check circuit breaker first
    if (circuit_breaker_active_.load()) {
        rejection_reason = "Circuit breaker active: " + circuit_breaker_reason_;
        return false;
    }
    
    // Check position limits
    if (!checkPositionLimits(symbol, side, quantity)) {
        rejection_reason = "Position limit exceeded for " + symbol;
        recordRiskEvent(RiskEventType::POSITION_LIMIT_EXCEEDED, RiskLevel::CRITICAL,
                       "Order rejected: Position limit exceeded", symbol, quantity, 
                       position_limits_[symbol]);
        return false;
    }
    
    // Check financial limits
    if (!checkFinancialLimits(0.0)) {
        rejection_reason = "Financial risk limits exceeded";
        return false;
    }
    
    // Check operational limits
    if (!checkOperationalLimits()) {
        rejection_reason = "Order rate limit exceeded";
        recordRiskEvent(RiskEventType::ORDER_RATE_LIMIT_EXCEEDED, RiskLevel::WARNING,
                       "Order rejected: Rate limit exceeded");
        return false;
    }
    
    return true;
}

void RiskManager::updatePosition(const std::string& symbol, double quantity, double price, const std::string& side) {
    std::lock_guard<std::mutex> lock(position_mutex_);
    
    double position_change = (side == "BUY") ? quantity : -quantity;
    positions_[symbol] += position_change;
    
    // Check for position warnings
    auto it = position_limits_.find(symbol);
    if (it != position_limits_.end()) {
        double position_utilization = std::abs(positions_[symbol]) / it->second;
        
        if (position_utilization > 0.8) {  // 80% warning threshold
            recordRiskEvent(RiskEventType::POSITION_WARNING, RiskLevel::WARNING,
                           "Position utilization high: " + std::to_string(position_utilization * 100) + "%",
                           symbol, std::abs(positions_[symbol]), it->second);
        }
    }
    
    std::cout << "📊 Position Update: " << symbol << " = " << positions_[symbol] 
              << " (change: " << (side == "BUY" ? "+" : "") << position_change << ")" << std::endl;
}

void RiskManager::updatePnL(double pnl_change) {
    std::lock_guard<std::mutex> lock(financial_mutex_);
    
    current_pnl_ += pnl_change;
    daily_pnl_ += pnl_change;
    
    // Update peak PnL for drawdown calculation
    if (current_pnl_ > peak_pnl_) {
        peak_pnl_ = current_pnl_;
    }
    
    // Check daily loss limit
    if (daily_pnl_ <= max_daily_loss_limit_) {
        recordRiskEvent(RiskEventType::DAILY_LOSS_LIMIT_EXCEEDED, RiskLevel::EMERGENCY,
                       "Daily loss limit exceeded: $" + std::to_string(daily_pnl_));
        triggerCircuitBreaker("Daily loss limit exceeded");
    }
    
    // Check drawdown limit
    double current_drawdown = peak_pnl_ - current_pnl_;
    if (current_drawdown >= std::abs(max_drawdown_limit_)) {
        recordRiskEvent(RiskEventType::DRAWDOWN_LIMIT_EXCEEDED, RiskLevel::EMERGENCY,
                       "Drawdown limit exceeded: $" + std::to_string(current_drawdown));
        triggerCircuitBreaker("Drawdown limit exceeded");
    }
    
    // PnL warnings
    if (daily_pnl_ <= max_daily_loss_limit_ * 0.7) {  // 70% of daily limit
        recordRiskEvent(RiskEventType::PNL_WARNING, RiskLevel::WARNING,
                       "Approaching daily loss limit: $" + std::to_string(daily_pnl_));
    }
}

void RiskManager::recordOrderPlaced() {
    updateOrderRate();
}

RiskStatus RiskManager::getCurrentRiskStatus() const {
    if (circuit_breaker_active_.load()) {
        return RiskStatus::EMERGENCY;
    }
    
    // Check recent risk events for critical issues
    std::lock_guard<std::mutex> lock(events_mutex_);
    auto now = std::chrono::system_clock::now();
    auto five_minutes_ago = now - std::chrono::minutes(5);
    
    int critical_events = 0;
    int warning_events = 0;
    
    for (const auto& event : risk_events_) {
        if (event.timestamp > five_minutes_ago) {
            if (event.level == RiskLevel::CRITICAL || event.level == RiskLevel::EMERGENCY) {
                critical_events++;
            } else if (event.level == RiskLevel::WARNING) {
                warning_events++;
            }
        }
    }
    
    if (critical_events > 0) {
        return RiskStatus::CRITICAL;
    } else if (warning_events > 3) {  // Multiple warnings
        return RiskStatus::WARNING;
    }
    
    return RiskStatus::NORMAL;
}

std::vector<RiskEvent> RiskManager::getRecentRiskEvents(int count) const {
    std::lock_guard<std::mutex> lock(events_mutex_);
    
    std::vector<RiskEvent> recent_events;
    int start_idx = std::max(0, static_cast<int>(risk_events_.size()) - count);
    
    for (int i = start_idx; i < static_cast<int>(risk_events_.size()); ++i) {
        recent_events.push_back(risk_events_[i]);
    }
    
    return recent_events;
}

PositionRisk RiskManager::getPositionRisk(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    
    PositionRisk risk;
    
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        risk.current_position = pos_it->second;
    }
    
    auto limit_it = position_limits_.find(symbol);
    if (limit_it != position_limits_.end()) {
        risk.max_position_limit = limit_it->second;
        risk.position_utilization = std::abs(risk.current_position) / risk.max_position_limit;
        risk.position_limit_breached = risk.position_utilization > 1.0;
    }
    
    return risk;
}

FinancialRisk RiskManager::getFinancialRisk() const {
    std::lock_guard<std::mutex> lock(financial_mutex_);
    
    FinancialRisk risk;
    risk.current_pnl = current_pnl_;
    risk.daily_pnl = daily_pnl_;
    risk.max_daily_loss_limit = max_daily_loss_limit_;
    risk.max_drawdown_limit = max_drawdown_limit_;
    risk.current_drawdown = peak_pnl_ - current_pnl_;
    risk.peak_pnl = peak_pnl_;
    risk.daily_loss_limit_breached = daily_pnl_ <= max_daily_loss_limit_;
    risk.drawdown_limit_breached = risk.current_drawdown >= std::abs(max_drawdown_limit_);
    
    return risk;
}

OperationalRisk RiskManager::getOperationalRisk() const {
    std::lock_guard<std::mutex> lock(operational_mutex_);
    
    OperationalRisk risk;
    
    // Calculate current orders per second
    auto now = std::chrono::system_clock::now();
    auto one_second_ago = now - std::chrono::seconds(1);
    
    risk.orders_per_second = std::count_if(recent_orders_.begin(), recent_orders_.end(),
        [one_second_ago](const auto& timestamp) {
            return timestamp > one_second_ago;
        });
    
    risk.max_orders_per_second = max_orders_per_second_;
    risk.session_start_time = session_start_time_;
    risk.order_rate_limit_breached = risk.orders_per_second > max_orders_per_second_;
    risk.circuit_breaker_active = circuit_breaker_active_.load();
    risk.circuit_breaker_reason = circuit_breaker_reason_;
    
    if (!recent_orders_.empty()) {
        risk.last_order_time = recent_orders_.back();
    }
    
    return risk;
}

void RiskManager::triggerCircuitBreaker(const std::string& reason) {
    circuit_breaker_active_.store(true);
    circuit_breaker_reason_ = reason;
    
    recordRiskEvent(RiskEventType::CIRCUIT_BREAKER_TRIGGERED, RiskLevel::EMERGENCY,
                   "Circuit breaker triggered: " + reason);
    
    std::cout << "🚨 CIRCUIT BREAKER TRIGGERED: " << reason << std::endl;
}

void RiskManager::resetCircuitBreaker() {
    circuit_breaker_active_.store(false);
    circuit_breaker_reason_.clear();
    
    recordRiskEvent(RiskEventType::CIRCUIT_BREAKER_TRIGGERED, RiskLevel::INFO,
                   "Circuit breaker reset");
    
    std::cout << "✅ Circuit breaker reset" << std::endl;
}

bool RiskManager::isCircuitBreakerActive() const {
    return circuit_breaker_active_.load();
}

void RiskManager::setPositionLimit(const std::string& symbol, double limit) {
    std::lock_guard<std::mutex> lock(position_mutex_);
    position_limits_[symbol] = limit;
    
    std::cout << "📊 Position limit set: " << symbol << " = " << limit << std::endl;
}

void RiskManager::setDailyLossLimit(double limit) {
    std::lock_guard<std::mutex> lock(financial_mutex_);
    max_daily_loss_limit_ = -std::abs(limit);  // Ensure it's negative
    
    std::cout << "💰 Daily loss limit set: $" << std::abs(limit) << std::endl;
}

void RiskManager::setDrawdownLimit(double limit) {
    std::lock_guard<std::mutex> lock(financial_mutex_);
    max_drawdown_limit_ = -std::abs(limit);  // Ensure it's negative
    
    std::cout << "📉 Drawdown limit set: $" << std::abs(limit) << std::endl;
}

void RiskManager::setOrderRateLimit(uint64_t orders_per_second) {
    std::lock_guard<std::mutex> lock(operational_mutex_);
    max_orders_per_second_ = orders_per_second;
    
    std::cout << "⚡ Order rate limit set: " << orders_per_second << "/sec" << std::endl;
}

void RiskManager::startRiskMonitoring() {
    if (monitoring_active_.load()) {
        return;
    }
    
    monitoring_active_.store(true);
    monitoring_thread_ = std::thread(&RiskManager::monitoringLoop, this);
    
    std::cout << "👁️  Risk monitoring started" << std::endl;
}

void RiskManager::stopRiskMonitoring() {
    if (!monitoring_active_.load()) {
        return;
    }
    
    monitoring_active_.store(false);
    
    if (monitoring_thread_.joinable()) {
        monitoring_thread_.join();
    }
    
    std::cout << "👁️  Risk monitoring stopped" << std::endl;
}

void RiskManager::generateRiskReport(const std::string& filename) const {
    std::ofstream report_file(filename);
    
    if (!report_file.is_open()) {
        std::cerr << "Failed to open risk report file: " << filename << std::endl;
        return;
    }
    
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    
    report_file << "================================================================================\n";
    report_file << "                           RISK MANAGEMENT REPORT\n";
    report_file << "================================================================================\n";
    report_file << "Generated: " << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S") << "\n\n";
    
    // Risk Status
    report_file << "🚦 CURRENT RISK STATUS: ";
    switch (getCurrentRiskStatus()) {
        case RiskStatus::NORMAL: report_file << "NORMAL"; break;
        case RiskStatus::WARNING: report_file << "WARNING"; break;
        case RiskStatus::CRITICAL: report_file << "CRITICAL"; break;
        case RiskStatus::EMERGENCY: report_file << "EMERGENCY"; break;
    }
    report_file << "\n\n";
    
    // Financial Risk
    auto financial_risk = getFinancialRisk();
    report_file << "💰 FINANCIAL RISK:\n";
    report_file << "  Current P&L: $" << std::fixed << std::setprecision(4) << financial_risk.current_pnl << "\n";
    report_file << "  Daily P&L: $" << financial_risk.daily_pnl << "\n";
    report_file << "  Peak P&L: $" << financial_risk.peak_pnl << "\n";
    report_file << "  Current Drawdown: $" << financial_risk.current_drawdown << "\n";
    report_file << "  Daily Loss Limit: $" << std::abs(financial_risk.max_daily_loss_limit) << "\n";
    report_file << "  Drawdown Limit: $" << std::abs(financial_risk.max_drawdown_limit) << "\n";
    report_file << "  Daily Limit Breached: " << (financial_risk.daily_loss_limit_breached ? "YES" : "NO") << "\n";
    report_file << "  Drawdown Limit Breached: " << (financial_risk.drawdown_limit_breached ? "YES" : "NO") << "\n\n";
    
    // Position Risk
    report_file << "📊 POSITION RISK:\n";
    {
        std::lock_guard<std::mutex> lock(position_mutex_);
        for (const auto& [symbol, position] : positions_) {
            auto risk = getPositionRisk(symbol);
            report_file << "  " << symbol << ":\n";
            report_file << "    Current Position: " << risk.current_position << "\n";
            report_file << "    Position Limit: " << risk.max_position_limit << "\n";
            report_file << "    Utilization: " << (risk.position_utilization * 100) << "%\n";
            report_file << "    Limit Breached: " << (risk.position_limit_breached ? "YES" : "NO") << "\n";
        }
    }
    report_file << "\n";
    
    // Operational Risk
    auto operational_risk = getOperationalRisk();
    report_file << "⚙️  OPERATIONAL RISK:\n";
    report_file << "  Current Order Rate: " << operational_risk.orders_per_second << "/sec\n";
    report_file << "  Max Order Rate: " << operational_risk.max_orders_per_second << "/sec\n";
    report_file << "  Rate Limit Breached: " << (operational_risk.order_rate_limit_breached ? "YES" : "NO") << "\n";
    report_file << "  Circuit Breaker Active: " << (operational_risk.circuit_breaker_active ? "YES" : "NO") << "\n";
    if (operational_risk.circuit_breaker_active) {
        report_file << "  Circuit Breaker Reason: " << operational_risk.circuit_breaker_reason << "\n";
    }
    report_file << "\n";
    
    // Recent Risk Events
    report_file << "📋 RECENT RISK EVENTS (Last 20):\n";
    auto recent_events = getRecentRiskEvents(20);
    for (const auto& event : recent_events) {
        report_file << "  " << formatRiskEvent(event) << "\n";
    }
    
    report_file << "\n================================================================================\n";
    report_file.close();
    
    std::cout << "📋 Risk report generated: " << filename << std::endl;
}

std::string RiskManager::getRiskSummary() const {
    std::ostringstream summary;
    
    auto financial_risk = getFinancialRisk();
    auto operational_risk = getOperationalRisk();
    
    summary << "🛡️  Risk Summary: ";
    
    switch (getCurrentRiskStatus()) {
        case RiskStatus::NORMAL: summary << "NORMAL"; break;
        case RiskStatus::WARNING: summary << "WARNING"; break;
        case RiskStatus::CRITICAL: summary << "CRITICAL"; break;
        case RiskStatus::EMERGENCY: summary << "EMERGENCY"; break;
    }
    
    summary << " | P&L: $" << std::fixed << std::setprecision(2) << financial_risk.current_pnl;
    summary << " | Daily: $" << financial_risk.daily_pnl;
    summary << " | Orders/sec: " << operational_risk.orders_per_second;
    
    if (operational_risk.circuit_breaker_active) {
        summary << " | CB: ACTIVE";
    }
    
    return summary.str();
}

// Private methods

void RiskManager::loadConfiguration() {
    // Parse config file
    std::ifstream config_file(config_file_);
    if (!config_file.is_open()) {
        std::cout << "⚠️  Warning: Could not open config file, using defaults" << std::endl;
        // Set defaults
        setPositionLimit("ETHUSDT", 1.0);
        setDailyLossLimit(100.0);
        setDrawdownLimit(50.0);
        setOrderRateLimit(5);
        return;
    }
    
    std::string line;
    double daily_loss_limit = 100.0;
    double drawdown_limit = 50.0;
    double position_limit = 1.0;
    uint64_t order_rate_limit = 5;
    
    while (std::getline(config_file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);
        
        if (key == "MAX_DAILY_LOSS_LIMIT") {
            daily_loss_limit = std::stod(value);
        } else if (key == "MAX_DRAWDOWN_LIMIT") {
            drawdown_limit = std::stod(value);
        } else if (key == "POSITION_LIMIT_ETHUSDT") {
            position_limit = std::stod(value);
        } else if (key == "ORDER_RATE_LIMIT") {
            order_rate_limit = std::stoull(value);
        }
    }
    
    config_file.close();
    
    // Apply loaded configuration
    setPositionLimit("ETHUSDT", position_limit);
    setDailyLossLimit(daily_loss_limit);
    setDrawdownLimit(drawdown_limit);
    setOrderRateLimit(order_rate_limit);
}

void RiskManager::resetDailyLimits() {
    std::lock_guard<std::mutex> lock(financial_mutex_);
    
    daily_pnl_ = 0.0;
    daily_reset_time_ = std::chrono::system_clock::now();
    
    recordRiskEvent(RiskEventType::SYSTEM_HEALTH_CRITICAL, RiskLevel::INFO,
                   "Daily limits reset");
}

bool RiskManager::checkPositionLimits(const std::string& symbol, const std::string& side, double quantity) const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    
    auto limit_it = position_limits_.find(symbol);
    if (limit_it == position_limits_.end()) {
        return true;  // No limit set
    }
    
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = pos_it->second;
    }
    
    double position_change = (side == "BUY") ? quantity : -quantity;
    double new_position = current_position + position_change;
    
    return std::abs(new_position) <= limit_it->second;
}

bool RiskManager::checkFinancialLimits(double estimated_pnl_impact) const {
    std::lock_guard<std::mutex> lock(financial_mutex_);
    
    // Check if we're already at limits
    if (daily_pnl_ <= max_daily_loss_limit_) {
        return false;
    }
    
    double current_drawdown = peak_pnl_ - current_pnl_;
    if (current_drawdown >= std::abs(max_drawdown_limit_)) {
        return false;
    }
    
    return true;
}

bool RiskManager::checkOperationalLimits() const {
    std::lock_guard<std::mutex> lock(operational_mutex_);
    
    // Count orders in the last second
    auto now = std::chrono::system_clock::now();
    auto one_second_ago = now - std::chrono::seconds(1);
    
    uint64_t orders_last_second = std::count_if(recent_orders_.begin(), recent_orders_.end(),
        [one_second_ago](const auto& timestamp) {
            return timestamp > one_second_ago;
        });
    
    return orders_last_second < max_orders_per_second_;
}

void RiskManager::recordRiskEvent(RiskEventType type, RiskLevel level, const std::string& message, 
                                 const std::string& symbol, double value, double limit) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    
    RiskEvent event;
    event.type = type;
    event.level = level;
    event.message = message;
    event.timestamp = std::chrono::system_clock::now();
    event.symbol = symbol;
    event.value = value;
    event.limit = limit;
    
    risk_events_.push_back(event);
    
    // Keep only the most recent events
    if (risk_events_.size() > MAX_RISK_EVENTS) {
        risk_events_.erase(risk_events_.begin(), risk_events_.begin() + (risk_events_.size() - MAX_RISK_EVENTS));
    }
    
    // Log critical events
    if (level == RiskLevel::CRITICAL || level == RiskLevel::EMERGENCY) {
        std::cout << "🚨 RISK EVENT: " << formatRiskEvent(event) << std::endl;
    }
}

void RiskManager::monitoringLoop() {
    while (monitoring_active_.load()) {
        // Check if we need to reset daily limits
        auto now = std::chrono::system_clock::now();
        auto time_since_reset = std::chrono::duration_cast<std::chrono::hours>(now - daily_reset_time_);
        
        if (time_since_reset.count() >= 24) {
            resetDailyLimits();
        }
        
        // Clean up old order timestamps
        {
            std::lock_guard<std::mutex> lock(operational_mutex_);
            auto five_seconds_ago = now - std::chrono::seconds(5);
            recent_orders_.erase(
                std::remove_if(recent_orders_.begin(), recent_orders_.end(),
                    [five_seconds_ago](const auto& timestamp) {
                        return timestamp < five_seconds_ago;
                    }),
                recent_orders_.end());
        }
        
        // Sleep for 1 second
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void RiskManager::updateOrderRate() {
    std::lock_guard<std::mutex> lock(operational_mutex_);
    
    auto now = std::chrono::system_clock::now();
    recent_orders_.push_back(now);
    
    // Clean up old timestamps (keep only last 5 seconds)
    auto five_seconds_ago = now - std::chrono::seconds(5);
    recent_orders_.erase(
        std::remove_if(recent_orders_.begin(), recent_orders_.end(),
            [five_seconds_ago](const auto& timestamp) {
                return timestamp < five_seconds_ago;
            }),
        recent_orders_.end());
}

std::string RiskManager::formatRiskEvent(const RiskEvent& event) const {
    std::ostringstream oss;
    
    auto tt = std::chrono::system_clock::to_time_t(event.timestamp);
    oss << std::put_time(std::localtime(&tt), "%H:%M:%S") << " ";
    
    switch (event.level) {
        case RiskLevel::INFO: oss << "[INFO] "; break;
        case RiskLevel::WARNING: oss << "[WARN] "; break;
        case RiskLevel::CRITICAL: oss << "[CRIT] "; break;
        case RiskLevel::EMERGENCY: oss << "[EMER] "; break;
    }
    
    oss << event.message;
    
    if (!event.symbol.empty()) {
        oss << " (" << event.symbol << ")";
    }
    
    if (event.value != 0.0 && event.limit != 0.0) {
        oss << " Value:" << event.value << " Limit:" << event.limit;
    }
    
    return oss.str();
} 