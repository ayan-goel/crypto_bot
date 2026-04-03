#include "risk/risk_manager.h"
#include "core/config.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>

RiskManager::RiskManager() {
    daily_reset_time_ = std::chrono::system_clock::now();
}

RiskManager::~RiskManager() {
    shutdown();
}

bool RiskManager::initialize(const std::string& /*config_file*/) {
    loadConfiguration();

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&tt);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    daily_reset_time_ = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    recordRiskEvent(RiskEventType::SYSTEM_INFO, RiskLevel::INFO,
                    "Risk Manager initialized successfully");

    std::cout << "Risk Manager initialized with limits:" << std::endl;
    std::cout << "   Daily Loss Limit: $" << std::abs(max_daily_loss_limit_) << std::endl;
    std::cout << "   Drawdown Limit: $" << std::abs(max_drawdown_limit_) << std::endl;
    std::cout << "   Order Rate Limit: " << max_orders_per_second_ << "/sec" << std::endl;

    return true;
}

void RiskManager::shutdown() {
    recordRiskEvent(RiskEventType::SYSTEM_INFO, RiskLevel::INFO,
                    "Risk Manager shutting down");
    std::cout << "Risk Manager shutdown complete" << std::endl;
}

bool RiskManager::canPlaceOrder(const std::string& symbol, const std::string& side,
                                double /*price*/, double quantity, std::string& rejection_reason) {
    rejection_reason.clear();

    if (circuit_breaker_active_.load()) {
        rejection_reason = "Circuit breaker active: " + circuit_breaker_reason_;
        return false;
    }

    if (!checkPositionLimits(symbol, side, quantity)) {
        double limit = 0.0;
        {
            std::lock_guard<std::mutex> lock(position_mutex_);
            auto it = position_limits_.find(symbol);
            if (it != position_limits_.end()) limit = it->second;
        }
        rejection_reason = "Position limit exceeded for " + symbol;
        recordRiskEvent(RiskEventType::POSITION_LIMIT_EXCEEDED, RiskLevel::CRITICAL,
                        "Order rejected: Position limit exceeded", symbol, quantity, limit);
        return false;
    }

    if (!checkFinancialLimits(0.0)) {
        rejection_reason = "Financial risk limits exceeded";
        return false;
    }

    if (!checkOperationalLimits()) {
        rejection_reason = "Order rate limit exceeded";
        recordRiskEvent(RiskEventType::ORDER_RATE_LIMIT_EXCEEDED, RiskLevel::WARNING,
                        "Order rejected: Rate limit exceeded");
        return false;
    }

    return true;
}

void RiskManager::updatePnL(double pnl_change) {
    std::lock_guard<std::mutex> lock(financial_mutex_);

    current_pnl_ += pnl_change;
    daily_pnl_ += pnl_change;

    if (current_pnl_ > peak_pnl_) {
        peak_pnl_ = current_pnl_;
    }

    if (daily_pnl_ <= max_daily_loss_limit_) {
        recordRiskEvent(RiskEventType::DAILY_LOSS_LIMIT_EXCEEDED, RiskLevel::EMERGENCY,
                        "Daily loss limit exceeded: $" + std::to_string(daily_pnl_));
        triggerCircuitBreaker("Daily loss limit exceeded");
    }

    double current_drawdown = peak_pnl_ - current_pnl_;
    if (current_drawdown >= std::abs(max_drawdown_limit_)) {
        recordRiskEvent(RiskEventType::DRAWDOWN_LIMIT_EXCEEDED, RiskLevel::EMERGENCY,
                        "Drawdown limit exceeded: $" + std::to_string(current_drawdown));
        triggerCircuitBreaker("Drawdown limit exceeded");
    }

    if (daily_pnl_ <= max_daily_loss_limit_ * 0.7) {
        recordRiskEvent(RiskEventType::PNL_WARNING, RiskLevel::WARNING,
                        "Approaching daily loss limit: $" + std::to_string(daily_pnl_));
    }
}

RiskStatus RiskManager::getCurrentRiskStatus() const {
    if (circuit_breaker_active_.load()) {
        return RiskStatus::EMERGENCY;
    }

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

    if (critical_events > 0) return RiskStatus::CRITICAL;
    if (warning_events > 3) return RiskStatus::WARNING;

    return RiskStatus::NORMAL;
}

bool RiskManager::isCircuitBreakerActive() const {
    return circuit_breaker_active_.load();
}

void RiskManager::loadConfiguration() {
    Config& config = Config::getInstance();

    std::string trading_symbol = config.getConfig("TRADING_SYMBOL", "ETH-USD");
    double position_limit = std::stod(config.getConfig("POSITION_LIMIT_ETHUSDT", "1.0"));
    double daily_loss_limit = std::stod(config.getConfig("MAX_DAILY_LOSS_LIMIT", "100.0"));
    double drawdown_limit = std::stod(config.getConfig("MAX_DRAWDOWN_LIMIT", "50.0"));
    uint64_t order_rate_limit = static_cast<uint64_t>(config.getOrderRateLimit());

    {
        std::lock_guard<std::mutex> lock(position_mutex_);
        position_limits_[trading_symbol] = position_limit;
    }
    {
        std::lock_guard<std::mutex> lock(financial_mutex_);
        max_daily_loss_limit_ = -std::abs(daily_loss_limit);
        max_drawdown_limit_ = -std::abs(drawdown_limit);
    }
    {
        std::lock_guard<std::mutex> lock(operational_mutex_);
        max_orders_per_second_ = order_rate_limit;
    }
}

bool RiskManager::checkPositionLimits(const std::string& symbol, const std::string& side, double quantity) const {
    std::lock_guard<std::mutex> lock(position_mutex_);

    auto limit_it = position_limits_.find(symbol);
    if (limit_it == position_limits_.end()) {
        return true;
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

    double projected_daily_pnl = daily_pnl_ + estimated_pnl_impact;
    if (projected_daily_pnl <= max_daily_loss_limit_) {
        return false;
    }

    double projected_pnl = current_pnl_ + estimated_pnl_impact;
    double current_drawdown = peak_pnl_ - projected_pnl;
    if (current_drawdown >= std::abs(max_drawdown_limit_)) {
        return false;
    }

    return true;
}

bool RiskManager::checkOperationalLimits() {
    std::lock_guard<std::mutex> lock(operational_mutex_);

    auto now = std::chrono::system_clock::now();
    recent_orders_.push_back(now);

    cleanupOldOrders();

    auto one_second_ago = now - std::chrono::seconds(1);
    uint64_t orders_last_second = std::count_if(recent_orders_.begin(), recent_orders_.end(),
        [one_second_ago](const auto& timestamp) {
            return timestamp > one_second_ago;
        });

    return orders_last_second < max_orders_per_second_;
}

void RiskManager::triggerCircuitBreaker(const std::string& reason) {
    circuit_breaker_active_.store(true);
    circuit_breaker_reason_ = reason;

    recordRiskEvent(RiskEventType::CIRCUIT_BREAKER_TRIGGERED, RiskLevel::EMERGENCY,
                    "Circuit breaker triggered: " + reason);

    std::cout << "CIRCUIT BREAKER TRIGGERED: " << reason << std::endl;
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

    if (risk_events_.size() > MAX_RISK_EVENTS) {
        risk_events_.erase(risk_events_.begin(), risk_events_.begin() + static_cast<long>(risk_events_.size() - MAX_RISK_EVENTS));
    }

    if (level == RiskLevel::CRITICAL || level == RiskLevel::EMERGENCY) {
        std::cout << "RISK EVENT: " << message << std::endl;
    }
}

void RiskManager::cleanupOldOrders() {
    auto five_seconds_ago = std::chrono::system_clock::now() - std::chrono::seconds(5);
    recent_orders_.erase(
        std::remove_if(recent_orders_.begin(), recent_orders_.end(),
            [five_seconds_ago](const auto& timestamp) {
                return timestamp < five_seconds_ago;
            }),
        recent_orders_.end());
}
