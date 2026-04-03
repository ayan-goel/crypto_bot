#pragma once

#include <string>
#include <chrono>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>

enum class RiskEventType {
    POSITION_LIMIT_EXCEEDED,
    DAILY_LOSS_LIMIT_EXCEEDED,
    DRAWDOWN_LIMIT_EXCEEDED,
    ORDER_RATE_LIMIT_EXCEEDED,
    CIRCUIT_BREAKER_TRIGGERED,
    SYSTEM_INFO,
    POSITION_WARNING,
    PNL_WARNING
};

enum class RiskLevel {
    INFO,
    WARNING,
    CRITICAL,
    EMERGENCY
};

struct RiskEvent {
    RiskEventType type;
    RiskLevel level;
    std::string message;
    std::chrono::system_clock::time_point timestamp;
    std::string symbol;
    double value;
    double limit;
};

enum class RiskStatus {
    NORMAL,
    WARNING,
    CRITICAL,
    EMERGENCY
};

class RiskManager {
public:
    RiskManager();
    ~RiskManager();

    bool initialize(const std::string& config_file);
    void shutdown();

    bool canPlaceOrder(const std::string& symbol, const std::string& side,
                       double price, double quantity, std::string& rejection_reason);

    void updatePnL(double pnl_change);
    void updatePosition(const std::string& symbol, double position);

    RiskStatus getCurrentRiskStatus() const;
    bool isCircuitBreakerActive() const;

private:
    mutable std::mutex position_mutex_;
    std::map<std::string, double> positions_;
    std::map<std::string, double> position_limits_;

    mutable std::mutex financial_mutex_;
    double current_pnl_ = 0.0;
    double daily_pnl_ = 0.0;
    double peak_pnl_ = 0.0;
    double max_daily_loss_limit_ = -100.0;
    double max_drawdown_limit_ = -50.0;
    std::chrono::system_clock::time_point daily_reset_time_;

    mutable std::mutex operational_mutex_;
    std::vector<std::chrono::system_clock::time_point> recent_orders_;
    uint64_t max_orders_per_second_ = 10;
    std::atomic<bool> circuit_breaker_active_{false};
    std::string circuit_breaker_reason_;

    mutable std::mutex events_mutex_;
    std::vector<RiskEvent> risk_events_;
    static constexpr size_t MAX_RISK_EVENTS = 1000;

    void loadConfiguration();
    bool checkPositionLimits(const std::string& symbol, const std::string& side, double quantity) const;
    bool checkFinancialLimits(double estimated_pnl_impact) const;
    bool checkOperationalLimits();
    void triggerCircuitBreaker(const std::string& reason);
    void recordRiskEvent(RiskEventType type, RiskLevel level, const std::string& message,
                         const std::string& symbol = "", double value = 0.0, double limit = 0.0);
    void cleanupOldOrders();
};
