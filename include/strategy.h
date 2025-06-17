#pragma once

#include "order_book.h"
#include <string>
#include <vector>
#include <chrono>
#include <memory>

struct Order {
    std::string order_id;
    std::string symbol;
    std::string side;  // BUY or SELL
    std::string type;  // LIMIT, MARKET, etc.
    double price;
    double quantity;
    double filled_quantity = 0.0;
    std::string status;  // NEW, PARTIALLY_FILLED, FILLED, CANCELED, etc.
    std::chrono::system_clock::time_point create_time;
    std::chrono::system_clock::time_point update_time;
    std::string client_order_id;
};

struct Position {
    std::string symbol;
    double quantity = 0.0;  // Net position (positive = long, negative = short)
    double avg_price = 0.0;
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
    std::chrono::system_clock::time_point last_update;
};

struct StrategySignal {
    bool should_place_bid = false;
    bool should_place_ask = false;
    bool should_cancel_orders = false;
    double bid_price = 0.0;
    double ask_price = 0.0;
    double bid_quantity = 0.0;
    double ask_quantity = 0.0;
    std::string reason;
};

class Strategy {
public:
    explicit Strategy(const std::string& symbol);
    ~Strategy() = default;
    
    // Main strategy logic
    StrategySignal generateSignal(const OrderBookSnapshot& orderbook);
    
    // Position management
    void updatePosition(const Order& filled_order);
    Position getCurrentPosition() const;
    double getUnrealizedPnL(double current_price) const;
    double getTotalPnL(double current_price) const;
    
    // Risk checks
    bool isWithinRiskLimits(const StrategySignal& signal) const;
    bool isInventoryWithinLimits(double additional_quantity) const;
    bool isDailyDrawdownExceeded() const;
    
    // Order management helpers
    void addPendingOrder(const Order& order);
    void updatePendingOrder(const Order& order);
    void removePendingOrder(const std::string& order_id);
    std::vector<Order> getPendingOrders() const;
    
    // Strategy parameters (can be updated dynamically)
    void setSpreadThreshold(double bps);
    void setOrderSize(double size);
    void setMaxInventory(double max_inventory);
    void setOrderOffset(double bid_offset_bps, double ask_offset_bps);
    
    // Statistics and monitoring
    void updateStats(const OrderBookSnapshot& orderbook);
    void printStats() const;
    void reset();
    
    // Circuit breaker
    void enableCircuitBreaker(bool enabled);
    bool isCircuitBreakerTriggered() const;

private:
    std::string symbol_;
    
    // Strategy parameters
    double spread_threshold_bps_ = 5.0;
    double order_size_ = 0.001;
    double max_inventory_ = 0.01;
    double bid_offset_bps_ = 1.0;  // How far below best bid to place our bid
    double ask_offset_bps_ = 1.0;  // How far above best ask to place our ask
    double max_daily_drawdown_ = 20.0;
    
    // State
    Position current_position_;
    std::vector<Order> pending_orders_;
    mutable std::mutex mutex_;
    
    // Risk management
    bool circuit_breaker_enabled_ = true;
    bool circuit_breaker_triggered_ = false;
    double daily_start_balance_ = 0.0;
    std::chrono::system_clock::time_point day_start_time_;
    
    // Statistics
    uint64_t signals_generated_ = 0;
    uint64_t orders_placed_ = 0;
    uint64_t orders_filled_ = 0;
    double total_volume_ = 0.0;
    double total_fees_ = 0.0;
    
    // Helper methods
    double calculateOptimalBidPrice(const OrderBookSnapshot& orderbook) const;
    double calculateOptimalAskPrice(const OrderBookSnapshot& orderbook) const;
    double calculateOrderQuantity(const std::string& side) const;
    bool shouldPlaceNewOrders(const OrderBookSnapshot& orderbook) const;
    bool hasRecentOrders() const;
    void updateDailyTracking();
    
    // Validation
    bool validateSignal(const StrategySignal& signal, const OrderBookSnapshot& orderbook) const;
}; 