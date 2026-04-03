#pragma once

#include "core/types.h"
#include <string>
#include <mutex>
#include <chrono>
#include <cstdint>

class RiskManager;

struct OrderResponse {
    bool success = false;
    std::string order_id;
    std::string status;
    std::string error_message;
    double filled_quantity = 0.0;
    double avg_fill_price = 0.0;
};

class OrderManager {
public:
    OrderManager();
    ~OrderManager();

    bool initialize();
    void shutdown();

    OrderResponse placeOrder(const std::string& symbol, Side side, double price, double quantity);

    uint64_t getTotalTrades() const;
    double getCurrentPnL() const;
    double getCurrentPosition() const;

    void setRiskManager(RiskManager* risk_manager);

private:
    mutable std::mutex pnl_mutex_;
    double current_position_ = 0.0;
    double previous_position_ = 0.0;
    double avg_buy_price_ = 0.0;
    double cumulative_pnl_ = 0.0;

    mutable std::mutex session_mutex_;
    std::chrono::system_clock::time_point session_start_time_;
    std::chrono::system_clock::time_point session_end_time_;
    uint64_t buy_trades_ = 0;
    uint64_t sell_trades_ = 0;
    double total_buy_volume_ = 0.0;
    double total_sell_volume_ = 0.0;

    uint64_t orders_placed_ = 0;
    uint64_t orders_filled_ = 0;

    RiskManager* risk_manager_ = nullptr;

    std::string generateClientOrderId();

    OrderResponse executeOrder(const std::string& symbol, Side side, double price, double quantity);
    bool validateOrder(const std::string& symbol, Side side, double price, double quantity) const;
    void simulateOrderFill(Order& order);
    void updatePositionAndPnL(const Order& order);

    void initializeSession();
    void updateSessionStats(const Order& order);
    void generateSessionSummary();
};
