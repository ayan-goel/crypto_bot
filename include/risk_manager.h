#pragma once

#include <string>
#include <chrono>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <memory>
#include <thread>

// Risk event types for monitoring and alerting
enum class RiskEventType {
    POSITION_LIMIT_EXCEEDED,
    DAILY_LOSS_LIMIT_EXCEEDED,
    DRAWDOWN_LIMIT_EXCEEDED,
    ORDER_RATE_LIMIT_EXCEEDED,
    CIRCUIT_BREAKER_TRIGGERED,
    PRICE_DEVIATION_EXTREME,
    SYSTEM_HEALTH_CRITICAL,
    POSITION_WARNING,
    PNL_WARNING
};

// Risk levels for different types of events
enum class RiskLevel {
    INFO,       // Informational
    WARNING,    // Warning but trading continues
    CRITICAL,   // Critical - reduce trading
    EMERGENCY   // Emergency - stop all trading
};

// Risk event structure
struct RiskEvent {
    RiskEventType type;
    RiskLevel level;
    std::string message;
    std::chrono::system_clock::time_point timestamp;
    std::string symbol;
    double value;
    double limit;
};

// Position risk metrics
struct PositionRisk {
    double current_position = 0.0;
    double max_position_limit = 0.0;
    double position_utilization = 0.0;  // % of limit used
    bool position_limit_breached = false;
};

// Financial risk metrics  
struct FinancialRisk {
    double current_pnl = 0.0;
    double daily_pnl = 0.0;
    double max_daily_loss_limit = 0.0;
    double max_drawdown_limit = 0.0;
    double current_drawdown = 0.0;
    double peak_pnl = 0.0;
    bool daily_loss_limit_breached = false;
    bool drawdown_limit_breached = false;
};

// Operational risk metrics
struct OperationalRisk {
    uint64_t orders_per_second = 0;
    uint64_t max_orders_per_second = 0;
    std::chrono::system_clock::time_point last_order_time;
    std::chrono::system_clock::time_point session_start_time;
    bool order_rate_limit_breached = false;
    bool circuit_breaker_active = false;
    std::string circuit_breaker_reason;
};

// Overall risk status
enum class RiskStatus {
    NORMAL,     // All systems normal
    WARNING,    // Some warnings but trading continues
    CRITICAL,   // Critical issues - reduce trading
    EMERGENCY   // Emergency stop - halt all trading
};

class RiskManager {
public:
    RiskManager();
    ~RiskManager();
    
    // Initialization
    bool initialize(const std::string& config_file);
    void shutdown();
    
    // Pre-trade risk checks
    bool canPlaceOrder(const std::string& symbol, const std::string& side, 
                      double price, double quantity, std::string& rejection_reason);
    
    // Post-trade updates
    void updatePosition(const std::string& symbol, double quantity, double price, const std::string& side);
    void updatePnL(double pnl_change);
    void recordOrderPlaced();
    
    // Risk monitoring
    RiskStatus getCurrentRiskStatus() const;
    std::vector<RiskEvent> getRecentRiskEvents(int count = 10) const;
    
    // Risk metrics
    PositionRisk getPositionRisk(const std::string& symbol) const;
    FinancialRisk getFinancialRisk() const;
    OperationalRisk getOperationalRisk() const;
    
    // Circuit breaker controls
    void triggerCircuitBreaker(const std::string& reason);
    void resetCircuitBreaker();
    bool isCircuitBreakerActive() const;
    
    // Risk limits management
    void setPositionLimit(const std::string& symbol, double limit);
    void setDailyLossLimit(double limit);
    void setDrawdownLimit(double limit);
    void setOrderRateLimit(uint64_t orders_per_second);
    
    // Real-time monitoring
    void startRiskMonitoring();
    void stopRiskMonitoring();
    
    // Risk reporting
    void generateRiskReport(const std::string& filename) const;
    std::string getRiskSummary() const;
    
private:
    // Configuration
    mutable std::mutex config_mutex_;
    std::string config_file_;
    
    // Position tracking
    mutable std::mutex position_mutex_;
    std::map<std::string, double> positions_;  // symbol -> net position
    std::map<std::string, double> position_limits_;  // symbol -> max position
    
    // Financial tracking
    mutable std::mutex financial_mutex_;
    double current_pnl_ = 0.0;
    double daily_pnl_ = 0.0;
    double peak_pnl_ = 0.0;
    double max_daily_loss_limit_ = -100.0;  // Default: -$100
    double max_drawdown_limit_ = -50.0;     // Default: -$50
    std::chrono::system_clock::time_point daily_reset_time_;
    
    // Operational tracking
    mutable std::mutex operational_mutex_;
    std::vector<std::chrono::system_clock::time_point> recent_orders_;
    uint64_t max_orders_per_second_ = 10;  // Default: 10 orders/sec
    std::atomic<bool> circuit_breaker_active_{false};
    std::string circuit_breaker_reason_;
    std::chrono::system_clock::time_point session_start_time_;
    
    // Risk events
    mutable std::mutex events_mutex_;
    std::vector<RiskEvent> risk_events_;
    static constexpr size_t MAX_RISK_EVENTS = 1000;
    
    // Risk monitoring
    std::atomic<bool> monitoring_active_{false};
    std::thread monitoring_thread_;
    
    // Internal methods
    void loadConfiguration();
    void resetDailyLimits();
    bool checkPositionLimits(const std::string& symbol, const std::string& side, double quantity) const;
    bool checkFinancialLimits(double estimated_pnl_impact) const;
    bool checkOperationalLimits() const;
    void recordRiskEvent(RiskEventType type, RiskLevel level, const std::string& message, 
                        const std::string& symbol = "", double value = 0.0, double limit = 0.0);
    void monitoringLoop();
    void updateOrderRate();
    std::string formatRiskEvent(const RiskEvent& event) const;
}; 